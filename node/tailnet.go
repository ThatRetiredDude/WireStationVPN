package main

// What the PS5 serves to the tailnet itself, on its own Tailscale address:
//
//   - http://ps5/ (port 80): the status page, so it can be checked and
//     controlled from anywhere, and the shared folders, read-only, to browse
//     and download (resumable, and videos play in the browser).
//   - Taildrop: files sent to the PS5 from the Tailscale app ("Send with
//     Tailscale" / "tailscale file cp") land in the taildrop folder.

import (
	"context"
	"fmt"
	"html/template"
	"io"
	"net/http"
	"net/url"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"

	"tailscale.com/client/local"
	"tailscale.com/tsnet"
)

// serveTailnet serves the status page and the shared folders on port 80 of
// the PS5's Tailscale address.
func serveTailnet(srv *tsnet.Server, st *state, dir string) {
	ln, err := srv.Listen("tcp", ":80")
	if err != nil {
		st.log.Say("can't serve on the tailnet: %v", err)
		return
	}
	mux := http.NewServeMux()
	mux.Handle("/files/", http.StripPrefix("/files", &fileBrowser{roots: sharedRoots(st.cfg.Shared), hidden: hiddenPaths(dir)}))
	mux.Handle("/", &statusPage{st})
	hs := &http.Server{Handler: homeOnly(st.cfg, mux), ReadHeaderTimeout: 10 * time.Second,
		IdleTimeout: 2 * time.Minute, MaxHeaderBytes: 16 << 10}
	st.log.Say("on the tailnet: http://%s/", st.cfg.Hostname)
	hs.Serve(ln)
}

// hiddenPaths: never shown or served, even inside a shared folder: the
// PS5's Tailscale keys, settings.conf (it can hold an auth key), and in
// server mode the server's keys and database.
func hiddenPaths(dir string) []string {
	var out []string
	for _, n := range []string{"tailscale", "tailscale-own-server", "headscale", "settings.conf"} {
		p := filepath.Join(dir, n)
		out = append(out, p)
		if real, err := filepath.EvalSymlinks(p); err == nil && real != p {
			out = append(out, real)
		}
	}
	return out
}

type root struct{ Name, Path string }

// sharedRoots: the shared folders that exist, each named by its path
// ("/mnt/usb0" is "mnt-usb0").
func sharedRoots(folders []string) []root {
	var out []root
	for _, f := range folders {
		f = filepath.Clean(f)
		if fi, err := os.Stat(f); err != nil || !fi.IsDir() {
			continue
		}
		name := strings.ReplaceAll(strings.Trim(f, "/"), "/", "-")
		if name == "" {
			name = "root"
		}
		out = append(out, root{name, f})
	}
	return out
}

type fileBrowser struct {
	roots  []root
	hidden []string
}

func (b *fileBrowser) isHidden(p string) bool {
	real, err := filepath.EvalSymlinks(p)
	if err != nil {
		real = p
	}
	for _, h := range b.hidden {
		for _, q := range []string{p, real} {
			if q == h || strings.HasPrefix(q, h+string(filepath.Separator)) {
				return true
			}
		}
	}
	return false
}

func (b *fileBrowser) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		http.Error(w, "read-only", http.StatusMethodNotAllowed)
		return
	}
	clean := path.Clean("/" + r.URL.Path)
	if clean == "/" {
		listing(w, "Shared folders", "", nil, b.roots)
		return
	}
	first, rest, _ := strings.Cut(strings.TrimPrefix(clean, "/"), "/")
	var base string
	for _, rt := range b.roots {
		if rt.Name == first {
			base = rt.Path
		}
	}
	if base == "" {
		http.NotFound(w, r)
		return
	}
	full := filepath.Join(base, filepath.FromSlash(rest))
	if full != base && !strings.HasPrefix(full, base+string(filepath.Separator)) || b.isHidden(full) {
		http.NotFound(w, r)
		return
	}
	f, err := os.Open(full)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	defer f.Close()
	fi, err := f.Stat()
	if err != nil {
		http.NotFound(w, r)
		return
	}
	if !fi.IsDir() {
		http.ServeContent(w, r, fi.Name(), fi.ModTime(), f)
		return
	}
	if !strings.HasSuffix(r.URL.Path, "/") {
		http.Redirect(w, r, "/files"+clean+"/", http.StatusMovedPermanently)
		return
	}
	entries, _ := f.ReadDir(-1)
	var shown []os.DirEntry
	for _, e := range entries {
		if !b.isHidden(filepath.Join(full, e.Name())) {
			shown = append(shown, e)
		}
	}
	sort.Slice(shown, func(i, j int) bool {
		if shown[i].IsDir() != shown[j].IsDir() {
			return shown[i].IsDir()
		}
		return strings.ToLower(shown[i].Name()) < strings.ToLower(shown[j].Name())
	})
	listing(w, base+"/"+rest, "/files"+path.Dir(strings.TrimSuffix(clean, "/"))+"/", shown, nil)
}

