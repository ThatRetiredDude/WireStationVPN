package main

import (
	"bufio"
	"fmt"
	"net/netip"
	"net/url"
	"os"
	"strconv"
	"strings"
)

// settings.conf: "key = value" lines, # for comments. All optional.
type settings struct {
	Hostname    string   // the PS5's name in the tailnet
	ExitNode    bool     // offer the PS5 as an exit node
	HomeNetwork bool     // offer the home network (the PS5's LAN) as a route
	AuthKey     string   // a Tailscale auth key, instead of signing in with the link
	ControlURL  string   // a coordination server other than Tailscale's (Headscale)
	Port        int      // Tailscale's UDP port; 0 picks one
	FixedPort   bool     // when Port is 0: always use DefaultFixedPort, to forward on the router
	StatusPort  int      // the status page on the home network
	DNS         string   // the DNS server to use when the system names none
	Tags        []string // ACL tags to ask for, such as tag:ps5
	TailnetPage bool     // the status page and shared folders on http://ps5/
	Taildrop    bool     // take files sent with Taildrop
	TaildropDir string   // where they go; empty: received/ next to settings.conf
	Shared      []string // folders to share read-only; "taildrop" means TaildropDir

	// The PS5 as its own tailnet server (Headscale), instead of Tailscale's.
	Server       bool   // run the server; the PS5 joins it, not Tailscale's
	ServerPort   int    // the port devices reach it on (forwarded on the router)
	ServerURL    string // the address devices use; empty: http://HOME_IP:ServerPort
	ServerDomain string // MagicDNS names end in it: ps5.ServerDomain

	// Its home name (ddns.go), https (certs.go) and guard (guard.go).
	DuckDomain    string // a DuckDNS name: myps5 or myps5.duckdns.org
	DuckToken     string // the DuckDNS account's token
	DDNSUpdateURL string // or another service's update link
	HTTPS         bool   // https when there's a certificate to be had
	TLSCert       string // a certificate of your own (PEM), instead of Let's Encrypt
	TLSKey        string // its key
	ACMEEmail     string // for Let's Encrypt's expiry warnings; optional
	Guard         bool   // block addresses that misbehave
	CrowdSecURL   string // a CrowdSec local API, such as http://192.168.1.10:8080
	CrowdSecKey   string // its bouncer key
}

// DefaultFixedPort: Tailscale's usual UDP port, used with fixed_port = on.
const DefaultFixedPort = 41641

// udpPort: the UDP port Tailscale uses, 0 for any.
func (s settings) udpPort() int {
	if s.Port == 0 && s.FixedPort {
		return DefaultFixedPort
	}
	return s.Port
}

func defaults() settings {
	return settings{Hostname: "ps5", ExitNode: true, HomeNetwork: true, StatusPort: 9039, DNS: "1.1.1.1",
		TailnetPage: true, Taildrop: true, ServerPort: 8080, ServerDomain: "ps5net", HTTPS: true, Guard: true, Shared: []string{"taildrop", "/mnt/usb0", "/mnt/usb1", "/mnt/ext0", "/mnt/ext1"}}
}

