package main

import (
	"context"
	"encoding/base64"
	"fmt"
	"html/template"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"sort"
	"strings"
	"time"

	qrcode "github.com/skip2/go-qrcode"
	"tailscale.com/ipn/ipnstate"
)

// statusPage is the page on the home network: the sign-in link and QR code
// while the PS5 needs one, then its tailnet name, what it offers and who is
// online. /stop stops the program, /logout signs the PS5 out.
type statusPage struct{ st *state }

func serveStatus(ln net.Listener, st *state) {
	srv := &http.Server{Handler: &statusPage{st}, ReadHeaderTimeout: 10 * time.Second}
	srv.Serve(ln)
}

type peerRow struct {
	Name, IP, Seen string
	Online, Exit   bool
}

type pageData struct {
	Version, Hostname, Backend, Uptime string
	LoginURL, Error                    string
	QR                                 template.URL
	Name, IPs                          string
	Offered                            []string
	ExitApproved, ExitOffered          bool
	Waiting                            []string // routes offered but not approved yet
	Peers                              []peerRow
	Status                             *ipnstate.Status
}

func (p *statusPage) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	st := p.st
	switch r.URL.Path {
	case "/stop":
		if r.Method != http.MethodPost {
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			fmt.Fprint(w, stopPage)
			return
		}
		if !sameSite(r) {
			http.Error(w, "use the button on the status page", http.StatusForbidden)
			return
		}
		fmt.Fprintln(w, "WireStationVPN: stopping")
		st.log.Say("asked to stop on the status page")
		go st.quit()
		return
	case "/logout":
		if r.Method != http.MethodPost || !sameSite(r) {
			http.Error(w, "use the button on the status page", http.StatusMethodNotAllowed)
			return
		}
		if lc := st.client(); lc != nil {
			if err := lc.Logout(r.Context()); err != nil {
				st.log.Say("sign-out failed: %v", err)
			} else {
				st.log.Say("signed out on the status page")
			}
		}
		http.Redirect(w, r, "/", http.StatusSeeOther)
		return
	case "/":
	default:
		http.NotFound(w, r)
		return
	}

	d := pageData{Version: version, Hostname: st.cfg.Hostname, Uptime: time.Since(st.started).Round(time.Second).String()}
	st.mu.Lock()
	d.Backend, d.LoginURL, d.Error = st.backend, st.loginURL, st.lastErr
	offered := append([]netip.Prefix(nil), st.routes...)
	st.mu.Unlock()
	if d.Backend == "" {
		d.Backend = "starting"
	}
	if d.LoginURL != "" {
		if png, err := qrcode.Encode(d.LoginURL, qrcode.Medium, 256); err == nil {
			d.QR = template.URL("data:image/png;base64," + base64.StdEncoding.EncodeToString(png))
		}
	}
	for _, r := range offered {
		if r.Bits() == 0 {
			d.ExitOffered = true
			continue
		}
		d.Offered = append(d.Offered, r.String())
	}
	if lc := st.client(); lc != nil {
		ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
		s, err := lc.Status(ctx)
		cancel()
		if err == nil {
			fillFromStatus(&d, s, offered)
		}
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := page.Execute(w, d); err != nil {
		st.log.Say("status page: %v", err)
	}
}

// sameSite: a button press on this page, not another web site making the
// browser send the request. Browsers say where a form was sent from;
// requests that say nothing (curl, scripts) are let through.
func sameSite(r *http.Request) bool {
	from := r.Header.Get("Origin")
	if from == "" {
		from = r.Header.Get("Referer")
	}
	if from == "" {
		return true
	}
	u, err := url.Parse(from)
	return err == nil && u.Host == r.Host
}

const stopPage = `<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Stop WireStationVPN</title>
<style>body{margin:0;background:#0b0b0c;color:#f2f2f2;font:16px/1.5 system-ui,sans-serif}main{max-width:640px;margin:0 auto;padding:24px 16px}
button{background:#ff3b3b;color:#fff;border:0;border-radius:6px;padding:10px 18px;font:inherit;cursor:pointer}a{color:#ff3b3b}</style>
</head><body><main><h1>Stop WireStationVPN?</h1>
<p>Devices using the PS5 as their exit node lose their connection until it runs again.</p>
<form method="post" action="/stop"><button>Stop it</button></form><p><a href="/">Back</a></p></main></body></html>`