type fileRow struct {
	Name, Href, Size, When string
	Dir                    bool
}

func listing(w http.ResponseWriter, title, up string, entries []os.DirEntry, roots []root) {
	var rows []fileRow
	for _, rt := range roots {
		rows = append(rows, fileRow{Name: rt.Path, Href: "/files/" + url.PathEscape(rt.Name) + "/", Dir: true})
	}
	for _, e := range entries {
		row := fileRow{Name: e.Name(), Href: url.PathEscape(e.Name()), Dir: e.IsDir()}
		if e.IsDir() {
			row.Href += "/"
		} else if fi, err := e.Info(); err == nil {
			row.Size = humanSize(fi.Size())
			row.When = fi.ModTime().Format("2006-01-02 15:04")
		}
		rows = append(rows, row)
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	filesPage.Execute(w, struct {
		Title, Up string
		Rows      []fileRow
	}{strings.TrimSuffix(title, "/"), up, rows})
}

func humanSize(n int64) string {
	switch {
	case n >= 1<<30:
		return fmt.Sprintf("%.1f GB", float64(n)/(1<<30))
	case n >= 1<<20:
		return fmt.Sprintf("%.1f MB", float64(n)/(1<<20))
	case n >= 1<<10:
		return fmt.Sprintf("%.0f KB", float64(n)/(1<<10))
	}
	return strconv.FormatInt(n, 10) + " bytes"
}

var filesPage = template.Must(template.New("files").Parse(`<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{.Title}} · TailStation</title>
<style>
:root { color-scheme: dark; }
body { margin: 0; background: #0b0b0c; color: #f2f2f2; font: 16px/1.5 system-ui, sans-serif; }
main { max-width: 760px; margin: 0 auto; padding: 24px 16px 48px; }
h1 { color: #e4eaf3; font-size: 22px; margin: 0 0 12px; word-break: break-all; }
a { color: #e4eaf3; text-decoration: none; word-break: break-all; }
table { width: 100%; border-collapse: collapse; }
td { padding: 6px 4px; border-bottom: 1px solid #2a2a2e; }
.dim { color: #9a9a9e; white-space: nowrap; }
</style></head><body><main>
<p><a href="/">← TailStation</a>{{if .Up}} · <a href="{{.Up}}">up a folder</a>{{end}}</p>
<h1>{{.Title}}</h1>
{{if .Rows}}<table>
{{range .Rows}}<tr><td><a href="{{.Href}}">{{if .Dir}}📁 {{end}}{{.Name}}</a></td><td class="dim">{{.Size}}</td><td class="dim">{{.When}}</td></tr>
{{end}}</table>{{else}}<p class="dim">Nothing here.</p>{{end}}
</main></body></html>
`))

// receiveFiles moves files sent to the PS5 with Taildrop into the taildrop
// folder as they arrive.
func receiveFiles(ctx context.Context, lc *local.Client, st *state, folder string) {
	if err := os.MkdirAll(folder, 0o755); err != nil {
		st.log.Say("Taildrop: can't make %s: %v", folder, err)
		return
	}
	for ctx.Err() == nil {
		files, err := lc.AwaitWaitingFiles(ctx, 10*time.Minute)
		if err != nil {
			if ctx.Err() == nil {
				time.Sleep(5 * time.Second)
			}
			continue
		}
		for _, wf := range files {
			name, err := takeFile(ctx, lc, wf.Name, folder)
			if err != nil {
				st.log.Say("Taildrop: %s: %v", wf.Name, err)
				continue
			}
			st.log.Say("Taildrop: received %s", name)
			st.set(func(s *state) {
				s.received = append([]string{filepath.Base(name)}, s.received...)
				if len(s.received) > 10 {
					s.received = s.received[:10]
				}
			})
		}
	}
}

func takeFile(ctx context.Context, lc *local.Client, name, folder string) (string, error) {
	rc, _, err := lc.GetWaitingFile(ctx, name)
	if err != nil {
		return "", err
	}
	defer rc.Close()
	dst := freeName(folder, filepath.Base(name))
	f, err := os.OpenFile(dst, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0o644)
	if err != nil {
		return "", err
	}
	if _, err := io.Copy(f, rc); err != nil {
		f.Close()
		os.Remove(dst)
		return "", err
	}
	if err := f.Close(); err != nil {
		return "", err
	}
	return dst, lc.DeleteWaitingFile(ctx, name)
}

// freeName: folder/name, or "name (2).ext" and so on if that's taken.
func freeName(folder, name string) string {
	p := filepath.Join(folder, name)
	ext := filepath.Ext(name)
	for i := 2; ; i++ {
		if _, err := os.Lstat(p); os.IsNotExist(err) {
			return p
		}
		p = filepath.Join(folder, fmt.Sprintf("%s (%d)%s", strings.TrimSuffix(name, ext), i, ext))
	}
}
