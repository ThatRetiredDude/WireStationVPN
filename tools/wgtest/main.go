// wgtest: connects to a WireStation server the way a phone would, with
// wireguard-go (the WireGuard project's own implementation, in user space:
// no root, no network interface), and checks the tunnel works.
//
//	go run . [-n 20] [-hold 3m] [-endpoint host:port] path/to/device1.conf
//
// It reads the device's config (the NAME.conf the server writes), pings the
// server's tunnel address, then fetches its status page through the tunnel
// n times. Exit status 0 when everything answered.
package main

import (
	"bufio"
	"encoding/base64"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"os"
	"strings"
	"time"

	"golang.org/x/net/icmp"
	"golang.org/x/net/ipv4"
	"golang.zx2c4.com/wireguard/conn"
	"golang.zx2c4.com/wireguard/device"
	"golang.zx2c4.com/wireguard/tun/netstack"
)

func hexKey(b64 string) string {
	k, err := base64.StdEncoding.DecodeString(strings.TrimSpace(b64))
	if err != nil || len(k) != 32 {
		fail("bad key %q", b64)
	}
	return hex.EncodeToString(k)
}

func fail(f string, a ...any) {
	fmt.Fprintf(os.Stderr, "wgtest: "+f+"\n", a...)
	os.Exit(1)
}

func main() {
	n := flag.Int("n", 5, "status page fetches")
	endpoint := flag.String("endpoint", "", "the server's host:port, instead of the config's Endpoint")
	hold := flag.Duration("hold", 0, "then keep the tunnel up this long, fetching the page every 10 s (past WireGuard's 2-minute rekey, say)")
	flag.Parse()
	if flag.NArg() != 1 {
		fail("usage: wgtest [-n N] [-endpoint host:port] device.conf")
	}
	f, err := os.Open(flag.Arg(0))
	if err != nil {
		fail("%v", err)
	}
	v := map[string]string{}
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if k, val, ok := strings.Cut(line, "="); ok && !strings.HasPrefix(line, "#") {
			v[strings.TrimSpace(k)] = strings.TrimSpace(val)
		}
	}
	f.Close()
	if *endpoint != "" {
		v["Endpoint"] = *endpoint
	}
	addr := netip.MustParsePrefix(v["Address"]).Addr()
	server := netip.MustParsePrefix(v["AllowedIPs"]).Addr()
	ep, err := net.ResolveUDPAddr("udp4", v["Endpoint"])
	if err != nil {
		fail("endpoint %q: %v", v["Endpoint"], err)
	}

	tdev, tnet, err := netstack.CreateNetTUN([]netip.Addr{addr}, nil, 1420)
	if err != nil {
		fail("netstack: %v", err)
	}
	dev := device.NewDevice(tdev, conn.NewDefaultBind(), device.NewLogger(device.LogLevelError, "wgtest: "))
	uapi := fmt.Sprintf("private_key=%s\npublic_key=%s\nendpoint=%s\nallowed_ip=%s/32\npersistent_keepalive_interval=25\n",
		hexKey(v["PrivateKey"]), hexKey(v["PublicKey"]), ep.String(), server)
	if err := dev.IpcSet(uapi); err != nil {
		fail("config: %v", err)
	}
	if err := dev.Up(); err != nil {
		fail("up: %v", err)
	}
	defer dev.Close()
	fmt.Printf("wgtest: %s -> %s through %s\n", addr, server, ep)

	// Ping.
	start := time.Now()
	pc, err := tnet.Dial("ping4", server.String())
	if err != nil {
		fail("ping: %v", err)
	}
	msg := icmp.Message{Type: ipv4.ICMPTypeEcho, Body: &icmp.Echo{ID: 1, Seq: 1, Data: []byte("wirestation")}}
	b, _ := msg.Marshal(nil)
	pc.SetDeadline(time.Now().Add(10 * time.Second))
	if _, err := pc.Write(b); err != nil {
		fail("ping: %v", err)
	}
	buf := make([]byte, 1500)
	nr, err := pc.Read(buf)
	if err != nil {
		fail("ping: no answer (%v)", err)
	}
	reply, err := icmp.ParseMessage(1, buf[:nr])
	if err != nil || reply.Type != ipv4.ICMPTypeEchoReply {
		fail("ping: not an echo reply")
	}
	fmt.Printf("wgtest: ping %s: answered in %v (handshake included)\n", server, time.Since(start).Round(time.Millisecond))
	pc.Close()

	// The status page, n times, each on a new connection.
	client := &http.Client{
		Transport: &http.Transport{DialContext: tnet.DialContext, DisableKeepAlives: true},
		Timeout:   10 * time.Second,
	}
	var body []byte
	start = time.Now()
	for i := 0; i < *n; i++ {
		resp, err := client.Get("http://" + server.String() + "/")
		if err != nil {
			fail("status page, fetch %d: %v", i+1, err)
		}
		body, err = io.ReadAll(resp.Body)
		resp.Body.Close()
		if err != nil || resp.StatusCode != 200 || !strings.HasPrefix(string(body), "WireStationVPN server") {
			fail("status page, fetch %d: status %d, %v", i+1, resp.StatusCode, err)
		}
	}
	fmt.Printf("wgtest: status page: %d fetches in %v\n", *n, time.Since(start).Round(time.Millisecond))
	fmt.Printf("--- the last one ---\n%s", body)

	for end := time.Now().Add(*hold); time.Now().Before(end); {
		time.Sleep(10 * time.Second)
		t := time.Now()
		resp, err := client.Get("http://" + server.String() + "/")
		if err != nil {
			fail("hold: fetch at %v: %v", time.Since(start).Round(time.Second), err)
		}
		io.Copy(io.Discard, resp.Body)
		resp.Body.Close()
		fmt.Printf("wgtest: hold: fetched in %v\n", time.Since(t).Round(time.Millisecond))
	}
}