func fillFromStatus(d *pageData, s *ipnstate.Status, offered []netip.Prefix) {
	d.Status = s
	if self := s.Self; self != nil {
		d.Name = strings.TrimSuffix(self.DNSName, ".")
		var ips []string
		for _, ip := range self.TailscaleIPs {
			ips = append(ips, ip.String())
		}
		d.IPs = strings.Join(ips, ", ")
		// Approved routes show up in the PS5's own AllowedIPs.
		approved := map[netip.Prefix]bool{}
		if self.AllowedIPs != nil {
			for _, p := range self.AllowedIPs.All() {
				approved[p] = true
			}
		}
		d.ExitApproved = approved[netip.MustParsePrefix("0.0.0.0/0")]
		for _, r := range offered {
			if !approved[r] {
				d.Waiting = append(d.Waiting, r.String())
			}
		}
	}
	for _, p := range s.Peer {
		row := peerRow{Name: strings.TrimSuffix(p.DNSName, "."), Online: p.Online, Exit: p.ExitNode}
		if row.Name == "" {
			row.Name = p.HostName
		}
		if len(p.TailscaleIPs) > 0 {
			row.IP = p.TailscaleIPs[0].String()
		}
		if !p.Online && !p.LastSeen.IsZero() {
			row.Seen = time.Since(p.LastSeen).Round(time.Minute).String() + " ago"
		}
		d.Peers = append(d.Peers, row)
	}
	sort.Slice(d.Peers, func(i, j int) bool {
		if d.Peers[i].Online != d.Peers[j].Online {
			return d.Peers[i].Online
		}
		return d.Peers[i].Name < d.Peers[j].Name
	})
}

var page = template.Must(template.New("page").Parse(`<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="10">
<title>WireStationVPN</title>
<style>
:root { color-scheme: dark; --bg: #0b0b0c; --fg: #f2f2f2; --dim: #9a9a9e; --red: #ff3b3b; --line: #2a2a2e; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 16px/1.5 system-ui, sans-serif; }
main { max-width: 640px; margin: 0 auto; padding: 24px 16px 48px; }
h1 { color: var(--red); font-size: 28px; margin: 0 0 4px; letter-spacing: .5px; }
h2 { font-size: 18px; margin: 28px 0 8px; border-bottom: 1px solid var(--line); padding-bottom: 6px; }
.dim { color: var(--dim); }
.box { border: 1px solid var(--red); border-radius: 10px; padding: 16px; margin: 16px 0; }
.qr { background: #fff; padding: 8px; border-radius: 8px; width: 256px; max-width: 100%; display: block; margin: 12px 0; }
a { color: var(--red); word-break: break-all; }
table { width: 100%; border-collapse: collapse; }
td { padding: 6px 4px; border-bottom: 1px solid var(--line); vertical-align: top; }
.on { color: #4cd964; } .off { color: var(--dim); }
button { background: none; color: var(--dim); border: 1px solid var(--line); border-radius: 6px; padding: 6px 12px; font: inherit; cursor: pointer; }
</style></head><body><main>
<h1>WireStationVPN</h1>
<div class="dim">{{.Version}} · up {{.Uptime}} · Tailscale: {{.Backend}}</div>

{{if .Error}}<div class="box">Tailscale says: {{.Error}}</div>{{end}}

{{if .LoginURL}}
<div class="box">
<strong>Sign in to add this PS5 to your Tailscale network.</strong>
<p>Scan the code with your phone, or open the link, and sign in with your Tailscale account.</p>
{{if .QR}}<img class="qr" src="{{.QR}}" alt="QR code for the sign-in link">{{end}}
<a href="{{.LoginURL}}">{{.LoginURL}}</a>
</div>
{{else if eq .Backend "Running"}}
<h2>This PS5</h2>
<table>
<tr><td class="dim">Name</td><td>{{.Name}}</td></tr>
<tr><td class="dim">Tailscale address</td><td>{{.IPs}}</td></tr>
<tr><td class="dim">Exit node</td><td>{{if .ExitApproved}}<span class="on">ready</span>{{else if .ExitOffered}}offered, waiting for approval{{else}}off{{end}}</td></tr>
{{if .Offered}}<tr><td class="dim">Home network</td><td>{{range .Offered}}{{.}} {{end}}</td></tr>{{end}}
</table>
{{if .Waiting}}
<div class="box">
<strong>One step left:</strong> approve this PS5 in the
<a href="https://login.tailscale.com/admin/machines">Tailscale admin page</a>.
Find <em>{{.Name}}</em>, open its menu (⋯), choose <em>Edit route settings</em>,
and tick <em>Use as exit node</em>{{if .Offered}} and the home network route{{end}}.
</div>
{{end}}

<h2>Devices</h2>
{{if .Peers}}<table>
{{range .Peers}}<tr><td>{{.Name}}{{if .Exit}} <span class="dim">(exit node)</span>{{end}}</td><td class="dim">{{.IP}}</td>
<td>{{if .Online}}<span class="on">online</span>{{else}}<span class="off">offline{{if .Seen}}, {{.Seen}}{{end}}</span>{{end}}</td></tr>
{{end}}</table>
{{else}}<p class="dim">No other devices yet. Install the Tailscale app on your phone or computer and sign in with the same account.</p>{{end}}

<h2>Using it</h2>
<p>In the Tailscale app on your phone or computer, choose <em>Exit node</em> and pick <em>{{.Hostname}}</em>.
Your browsing then goes out through your home internet.</p>
<form method="post" action="/logout" style="display:inline"><button>Sign this PS5 out</button></form>
<a href="/stop"><button>Stop WireStationVPN</button></a>
{{else}}
<p>Starting Tailscale…</p>
{{end}}
</main></body></html>
`))