// loadSettings reads path over the defaults; a missing file is fine. It
// returns what it couldn't use, for the log.
func loadSettings(path string) (settings, []string) {
	s := defaults()
	var problems []string
	f, err := os.Open(path)
	if err != nil {
		if !os.IsNotExist(err) {
			problems = append(problems, err.Error())
		}
		return s, problems
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for no := 1; sc.Scan(); no++ {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			problems = append(problems, fmt.Sprintf("line %d: want key = value", no))
			continue
		}
		k, v = strings.TrimSpace(k), strings.TrimSpace(v)
		bad := func(want string) { problems = append(problems, fmt.Sprintf("line %d: %s wants %s", no, k, want)) }
		switch k {
		case "hostname":
			s.Hostname = v
		case "exit_node", "home_network", "tailnet_page", "taildrop", "server", "fixed_port", "https", "guard":
			b, ok := parseBool(v)
			if !ok {
				bad("on or off")
				continue
			}
			*map[string]*bool{"exit_node": &s.ExitNode, "home_network": &s.HomeNetwork,
				"tailnet_page": &s.TailnetPage, "taildrop": &s.Taildrop, "server": &s.Server, "fixed_port": &s.FixedPort,
				"https": &s.HTTPS, "guard": &s.Guard}[k] = b
		case "taildrop_folder":
			if !strings.HasPrefix(v, "/") {
				bad("a full path, such as /data/received")
				continue
			}
			s.TaildropDir = v
		case "shared_folders":
			s.Shared = nil
			for _, f := range strings.Split(v, ",") {
				if f = strings.TrimSpace(f); f != "" {
					s.Shared = append(s.Shared, f)
				}
			}
		case "auth_key":
			s.AuthKey = v
		case "dns":
			if _, err := netip.ParseAddr(v); err != nil {
				bad("an IP address")
				continue
			}
			s.DNS = v
		case "tags":
			s.Tags = nil
			for _, t := range strings.Split(v, ",") {
				t = strings.TrimSpace(t)
				if t == "" {
					continue
				}
				if !strings.HasPrefix(t, "tag:") || len(t) == len("tag:") {
					bad("names like tag:ps5, separated by commas")
					s.Tags = nil
					break
				}
				s.Tags = append(s.Tags, t)
			}
		case "control_url":
			s.ControlURL = v
		case "server_url":
			u, err := url.Parse(v)
			if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" || (u.Path != "" && u.Path != "/") {
				bad("an address like http://myhome.example.net:8080")
				continue
			}
			s.ServerURL = strings.TrimSuffix(v, "/")
		case "duckdns_domain":
			if !validDomain(v) {
				bad("your DuckDNS name, such as myps5")
				continue
			}
			s.DuckDomain = v
		case "duckdns_token", "acme_email", "crowdsec_key":
			*map[string]*string{"duckdns_token": &s.DuckToken, "acme_email": &s.ACMEEmail, "crowdsec_key": &s.CrowdSecKey}[k] = v
		case "ddns_update_url", "crowdsec_url":
			u, err := url.Parse(v)
			if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
				bad("a link starting with http:// or https://")
				continue
			}
			*map[string]*string{"ddns_update_url": &s.DDNSUpdateURL, "crowdsec_url": &s.CrowdSecURL}[k] = v
		case "tls_cert", "tls_key":
			if !strings.HasPrefix(v, "/") {
				bad("a full path, such as /data/tailstation/cert.pem")
				continue
			}
			*map[string]*string{"tls_cert": &s.TLSCert, "tls_key": &s.TLSKey}[k] = v
		case "server_domain":
			if !validDomain(v) {
				bad("a name like ps5net or home.arpa")
				continue
			}
			s.ServerDomain = strings.ToLower(v)
		case "port", "status_port", "server_port":
			n, err := strconv.Atoi(v)
			if err != nil || n < 0 || n > 65535 || (k != "port" && n == 0) {
				bad("a port number")
				continue
			}
			*map[string]*int{"port": &s.Port, "status_port": &s.StatusPort, "server_port": &s.ServerPort}[k] = n
		default:
			problems = append(problems, fmt.Sprintf("line %d: %s isn't a setting", no, k))
		}
	}
	if (s.TLSCert == "") != (s.TLSKey == "") {
		problems = append(problems, "tls_cert and tls_key go together; neither is used")
		s.TLSCert, s.TLSKey = "", ""
	}
	return s, problems
}

// saveSettings sets some keys in the file at path, keeping every other
// line (comments, the auth key) as it was; "" removes a key.
func saveSettings(path string, set map[string]string) error {
	old, err := os.ReadFile(path)
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	var out []string
	done := map[string]bool{}
	for _, line := range strings.Split(strings.TrimRight(string(old), "\n"), "\n") {
		t := strings.TrimSpace(line)
		if k, _, ok := strings.Cut(t, "="); ok && !strings.HasPrefix(t, "#") {
			k = strings.TrimSpace(k)
			if v, ours := set[k]; ours {
				if v != "" && !done[k] {
					out = append(out, k+" = "+v)
				}
				done[k] = true
				continue
			}
		}
		if line != "" || len(out) > 0 {
			out = append(out, line)
		}
	}
	for k, v := range set {
		if !done[k] && v != "" {
			out = append(out, k+" = "+v)
		}
	}
	return os.WriteFile(path, []byte(strings.Join(out, "\n")+"\n"), 0o600)
}

// validDomain: dot-separated labels of letters, digits and hyphens.
func validDomain(v string) bool {
	if v == "" || len(v) > 200 {
		return false
	}
	for _, l := range strings.Split(v, ".") {
		if l == "" || len(l) > 63 || l[0] == '-' || l[len(l)-1] == '-' {
			return false
		}
		for _, c := range strings.ToLower(l) {
			if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-') {
				return false
			}
		}
	}
	return true
}

func parseBool(v string) (bool, bool) {
	switch strings.ToLower(v) {
	case "on", "yes", "true", "1":
		return true, true
	case "off", "no", "false", "0":
		return false, true
	}
	return false, false
}
