package main

// The PS5 as its own tailnet server (server = on): Headscale, the
// open-source Tailscale coordination server, runs inside this program, so
// no Tailscale account is needed. The PS5 joins its own tailnet with a key
// it makes itself, and approves its own exit node and home network routes.
//
// Phones and computers join with the Tailscale app pointed at the server's
// address (server_url: by default http://HOME_IP:server_port, found by
// asking api.ipify.org). That port has to be forwarded on the router to the
// PS5. A device asking to join waits until it's approved on the PS5: in the
// app, on the status page at home, or on http://ps5/ from a device already
// in. Connections between devices still go directly where they can, or
// through Tailscale's public relays (DERP) where they can't.

import (
	"context"
	"errors"
	"fmt"
	"html/template"
	"io"
	"net"
	"net/http"
	"net/http/httputil"
	"net/netip"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	hscontrol "github.com/juanfont/headscale/hscontrol"
	hsstate "github.com/juanfont/headscale/hscontrol/state"
	"github.com/juanfont/headscale/hscontrol/types"
	"github.com/juanfont/headscale/hscontrol/util"
	"github.com/rs/zerolog"
	zlog "github.com/rs/zerolog/log"
	"tailscale.com/client/local"
	"tailscale.com/types/key"
)

const serverUser = "owner" // every device in the PS5's tailnet belongs to it

// relayMapURL: Tailscale's public relays (DERP), for devices that can't
// reach each other directly. The test points it at its own.
var relayMapURL = "https://controlplane.tailscale.com/derpmap/default"

type tailnetServer struct {
	app   *hscontrol.Headscale
	st    *hsstate.State
	url   string // what devices use: server_url
	local string // Headscale itself, on a loopback port that changes
	self  string // where the PS5's own node reaches it: the front door, by loopback
	log   *logger

	mu      sync.Mutex
	pending map[string]pendingJoin // by auth ID
	user    types.UserID
}

type pendingJoin struct {
	ID, Name string
	When     time.Time
}

// startServer starts Headscale and the front door devices reach it through.
func startServer(ctx context.Context, dir string, cfg settings, log *logger) (*tailnetServer, error) {
	hsDir := filepath.Join(dir, "headscale")
	if err := os.MkdirAll(hsDir, 0o700); err != nil {
		return nil, err
	}
	logFile := &cappedLog{path: filepath.Join(dir, "headscale.log")}
	zlog.Logger = zerolog.New(logFile).With().Timestamp().Logger()

	public := cfg.ServerURL
	if public == "" {
		ip, err := homeAddress(ctx)
		if err != nil {
			return nil, fmt.Errorf("can't find this home's internet address (%v): set server_url in settings.conf", err)
		}
		public = fmt.Sprintf("http://%s", net.JoinHostPort(ip, fmt.Sprint(cfg.ServerPort)))
	}
	inner, err := freeLocalPort()
	if err != nil {
		return nil, err
	}
	local := fmt.Sprintf("http://127.0.0.1:%d", inner)
	conf := fmt.Sprintf(`server_url: %q
listen_addr: 127.0.0.1:%d
metrics_listen_addr: ""
grpc_listen_addr: 127.0.0.1:0
grpc_allow_insecure: false
disable_check_updates: true
noise:
  private_key_path: %q
prefixes:
  v4: 100.64.0.0/10
  v6: fd7a:115c:a1e0::/48
  allocation: sequential
derp:
  server:
    enabled: false
  urls:
    - %s
  auto_update_enabled: true
  update_frequency: 3h
database:
  type: sqlite
  sqlite:
    path: %q
    write_ahead_log: true
log:
  level: info
  format: json
policy:
  mode: file
  path: ""
dns:
  magic_dns: true
  base_domain: %q
  override_local_dns: true
  nameservers:
    global:
      - %s
unix_socket: %q
unix_socket_permission: "0700"
logtail:
  enabled: false
taildrop:
  enabled: true
`, public, inner, filepath.Join(hsDir, "noise_private.key"), relayMapURL, filepath.Join(hsDir, "db.sqlite"),
		cfg.ServerDomain, cfg.DNS, filepath.Join(hsDir, "headscale.sock"))
	confPath := filepath.Join(hsDir, "config.yaml")
	if err := os.WriteFile(confPath, []byte(conf), 0o600); err != nil {
		return nil, err
	}
	if err := types.LoadConfig(confPath, true); err != nil {
		return nil, fmt.Errorf("Headscale's settings: %w", err)
	}
	hcfg, err := types.LoadServerConfig()
	if err != nil {
		return nil, fmt.Errorf("Headscale's settings: %w", err)
	}
	app, err := hscontrol.NewHeadscale(hcfg)
	if err != nil {
		return nil, fmt.Errorf("Headscale didn't start: %w", err)
	}
	s := &tailnetServer{app: app, st: app.GetState(), url: public, local: local, log: log,
		self: fmt.Sprintf("http://127.0.0.1:%d", cfg.ServerPort), pending: map[string]pendingJoin{}}
	u, err := s.st.GetUserByName(serverUser)
	if err != nil {
		u, _, err = s.st.CreateUser(types.User{Name: serverUser})
		if err != nil {
			return nil, fmt.Errorf("Headscale: %w", err)
		}
	}
	s.user = types.UserID(u.ID)

	served := make(chan error, 1)
	go func() { served <- app.Serve() }()
	if err := waitHTTP(ctx, local+"/health", served); err != nil {
		return nil, err
	}

	front, err := net.Listen("tcp", fmt.Sprintf(":%d", cfg.ServerPort))
	if err != nil {
		return nil, fmt.Errorf("the server port %d: %w", cfg.ServerPort, err)
	}
	go (&http.Server{Handler: s.frontDoor(), ReadHeaderTimeout: 10 * time.Second}).Serve(front)
	log.Say("tailnet server: devices join at %s (forward port %d on the router to this PS5)", public, cfg.ServerPort)
	return s, nil
}

