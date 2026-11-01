// TailStation's background part, the C++ build: for a PS5 where the Go
// build (node/) doesn't run. Tailscale's client from TokenStation5 (ts/),
// WireGuard and lwIP (tunnel.cpp), and the same settings, status page and
// /api/status as the Go build. Not here: running your own tailnet.
//
//   tailstation-lite [DATA_DIR]     (default /data/tailstation)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef WS_PS5
#include <ifaddrs.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "base.h"
#include "node.h"

#ifndef WS_VERSION
#define WS_VERSION "dev"
#endif

namespace ws {

namespace {

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool parse_bool(const std::string &v, bool &out) {
    std::string l;
    for (char c : v) l += (char)tolower((unsigned char)c);
    if (l == "on" || l == "yes" || l == "true" || l == "1") return out = true, true;
    if (l == "off" || l == "no" || l == "false" || l == "0") return out = false, true;
    return false;
}

std::vector<std::string> split_list(const std::string &v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

// The PS5's address on its home network: the one a packet out would leave
// from (nothing is sent). 0 when there's none.
uint32_t local_ip() {
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "1.1.1.1", &a.sin_addr);
    sockaddr_in me{};
    socklen_t len = sizeof me;
    uint32_t ip = 0;
    if (connect(s, (const sockaddr *)&a, sizeof a) == 0 && getsockname(s, (sockaddr *)&me, &len) == 0)
        ip = me.sin_addr.s_addr;
    close(s);
    return ip;
}

bool is_private(uint32_t h) {
    return (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8;
}

std::string cidr(uint32_t net, int bits) {
    in_addr a{};
    a.s_addr = htonl(net);
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, b, sizeof b);
    return std::string(b) + "/" + std::to_string(bits);
}

// The home networks: the PS5's private IPv4 networks (address, mask, host order).
std::vector<std::pair<uint32_t, uint32_t>> home_networks() {
    std::vector<std::pair<uint32_t, uint32_t>> out;
#ifndef WS_PS5
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs *i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || !i->ifa_netmask || i->ifa_addr->sa_family != AF_INET) continue;
            const uint32_t ip = ntohl(((sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
            const uint32_t mask = ntohl(((sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
            if (!is_private(ip) || mask < 0xFF000000u) continue;
            out.push_back({ip & mask, mask});
        }
        freeifaddrs(list);
    }
    if (!out.empty()) return out;
#endif
    // The PS5 lists no interfaces to a payload: its address, and the usual /24.
    const uint32_t ip = ntohl(local_ip());
    if (ip && is_private(ip)) out.push_back({ip & 0xFFFFFF00u, 0xFFFFFF00u});
    return out;
}

}  // namespace

Settings load_settings(const std::string &path, std::vector<std::string> &problems) {
    Settings s;
    std::ifstream f(path);
    if (!f) return s;
    std::string line;
    for (int no = 1; std::getline(f, line); no++) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            problems.push_back("line " + std::to_string(no) + ": want key = value");
            continue;
        }
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        auto bad = [&](const std::string &want) { problems.push_back("line " + std::to_string(no) + ": " + k + " wants " + want); };
        bool b;
        if (k == "hostname") s.hostname = v;
        else if (k == "exit_node" || k == "home_network" || k == "tailnet_page" || k == "taildrop" || k == "server" ||
                 k == "fixed_port") {
            if (!parse_bool(v, b)) {
                bad("on or off");
                continue;
            }
            if (k == "exit_node") s.exit_node = b;
            else if (k == "home_network") s.home_network = b;
            else if (k == "tailnet_page") s.tailnet_page = b;
            else if (k == "taildrop") s.taildrop = b;
            else if (k == "server") s.server = b;
            else s.fixed_port = b;
        } else if (k == "taildrop_folder") {
            if (v.empty() || v[0] != '/') bad("a full path, such as /data/received");
            else s.taildrop_dir = v;
        } else if (k == "shared_folders") {
            s.shared = split_list(v);
        } else if (k == "auth_key") {
            s.auth_key = v;
        } else if (k == "dns") {
            in_addr a{};
            if (inet_pton(AF_INET, v.c_str(), &a) != 1) bad("an IPv4 address");
            else s.dns = v;
        } else if (k == "tags") {
            s.tags.clear();
            for (const std::string &t : split_list(v)) {
                if (t.rfind("tag:", 0) != 0 || t.size() == 4) {
                    bad("names like tag:ps5, separated by commas");
                    s.tags.clear();
                    break;
                }
                s.tags.push_back(t);
            }
        } else if (k == "control_url") {
            s.control_url = v;
        } else if (k == "port" || k == "status_port") {
            const int p = atoi(v.c_str());
            if (p < (k == "port" ? 0 : 1) || p > 65535 || v.find_first_not_of("0123456789") != std::string::npos) {
                bad("a port number");
                continue;
            }
            (k == "port" ? s.port : s.status_port) = p;
        }
        // Everything else is the Go build's (its own tailnet, https, the
        // guard ...): left alone here.
    }
    return s;
}

}  // namespace ws

int main(int argc, char **argv) {
    using namespace ws;
    signal(SIGPIPE, SIG_IGN);
    static Node n;
    n.version = WS_VERSION;
    n.dir = argc > 1 ? argv[1] : "/data/tailstation";
    mkdir(n.dir.c_str(), 0755);
    log_open(n.dir + "/log.txt");
    log_line("TailStation %s (C++ build) starting", n.version.c_str());
    std::vector<std::string> problems;
    n.cfg = load_settings(n.dir + "/settings.conf", problems);
    for (const std::string &p : problems) log_line("settings.conf: %s", p.c_str());
    chmod((n.dir + "/settings.conf").c_str(), 0600);  // it can hold an auth key
    set_dns(n.cfg.dns);
    if (n.cfg.taildrop_dir.empty()) n.cfg.taildrop_dir = n.dir + "/received";
    for (std::string &f : n.cfg.shared)
        if (f == "taildrop") f = n.cfg.taildrop_dir;

    // One copy at a time: the status port is the lock.
    const int ln = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)n.cfg.status_port);
    const int one = 1;
    setsockopt(ln, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ln < 0 || bind(ln, (sockaddr *)&a, sizeof a) != 0 || listen(ln, 64) != 0) {
        log_line(errno == EADDRINUSE ? "already running (port %d is taken); leaving" : "can't open the status port %d",
                 n.cfg.status_port);
        return 1;
    }
    n.started = now_s();

    const uint16_t web = serve_web(n, ln);
    if (!web) {
        log_line("the web server didn't start");
        return 1;
    }

    if (n.cfg.server) {
        log_line("server = on (your own tailnet) needs the full build, which didn't run on this PS5; "
                 "turn it off to use a Tailscale account");
        while (!n.quit) usleep(200 * 1000);
        log_line("stopping");
        return 0;
    }

    // What the PS5 offers: the exit node, the home network.
    TunnelConfig tc;
    tc.exit_node = n.cfg.exit_node;
    tc.home_network = n.cfg.home_network;
    tc.home = home_networks();
    tc.web_port = web;
    tc.tailnet_port = n.cfg.tailnet_page ? 80 : 0;
    tc.peerapi_port = n.cfg.taildrop ? kPeerAPIPort : 0;
    if (n.cfg.exit_node) {
        n.routes.push_back("0.0.0.0/0");
        n.routes.push_back("::/0");
    }
    if (n.cfg.home_network)
        for (const auto &[net, mask] : tc.home) n.routes.push_back(cidr(net, __builtin_popcount(mask)));
    if (n.cfg.taildrop) mkdir(n.cfg.taildrop_dir.c_str(), 0755);

    ts::Config c;
    c.on = true;
    c.hostname = n.cfg.hostname;
    c.auth_key = n.cfg.auth_key;
    if (!n.cfg.control_url.empty()) c.control_url = n.cfg.control_url;
    c.state_file = n.dir + "/tailscale-lite.keys";
    c.version = n.version;
    c.routes = n.routes;
    c.tags = n.cfg.tags;
    c.peerapi_port = n.cfg.taildrop ? kPeerAPIPort : 0;
    c.udp_port = n.cfg.udp_port();
    n.client.apply(c);
    n.tunnel.start(&n.client, tc);
    std::string offered;
    for (const std::string &r : n.routes) offered += (offered.empty() ? "" : " ") + r;
    log_line("offering: %s", offered.empty() ? "nothing" : offered.c_str());

    ts::Status last;
    while (!n.quit) {
        usleep(200 * 1000);
        const ts::Status s = n.client.status();
        if (s.state != last.state || s.problem != last.problem) {
            static const char *names[] = {"off", "starting", "needs sign-in", "connecting", "running", "problem"};
            log_line("tailscale: %s%s%s", names[s.state], s.problem.empty() ? "" : ": ", s.problem.c_str());
        }
        last = s;
    }
    log_line("stopping");
    // The threads hold sockets: leave without waiting for them.
    fflush(stdout);
    _exit(0);
}
