package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"io"
	"log"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/letsencrypt/pebble/v2/ca"
	"github.com/letsencrypt/pebble/v2/db"
	"github.com/letsencrypt/pebble/v2/va"
	"github.com/letsencrypt/pebble/v2/wfe"
)

// fakeDuckDNS answers like DuckDNS and records what it was asked.
func fakeDuckDNS(t *testing.T) (asked func() []string) {
	var mu sync.Mutex
	var got []string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		got = append(got, r.URL.RawQuery)
		mu.Unlock()
		if r.URL.Query().Get("token") != "secret" {
			io.WriteString(w, "KO")
			return
		}
		io.WriteString(w, "OK")
	}))
	t.Cleanup(srv.Close)
	was := duckDNSAPI
	duckDNSAPI = srv.URL
	t.Cleanup(func() { duckDNSAPI = was })
	return func() []string {
		mu.Lock()
		defer mu.Unlock()
		return append([]string(nil), got...)
	}
}

func TestHomeName(t *testing.T) {
	asked := fakeDuckDNS(t)
	cfg := defaults()
	cfg.DuckDomain, cfg.DuckToken = "myps5", "secret"
	if cfg.homeName() != "myps5.duckdns.org" || duckName("x.duckdns.org") != "x.duckdns.org" {
		t.Errorf("names: %q", cfg.homeName())
	}
	if err := duckUpdate(context.Background(), cfg, "", false); err != nil {
		t.Fatal(err)
	}
	if q := asked(); len(q) != 1 || !strings.Contains(q[0], "domains=myps5&") || !strings.Contains(q[0], "ip=") {
		t.Errorf("DuckDNS was asked %q", q)
	}
	cfg.DuckToken = "wrong"
	if err := duckUpdate(context.Background(), cfg, "", false); err == nil || !strings.Contains(err.Error(), "duckdns_token") {
		t.Errorf("a wrong token: %v", err)
	}
}

// TestCertificate gets an https certificate the way the PS5 does, from a
// Let's Encrypt test server (Pebble) that takes the DNS check as passed.
func TestCertificate(t *testing.T) {
	asked := fakeDuckDNS(t)
	t.Setenv("PEBBLE_VA_ALWAYS_VALID", "1")
	t.Setenv("PEBBLE_VA_NOSLEEP", "1")
	quiet := log.New(io.Discard, "", 0)
	store := db.NewMemoryStore()
	authority := ca.New(quiet, store, "", "ecdsa", 0, 1, map[string]ca.Profile{"default": {ValidityPeriod: 0}})
	front := wfe.New(quiet, store, va.New(quiet, 0, 0, false, "", store), authority, nil, false, false, 0, 0)
	acmeSrv := httptest.NewTLSServer(front.Handler())
	t.Cleanup(acmeSrv.Close)
	wasDir, wasHTTP, wasSeen := acmeDirectory, acmeHTTP, txtSeen
	acmeDirectory, acmeHTTP, txtSeen = acmeSrv.URL+"/dir", acmeSrv.Client(), func(context.Context, string, string) {}
	t.Cleanup(func() { acmeDirectory, acmeHTTP, txtSeen = wasDir, wasHTTP, wasSeen })

	dir := t.TempDir()
	cfg := defaults()
	cfg.DuckDomain, cfg.DuckToken = "myps5", "secret"
	l := &logger{f: nopCloser{io.Discard}, start: time.Now()}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	certs, err := startTLS(ctx, dir, cfg, l)
	if err != nil || certs == nil {
		t.Fatalf("startTLS: %v", err)
	}
	cert, _ := certs.get(nil)
	if cert == nil || cert.Leaf.DNSNames[0] != "myps5.duckdns.org" || time.Until(cert.Leaf.NotAfter) < 24*time.Hour {
		t.Fatalf("certificate: %+v", cert)
	}
	var txt, cleared bool
	for _, q := range asked() {
		txt = txt || strings.Contains(q, "txt=") && !strings.Contains(q, "clear=true")
		cleared = cleared || strings.Contains(q, "clear=true")
	}
	if !txt || !cleared {
		t.Errorf("DuckDNS TXT record set %v, cleared %v: %q", txt, cleared, asked())
	}
	// Started again, it uses the saved one rather than asking again.
	n := len(asked())
	if _, err := startTLS(ctx, dir, cfg, l); err != nil || len(asked()) != n {
		t.Errorf("a second start asked again (%v)", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "headscale", "tls", "key.pem")); err != nil {
		t.Error(err)
	}
}

