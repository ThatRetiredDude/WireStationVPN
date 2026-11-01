package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
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
	tslogger "tailscale.com/types/logger"
)

// TestOwnServer runs the PS5 as its own tailnet server (server = on): it
// joins its own Headscale with its exit node approved by itself; a phone
// pointed at the server's address asks to join, waits until it's let in on
// the status page, then browses through the PS5.
func TestOwnServer(t *testing.T) {
	netns.SetEnabled(false)
	t.Cleanup(func() { netns.SetEnabled(true) })
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	// Relays of the test's own, instead of Tailscale's public ones.
	derpMap := integration.RunDERPAndSTUN(t, tslogger.Discard, "127.0.0.1")
	maps := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode(derpMap)
	}))
	t.Cleanup(maps.Close)

	dir := t.TempDir()
	statusPort, serverPort := freePort(t), freePort(t)
	serverURL := fmt.Sprintf("http://127.0.0.1:%d", serverPort)
	conf := fmt.Sprintf("hostname = ps5\nserver = on\nserver_port = %d\nserver_url = %s\nserver_domain = ps5net\nstatus_port = %d\n",
		serverPort, serverURL, statusPort)
	if err := os.WriteFile(filepath.Join(dir, "settings.conf"), []byte(conf), 0o644); err != nil {
		t.Fatal(err)
	}
	// Each run is its own process, as on the PS5: Headscale keeps running
	// until the process ends.
	done := runProcess(t, dir, maps.URL)
	status := fmt.Sprintf("http://127.0.0.1:%d/", statusPort)

	// 1. The PS5 is in its own tailnet with no sign-in, its exit node approved.
	waitFor(t, ctx, "the PS5 running on its own server with its exit node ready", func() (string, bool) {
		api := get(status + "api/status")
		return "", strings.Contains(api, "backend\tRunning\n") && strings.Contains(api, "exit\tready\n")
	})
	api := get(status + "api/status")
	for _, want := range []string{"server\t" + serverURL + "\n", "name\tps5.ps5net\n"} {
		if !strings.Contains(api, want) {
			t.Errorf("/api/status has no %q:\n%s", want, api)
		}
	}
	if page := get(status); !strings.Contains(page, "own tailnet") || strings.Contains(page, "admin page") {
		t.Error("the status page doesn't explain the PS5's own tailnet")
	}

	// 2. A phone asks to join; its sign-in page says to approve it on the PS5.
	phone := &tsnet.Server{Dir: t.TempDir(), Hostname: "phone", ControlURL: serverURL, Logf: tslogger.Discard}
	t.Cleanup(func() { phone.Close() })
	plc, err := phone.LocalClient()
	if err != nil {
		t.Fatal(err)
	}
	w, err := plc.WatchIPNBus(ctx, ipn.NotifyInitialState)
	if err != nil {
		t.Fatal(err)
	}
	approved := false
	for {
		n, err := w.Next()
		if err != nil {
			t.Fatal(err)
		}
		if n.BrowseToURL != nil && *n.BrowseToURL != "" && !approved {
			link := *n.BrowseToURL
			if !strings.HasPrefix(link, serverURL+"/register/") {
				t.Fatalf("the phone's sign-in link is %s", link)
			}
			if page := get(link); !strings.Contains(page, "<strong>phone</strong> is asking to join") {
				t.Fatalf("the phone's sign-in page:\n%s", page)
			}
			m := regexp.MustCompile(`(?m)^joining\t(\S+)\tphone$`).FindStringSubmatch(get(status + "api/status"))
			if m == nil {
				t.Fatalf("/api/status doesn't list the phone as joining:\n%s", get(status+"api/status"))
			}
			// From another site: refused.
			req, _ := http.NewRequest("POST", status+"server/approve", strings.NewReader(url.Values{"id": {m[1]}}.Encode()))
			req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
			req.Header.Set("Origin", "http://evil.example")
			if resp, err := http.DefaultClient.Do(req); err != nil || resp.StatusCode == 200 {
				t.Errorf("approval from another site: %v %v", resp, err)
			}
			resp, err := http.PostForm(status+"server/approve", url.Values{"id": {m[1]}})
			if err != nil || resp.StatusCode != 200 {
				b := []byte{}
				if resp != nil {
					b, _ = io.ReadAll(resp.Body)
				}
				t.Fatalf("approving the phone: %v %v %s", resp, err, b)
			}
			approved = true
		}
		if n.State != nil && *n.State == ipn.Running {
			break
		}
	}
	w.Close()
	if !approved {
		t.Fatal("the phone joined without approval")
	}
	if strings.Contains(get(status+"api/status"), "joining\t") {
		t.Error("the phone is still listed as joining")
	}

	// 3. It picks the PS5 as its exit node and reaches a web server outside.
	var ps5ID tailcfg.StableNodeID
	waitFor(t, ctx, "the PS5 as an exit node for the phone", func() (string, bool) {
		st, err := plc.Status(ctx)
		if err != nil {
			return "", false
		}
		for _, p := range st.Peer {
			if p.HostName == "ps5" && p.ExitNodeOption {
				ps5ID = p.ID
				if !strings.HasSuffix(p.DNSName, ".ps5net.") {
					t.Errorf("the PS5's MagicDNS name is %q", p.DNSName)
				}
				return "", true
			}
		}
		return "", false
	})
	if _, err := plc.EditPrefs(ctx, &ipn.MaskedPrefs{Prefs: ipn.Prefs{ExitNodeID: ps5ID}, ExitNodeIDSet: true}); err != nil {
		t.Fatal(err)
	}
	wl, err := net.Listen("tcp4", "0.0.0.0:0")
	if err != nil {
		t.Fatal(err)
	}
	web := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "hello from outside the tailnet")
	})}
	go web.Serve(wl)
	t.Cleanup(func() { web.Close() })
	target := fmt.Sprintf("%s:%d", outsideIP(t), wl.Addr().(*net.TCPAddr).Port)
	client := &http.Client{Transport: &http.Transport{DialContext: phone.Dial}, Timeout: 5 * time.Second}
	waitFor(t, ctx, "the phone reaching "+target+" through the PS5", func() (string, bool) {
		resp, err := client.Get("http://" + target + "/")
		if err != nil {
			return "", false
		}
		defer resp.Body.Close()
		b, _ := io.ReadAll(resp.Body)
		return "", string(b) == "hello from outside the tailnet"
	})

	// 4. The server's files aren't shared on the tailnet; then /stop.
	for _, h := range []string{"headscale", "tailscale-own-server"} {
		for _, p := range hiddenPaths(dir) {
			if p == filepath.Join(dir, h) {
				goto next
			}
		}
		t.Errorf("%s isn't hidden", h)
	next:
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

	// 5. Started again, it's the same tailnet: still signed in, the phone
	// still in it.
	done = runProcess(t, dir, maps.URL)
	waitFor(t, ctx, "the PS5 back in its own tailnet, the phone listed", func() (string, bool) {
		api := get(status + "api/status")
		return "", strings.Contains(api, "backend\tRunning\n") && strings.Contains(api, "exit\tready\n") &&
			regexp.MustCompile(`(?m)^peer\tphone\.ps5net\t`).MatchString(api)
	})
	if resp, err := http.Post(status+"stop", "text/plain", nil); err != nil || resp.StatusCode != 200 {
		t.Errorf("/stop: %v %v", resp, err)
	}
	select {
	case <-done:
	case <-ctx.Done():
		t.Fatal("it didn't stop")
	}
	log, _ := os.ReadFile(filepath.Join(dir, "log.txt"))
	if n := strings.Count(string(log), "sign in at"); n != 0 {
		t.Errorf("the PS5 asked to sign in %d times", n)
	}
	for _, want := range []string{"devices join at " + serverURL, "phone asks to join", "phone joined", "approved"} {
		if !strings.Contains(string(log), want) {
			t.Errorf("log.txt has no %q:\n%s", want, log)
		}
	}
}

