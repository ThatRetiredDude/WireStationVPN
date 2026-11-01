package main

// The guard: the PS5's own tailnet server faces the internet (one port
// forwarded on the router), so like fail2ban it blocks addresses that
// misbehave, for an hour:
//
//   - more than 120 requests a minute (a phone checking in makes a few);
//   - more than 20 refused requests in 10 minutes (someone scanning for
//     pages that aren't there);
//   - more than 3 devices asking to join from one address at once;
//
// and for a day, an address whose device was turned away on the PS5.
// Addresses on the home network and the PS5 itself are never blocked.
//
// With CrowdSec running elsewhere at home (crowdsec_url, crowdsec_key: a
// bouncer key from "cscli bouncers add tailstation"), addresses CrowdSec
// has decided to block are blocked here too.

import (
	"bufio"
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/netip"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	guardRate        = 120 // requests a minute
	guardRefused     = 20  // refused requests in 10 minutes
	guardJoins       = 3   // devices waiting to join, from one address
	guardBan         = time.Hour
	guardTurnedAway  = 24 * time.Hour
	crowdSecCacheFor = 5 * time.Minute
)

type guard struct {
	log         *logger
	crowdURL    string
	crowdKey    string
	mu          sync.Mutex
	hits        map[netip.Addr][]time.Time // requests in the last minute
	refused     map[netip.Addr][]time.Time // refused (4xx) in the last 10 minutes
	banned      map[netip.Addr]time.Time   // until
	crowd       map[netip.Addr]crowdAnswer
	now         func() time.Time
	lastCleanup time.Time
}

type crowdAnswer struct {
	block bool
	at    time.Time
}

func newGuard(cfg settings, log *logger) *guard {
	return &guard{log: log, crowdURL: strings.TrimSuffix(cfg.CrowdSecURL, "/"), crowdKey: cfg.CrowdSecKey,
		hits: map[netip.Addr][]time.Time{}, refused: map[netip.Addr][]time.Time{}, banned: map[netip.Addr]time.Time{},
		crowd: map[netip.Addr]crowdAnswer{}, now: time.Now}
}

// trusted: the PS5 itself and the home network.
func trusted(a netip.Addr) bool {
	a = a.Unmap()
	return a.IsLoopback() || a.IsPrivate() || a.IsLinkLocalUnicast()
}

func remoteAddr(s string) netip.Addr {
	ap, err := netip.ParseAddrPort(s)
	if err != nil {
		return netip.Addr{}
	}
	return ap.Addr().Unmap()
}

// blocked: whether a, connecting now, is turned away.
func (g *guard) blocked(a netip.Addr) bool {
	if !a.IsValid() || trusted(a) {
		return false
	}
	g.mu.Lock()
	until, ok := g.banned[a]
	now := g.now()
	if ok && now.After(until) {
		delete(g.banned, a)
		ok = false
	}
	g.mu.Unlock()
	if ok {
		return true
	}
	return g.crowdSecSays(a)
}

func (g *guard) ban(a netip.Addr, d time.Duration, why string) {
	if !a.IsValid() || trusted(a) {
		return
	}
	g.mu.Lock()
	until := g.now().Add(d)
	if old, ok := g.banned[a]; ok && old.After(until) {
		until = old
	}
	_, was := g.banned[a]
	g.banned[a] = until
	g.mu.Unlock()
	if !was {
		g.log.Say("guard: blocked %s for %s: %s", a, d, why)
	}
}

// note counts a request and its answer; true when that got a ban.
func (g *guard) note(a netip.Addr, status int) bool {
	if !a.IsValid() || trusted(a) {
		return false
	}
	g.mu.Lock()
	now := g.now()
	g.cleanup(now)
	g.hits[a] = append(keepSince(g.hits[a], now.Add(-time.Minute)), now)
	many := len(g.hits[a]) > guardRate
	refusedMany := false
	if status >= 400 && status < 500 {
		g.refused[a] = append(keepSince(g.refused[a], now.Add(-10*time.Minute)), now)
		refusedMany = len(g.refused[a]) > guardRefused
	}
	g.mu.Unlock()
	switch {
	case many:
		g.ban(a, guardBan, "too many requests")
		return true
	case refusedMany:
		g.ban(a, guardBan, "too many refused requests")
		return true
	}
	return false
}