// homeAddress: this home's internet address, as a web service sees it.
func homeAddress(ctx context.Context) (string, error) {
	ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	for _, u := range []string{"https://api.ipify.org", "https://ifconfig.me/ip"} {
		req, _ := http.NewRequestWithContext(ctx, "GET", u, nil)
		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			continue
		}
		b, _ := io.ReadAll(io.LimitReader(resp.Body, 100))
		resp.Body.Close()
		if a, err := netip.ParseAddr(strings.TrimSpace(string(b))); err == nil {
			return a.String(), nil
		}
	}
	return "", errors.New("no answer from api.ipify.org or ifconfig.me")
}

func freeLocalPort() (int, error) {
	l, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return 0, err
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port, nil
}

func waitHTTP(ctx context.Context, u string, served <-chan error) error {
	for i := 0; i < 150; i++ {
		select {
		case err := <-served:
			return fmt.Errorf("Headscale stopped: %v", err)
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
		if resp, err := http.Get(u); err == nil {
			resp.Body.Close()
			return nil
		}
		time.Sleep(200 * time.Millisecond)
	}
	return errors.New("Headscale didn't answer within 30 seconds")
}

// frontDoor: Headscale itself, except the page a joining device's browser
// opens, which says to approve it on the PS5 (and notes it as waiting).
func (s *tailnetServer) frontDoor() http.Handler {
	target, _ := url.Parse(s.local)
	proxy := httputil.NewSingleHostReverseProxy(target)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		rest, ok := strings.CutPrefix(r.URL.Path, "/register/")
		if ok && rest != "" && !strings.Contains(rest, "/") {
			if name, ok := s.noteJoin(rest); ok {
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				joinPage.Execute(w, name)
				return
			}
		}
		proxy.ServeHTTP(w, r)
	})
}

// noteJoin records a device waiting to join, by its auth ID; false when
// Headscale has no such request.
func (s *tailnetServer) noteJoin(id string) (string, bool) {
	aid, err := types.AuthIDFromString(id)
	if err != nil {
		return "", false
	}
	req, ok := s.st.GetAuthCacheEntry(aid)
	if !ok || !req.IsRegistration() {
		return "", false
	}
	name := "a device"
	if rd := req.RegistrationData(); rd != nil && rd.Hostname != "" {
		name = rd.Hostname
	}
	s.mu.Lock()
	if _, seen := s.pending[id]; !seen {
		s.pending[id] = pendingJoin{ID: id, Name: name, When: time.Now()}
		s.log.Say("tailnet server: %s asks to join; approve it on the PS5", name)
	}
	s.mu.Unlock()
	return name, true
}