// TestMain: when the test starts itself as a separate process (runProcess),
// that process is the program.
func TestMain(m *testing.M) {
	if dir := os.Getenv("WS_TEST_RUN"); dir != "" {
		relayMapURL = os.Getenv("WS_TEST_RELAYS")
		os.Exit(run(dir))
	}
	// A test certificate authority the whole test trusts, in place of the
	// system's, for the https test: set before anything reads the system's.
	caDir, err := os.MkdirTemp("", "tailstation-ca")
	if err != nil {
		panic(err)
	}
	testCert, testKey = makeTestCerts(caDir)
	os.Setenv("SSL_CERT_FILE", filepath.Join(caDir, "ca.pem"))
	code := m.Run()
	os.RemoveAll(caDir)
	os.Exit(code)
}

var testCert, testKey string // a certificate for 127.0.0.1 from the test authority

func makeTestCerts(dir string) (certPath, keyPath string) {
	must := func(err error) {
		if err != nil {
			panic(err)
		}
	}
	caKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	must(err)
	caTmpl := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "TailStation test CA"},
		NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(24 * time.Hour), IsCA: true,
		KeyUsage: x509.KeyUsageCertSign, BasicConstraintsValid: true}
	caDER, err := x509.CreateCertificate(rand.Reader, caTmpl, caTmpl, &caKey.PublicKey, caKey)
	must(err)
	caCert, _ := x509.ParseCertificate(caDER)
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	must(err)
	leaf := &x509.Certificate{SerialNumber: big.NewInt(2), Subject: pkix.Name{CommonName: "127.0.0.1"},
		IPAddresses: []net.IP{net.ParseIP("127.0.0.1")}, NotBefore: time.Now().Add(-time.Hour),
		NotAfter: time.Now().Add(24 * time.Hour), KeyUsage: x509.KeyUsageDigitalSignature,
		ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
	der, err := x509.CreateCertificate(rand.Reader, leaf, caCert, &key.PublicKey, caKey)
	must(err)
	kb, err := x509.MarshalECPrivateKey(key)
	must(err)
	write := func(name, typ string, b []byte) string {
		p := filepath.Join(dir, name)
		must(os.WriteFile(p, pem.EncodeToMemory(&pem.Block{Type: typ, Bytes: b}), 0o600))
		return p
	}
	write("ca.pem", "CERTIFICATE", caDER)
	return write("cert.pem", "CERTIFICATE", der), write("key.pem", "EC PRIVATE KEY", kb)
}

