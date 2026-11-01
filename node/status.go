package main

import (
	"context"
	"encoding/base64"
	"fmt"
	"html/template"
	"io"
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
	Name, IP, Seen, OS string
	Online, Exit       bool
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
	TailnetPage, Taildrop              bool
	Received                           []string
	Server                             string // server mode: the address devices join at
	ServerPort                         int
	ServerQR                           template.URL
	Joining                            []pendingJoin // devices waiting for approval
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
		fmt.Fprintln(w, "TailStation: stopping")
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
	case "/server/approve", "/server/deny":
		hs := st.tailnetServer()
		if r.Method != http.MethodPost || !sameSite(r) || hs == nil {
			http.Error(w, "use the buttons on the status page", http.StatusMethodNotAllowed)
			return
		}
		id := r.FormValue("id")
		if r.URL.Path == "/server/deny" {
			hs.deny(id)
		} else if err := hs.approve(id); err != nil {
			st.log.Say("tailnet server: approving %s: %v", id, err)
			http.Error(w, "couldn't approve it: "+err.Error(), http.StatusConflict)
			return
		}
		if r.Header.Get("Origin") == "" && r.Header.Get("Referer") == "" {
			fmt.Fprintln(w, "ok") // the app
			return
		}
		http.Redirect(w, r, "/", http.StatusSeeOther)
		return
	case "/", "/api/status":
	default:
		http.NotFound(w, r)
		return
	}

	d := p.collect(r.Context())
	if r.URL.Path == "/api/status" {
		w.Header().Set("Content-Type", "text/tab-separated-values; charset=utf-8")
		writeStatusTSV(w, &d)
		return
	}
	qr := func(s string) template.URL {
		png, err := qrcode.Encode(s, qrcode.Medium, 256)
		if err != nil {
			return ""
		}
		return template.URL("data:image/png;base64," + base64.StdEncoding.EncodeToString(png))
	}
	if d.LoginURL != "" {
		d.QR = qr(d.LoginURL)
	}
	if d.Server != "" {
		d.ServerQR = qr(d.Server)
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := page.Execute(w, d); err != nil {
		st.log.Say("status page: %v", err)
	}
}

// collect gathers what the page and /api/status show.
func (p *statusPage) collect(ctx context.Context) pageData {
	st := p.st
	d := pageData{Version: version, Hostname: st.cfg.Hostname, Uptime: time.Since(st.started).Round(time.Second).String()}
	st.mu.Lock()
	d.Backend, d.LoginURL, d.Error = st.backend, st.loginURL, st.lastErr
	d.Received = append([]string(nil), st.received...)
	d.TailnetPage, d.Taildrop = st.cfg.TailnetPage, st.cfg.Taildrop
	offered := append([]netip.Prefix(nil), st.routes...)
	hs := st.server
	st.mu.Unlock()
	if hs != nil {
		d.Server, d.ServerPort, d.Joining = hs.url, st.cfg.ServerPort, hs.waiting()
	}
	if d.Backend == "" {
		d.Backend = "starting"
	}
	for _, r := range offered {
		if r.Bits() == 0 {
			d.ExitOffered = true
			continue
		}
		d.Offered = append(d.Offered, r.String())
	}
	if lc := st.client(); lc != nil {
		ctx, cancel := context.WithTimeout(ctx, 3*time.Second)
		s, err := lc.Status(ctx)
		cancel()
		if err == nil {
			fillFromStatus(&d, s, offered)
		}
	}
	return d
}

