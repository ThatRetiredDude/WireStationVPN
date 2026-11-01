// wirestation: puts a PS5 on a Tailscale network, as a payload.
//
// Started through the jailbreak's ELF loader (port 9021), it joins the
// owner's tailnet with Tailscale's own code (tsnet, in user space: no
// network interface or root needed) and offers the PS5 as an exit node
// and as a route to the home network. Phones and computers use the normal
// Tailscale app; nothing needs a router port forward.
//
// Its files live in WS_DIR (/data/wirestation on the console):
//
//	settings.conf  optional: hostname, exit_node, home_network, auth_key,
//	               control_url, port, status_port, dns, tags (see
//	               settings.go)
//	tailscale/     Tailscale's state: the node's keys and login
//	log.txt        what it did, with a line a minute while it runs
//	tailscale.log  Tailscale's own log, for troubleshooting
//
// A page on the home network, http://PS5_IP:9039/, shows the sign-in link
// and QR code, then the PS5's tailnet name and who is connected. That port
// is also how a second copy finds the first and leaves.
package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/netip"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"tailscale.com/client/local"
	_ "tailscale.com/feature/taildrop"
	"tailscale.com/ipn"
	"tailscale.com/net/tsaddr"
	"tailscale.com/tsnet"
	"tailscale.com/types/nettype"
	"tailscale.com/wgengine/netstack"
)

var version = "dev"

const defaultDir = "/data/wirestation"

func main() {
	dir := os.Getenv("WS_DIR")
	if len(os.Args) > 1 {
		dir = os.Args[1]
	}
	if dir == "" {
		dir = defaultDir
	}
	os.Exit(run(dir))
}

func run(dir string) int {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		fmt.Fprintf(os.Stderr, "wirestation: can't make %s: %v\n", dir, err)
		return 1
	}
	log := openLog(dir)
	defer log.Close()
	log.Say("WireStationVPN %s starting, files in %s", version, dir)

	cfg, problems := loadSettings(filepath.Join(dir, "settings.conf"))
	for _, p := range problems {
		log.Say("settings.conf: %s", p)
	}
	if cfg.TaildropDir == "" {
		cfg.TaildropDir = filepath.Join(dir, "received")
	}
	for i, f := range cfg.Shared {
		if f == "taildrop" {
			cfg.Shared[i] = cfg.TaildropDir
		}
	}

	useDNS(cfg.DNS, log)

	// One copy at a time: the status port is the lock.
	ln, err := net.Listen("tcp4", fmt.Sprintf(":%d", cfg.StatusPort))
	if err != nil {
		if errors.Is(err, syscall.EADDRINUSE) {
			log.Say("already running (port %d is taken); leaving", cfg.StatusPort)
		} else {
			log.Say("can't open the status port %d: %v", cfg.StatusPort, err)
		}
		return 1
	}

	tsLog := openTailscaleLog(dir)
	defer tsLog.Close()
	srv := &tsnet.Server{
		Dir:           filepath.Join(dir, "tailscale"),
		Hostname:      cfg.Hostname,
		AuthKey:       cfg.AuthKey,
		ControlURL:    cfg.ControlURL,
		Port:          uint16(cfg.Port),
		AdvertiseTags: cfg.Tags,
		Logf:          func(f string, a ...any) { fmt.Fprintf(tsLog, time.Now().Format("15:04:05 ")+f+"\n", a...) },
		UserLogf:      func(f string, a ...any) { log.Say("tailscale: "+f, a...) },
	}
	defer srv.Close()

	st := &state{cfg: cfg, started: time.Now(), log: log}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	st.quit = stop

	go serveStatus(ln, st)

	if err := srv.Start(); err != nil {
		log.Say("Tailscale didn't start: %v", err)
		return 1
	}
	if err := forwardLikeTailscaled(srv); err != nil {
		log.Say("%v", err)
		return 1
	}
	lc, err := srv.LocalClient()
	if err != nil {
		log.Say("no Tailscale client: %v", err)
		return 1
	}
	st.set(func(s *state) { s.lc = lc })
	go watchBus(ctx, lc, st)
	go alive(ctx, lc, st)
	if cfg.Taildrop {
		go receiveFiles(ctx, lc, st, cfg.TaildropDir)
	}
	if cfg.TailnetPage {
		go serveTailnet(srv, st, dir)
	}

	<-ctx.Done()
	log.Say("stopping")
	return 0
}