// waiting: devices asking to join, oldest first; gone ones dropped.
func (s *tailnetServer) waiting() []pendingJoin {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []pendingJoin
	for id, p := range s.pending {
		aid, err := types.AuthIDFromString(id)
		if req, ok := s.st.GetAuthCacheEntry(aid); err != nil || !ok || req.IsFinished() {
			delete(s.pending, id)
			continue
		}
		out = append(out, p)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].When.Before(out[j].When) })
	return out
}

// approve lets a waiting device in; deny forgets it (its request times out).
func (s *tailnetServer) approve(id string) error {
	aid, err := types.AuthIDFromString(id)
	if err != nil {
		return err
	}
	node, nodeChange, err := s.st.HandleNodeFromAuthPath(aid, s.user, nil, util.RegisterMethodCLI)
	if err != nil {
		return err
	}
	routeChange, err := s.st.AutoApproveRoutes(node)
	if err != nil {
		return err
	}
	s.app.Change(nodeChange, routeChange)
	s.mu.Lock()
	delete(s.pending, id)
	s.mu.Unlock()
	s.log.Say("tailnet server: %s joined", node.Hostname())
	return nil
}

func (s *tailnetServer) deny(id string) {
	s.mu.Lock()
	if p, ok := s.pending[id]; ok {
		s.log.Say("tailnet server: %s turned away", p.Name)
	}
	delete(s.pending, id)
	s.mu.Unlock()
}

// keyForPS5: a one-time key the PS5's own node signs in with.
func (s *tailnetServer) keyForPS5() (string, error) {
	exp := time.Now().Add(time.Hour)
	k, err := s.st.CreatePreAuthKey(&s.user, false, false, &exp, nil)
	if err != nil {
		return "", err
	}
	return k.Key, nil
}

// approveOwnRoutes approves whatever the PS5's own node offers (the exit
// node, the home network): on its own server, the owner already chose them
// in settings.conf.
func (s *tailnetServer) approveOwnRoutes(self key.NodePublic) {
	n, ok := s.st.GetNodeByNodeKey(self)
	if !ok {
		return
	}
	offered := n.AnnouncedRoutes()
	if samePrefixes(offered, n.ApprovedRoutes().AsSlice()) {
		return
	}
	_, c, err := s.st.SetApprovedRoutes(n.ID(), offered)
	if err != nil {
		s.log.Say("tailnet server: approving %v: %v", offered, err)
		return
	}
	s.app.Change(c)
	s.log.Say("tailnet server: the PS5's %v approved", offered)
}

// keepOwnRoutesApproved follows the PS5's node: every few seconds at first,
// then every minute (the home network can change).
func (s *tailnetServer) keepOwnRoutesApproved(ctx context.Context, lc *local.Client) {
	for i := 0; ctx.Err() == nil; i++ {
		wait := 3 * time.Second
		if i > 40 {
			wait = time.Minute
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(wait):
		}
		sctx, cancel := context.WithTimeout(ctx, 5*time.Second)
		st, err := lc.StatusWithoutPeers(sctx)
		cancel()
		if err == nil && st.Self != nil && !st.Self.PublicKey.IsZero() {
			s.approveOwnRoutes(st.Self.PublicKey)
		}
	}
}

// devices: who is in the PS5's tailnet, as the server knows them.
func (s *tailnetServer) devices() int { return s.st.ListNodes().Len() }

var joinPage = template.Must(template.New("join").Parse(`<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Waiting for approval · TailStation</title>
<style>body{margin:0;background:#08090b;color:#f2f2f2;font:17px/1.5 system-ui,sans-serif}
main{max-width:600px;margin:0 auto;padding:32px 18px}h1{color:#e4eaf3;font-size:26px}.dim{color:#9a9a9e}</style>
</head><body><main>
<h1>Almost in</h1>
<p><strong>{{.}}</strong> is asking to join the tailnet run by a PS5. The PS5's owner approves it:</p>
<ul>
<li>on the PS5: the TailStation app, <em>Devices</em> page, or</li>
<li>at home: the PS5's status page (port 9039), or</li>
<li>from a device already in: <em>http://ps5/</em></li>
</ul>
<p class="dim">Keep the Tailscale app open: it connects by itself once approved.</p>
</main></body></html>
`))