// writeStatusTSV: /api/status, for the PS5 app. One fact a line, its name,
// a tab, then its values separated by tabs (none contain a tab or newline).
func writeStatusTSV(w io.Writer, d *pageData) {
	clean := func(s string) string { return strings.NewReplacer("\t", " ", "\n", " ", "\r", " ").Replace(s) }
	line := func(k string, v ...string) {
		for i := range v {
			v[i] = clean(v[i])
		}
		fmt.Fprintf(w, "%s\t%s\n", k, strings.Join(v, "\t"))
	}
	onOff := func(b bool) string {
		if b {
			return "on"
		}
		return "off"
	}
	line("version", d.Version)
	line("uptime", d.Uptime)
	line("hostname", d.Hostname)
	line("backend", d.Backend)
	if d.LoginURL != "" {
		line("login", d.LoginURL)
	}
	if d.Error != "" {
		line("error", d.Error)
	}
	if d.Name != "" {
		line("name", d.Name)
	}
	if d.IPs != "" {
		line("ips", d.IPs)
	}
	exit := "off"
	if d.ExitApproved {
		exit = "ready"
	} else if d.ExitOffered {
		exit = "waiting"
	}
	line("exit", exit)
	waiting := map[string]bool{}
	for _, r := range d.Waiting {
		waiting[r] = true
	}
	for _, r := range d.Offered {
		state := "approved"
		if waiting[r] {
			state = "waiting"
		}
		line("route", r, state)
	}
	if d.Server != "" {
		line("server", d.Server)
	}
	for _, j := range d.Joining {
		line("joining", j.ID, j.Name)
	}
	line("tailnet_page", onOff(d.TailnetPage))
	line("taildrop", onOff(d.Taildrop))
	for _, p := range d.Peers {
		online := "offline"
		if p.Online {
			online = "online"
		}
		line("peer", p.Name, p.IP, online, p.OS, p.Seen)
	}
	for _, f := range d.Received {
		line("received", f)
	}
}