// forwardLikeTailscaled makes the PS5 forward what devices send through it
// (exit node, home network) to the internet and the LAN, TCP and UDP, as
// tailscaled does in its user-space mode. tsnet on its own only serves its
// own listeners and drops the rest. Connections to the PS5's own Tailscale
// address still go to tsnet: to this program's tailnet listeners (port 80)
// or refused, so the console's local services (the ELF loader, FTP) stay
// off the tailnet. Set before sign-in, so before any traffic.
func forwardLikeTailscaled(srv *tsnet.Server) error {
	ns, ok := srv.Sys().Netstack.Get().(*netstack.Impl)
	if !ok {
		return errors.New("Tailscale's network stack isn't the one this program expects")
	}
	own := func(dst netip.AddrPort) bool {
		a := dst.Addr()
		return tsaddr.IsTailscaleIP(a) && a != tsaddr.TailscaleServiceIP() && a != tsaddr.TailscaleServiceIPv6()
	}
	tsnetTCP, tsnetUDP := ns.GetTCPHandlerForFlow, ns.GetUDPHandlerForFlow
	if tsnetTCP == nil || tsnetUDP == nil {
		return errors.New("Tailscale's network stack has no flow handlers to wrap")
	}
	ns.GetTCPHandlerForFlow = func(src, dst netip.AddrPort) (func(net.Conn), bool) {
		if own(dst) {
			return tsnetTCP(src, dst)
		}
		return nil, false // forward
	}
	ns.GetUDPHandlerForFlow = func(src, dst netip.AddrPort) (func(nettype.ConnPacketConn), bool) {
		if own(dst) {
			return tsnetUDP(src, dst)
		}
		return nil, false
	}
	return nil
}

// useDNS points Go's resolver at server when the system has no
// /etc/resolv.conf, which the PS5 isn't known to have: without one, Go
// asks 127.0.0.1, where nothing answers.
func useDNS(server string, log *logger) {
	if _, err := os.Stat("/etc/resolv.conf"); err == nil {
		return
	}
	addr := net.JoinHostPort(server, "53")
	var d net.Dialer
	net.DefaultResolver = &net.Resolver{
		PreferGo: true,
		Dial: func(ctx context.Context, network, _ string) (net.Conn, error) {
			return d.DialContext(ctx, network, addr)
		},
	}
	log.Say("no /etc/resolv.conf: DNS goes to %s", server)
}

// state is what the status page shows, kept up by watchBus and alive.
type state struct {
	cfg     settings
	started time.Time
	log     *logger
	quit    func()

	mu       sync.Mutex
	lc       *local.Client // set once Tailscale has started
	backend  string        // Tailscale's state: NeedsLogin, Starting, Running ...
	loginURL string
	routes   []netip.Prefix // what the PS5 offers
	received []string       // the last files received with Taildrop
	lastErr  string
}

func (s *state) client() *local.Client {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.lc
}

func (s *state) set(f func(*state)) {
	s.mu.Lock()
	defer s.mu.Unlock()
	f(s)
}

// watchBus follows Tailscale's state: the sign-in link while it needs one,
// and the routes to offer once it's running.
func watchBus(ctx context.Context, lc *local.Client, st *state) {
	for ctx.Err() == nil {
		w, err := lc.WatchIPNBus(ctx, ipn.NotifyInitialState|ipn.NotifyInitialPrefs)
		if err != nil {
			st.log.Say("can't follow Tailscale: %v", err)
			time.Sleep(2 * time.Second)
			continue
		}
		for {
			n, err := w.Next()
			if err != nil {
				break
			}
			if n.ErrMessage != nil {
				st.log.Say("tailscale: %s", *n.ErrMessage)
				st.set(func(s *state) { s.lastErr = *n.ErrMessage })
			}
			if n.BrowseToURL != nil && *n.BrowseToURL != "" {
				url := *n.BrowseToURL
				st.set(func(s *state) { s.loginURL = url })
				st.log.Say("sign in at %s (the status page has a QR code)", url)
			}
			if n.State != nil {
				was := ""
				st.set(func(s *state) { was = s.backend; s.backend = n.State.String() })
				if was != n.State.String() {
					st.log.Say("tailscale: %s", n.State)
				}
				if *n.State == ipn.Running {
					st.set(func(s *state) { s.loginURL = ""; s.lastErr = "" })
					offerRoutes(ctx, lc, st)
				}
			}
		}
		w.Close()
	}
}

// offerRoutes advertises the exit node (all of the internet) and the home
// network, as settings.conf says. The tailnet's owner approves them once in
// Tailscale's admin page.
func offerRoutes(ctx context.Context, lc *local.Client, st *state) {
	var routes []netip.Prefix
	if st.cfg.ExitNode {
		routes = append(routes, netip.MustParsePrefix("0.0.0.0/0"), netip.MustParsePrefix("::/0"))
	}
	if st.cfg.HomeNetwork {
		routes = append(routes, homeNetworks(st.log)...)
	}
	prefs, err := lc.GetPrefs(ctx)
	if err == nil && samePrefixes(prefs.AdvertiseRoutes, routes) {
		st.set(func(s *state) { s.routes = routes })
		return
	}
	_, err = lc.EditPrefs(ctx, &ipn.MaskedPrefs{
		Prefs:              ipn.Prefs{AdvertiseRoutes: routes},
		AdvertiseRoutesSet: true,
	})
	if err != nil {
		st.log.Say("can't offer routes %v: %v", routes, err)
		return
	}
	st.set(func(s *state) { s.routes = routes })
	st.log.Say("offering %v", routes)
}

