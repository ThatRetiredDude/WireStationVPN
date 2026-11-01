// Tests for the C++ build (lite/), run against Tailscale's test coordination
// server and relay, with real Tailscale clients (tsnet) as the phone: the
// same steps as node's TestPS5Node. Build it first: lite/build.sh host.
package litetest

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os"
	"os/exec"
	"path/filepath"
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

func binary(t *testing.T) string {
	p := os.Getenv("WS_LITE_BIN")
	if p == "" {
		p, _ = filepath.Abs("../../build/host/tailstation-lite")
	}
	if _, err := os.Stat(p); err != nil {
		if os.Getenv("CI") != "" {
			t.Fatalf("no C++ build at %s (lite/build.sh host)", p)
		}
		t.Skipf("no C++ build at %s (lite/build.sh host)", p)
	}
	return p
}

func TestLite(t *testing.T) {
	bin := binary(t)
	netns.SetEnabled(false)
	t.Cleanup(func() { netns.SetEnabled(true) })
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
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
	conf := fmt.Sprintf("hostname = ps5\ncontrol_url = %s\nstatus_port = %d\nshared_folders = taildrop, %s\ndns = 127.0.0.1\n",
		controlURL, statusPort, dir)
	if err := os.WriteFile(filepath.Join(dir, "settings.conf"), []byte(conf), 0o644); err != nil {
		t.Fatal(err)
	}
	start := func() *exec.Cmd {
		cmd := exec.Command(bin, dir)
		if testing.Verbose() {
			cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
		}
		if err := cmd.Start(); err != nil {
			t.Fatal(err)
		}
		return cmd
	}
	cmd := start()
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	t.Cleanup(func() {
		cmd.Process.Kill()
		if t.Failed() {
			log, _ := os.ReadFile(filepath.Join(dir, "log.txt"))
			t.Logf("log.txt:\n%s", log)
			ents, _ := os.ReadDir(dir)
			for _, e := range ents {
				fi, _ := e.Info()
				t.Logf("  %s %d", e.Name(), fi.Size())
			}
		}
	})
	status := fmt.Sprintf("http://127.0.0.1:%d/", statusPort)

	// 1. The status page shows the sign-in link; signing in finishes it.
	link := waitFor(t, ctx, "the sign-in link", func() (string, bool) {
		m := regexp.MustCompile(`href="(` + regexp.QuoteMeta(controlURL) + `[^"]+)"`).FindStringSubmatch(get(status))
		if m == nil {
			return "", false
		}
		return strings.ReplaceAll(m[1], "&amp;", "&"), true
	})
	if !strings.Contains(get(status), `src="data:image/svg+xml,`) {
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
	control.SetSubnetRoutes(ps5.Key, ps5.Hostinfo.RoutableIPs().AsSlice())
	waitFor(t, ctx, "the status page saying the exit node is ready", func() (string, bool) {
		return "", strings.Contains(get(status), ">ready<")
	})

	// 3. A phone signs in, picks the PS5 as its exit node, and reaches a web
	// server outside the tailnet through it, TCP and UDP.
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

	big := strings.Repeat("0123456789abcdef", 1<<16) // 1 MiB: windows and buffers
	web := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/big" {
			io.WriteString(w, big)
			return
		}
		if r.Method == "POST" {
			b, _ := io.ReadAll(r.Body)
			fmt.Fprintf(w, "got %d bytes", len(b))
			return
		}
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
	client := &http.Client{Transport: &http.Transport{DialContext: phone.Dial}, Timeout: 10 * time.Second}
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
	// A megabyte each way.
	if resp, err := client.Get("http://" + target + "/big"); err != nil {
		t.Errorf("1 MiB down: %v", err)
	} else {
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if string(b) != big {
			t.Errorf("1 MiB down: got %d bytes, not the same", len(b))
		}
	}
	if resp, err := client.Post("http://"+target+"/", "application/octet-stream", strings.NewReader(big)); err != nil {
		t.Errorf("1 MiB up: %v", err)
	} else {
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if string(b) != fmt.Sprintf("got %d bytes", len(big)) {
			t.Errorf("1 MiB up: %q", b)
		}
	}
	// Ten at once.
	errs := make(chan error, 10)
	for i := 0; i < 10; i++ {
		go func() {
			resp, err := client.Get("http://" + target + "/big")
			if err == nil {
				b, _ := io.ReadAll(resp.Body)
				resp.Body.Close()
				if len(b) != len(big) {
					err = fmt.Errorf("got %d bytes", len(b))
				}
			}
			errs <- err
		}()
	}
	for i := 0; i < 10; i++ {
		if err := <-errs; err != nil {
			t.Errorf("ten at once: %v", err)
		}
	}
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

	// The PS5's own address: only its page and Taildrop, not the status port.
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
	refused := func(a string) {
		got := make(chan error, 1)
		go func() {
			c, err := phone.Dial(ctx, "tcp", a)
			if err == nil {
				// lwIP takes the connection, then resets it: a read says.
				c.SetReadDeadline(time.Now().Add(2 * time.Second))
				_, err = c.Read(make([]byte, 1))
				if err == io.EOF {
					err = nil
				}
				c.Close()
				if err != nil {
					err = fmt.Errorf("reset: %w", err)
				} else {
					err = nil
				}
			}
			got <- err
		}()
		select {
		case err := <-got:
			if err == nil {
				t.Errorf("the phone reached %v", a)
			}
		case <-time.After(4 * time.Second):
		}
	}
	refused(netip.AddrPortFrom(ps5IP, uint16(statusPort)).String())
	refused(fmt.Sprintf("127.0.0.1:%d", statusPort))

	// 4. The PS5 on the tailnet: its page and shared folders on port 80.
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
		strings.Contains(body, "settings.conf") || strings.Contains(body, "tailscale-lite.keys") {
		t.Errorf("the shared folder's listing (%d) shows the wrong files:\n%s", code, body)
	}
	if code, body := onTailnet("/files/" + share + "/log.txt"); code != 200 || !strings.Contains(body, "starting") {
		t.Errorf("log.txt from the shared folder: %d", code)
	}
	for _, hidden := range []string{"settings.conf", "tailscale-lite.keys", "../" + filepath.Base(dir) + "/settings.conf"} {
		if code, _ := onTailnet("/files/" + share + "/" + hidden); code == 200 {
			t.Errorf("%s is served on the tailnet", hidden)
		}
	}

	// 5. Taildrop.
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

	// The app's view: /api/status, a fact a line.
	api := get(status + "api/status")
	for _, want := range []string{"backend\tRunning\n", "exit\tready\n", "hostname\tps5\n", "received\thello.txt\n",
		"taildrop\ton\n", "build\tlite\n"} {
		if !strings.Contains(api, want) {
			t.Errorf("/api/status has no %q:\n%s", want, api)
		}
	}
	if !regexp.MustCompile(`(?m)^peer\tphone[^\t]*\t100\.[0-9.]+\t(online|offline)\tlinux\t`).MatchString(api) {
		t.Errorf("/api/status doesn't list the phone:\n%s", api)
	}

	// 6. Only the home network asks: a Host that isn't the PS5's is refused.
	req, _ := http.NewRequest("GET", status, nil)
	req.Host = "evil.example.com"
	if resp, err := http.DefaultClient.Do(req); err == nil {
		resp.Body.Close()
		if resp.StatusCode != http.StatusMisdirectedRequest {
			t.Errorf("Host evil.example.com: %d", resp.StatusCode)
		}
	}

	// 7. A second copy leaves; /stop stops the first.
	second := exec.Command(bin, dir)
	if err := second.Run(); err == nil {
		t.Error("a second copy kept running")
	}
	if resp, err := http.Post(status+"stop", "text/plain", nil); err != nil || resp.StatusCode != 200 {
		t.Errorf("/stop: %v %v", resp, err)
	}
	select {
	case err := <-done:
		if err != nil {
			t.Errorf("it stopped with %v", err)
		}
	case <-ctx.Done():
		t.Fatal("it didn't stop")
	}
	log, _ := os.ReadFile(filepath.Join(dir, "log.txt"))
	for _, want := range []string{"sign in at", "offering", "stopping", "received hello.txt"} {
		if !strings.Contains(string(log), want) {
			t.Errorf("log.txt has no %q:\n%s", want, log)
		}
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
