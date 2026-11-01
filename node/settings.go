package main

import (
	"bufio"
	"fmt"
	"net/netip"
	"os"
	"strconv"
	"strings"
)

// settings.conf: "key = value" lines, # for comments. All optional.
type settings struct {
	Hostname    string // the PS5's name in the tailnet
	ExitNode    bool   // offer the PS5 as an exit node
	HomeNetwork bool   // offer the home network (the PS5's LAN) as a route
	AuthKey     string // a Tailscale auth key, instead of signing in with the link
	ControlURL  string // a coordination server other than Tailscale's (Headscale)
	Port        int    // Tailscale's UDP port; 0 picks one
	StatusPort  int    // the status page on the home network
	DNS         string // the DNS server to use when the system names none
}

func defaults() settings {
	return settings{Hostname: "ps5", ExitNode: true, HomeNetwork: true, StatusPort: 9039, DNS: "1.1.1.1"}
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
		case "exit_node", "home_network":
			b, ok := parseBool(v)
			if !ok {
				bad("on or off")
				continue
			}
			if k == "exit_node" {
				s.ExitNode = b
			} else {
				s.HomeNetwork = b
			}
		case "auth_key":
			s.AuthKey = v
		case "dns":
			if _, err := netip.ParseAddr(v); err != nil {
				bad("an IP address")
				continue
			}
			s.DNS = v
		case "control_url":
			s.ControlURL = v
		case "port", "status_port":
			n, err := strconv.Atoi(v)
			if err != nil || n < 0 || n > 65535 || (k == "status_port" && n == 0) {
				bad("a port number")
				continue
			}
			if k == "port" {
				s.Port = n
			} else {
				s.StatusPort = n
			}
		default:
			problems = append(problems, fmt.Sprintf("line %d: %s isn't a setting", no, k))
		}
	}
	return s, problems
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