func samePrefixes(a, b []netip.Prefix) bool {
	if len(a) != len(b) {
		return false
	}
	seen := map[netip.Prefix]bool{}
	for _, p := range a {
		seen[p] = true
	}
	for _, p := range b {
		if !seen[p] {
			return false
		}
	}
	return true
}

// alive writes a line a minute, so gaps in log.txt show when the PS5 held
// the process (a game starting, rest mode).
func alive(ctx context.Context, lc *local.Client, st *state) {
	t := time.NewTicker(time.Minute)
	defer t.Stop()
	last := time.Now()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-t.C:
			if gap := now.Sub(last); gap > 90*time.Second {
				st.log.Say("the last minute took %s: the process was held", gap.Round(time.Second))
			}
			last = now
			sctx, cancel := context.WithTimeout(ctx, 5*time.Second)
			s, err := lc.Status(sctx)
			cancel()
			if err != nil {
				st.log.Say("alive, but no status: %v", err)
				continue
			}
			online := 0
			for _, p := range s.Peer {
				if p.Online {
					online++
				}
			}
			st.log.Say("alive: %s, %d of %d devices online", s.BackendState, online, len(s.Peer))
		}
	}
}

// homeNetworks: the private IPv4 networks the PS5 is on. When the system
// won't list its interfaces, the address it sends from, as a /24.
func homeNetworks(log *logger) []netip.Prefix {
	var out []netip.Prefix
	ifs, err := net.Interfaces()
	if err == nil {
		for _, ifc := range ifs {
			if ifc.Flags&net.FlagUp == 0 || ifc.Flags&net.FlagLoopback != 0 {
				continue
			}
			addrs, _ := ifc.Addrs()
			for _, a := range addrs {
				n, ok := a.(*net.IPNet)
				if !ok {
					continue
				}
				ones, _ := n.Mask.Size()
				ip, ok := netip.AddrFromSlice(n.IP)
				if !ok || !ip.Unmap().Is4() || !ip.IsPrivate() || ones < 8 {
					continue
				}
				out = append(out, netip.PrefixFrom(ip.Unmap(), ones).Masked())
			}
		}
	}
	if len(out) > 0 {
		return out
	}
	if err != nil {
		log.Say("can't list the network interfaces (%v); guessing the home network", err)
	}
	c, err := net.Dial("udp4", "1.1.1.1:53") // nothing is sent
	if err != nil {
		return nil
	}
	defer c.Close()
	ip, ok := netip.AddrFromSlice(c.LocalAddr().(*net.UDPAddr).IP)
	if !ok || !ip.Unmap().IsPrivate() {
		return nil
	}
	return []netip.Prefix{netip.PrefixFrom(ip.Unmap(), 24).Masked()}
}

// logger writes log.txt and stdout, with the time of day and the seconds
// since the start.
type logger struct {
	mu    sync.Mutex
	f     io.WriteCloser
	start time.Time
}

func openLog(dir string) *logger {
	path := filepath.Join(dir, "log.txt")
	if fi, err := os.Stat(path); err == nil && fi.Size() > 1<<20 {
		os.Rename(path, filepath.Join(dir, "log.old.txt"))
	}
	l := &logger{start: time.Now()}
	if f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
		l.f = f
	}
	return l
}

func (l *logger) Say(format string, a ...any) {
	line := fmt.Sprintf("%s +%.0fs %s\n", time.Now().Format("2006-01-02 15:04:05"), time.Since(l.start).Seconds(),
		fmt.Sprintf(format, a...))
	l.mu.Lock()
	defer l.mu.Unlock()
	os.Stdout.WriteString("wirestation: " + line)
	if l.f != nil {
		io.WriteString(l.f, line)
	}
}

func (l *logger) Close() {
	if l.f != nil {
		l.f.Close()
	}
}

// openTailscaleLog: Tailscale's own log, started fresh each run and moved
// to tailscale.old.log at 4 MiB, so a long run can't fill the disk.
func openTailscaleLog(dir string) io.WriteCloser {
	return &cappedLog{path: filepath.Join(dir, "tailscale.log")}
}

type cappedLog struct {
	mu   sync.Mutex
	path string
	f    *os.File
	n    int64
}

func (c *cappedLog) Write(b []byte) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.f == nil || c.n > 4<<20 {
		if c.f != nil {
			c.f.Close()
			os.Rename(c.path, strings.TrimSuffix(c.path, ".log")+".old.log")
		}
		f, err := os.Create(c.path)
		if err != nil {
			return len(b), nil // the log is a help, not a reason to fail
		}
		c.f, c.n = f, 0
	}
	n, err := c.f.Write(b)
	c.n += int64(n)
	return n, err
}

func (c *cappedLog) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.f == nil {
		return nil
	}
	return c.f.Close()
}
