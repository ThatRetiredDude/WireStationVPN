package main

import (
	"net"
	"net/http"
	"net/http/httptest"
	"net/netip"
	"testing"
)

func TestJoinPort(t *testing.T) {
	s := &tailnetServer{local: "http://127.0.0.1:1", certs: &certSource{}, pending: map[string]pendingJoin{}}
	door := s.frontDoor()
	for _, c := range []struct {
		method, path string
		want         int
	}{
		{"GET", "/api/v1/node", 404},
		{"POST", "/api/v1/apikey", 404},
		{"GET", "/swagger", 404},
		{"GET", "/version", 404},
		{"GET", "/apple", 404},
		{"GET", "/", 404},
		{"GET", "/register/a/../../api/v1/node", 404},
		{"POST", "/key", 404},
		{"DELETE", "/ts2021", 404},
		{"GET", "/register/abc", http.StatusPermanentRedirect}, // plain http from outside, with a certificate
	} {
		r := httptest.NewRequest(c.method, "http://ps5.example.org:8080"+c.path, nil)
		r.RemoteAddr = "203.0.113.9:5555"
		w := httptest.NewRecorder()
		door.ServeHTTP(w, r)
		if w.Code != c.want {
			t.Errorf("%s %s: %d, want %d", c.method, c.path, w.Code, c.want)
		}
		if w.Header().Get("X-Frame-Options") != "DENY" {
			t.Errorf("%s %s: no X-Frame-Options", c.method, c.path)
		}
	}
	for _, p := range []string{"/ts2021", "/key", "/health", "/register/abc"} {
		r := httptest.NewRequest("GET", p, nil)
		if !joinPath(r) {
			t.Errorf("%s is refused", p)
		}
	}
}

func TestHomeOnly(t *testing.T) {
	cfg := defaults()
	page := homeOnly(cfg, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {}))
	for _, c := range []struct {
		from, host string
		want       int
	}{
		{"192.168.1.20:4000", "192.168.1.5:9039", 200},
		{"127.0.0.1:4000", "127.0.0.1:9039", 200},
		{"100.101.102.103:4000", "ps5", 200},
		{"100.101.102.103:4000", "ps5.tail1234.ts.net", 200},
		{"100.101.102.103:4000", "ps5.ps5net", 200},
		{"[fd7a:115c:a1e0::1]:4000", "[fd7a:115c:a1e0::2]", 200},
		{"203.0.113.9:4000", "198.51.100.1:9039", 403},
		{"192.168.1.20:4000", "evil.example.com:9039", 421},
		{"192.168.1.20:4000", "ps5.evil.example.com:9039", 421},
		{"192.168.1.20:4000", "ps5.ts.net.evil.example.com", 421},
	} {
		r := httptest.NewRequest("GET", "/", nil)
		r.RemoteAddr, r.Host = c.from, c.host
		w := httptest.NewRecorder()
		page.ServeHTTP(w, r)
		if w.Code != c.want {
			t.Errorf("from %s as %s: %d, want %d", c.from, c.host, w.Code, c.want)
		}
	}
}

func TestConnCap(t *testing.T) {
	var c connCap
	out, home := netip.MustParseAddr("203.0.113.9"), netip.MustParseAddr("192.168.1.20")
	for i := 0; i < connsPerAddr; i++ {
		if !c.take(out) {
			t.Fatalf("connection %d refused", i+1)
		}
	}
	if c.take(out) {
		t.Fatal("one too many allowed")
	}
	c.give(out)
	if !c.take(out) {
		t.Fatal("a closed connection wasn't given back")
	}
	for i := 0; i < 3*connsPerAddr; i++ {
		if !c.take(home) {
			t.Fatal("the home network is capped")
		}
	}

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer ln.Close()
	cl := &cappedListener{Listener: ln}
	go func() {
		d, err := net.Dial("tcp", ln.Addr().String())
		if err == nil {
			d.Close()
		}
	}()
	conn, err := cl.Accept()
	if err != nil {
		t.Fatal(err)
	}
	conn.Close()
	conn.Close() // twice is fine
}
