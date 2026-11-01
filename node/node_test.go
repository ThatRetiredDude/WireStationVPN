package main

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"strings"
	"testing"
	"time"

	"tailscale.com/ipn"
	"tailscale.com/net/netns"
	"tailscale.com/tailcfg"
	"tailscale.com/tsnet"
	"tailscale.com/tstest/integration"
	"tailscale.com/tstest/integration/testcontrol"
	tslogger "tailscale.com/types/logger"
)

// TestPS5Node runs the program against Tailscale's test coordination
// server, as a PS5 would against Tailscale's: it shows a sign-in link,
// joins once the link is used, offers itself as an exit node, and after the
// owner approves that, a "phone" (another tsnet node) reaches a web server
// through it.
func TestPS5Node(t *testing.T) {
	netns.SetEnabled(false)
	t.Cleanup(func() { netns.SetEnabled(true) })
	ctx, cancel := context.WithTimeout(context.Background(), 90*time.Second)
	defer cancel()

	derpMap := integration.RunDERPAndSTUN(t, tslogger.Discard, "127.0.0.1")
	control := &testcontrol.Server{
		DERPMap:          derpMap,
		RequireAuth:      true,
		AllNodesSameUser: true,
		DNSConfig:        &tailcfg.DNSConfig{Proxied: true},
		MagicDNSDomain:   "tail-scale.ts.net",
		Logf:             tslogger.Discard,
	}
	control.HTTPTestServer = httptest.NewUnstartedServer(control)
	control.HTTPTestServer.Start()
	t.Cleanup(control.HTTPTestServer.Close)
	controlURL := control.HTTPTestServer.URL

	dir := t.TempDir()
	statusPort := freePort(t)
	conf := fmt.Sprintf("hostname = ps5\ncontrol_url = %s\nstatus_port = %d\nshared_folders = taildrop, %s\n", controlURL, statusPort, dir)
	if err := os.WriteFile(filepath.Join(dir, "settings.conf"), []byte(conf), 0o644); err != nil {
		t.Fatal(err)
	}
	done := make(chan int, 1)
	go func() { done <- run(dir) }()
	status := fmt.Sprintf("http://127.0.0.1:%d/", statusPort)

	// 1. The status page shows the sign-in link; signing in finishes it.
	link := waitFor(t, ctx, "the sign-in link", func() (string, bool) {
		m := regexp.MustCompile(`href="(` + regexp.QuoteMeta(controlURL) + `[^"]+)"`).FindStringSubmatch(get(status))
		if m == nil {
			return "", false
		}
		return m[1], true
	})
	if !strings.Contains(get(status), `src="data:image/png;base64,`) {
		t.Error("no QR code next to the sign-in link")
	}
	if !control.CompleteAuth(link) {
		t.Fatalf("the test server didn't take %s", link)
	}
	waitFor(t, ctx, "Tailscale running", func() (string, bool) {
		return "", strings.Contains(get(status), "Tailscale: Running")
	})

	// 2. It offers the exit node routes; the owner approves them.
	ps5 := waitFor(t, ctx, "the PS5 offering an exit node", func() (*tailcfg.Node, bool) {
		for _, n := range control.AllNodes() {
			if n.Hostinfo.Hostname() != "ps5" {
				continue
			}
			for _, r := range n.Hostinfo.RoutableIPs().All() {
				if r.Bits() == 0 {
					return n, true
				}
			}
		}
		return nil, false
	})
	if !strings.Contains(get(status), "waiting for approval") {
		t.Error("the status page doesn't say the exit node waits for approval")
	}
	// Everything it offers: the exit node, and the home network when this
	// machine is on a private one. Private addresses go through the PS5 only
	// as a home route: Tailscale exit nodes don't forward to their own LAN.
	control.SetSubnetRoutes(ps5.Key, ps5.Hostinfo.RoutableIPs().AsSlice())
	waitFor(t, ctx, "the status page saying the exit node is ready", func() (string, bool) {
		return "", strings.Contains(get(status), ">ready<")
	})

	// 3. A phone signs in, picks the PS5 as its exit node, and reaches a
	// web server outside the tailnet through it.
	phone := &tsnet.Server{Dir: t.TempDir(), Hostname: "phone", ControlURL: controlURL, Logf: tslogger.Discard}
	t.Cleanup(func() { phone.Close() })
	plc, err := phone.LocalClient()
	if err != nil {
		t.Fatal(err)
	}
	w, err := plc.WatchIPNBus(ctx, ipn.NotifyInitialState)
	if err != nil {
		t.Fatal(err)
	}
	for {
		n, err := w.Next()
		if err != nil {
			t.Fatal(err)
		}
		if n.BrowseToURL != nil && *n.BrowseToURL != "" {
			control.CompleteAuth(*n.BrowseToURL)
		}
		if n.State != nil && *n.State == ipn.Running {
			break
		}
	}
	w.Close()
	if _, err := plc.EditPrefs(ctx, &ipn.MaskedPrefs{Prefs: ipn.Prefs{ExitNodeID: ps5.StableID}, ExitNodeIDSet: true}); err != nil {
		t.Fatal(err)
	}

	web := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "hello from outside the tailnet")
	})}
	wl, err := net.Listen("tcp4", "0.0.0.0:0")
	if err != nil {
		t.Fatal(err)
	}
	go web.Serve(wl)
	t.Cleanup(func() { web.Close() })
	target := fmt.Sprintf("%s:%d", outsideIP(t), wl.Addr().(*net.TCPAddr).Port)
	seenErr := ""
	client := &http.Client{Transport: &http.Transport{DialContext: phone.Dial}, Timeout: 5 * time.Second}
	waitFor(t, ctx, "the phone reaching "+target+" through the PS5", func() (string, bool) {
		resp, err := client.Get("http://" + target + "/")
		if err != nil {
			if lastErr := err.Error(); lastErr != seenErr {
				t.Logf("phone: %v", err)
				seenErr = lastErr
			}
			return "", false
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		return "", string(b) == "hello from outside the tailnet"
	})
	// UDP too (DNS, QUIC, calls): an echo server outside the tailnet.
	uc, err := net.ListenPacket("udp4", "0.0.0.0:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { uc.Close() })
	go func() {
		buf := make([]byte, 1500)
		for {
			n, from, err := uc.ReadFrom(buf)
			if err != nil {
				return
			}
			uc.WriteTo(buf[:n], from)
		}
	}()
	udpTarget := fmt.Sprintf("%s:%d", outsideIP(t), uc.LocalAddr().(*net.UDPAddr).Port)
	waitFor(t, ctx, "a UDP echo from "+udpTarget+" through the PS5", func() (string, bool) {
		c, err := phone.Dial(ctx, "udp", udpTarget)
		if err != nil {
			return "", false
		}
		defer c.Close()
		c.SetDeadline(time.Now().Add(time.Second))
		c.Write([]byte("ping"))
		buf := make([]byte, 16)
		n, err := c.Read(buf)
		return "", err == nil && string(buf[:n]) == "ping"
	})

	// The PS5's own address is refused: its local services stay off the tailnet.
	if st, err := plc.Status(ctx); err == nil {
		for _, p := range st.Peer {
			if p.HostName == "ps5" && len(p.TailscaleIPs) > 0 {
				addr := netip.AddrPortFrom(p.TailscaleIPs[0], uint16(statusPort))
				if _, err := phone.Dial(ctx, "tcp", addr.String()); err == nil {
					t.Errorf("the phone reached %v, the PS5's own address", addr)
				}
			}
		}
	}
	// So is the PS5's loopback (where the ELF loader listens) through the exit
	// node: Tailscale's network stack drops those packets, so the dial gets
	// no answer. tsnet's Dial doesn't give up when its context ends, hence
	// the goroutine.
	for _, a := range []string{fmt.Sprintf("127.0.0.1:%d", statusPort), fmt.Sprintf("[::1]:%d", statusPort)} {
		got := make(chan error, 1)
		go func() {
			c, err := phone.Dial(ctx, "tcp", a)
			if err == nil {
				c.Close()
			}
			got <- err
		}()
		select {
		case err := <-got:
			if err == nil {
				t.Errorf("the phone reached %v, the PS5's loopback", a)
			}
		case <-time.After(3 * time.Second):
		}
	}
	// 4. The PS5 on the tailnet: its page and shared folders on port 80 of
	// its Tailscale address, and files sent to it with Taildrop.
	ps5IP := waitFor(t, ctx, "the PS5's Tailscale address", func() (netip.Addr, bool) {
		st, err := plc.Status(ctx)
		if err != nil {
			return netip.Addr{}, false
		}
		for _, p := range st.Peer {
			if p.HostName == "ps5" && len(p.TailscaleIPs) > 0 {
				return p.TailscaleIPs[0], true
			}
		}
		return netip.Addr{}, false
	})
	onTailnet := func(path string) (int, string) {
		resp, err := client.Get("http://" + ps5IP.String() + path)
		if err != nil {
			t.Errorf("GET %s on the tailnet: %v", path, err)
			return 0, ""
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		return resp.StatusCode, string(b)
	}
	if code, body := onTailnet("/"); code != 200 || !strings.Contains(body, "<h1>TailStation</h1>") {
		t.Errorf("the status page on the tailnet: %d", code)
	}
	share := strings.ReplaceAll(strings.Trim(dir, "/"), "/", "-")
	if code, body := onTailnet("/files/"); code != 200 || !strings.Contains(body, dir) {
		t.Errorf("/files/: %d %s", code, body)
	}
	if code, body := onTailnet("/files/" + share + "/"); code != 200 || !strings.Contains(body, "log.txt") ||
		strings.Contains(body, "settings.conf") || strings.Contains(body, ">tailscale<") {
		t.Errorf("the shared folder's listing (%d) shows the wrong files:\n%s", code, body)
	}
	if code, body := onTailnet("/files/" + share + "/log.txt"); code != 200 || !strings.Contains(body, "starting") {
		t.Errorf("log.txt from the shared folder: %d", code)
	}
	for _, hidden := range []string{"settings.conf", "tailscale/", "tailscale/tailscaled.state", "../" + filepath.Base(dir) + "/settings.conf"} {
		if code, _ := onTailnet("/files/" + share + "/" + hidden); code == 200 {
			t.Errorf("%s is served on the tailnet", hidden)
		}
	}
	if code, _ := onTailnet("/stop"); code != 200 {
		t.Errorf("/stop's page on the tailnet: %d", code)
	}

	var ps5Target tailcfg.StableNodeID
	waitFor(t, ctx, "the PS5 as a Taildrop target", func() (string, bool) {
		targets, err := plc.FileTargets(ctx)
		if err != nil {
			return "", false
		}
		for _, ft := range targets {
			if ft.Node.Hostinfo.Hostname() == "ps5" {
				ps5Target = ft.Node.StableID
				return "", true
			}
		}
		return "", false
	})
	sent := "a file from the phone"
	if err := plc.PushFile(ctx, ps5Target, int64(len(sent)), "hello.txt", strings.NewReader(sent)); err != nil {
		t.Fatalf("Taildrop to the PS5: %v", err)
	}
	waitFor(t, ctx, "hello.txt in the taildrop folder", func() (string, bool) {
		b, err := os.ReadFile(filepath.Join(dir, "received", "hello.txt"))
		return "", err == nil && string(b) == sent
	})
	waitFor(t, ctx, "the status page listing hello.txt", func() (string, bool) {
		return "", strings.Contains(get(status), "hello.txt")
	})

	// The PS5 app's view of the same: /api/status, a fact a line. (The test
	// control server doesn't tell nodes who is online.)
	api := get(status + "api/status")
	for _, want := range []string{"backend\tRunning\n", "exit\tready\n", "hostname\tps5\n", "received\thello.txt\n", "taildrop\ton\n"} {
		if !strings.Contains(api, want) {
			t.Errorf("/api/status has no %q:\n%s", want, api)
		}
	}
	if !regexp.MustCompile(`(?m)^peer\tphone[^\t]*\t100\.[0-9.]+\t(online|offline)\tlinux\t`).MatchString(api) {
		t.Errorf("/api/status doesn't list the phone:\n%s", api)
	}
	if !strings.Contains(get(status), "phone") {
		t.Error("the status page doesn't list the phone")
	}

	// 5. A second copy leaves; /stop stops the first.
	if code := run(dir); code != 1 {
		t.Errorf("a second copy returned %d, want 1", code)
	}
	if resp, err := http.Post(status+"stop", "text/plain", nil); err != nil || resp.StatusCode != 200 {
		t.Errorf("/stop: %v %v", resp, err)
	}
	select {
	case code := <-done:
		if code != 0 {
			t.Errorf("run returned %d", code)
		}
	case <-ctx.Done():
		t.Fatal("it didn't stop")
	}
	log, _ := os.ReadFile(filepath.Join(dir, "log.txt"))
	for _, want := range []string{"sign in at", "offering", "stopping"} {
		if !strings.Contains(string(log), want) {
			t.Errorf("log.txt has no %q:\n%s", want, log)
		}
	}
}

func TestSettings(t *testing.T) {
	path := filepath.Join(t.TempDir(), "settings.conf")
	os.WriteFile(path, []byte("# comment\nhostname = living-room\nexit_node = off\nport = 41641\nbogus = 1\nstatus_port = 0\ntags = tag:ps5, tag:home\n"), 0o644)
	s, problems := loadSettings(path)
	if s.Hostname != "living-room" || s.ExitNode || !s.HomeNetwork || s.Port != 41641 || s.StatusPort != 9039 ||
		strings.Join(s.Tags, " ") != "tag:ps5 tag:home" {
		t.Errorf("got %+v", s)
	}
	if len(problems) != 2 {
		t.Errorf("want 2 problems, got %q", problems)
	}

	// The own-tailnet settings, and what they refuse.
	os.WriteFile(path, []byte("fixed_port = on\nserver = on\nserver_port = 443\nserver_url = https://home.example.net/\nserver_domain = Home.Arpa\n"+
		"server_url = ftp://x\nserver_domain = bad_name\nserver_port = 0\n"), 0o644)
	s, problems = loadSettings(path)
	if !s.Server || s.ServerPort != 443 || s.ServerURL != "https://home.example.net" || s.ServerDomain != "home.arpa" {
		t.Errorf("server settings: %+v", s)
	}
	if s.udpPort() != DefaultFixedPort {
		t.Errorf("fixed_port: UDP port %d", s.udpPort())
	}
	if s.Port = 5000; s.udpPort() != 5000 {
		t.Error("port should win over fixed_port")
	}
	if len(problems) != 3 {
		t.Errorf("want 3 problems, got %q", problems)
	}
	if s, p := loadSettings(filepath.Join(t.TempDir(), "none")); !reflect.DeepEqual(s, defaults()) || len(p) != 0 {
		t.Errorf("a missing file: %+v %q", s, p)
	}
}

func waitFor[T any](t *testing.T, ctx context.Context, what string, f func() (T, bool)) T {
	t.Helper()
	for {
		if v, ok := f(); ok {
			return v
		}
		select {
		case <-ctx.Done():
			t.Fatalf("timed out waiting for %s", what)
		case <-time.After(200 * time.Millisecond):
		}
	}
}

func get(url string) string {
	c := &http.Client{Timeout: 5 * time.Second}
	resp, err := c.Get(url)
	if err != nil {
		return ""
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return string(b)
}

func freePort(t *testing.T) int {
	l, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

// outsideIP: an address of this machine that isn't loopback, standing in
// for a web server on the internet.
func outsideIP(t *testing.T) string {
	addrs, _ := net.InterfaceAddrs()
	for _, a := range addrs {
		if n, ok := a.(*net.IPNet); ok && n.IP.To4() != nil && !n.IP.IsLoopback() {
			return n.IP.String()
		}
	}
	t.Skip("no non-loopback IPv4 address to stand in for the internet")
	return ""
}