type nopCloser struct{ io.Writer }

func (nopCloser) Close() error { return nil }

// TestSniff: https and plain http on the same port.
func TestSniff(t *testing.T) {
	srv := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "hello")
	}))
	srv.StartTLS() // for its certificate
	cert := srv.TLS.Certificates[0]
	srv.Close()
	plain := httptest.NewUnstartedServer(srv.Config.Handler)
	plain.Listener = &sniffListener{Listener: plain.Listener, tls: &tls.Config{Certificates: []tls.Certificate{cert}}}
	plain.Start()
	defer plain.Close()
	pool := x509.NewCertPool()
	leaf, _ := x509.ParseCertificate(cert.Certificate[0])
	pool.AddCert(leaf)
	for _, c := range []struct {
		url    string
		client *http.Client
	}{
		{plain.URL, http.DefaultClient},
		{strings.Replace(plain.URL, "http:", "https:", 1), &http.Client{Transport: &http.Transport{TLSClientConfig: &tls.Config{RootCAs: pool}}}},
	} {
		resp, err := c.client.Get(c.url)
		if err != nil {
			t.Errorf("%s: %v", c.url, err)
			continue
		}
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		if string(b) != "hello" {
			t.Errorf("%s: %q", c.url, b)
		}
	}
}

func TestGuard(t *testing.T) {
	now := time.Unix(1_800_000_000, 0)
	l := &logger{f: nopCloser{io.Discard}, start: now}
	crowd := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Api-Key") != "k" {
			w.WriteHeader(403)
			return
		}
		if r.URL.Query().Get("ip") == "198.51.100.66" {
			io.WriteString(w, `[{"type":"ban","value":"198.51.100.66"}]`)
			return
		}
		io.WriteString(w, "null")
	}))
	defer crowd.Close()
	cfg := defaults()
	cfg.CrowdSecURL, cfg.CrowdSecKey = crowd.URL, "k"
	g := newGuard(cfg, l)
	g.now = func() time.Time { return now }
	stranger, home := netip.MustParseAddr("203.0.113.9"), netip.MustParseAddr("192.168.1.20")

	for i := 0; i < guardRate; i++ {
		g.note(stranger, 200)
		g.note(home, 200)
	}
	if g.blocked(stranger) || g.blocked(home) {
		t.Fatal("blocked at the limit")
	}
	g.note(stranger, 200)
	g.note(home, 200)
	if !g.blocked(stranger) || g.blocked(home) {
		t.Errorf("over the limit: stranger %v, home %v", g.blocked(stranger), g.blocked(home))
	}
	now = now.Add(guardBan + time.Second)
	if g.blocked(stranger) {
		t.Error("still blocked after the hour")
	}

	scanner := netip.MustParseAddr("203.0.113.10")
	for i := 0; i <= guardRefused; i++ {
		now = now.Add(5 * time.Second)
		g.note(scanner, 404)
	}
	if !g.blocked(scanner) || g.blockedCount() != 1 {
		t.Errorf("a scanner: blocked %v, count %d", g.blocked(scanner), g.blockedCount())
	}
	g.unblockAll()
	if g.blocked(scanner) {
		t.Error("unblock all left it blocked")
	}
	g.ban(stranger, guardTurnedAway, "test")
	now = now.Add(23 * time.Hour)
	if !g.blocked(stranger) {
		t.Error("a turned-away address unblocked within the day")
	}
	if !g.blocked(netip.MustParseAddr("198.51.100.66")) || g.blocked(netip.MustParseAddr("198.51.100.67")) {
		t.Error("CrowdSec's decisions weren't followed")
	}
}