// ago: "5 minutes ago", "3 hours ago", "2 days ago".
func ago(d time.Duration) string {
	n, unit := 0, ""
	switch {
	case d < time.Minute:
		return "just now"
	case d < time.Hour:
		n, unit = int(d/time.Minute), "minute"
	case d < 48*time.Hour:
		n, unit = int(d/time.Hour), "hour"
	default:
		n, unit = int(d/(24*time.Hour)), "day"
	}
	if n != 1 {
		unit += "s"
	}
	return fmt.Sprintf("%d %s ago", n, unit)
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
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Stop TailStation</title>
<style>body{margin:0;background:#08090b;color:#f2f2f2;font:16px/1.5 system-ui,sans-serif}main{max-width:640px;margin:0 auto;padding:24px 16px}
button{background:#e4eaf3;color:#fff;border:0;border-radius:6px;padding:10px 18px;font:inherit;cursor:pointer}a{color:#e4eaf3}</style>
</head><body><main><h1>Stop TailStation?</h1>
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
		row := peerRow{Name: strings.TrimSuffix(p.DNSName, "."), Online: p.Online, Exit: p.ExitNode, OS: p.OS}
		if row.Name == "" {
			row.Name = p.HostName
		}
		if len(p.TailscaleIPs) > 0 {
			row.IP = p.TailscaleIPs[0].String()
		}
		if !p.Online && !p.LastSeen.IsZero() {
			row.Seen = ago(time.Since(p.LastSeen))
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
<title>TailStation</title>
<style>
:root { color-scheme: dark; --bg: #08090b; --fg: #f2f2f2; --dim: #9a9a9e; --accent: #e4eaf3; --line: #2c2f36; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 16px/1.5 system-ui, sans-serif; }
main { max-width: 640px; margin: 0 auto; padding: 24px 16px 48px; }
h1 { color: var(--accent); font-size: 28px; margin: 0 0 4px; letter-spacing: .5px; }
h2 { font-size: 18px; margin: 28px 0 8px; border-bottom: 1px solid var(--line); padding-bottom: 6px; }
.dim { color: var(--dim); }
.box { border: 1px solid var(--accent); border-radius: 10px; padding: 16px; margin: 16px 0; }
.qr { background: #fff; padding: 8px; border-radius: 8px; width: 256px; max-width: 100%; display: block; margin: 12px 0; }
a { color: var(--accent); word-break: break-all; }
table { width: 100%; border-collapse: collapse; }
td { padding: 6px 4px; border-bottom: 1px solid var(--line); vertical-align: top; }
.on { color: #4cd964; } .off { color: var(--dim); }
button.yes { color: #050505; background: var(--accent); border-color: var(--accent); }
button { background: none; color: var(--dim); border: 1px solid var(--line); border-radius: 6px; padding: 6px 12px; font: inherit; cursor: pointer; }
</style></head><body><main>
<h1>TailStation</h1>
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
{{if and .Waiting (not .Server)}}
<div class="box">
<strong>One step left:</strong> approve this PS5 in the
<a href="https://login.tailscale.com/admin/machines">Tailscale admin page</a>.
Find <em>{{.Name}}</em>, open its menu (⋯), choose <em>Edit route settings</em>,
and tick <em>Use as exit node</em>{{if .Offered}} and the home network route{{end}}.
</div>
{{end}}

{{if .Server}}
<h2>This PS5's own tailnet</h2>
{{range .Joining}}<div class="box"><strong>{{.Name}}</strong> asks to join.
<form method="post" action="/server/approve" style="display:inline"><input type="hidden" name="id" value="{{.ID}}"><button class="yes">Let it in</button></form>
<form method="post" action="/server/deny" style="display:inline"><input type="hidden" name="id" value="{{.ID}}"><button>Turn it away</button></form>
</div>{{end}}
<p>This PS5 runs the tailnet itself (Headscale), so no Tailscale account is needed. To add a phone or computer:</p>
<ol>
<li>Forward port <strong>{{.ServerPort}}</strong> (TCP) on your router to this PS5, once.</li>
<li>In the Tailscale app, use a custom (alternate) coordination server: <a href="{{.Server}}">{{.Server}}</a>
<span class="dim">(phones: in the app's account settings, before signing in; computers: <code>tailscale up --login-server {{.Server}}</code>)</span>.</li>
<li>Approve it here, or in the PS5 app.</li>
</ol>
{{if .ServerQR}}<img class="qr" src="{{.ServerQR}}" alt="QR code for the server address">{{end}}
{{end}}

<h2>Devices</h2>
{{if .Peers}}<table>
{{range .Peers}}<tr><td>{{.Name}}{{if .Exit}} <span class="dim">(exit node)</span>{{end}}</td><td class="dim">{{.IP}}</td>
<td>{{if .Online}}<span class="on">online</span>{{else}}<span class="off">offline{{if .Seen}}, {{.Seen}}{{end}}</span>{{end}}</td></tr>
{{end}}</table>
{{else if .Server}}<p class="dim">No other devices yet.</p>
{{else}}<p class="dim">No other devices yet. Install the Tailscale app on your phone or computer and sign in with the same account.</p>{{end}}

{{if or .TailnetPage .Taildrop}}
<h2>On your tailnet</h2>
{{if .TailnetPage}}<p>From any of your devices, anywhere: <a href="http://{{.Hostname}}/">http://{{.Hostname}}/</a> (this page)
and <a href="http://{{.Hostname}}/files/">shared folders</a> to browse and download.</p>{{end}}
{{if .Taildrop}}<p>Send files to the PS5 with the Tailscale app: <em>Share</em> → <em>Tailscale</em> → <em>{{.Hostname}}</em>.</p>
{{if .Received}}<p class="dim">Received: {{range .Received}}{{.}} · {{end}}</p>{{end}}{{end}}
{{end}}

<h2>Using it</h2>
<p>In the Tailscale app on your phone or computer, choose <em>Exit node</em> and pick <em>{{.Hostname}}</em>.
Your browsing then goes out through your home internet.</p>
<form method="post" action="/logout" style="display:inline"><button>Sign this PS5 out</button></form>
<a href="/stop"><button>Stop TailStation</button></a>
{{else}}
<p>Starting Tailscale…</p>
{{end}}
</main></body></html>
`))
