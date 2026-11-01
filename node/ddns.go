package main

// A home name for the PS5's own tailnet: devices join at a name such as
// myps5.duckdns.org instead of the home's internet address, which can
// change. TailStation keeps the name pointing at the home:
//
//   - DuckDNS (free, duckdns.org): duckdns_domain and duckdns_token. It
//     also lets TailStation get an https certificate with no extra port
//     forward (certs.go).
//   - any other service with an update link (No-IP, Dynu, FreeDNS ...):
//     ddns_update_url, opened every few minutes.

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
)

// duckDNSAPI: DuckDNS's update address. The test points it at its own.
var duckDNSAPI = "https://www.duckdns.org/update"

// duckName: the full name for duckdns_domain, which may be given with or
// without .duckdns.org.
func duckName(d string) string {
	d = strings.ToLower(strings.TrimSuffix(strings.TrimSpace(d), "."))
	if d == "" || strings.HasSuffix(d, ".duckdns.org") {
		return d
	}
	return d + ".duckdns.org"
}

// homeName: the name devices use for the home, "" when none is set up.
func (s settings) homeName() string {
	if s.DuckDomain != "" {
		return duckName(s.DuckDomain)
	}
	return ""
}

// duckUpdate asks DuckDNS to point the name at this home's address (DuckDNS
// sees it), or with txt set, to publish that TXT record (for the https
// certificate; clear removes it).
func duckUpdate(ctx context.Context, cfg settings, txt string, clear bool) error {
	q := url.Values{"domains": {strings.TrimSuffix(duckName(cfg.DuckDomain), ".duckdns.org")}, "token": {cfg.DuckToken}}
	if txt != "" || clear {
		q.Set("txt", txt)
		if clear {
			q.Set("clear", "true")
		}
	} else {
		q.Set("ip", "")
	}
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	req, _ := http.NewRequestWithContext(ctx, "GET", duckDNSAPI+"?"+q.Encode(), nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(io.LimitReader(resp.Body, 200))
	if answer := strings.TrimSpace(string(b)); !strings.HasPrefix(answer, "OK") {
		return fmt.Errorf("DuckDNS said %q (check duckdns_domain and duckdns_token)", answer)
	}
	return nil
}

// keepHomeName updates the home name now and every 5 minutes.
func keepHomeName(ctx context.Context, cfg settings, log *logger) {
	if cfg.DuckDomain == "" && cfg.DDNSUpdateURL == "" {
		return
	}
	last := ""
	for {
		var err error
		what := ""
		if cfg.DuckDomain != "" {
			what = duckName(cfg.DuckDomain)
			err = duckUpdate(ctx, cfg, "", false)
		} else {
			what = "the home name"
			err = openLink(ctx, cfg.DDNSUpdateURL)
		}
		now := "ok"
		if err != nil {
			now = err.Error()
		}
		if now != last {
			if err != nil {
				log.Say("home name: updating %s failed: %v", what, err)
			} else {
				log.Say("home name: %s points at this home", what)
			}
			last = now
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(5 * time.Minute):
		}
	}
}

func openLink(ctx context.Context, link string) error {
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, "GET", link, nil)
	if err != nil {
		return err
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	resp.Body.Close()
	if resp.StatusCode >= 400 {
		return fmt.Errorf("the update link answered %s", resp.Status)
	}
	return nil
}