func keepSince(ts []time.Time, since time.Time) []time.Time {
	i := sort.Search(len(ts), func(i int) bool { return ts[i].After(since) })
	return ts[i:]
}

// cleanup forgets quiet addresses, once a minute. Called with mu held.
func (g *guard) cleanup(now time.Time) {
	if now.Sub(g.lastCleanup) < time.Minute {
		return
	}
	g.lastCleanup = now
	for a, ts := range g.hits {
		if len(keepSince(ts, now.Add(-time.Minute))) == 0 {
			delete(g.hits, a)
		}
	}
	for a, ts := range g.refused {
		if len(keepSince(ts, now.Add(-10*time.Minute))) == 0 {
			delete(g.refused, a)
		}
	}
	for a, until := range g.banned {
		if now.After(until) {
			delete(g.banned, a)
		}
	}
	for a, c := range g.crowd {
		if now.Sub(c.at) > crowdSecCacheFor {
			delete(g.crowd, a)
		}
	}
}

// blockedCount: addresses blocked now.
func (g *guard) blockedCount() int {
	g.mu.Lock()
	defer g.mu.Unlock()
	n := 0
	now := g.now()
	for _, until := range g.banned {
		if now.Before(until) {
			n++
		}
	}
	return n
}

func (g *guard) unblockAll() {
	g.mu.Lock()
	n := len(g.banned)
	g.banned = map[netip.Addr]time.Time{}
	g.hits = map[netip.Addr][]time.Time{}
	g.refused = map[netip.Addr][]time.Time{}
	g.mu.Unlock()
	g.log.Say("guard: unblocked %d addresses", n)
}

// crowdSecSays asks CrowdSec's local API whether a is to be blocked; when
// CrowdSec doesn't answer, the address isn't blocked for it.
func (g *guard) crowdSecSays(a netip.Addr) bool {
	if g.crowdURL == "" {
		return false
	}
	g.mu.Lock()
	c, ok := g.crowd[a]
	g.mu.Unlock()
	if ok && g.now().Sub(c.at) < crowdSecCacheFor {
		return c.block
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", g.crowdURL+"/v1/decisions?ip="+a.String(), nil)
	req.Header.Set("X-Api-Key", g.crowdKey)
	block := false
	resp, err := http.DefaultClient.Do(req)
	if err == nil {
		b, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
		resp.Body.Close()
		var decisions []struct {
			Type string `json:"type"`
		}
		if resp.StatusCode == 200 && json.Unmarshal(b, &decisions) == nil {
			for _, d := range decisions {
				if d.Type == "ban" || d.Type == "" {
					block = true
				}
			}
		}
	}
	g.mu.Lock()
	g.crowd[a] = crowdAnswer{block: block, at: g.now()}
	g.mu.Unlock()
	if block {
		g.log.Say("guard: CrowdSec blocks %s", a)
	}
	return block
}

// listener drops connections from blocked addresses before they're read.
type guardListener struct {
	net.Listener
	g *guard
}

func (l *guardListener) Accept() (net.Conn, error) {
	for {
		c, err := l.Listener.Accept()
		if err != nil {
			return nil, err
		}
		if l.g.blocked(remoteAddr(c.RemoteAddr().String())) {
			c.Close()
			continue
		}
		return c, nil
	}
}

// handler counts each request and its answer.
func (g *guard) handler(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		a := remoteAddr(r.RemoteAddr)
		if g.blocked(a) {
			http.Error(w, "blocked", http.StatusForbidden)
			return
		}
		sw := &statusWriter{ResponseWriter: w, status: 200}
		next.ServeHTTP(sw, r)
		g.note(a, sw.status)
	})
}

type statusWriter struct {
	http.ResponseWriter
	status int
	wrote  bool
}

func (s *statusWriter) WriteHeader(code int) {
	if !s.wrote {
		s.status, s.wrote = code, true
	}
	s.ResponseWriter.WriteHeader(code)
}

func (s *statusWriter) Flush() {
	if f, ok := s.ResponseWriter.(http.Flusher); ok {
		f.Flush()
	}
}

// Unwrap and Hijack keep the upgrade to Tailscale's control protocol
// working through the guard.
func (s *statusWriter) Unwrap() http.ResponseWriter { return s.ResponseWriter }

func (s *statusWriter) Hijack() (net.Conn, *bufio.ReadWriter, error) {
	s.status = http.StatusSwitchingProtocols
	return http.NewResponseController(s.ResponseWriter).Hijack()
}