// TestOwnServerHTTPS: the own tailnet with a certificate (tls_cert): a phone
// joins over https; plain http still answers on the same port.
func TestOwnServerHTTPS(t *testing.T) {
	netns.SetEnabled(false)
	t.Cleanup(func() { netns.SetEnabled(true) })
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	derpMap := integration.RunDERPAndSTUN(t, tslogger.Discard, "127.0.0.1")
	maps := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode(derpMap)
	}))
	t.Cleanup(maps.Close)

	dir := t.TempDir()
	statusPort, serverPort := freePort(t), freePort(t)
	serverURL := fmt.Sprintf("https://127.0.0.1:%d", serverPort)
	conf := fmt.Sprintf("server = on\nserver_port = %d\nserver_url = %s\nstatus_port = %d\ntls_cert = %s\ntls_key = %s\n",
		serverPort, serverURL, statusPort, testCert, testKey)
	if err := os.WriteFile(filepath.Join(dir, "settings.conf"), []byte(conf), 0o644); err != nil {
		t.Fatal(err)
	}
	done := runProcess(t, dir, maps.URL)
	status := fmt.Sprintf("http://127.0.0.1:%d/", statusPort)
	waitFor(t, ctx, "the PS5 running on its own server", func() (string, bool) {
		return "", strings.Contains(get(status+"api/status"), "backend\tRunning\n")
	})
	if api := get(status + "api/status"); !strings.Contains(api, "https\tyour own certificate\t") || !strings.Contains(api, "blocked\t0\n") {
		t.Errorf("/api/status:\n%s", api)
	}
	if resp, err := http.Get(fmt.Sprintf("http://127.0.0.1:%d/health", serverPort)); err != nil || resp.StatusCode != 200 {
		t.Errorf("plain http on the server port: %v %v", resp, err)
	}

	phone := &tsnet.Server{Dir: t.TempDir(), Hostname: "phone", ControlURL: serverURL, Logf: tslogger.Discard}
	t.Cleanup(func() { phone.Close() })
	plc, err := phone.LocalClient()
	if err != nil {
		t.Fatal(err)
	}
	w, err := plc.WatchIPNBus(ctx, ipn.NotifyInitialState)
	if err != nil {
		t.Fatal(err)
	}
	for approved := false; ; {
		n, err := w.Next()
		if err != nil {
			t.Fatal(err)
		}
		if n.BrowseToURL != nil && strings.HasPrefix(*n.BrowseToURL, serverURL+"/register/") && !approved {
			if page := get(*n.BrowseToURL); !strings.Contains(page, "is asking to join") {
				t.Fatalf("the sign-in page over https:\n%s", page)
			}
			m := regexp.MustCompile(`(?m)^joining\t(\S+)\tphone$`).FindStringSubmatch(get(status + "api/status"))
			if m == nil {
				t.Fatal("the phone isn't listed as joining")
			}
			if resp, err := http.PostForm(status+"server/approve", url.Values{"id": {m[1]}}); err != nil || resp.StatusCode != 200 {
				t.Fatalf("approving: %v %v", resp, err)
			}
			approved = true
		}
		if n.State != nil && *n.State == ipn.Running {
			break
		}
	}
	w.Close()
	http.Post(status+"stop", "text/plain", nil)
	select {
	case <-done:
	case <-ctx.Done():
		t.Fatal("it didn't stop")
	}
}

// runProcess runs the program on dir in a process of its own; its exit
// code arrives on the channel.
func runProcess(t *testing.T, dir, relays string) <-chan int {
	cmd := exec.Command(os.Args[0], "-test.run=^$")
	cmd.Env = append(os.Environ(), "WS_TEST_RUN="+dir, "WS_TEST_RELAYS="+relays)
	cmd.Stdout, cmd.Stderr = os.Stderr, os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { cmd.Process.Kill() })
	done := make(chan int, 1)
	go func() {
		cmd.Wait()
		done <- cmd.ProcessState.ExitCode()
	}()
	return done
}
