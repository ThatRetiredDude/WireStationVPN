package main

// What keeps the parts that other people can reach safe:
//
//   - The join port (own tailnet) passes only what a device needs to join
//     and stay connected; Headscale's admin API, version and other pages
//     answer "not found" from outside.
//   - With a certificate, a browser opening a join link over plain http
//     from the internet is sent to https.
//   - At most connsPerAddr connections at once from one internet address,
//     and short time and size limits on what's sent, so a slow or flooding
//     client can't tie the PS5 up.
//   - The status page answers only the home network, the PS5 itself and
//     the tailnet, and only when asked for by the PS5's address or name,
//     so a web page elsewhere can't use a visitor's browser to press its
//     buttons (DNS rebinding).
//   - Pages tell browsers not to frame them, guess their type or pass the
//     address on.

import (
	"context"
	"net"
	"net/http"
	"net/netip"
	"strings"
	"sync"

	"tailscale.com/net/tsaddr"
)

const (
	connsPerAddr = 16      // open connections from one internet address
	maxBody      = 1 << 20 // bytes in a request to the join port
)

// joinPath: whether the join port passes r on to Headscale.
func joinPath(r *http.Request) bool {
	get := r.Method == http.MethodGet || r.Method == http.MethodHead
	p := r.URL.Path
	switch p {
	case "/ts2021": // the control protocol: devices' keys, the tailnet map
		return r.Method == http.MethodGet || r.Method == http.MethodPost
	case "/key", "/health", "/robots.txt", "/favicon.ico":
		return get
	}
	if rest, ok := strings.CutPrefix(p, "/register/"); ok {
		return get && rest != "" && !strings.Contains(rest, "/")
	}
	return false
}

type connKey struct{}

func rememberConn(ctx context.Context, c net.Conn) context.Context {
	return context.WithValue(ctx, connKey{}, c)
}

// viaTLS: whether r came over https.
func viaTLS(r *http.Request) bool {
	if r.TLS != nil {
		return true
	}
	c, _ := r.Context().Value(connKey{}).(net.Conn)
	sc, ok := c.(*sniffConn)
	if !ok {
		return false
	}
	sc.once.Do(sc.decide)
	return sc.tc != nil
}

func safeHeaders(w http.ResponseWriter, page bool) {
	h := w.Header()
	h.Set("X-Content-Type-Options", "nosniff")
	h.Set("X-Frame-Options", "DENY")
	h.Set("Referrer-Policy", "no-referrer")
	if page {
		h.Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; img-src 'self' data:; form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
	}
}

// homeOrTailnet: the PS5 itself, the home network and the tailnet.
func homeOrTailnet(a netip.Addr) bool {
	a = a.Unmap()
	return trusted(a) || tsaddr.IsTailscaleIP(a)
}

// ownHost: whether a request names the PS5 itself: by an address, as
// localhost, or by its tailnet name. Anything else is another site's name
// pointed at the PS5.
func ownHost(host, name, domain string) bool {
	if h, _, err := net.SplitHostPort(host); err == nil {
		host = h
	}
	host = strings.ToLower(strings.TrimSuffix(strings.Trim(host, "[]"), "."))
	if _, err := netip.ParseAddr(host); err == nil {
		return true
	}
	name = strings.ToLower(name)
	if host == "localhost" || host == name {
		return true
	}
	rest, ok := strings.CutPrefix(host, name+".")
	if !ok {
		return false
	}
	if domain != "" && (rest == strings.ToLower(domain) || strings.HasSuffix(rest, "."+strings.ToLower(domain))) {
		return true
	}
	return strings.HasSuffix(rest, ".ts.net") || rest == "ts.net"
}

// homeOnly keeps a page to the home network and the tailnet.
func homeOnly(cfg settings, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if a := remoteAddr(r.RemoteAddr); a.IsValid() && !homeOrTailnet(a) {
			http.Error(w, "this page is for the home network", http.StatusForbidden)
			return
		}
		if !ownHost(r.Host, cfg.Hostname, cfg.ServerDomain) {
			http.Error(w, "open this page by the PS5's address", http.StatusMisdirectedRequest)
			return
		}
		next.ServeHTTP(w, r)
	})
}

// connCap: at most connsPerAddr open connections from one internet
// address; more are closed straight away.
type connCap struct {
	mu   sync.Mutex
	open map[netip.Addr]int
}

func (c *connCap) take(a netip.Addr) bool {
	if !a.IsValid() || trusted(a) {
		return true
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.open == nil {
		c.open = map[netip.Addr]int{}
	}
	if c.open[a] >= connsPerAddr {
		return false
	}
	c.open[a]++
	return true
}

func (c *connCap) give(a netip.Addr) {
	if !a.IsValid() || trusted(a) {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.open[a]--; c.open[a] <= 0 {
		delete(c.open, a)
	}
}

type cappedConn struct {
	net.Conn
	once sync.Once
	done func()
}

func (c *cappedConn) Close() error {
	c.once.Do(c.done)
	return c.Conn.Close()
}

type cappedListener struct {
	net.Listener
	cap connCap
}

func (l *cappedListener) Accept() (net.Conn, error) {
	for {
		c, err := l.Listener.Accept()
		if err != nil {
			return nil, err
		}
		a := remoteAddr(c.RemoteAddr().String())
		if !l.cap.take(a) {
			c.Close()
			continue
		}
		return &cappedConn{Conn: c, done: func() { l.cap.give(a) }}, nil
	}
}
