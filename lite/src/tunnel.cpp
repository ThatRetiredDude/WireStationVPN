#include "tunnel.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <list>

#include "base.h"
#include "ts/tailscale.h"

// The system's TCP_MSS (netinet/tcp.h) isn't the tunnel's (lwipopts.h).
#undef TCP_MSS
extern "C" {
#include "lwip/inet_chksum.h"
#include "lwip/init.h"
#include "lwip/ip.h"
#include "lwip/ip4.h"
#include "lwip/netif.h"
#include "lwip/prot/udp.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "crypto.h"
#include "wireguard.h"
#include "wireguardif.h"
#include "port/glue.h"
}

extern "C" void ws_wg_random(void *out, size_t n) {
    if (!ws::random_bytes((unsigned char *)out, n)) {
        // No random numbers means no safe keys: stop rather than go on.
        ws::log_line("tunnel: the system gave no random numbers");
        abort();
    }
}

extern "C" unsigned int ws_wg_rand(void) {
    unsigned int r;
    ws_wg_random(&r, sizeof r);
    return r;
}

extern "C" void ws_wg_diag(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    ws::log_line("tunnel: lwip: %s", line);
}

extern "C" void ws_wg_assert(const char *what, const char *file, int line) {
    ws::log_line("tunnel: lwip assertion failed: %s (%s:%d)", what, file, line);
    abort();
}

namespace ws {

namespace {

constexpr double kUdpIdle = 60, kDnsIdle = 15;  // seconds before a quiet UDP flow is closed
constexpr size_t kMaxUdpFlows = 512, kMaxTcp = 1024;

std::string b64(const uint8_t *key) {
    char out[64];
    size_t n = sizeof out;
    if (!wireguard_base64_encode(key, WIREGUARD_PUBLIC_KEY_LEN, out, &n)) return "";
    return std::string(out, strnlen(out, sizeof out));
}

bool unb64(const std::string &s, uint8_t *key) {
    size_t n = WIREGUARD_PUBLIC_KEY_LEN;
    return !s.empty() && wireguard_base64_decode(s.c_str(), key, &n) && n == WIREGUARD_PUBLIC_KEY_LEN;
}

bool parse_ip(const std::string &s, uint32_t &be) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    be = a.s_addr;
    return true;
}

std::string ip_text(uint32_t be) {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr a{};
    a.s_addr = be;
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

// A connection from a device: lwIP's side in the tunnel, and a socket on
// the PS5 (to the web server, or out to where the device was going).
struct Conn {
    tcp_pcb *pcb = nullptr;  // null once lwIP is done with it
    int fd = -1;
    uint16_t local_port = 0;  // to the web server: the socket's own port
    bool connecting = true;
    bool reached = false;     // its socket connected
    bool tunnel_eof = false;  // the device sent everything
    bool sock_eof = false;    // the other end sent everything
    bool shut_wr = false;
    std::string to_sock;      // from the device, not written to the socket yet
    std::string to_dev;       // from the socket, not taken by lwIP yet
    std::string what;         // for the log: "100.64.0.2 to 1.2.3.4:443"
};

// A UDP flow through the PS5: from a device's address and port, to dst.
struct Flow {
    uint32_t src = 0, dst = 0;  // network order
    uint16_t sport = 0, dport = 0;
    int fd = -1;
    double last = 0;
};

struct Loop {
    Tunnel *owner = nullptr;
    ts::Client *ts = nullptr;
    TunnelConfig cfg;
    netif nif{};
    wireguardif_init_data init{};
    std::string private_key;  // base64, as wireguardif takes it
    bool made = false, up = false;
    uint64_t version = ~0ull;
    std::string address;
    uint32_t ip = 0;  // the PS5's tailnet address, network order
    ts::Filter filter;
    std::map<uint32_t, double> refused;  // device address: when a refusal was last logged
    std::map<std::string, ts::Peer> known;  // by node key (32 bytes): the tailnet's devices
    // The tailnet's devices that have sent something, at most
    // WIREGUARD_MAX_PEERS at a time (the one quiet longest makes room), each
    // with a stand-in address (127.77.x.y, port 1) that tells ws_ts_send
    // which device a packet is for: the relay finds devices by key.
    struct Peer {
        uint8_t index = WIREGUARDIF_INVALID_INDEX;
        uint32_t standin = 0;  // network order
        double last = 0;
        std::string name, address;
        bool was_up = false;
    };
    std::map<std::string, Peer> peers;          // by node key
    std::map<uint32_t, std::string> standins;   // stand-in address: node key
    uint32_t next = 1;
    tcp_pcb *listener = nullptr;
    std::list<Conn *> conns;
    std::map<std::string, Flow *> flows;  // by (src, sport, dst, dport)
    bool poll_failed = false;
};

Loop *g_loop = nullptr;  // for lwIP's calls on the tunnel's thread

bool in_home(const Loop &L, uint32_t host) {
    for (const auto &[net, mask] : L.cfg.home)
        if ((host & mask) == (net & mask)) return true;
    return false;
}

// Whether a device may reach dst (host order) through the PS5, and why not.
const char *forward_refusal(const Loop &L, uint32_t src, uint32_t dst, uint16_t port, bool udp) {
    const uint8_t a = (uint8_t)(dst >> 24);
    if (a == 0 || a == 127 || a >= 224) return "not an address to reach";
    if ((dst & 0xFFC00000u) == 0x64400000u) return "another tailnet address";  // 100.64.0.0/10
    const bool home = in_home(L, dst);
    if (home ? !(L.cfg.home_network || L.cfg.exit_node) : !L.cfg.exit_node)
        return home ? "the home network isn't offered" : "the PS5 isn't offered as an exit node";
    if (!L.filter.allows(src, dst, port, udp))
        return L.filter.known ? "the tailnet's access rules don't allow it" : "no access rules from the control server yet";
    return nullptr;
}

void refused_log(Loop &L, uint32_t src_be, const std::string &what, const char *why) {
    const double now = now_s();
    double &last = L.refused[src_be];
    if (now - last > 60) {
        last = now;
        log_line("tunnel: %s refused: %s", what.c_str(), why);
    }
    if (L.refused.size() > 256) L.refused.clear();
}

void forget_pcb(Conn *c) {
    if (!c->pcb) return;
    tcp_arg(c->pcb, nullptr);
    tcp_recv(c->pcb, nullptr);
    tcp_sent(c->pcb, nullptr);
    tcp_err(c->pcb, nullptr);
    c->pcb = nullptr;
}

void drop_caller(Loop &L, Conn *c) {
    if (!c->local_port) return;
    std::lock_guard<std::mutex> lk(L.owner->mu_);
    const double now = now_s();
    auto &callers = L.owner->callers_;
    for (auto it = callers.begin(); it != callers.end();)
        it = it->second.second && it->second.second < now ? callers.erase(it) : std::next(it);
    auto it = callers.find(c->local_port);
    if (it != callers.end()) {
        // The web server may not have taken the connection yet: its caller
        // waits for it a while.
        if (c->reached) it->second.second = now + 30;
        else callers.erase(it);
    }
    c->local_port = 0;
}

err_t on_recv(void *arg, tcp_pcb *pcb, pbuf *p, err_t err) {
    Conn *c = (Conn *)arg;
    (void)pcb;
    if (!c) {
        if (p) pbuf_free(p);
        return ERR_OK;
    }
    if (!p || err != ERR_OK) {
        if (p) pbuf_free(p);
        c->tunnel_eof = true;
        return ERR_OK;
    }
    const size_t at = c->to_sock.size();
    c->to_sock.resize(at + p->tot_len);
    pbuf_copy_partial(p, &c->to_sock[at], p->tot_len, 0);
    pbuf_free(p);
    // tcp_recved once the socket has taken it, so the window holds the rest back.
    return ERR_OK;
}

void on_err(void *arg, err_t err) {
    Conn *c = (Conn *)arg;
    (void)err;
    if (c) c->pcb = nullptr;  // lwIP freed it
}

// A socket that connects to addr:port (network order) without waiting.
int start_connect(uint32_t addr, uint16_t port, uint16_t *local_port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (local_port) {
        // Bound first, so the caller is noted before the web server accepts.
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof me;
        if (bind(fd, (sockaddr *)&me, sizeof me) != 0 || getsockname(fd, (sockaddr *)&me, &len) != 0) {
            close(fd);
            return -1;
        }
        *local_port = ntohs(me.sin_port);
    }
    nonblocking(fd);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = addr;
    to.sin_port = htons(port);
    if (connect(fd, (sockaddr *)&to, sizeof to) != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    return fd;
}

err_t on_accept(void *arg, tcp_pcb *pcb, err_t err) {
    Loop &L = *(Loop *)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    const uint32_t src = ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip)), dst = ip4_addr_get_u32(ip_2_ip4(&pcb->local_ip));
    const uint16_t port = pcb->local_port;
    const std::string what = ip_text(src) + " to " + ip_text(dst) + ":" + std::to_string(port);
    auto refuse = [&](const char *why) {
        refused_log(L, src, what, why);
        tcp_abort(pcb);
        return ERR_ABRT;
    };
    if (L.conns.size() >= kMaxTcp) return refuse("too many connections open");
    Conn *c = new Conn;
    c->what = what;
    if (dst == L.ip) {
        // To the PS5 itself: only this program's own services.
        const bool page = L.cfg.tailnet_port && port == L.cfg.tailnet_port;
        const bool peerapi = L.cfg.peerapi_port && port == L.cfg.peerapi_port;
        if (!page && !peerapi) {
            delete c;
            return refuse("nothing on that port");
        }
        if (!L.filter.allows(ntohl(src), ntohl(dst), port)) {
            delete c;
            return refuse(L.filter.known ? "the tailnet's access rules don't allow it" : "no access rules yet");
        }
        c->fd = start_connect(htonl(INADDR_LOOPBACK), L.cfg.web_port, &c->local_port);
        if (c->fd < 0) {
            delete c;
            return refuse("the web server can't be reached");
        }
        Caller who;
        who.address = ip_text(src);
        who.port = port;
        std::lock_guard<std::mutex> lk(L.owner->mu_);
        L.owner->callers_[c->local_port] = {who, 0.0};
    } else {
        if (const char *why = forward_refusal(L, ntohl(src), ntohl(dst), port, false)) {
            delete c;
            return refuse(why);
        }
        c->fd = start_connect(dst, port, nullptr);
        if (c->fd < 0) {
            delete c;
            return refuse("no socket");
        }
    }
    c->pcb = pcb;
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    tcp_nagle_disable(pcb);
    L.conns.push_back(c);
    return ERR_OK;
}

// Moves what it can between the device and the socket; false once it's over.
bool pump(Loop &L, Conn *c) {
    if (c->connecting) {
        // Whatever poll said (on the PS5 it may not say a socket is ready).
        int e = 0;
        socklen_t len = sizeof e;
        getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &e, &len);
        sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        if (e != 0) {
            // It couldn't connect: the device gets a reset.
            if (c->pcb) {
                tcp_pcb *pcb = c->pcb;
                forget_pcb(c);
                tcp_abort(pcb);
            }
            c->sock_eof = true;
            c->connecting = false;
        } else if (getpeername(c->fd, (sockaddr *)&peer, &plen) == 0) {
            c->connecting = false;
            c->reached = true;
        }
    }
    if (c->connecting && c->pcb) return true;
    // Device to socket.
    if (!c->to_sock.empty() && !c->sock_eof) {
        const ssize_t n = ::send(c->fd, c->to_sock.data(), c->to_sock.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            c->to_sock.erase(0, (size_t)n);
            for (size_t left = (size_t)n; c->pcb && left > 0;) {
                const u16_t k = (u16_t)(left < 0xFFFF ? left : 0xFFFF);
                tcp_recved(c->pcb, k);
                left -= k;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            c->sock_eof = true;
            c->to_sock.clear();
        }
    }
    if (c->tunnel_eof && c->to_sock.empty() && !c->shut_wr) {
        shutdown(c->fd, SHUT_WR);
        c->shut_wr = true;
    }
    // Socket to device: read while there's room to keep it, then as much as
    // lwIP's send buffer takes.
    if (c->pcb && !c->sock_eof && !c->connecting) {
        while (c->to_dev.size() < 65536) {
            char buf[16384];
            const ssize_t n = ::recv(c->fd, buf, sizeof buf, MSG_DONTWAIT);
            if (n > 0) {
                c->to_dev.append(buf, (size_t)n);
                continue;
            }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) c->sock_eof = true;
            break;
        }
    }
    if (c->pcb && !c->to_dev.empty()) {
        size_t sent = 0;
        while (sent < c->to_dev.size()) {
            size_t k = c->to_dev.size() - sent;
            const size_t room = tcp_sndbuf(c->pcb);
            if (k > room) k = room;
            if (k > 0xFFFF) k = 0xFFFF;
            if (k == 0 || tcp_sndqueuelen(c->pcb) >= TCP_SND_QUEUELEN - 4) break;
            if (tcp_write(c->pcb, c->to_dev.data() + sent, (u16_t)k, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
            sent += k;
        }
        c->to_dev.erase(0, sent);
        tcp_output(c->pcb);
    }
    if (c->sock_eof && c->to_dev.empty() && c->pcb) {
        // Its queued data still goes out before the FIN; lwIP frees it after.
        tcp_pcb *pcb = c->pcb;
        forget_pcb(c);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
    if (!c->pcb) {
        drop_caller(L, c);
        close(c->fd);
        return false;
    }
    return true;
}

void drop_conns(Loop &L) {
    for (Conn *c : L.conns) {
        if (c->pcb) {
            tcp_pcb *pcb = c->pcb;
            forget_pcb(c);
            tcp_abort(pcb);
        }
        drop_caller(L, c);
        close(c->fd);
        delete c;
    }
    L.conns.clear();
}

std::string flow_key(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport) {
    std::string k(12, '\0');
    memcpy(&k[0], &src, 4);
    memcpy(&k[4], &sport, 2);
    memcpy(&k[6], &dst, 4);
    memcpy(&k[10], &dport, 2);
    return k;
}

void close_flow(Flow *f) {
    if (f->fd >= 0) close(f->fd);
    delete f;
}

// A UDP packet back to the device, from where its flow was going.
void udp_reply(Loop &L, const Flow &f, const char *data, size_t n) {
    if (n > 1420 - 28) return;  // no fragments in the tunnel
    pbuf *q = pbuf_alloc(PBUF_IP, (u16_t)(UDP_HLEN + n), PBUF_RAM);
    if (!q) return;
    udp_hdr *h = (udp_hdr *)q->payload;
    h->src = htons(f.dport);
    h->dest = htons(f.sport);
    h->len = htons((u16_t)(UDP_HLEN + n));
    h->chksum = 0;
    memcpy((char *)q->payload + UDP_HLEN, data, n);
    ip_addr_t from, to;
    ip_addr_set_ip4_u32_val(from, f.dst);
    ip_addr_set_ip4_u32_val(to, f.src);
    u16_t sum = ip_chksum_pseudo(q, IP_PROTO_UDP, q->tot_len, &from, &to);
    if (sum == 0) sum = 0xFFFF;
    h->chksum = sum;
    ip4_output_if_src(q, ip_2_ip4(&from), ip_2_ip4(&to), 64, 0, IP_PROTO_UDP, &L.nif);
    pbuf_free(q);
}

void pump_flows(Loop &L) {
    const double now = now_s();
    for (auto it = L.flows.begin(); it != L.flows.end();) {
        Flow *f = it->second;
        for (int k = 0; k < 64; k++) {
            char buf[2048];
            const ssize_t n = recv(f->fd, buf, sizeof buf, MSG_DONTWAIT);
            if (n <= 0) break;
            f->last = now;
            udp_reply(L, *f, buf, (size_t)n);
        }
        const double idle = f->dport == 53 ? kDnsIdle : kUdpIdle;
        if (now - f->last > idle) {
            close_flow(f);
            it = L.flows.erase(it);
        } else {
            ++it;
        }
    }
}

void drop_flows(Loop &L) {
    for (auto &[k, f] : L.flows) close_flow(f);
    L.flows.clear();
}

// Stops the devices' sessions.
void drop_peers(Loop &L) {
    for (auto &[key, p] : L.peers)
        if (p.index != WIREGUARDIF_INVALID_INDEX) wireguardif_remove_peer(&L.nif, p.index);
    L.peers.clear();
    L.standins.clear();
}

// Brings the tailnet's device in line with what the client knows.
void sync(Loop &L) {
    const uint64_t v = L.ts->netmap_version();
    if (v == L.version) return;
    L.version = v;
    const ts::Netmap m = L.ts->netmap();
    L.known.clear();
    for (const ts::Peer &p : m.peers) L.known[std::string((const char *)p.key, 32)] = p;
    L.filter = m.filter;
    uint32_t ip = 0;
    if (m.address.empty() || !parse_ip(m.address, ip)) {
        if (L.up) {
            drop_peers(L);
            drop_conns(L);
            drop_flows(L);
            netif_set_down(&L.nif);
            L.up = false;
            log_line("tunnel: off the tailnet");
        }
        return;
    }
    const std::string priv = b64(m.node_priv);
    ip4_addr_t addr, mask, gw;
    ip4_addr_set_u32(&addr, ip);
    ip4_addr_set_u32(&mask, htonl(0xFFC00000u));  // 100.64.0.0/10, where tailnets' addresses are
    ip4_addr_set_u32(&gw, 0);
    if (!L.made) {
        L.private_key = priv;
        L.init.private_key = L.private_key.c_str();
        L.init.listen_port = 1;  // no socket: its packets go through the Tailscale client
        L.init.bind_netif = nullptr;
        if (!netif_add(&L.nif, &addr, &mask, &gw, &L.init, wireguardif_init, ip_input)) {
            log_line("tunnel: the tailnet's interface didn't start");
            return;
        }
        netif_set_default(&L.nif);
        L.made = true;
    } else if (priv != L.private_key) {
        uint8_t key[WIREGUARD_PRIVATE_KEY_LEN];
        if (unb64(priv, key)) {
            drop_peers(L);
            wireguard_device_init((wireguard_device *)L.nif.state, key);
            crypto_zero(key, sizeof key);
            L.private_key = priv;
        }
    }
    if (m.address != L.address) {
        netif_set_ipaddr(&L.nif, &addr);
        L.address = m.address;
    }
    L.ip = ip;
    if (!L.up) {
        netif_set_up(&L.nif);
        netif_set_link_up(&L.nif);
        L.up = true;
        log_line("tunnel: the PS5 is %s on the tailnet", m.address.c_str());
    }
    // Devices gone from the tailnet, or moved to another address.
    for (auto it = L.peers.begin(); it != L.peers.end();) {
        auto k = L.known.find(it->first);
        if (k == L.known.end() || k->second.address != it->second.address) {
            if (it->second.index != WIREGUARDIF_INVALID_INDEX) wireguardif_remove_peer(&L.nif, it->second.index);
            L.standins.erase(it->second.standin);
            it = L.peers.erase(it);
        } else {
            ++it;
        }
    }
}

// A device as a peer of the tailnet's WireGuard device, the first time it
// sends something.
std::map<std::string, Loop::Peer>::iterator add_peer(Loop &L, const std::string &key) {
    auto known = L.known.find(key);
    if (known == L.known.end()) return L.peers.end();
    uint32_t ip;
    if (!parse_ip(known->second.address, ip)) return L.peers.end();
    if (L.peers.size() >= WIREGUARD_MAX_PEERS) {
        auto quiet = L.peers.begin();
        for (auto it = L.peers.begin(); it != L.peers.end(); ++it)
            if (it->second.last < quiet->second.last) quiet = it;
        log_line("tunnel: %s makes room for %s", quiet->second.name.c_str(), known->second.name.c_str());
        wireguardif_remove_peer(&L.nif, quiet->second.index);
        L.standins.erase(quiet->second.standin);
        L.peers.erase(quiet);
    }
    Loop::Peer p;
    do {
        p.standin = htonl(0x7F4D0000u | (L.next++ & 0xFFFFu));  // 127.77.x.y
    } while (L.standins.count(p.standin) || (ntohl(p.standin) & 0xFFFFu) == 0);
    p.name = known->second.name;
    p.address = known->second.address;
    const std::string pub = b64((const uint8_t *)key.data());
    wireguardif_peer peer;
    wireguardif_peer_init(&peer);
    peer.public_key = pub.c_str();
    peer.preshared_key = nullptr;
    ip_addr_set_ip4_u32_val(peer.allowed_ip, ip);
    ip_addr_set_ip4_u32_val(peer.allowed_mask, 0xFFFFFFFFu);
    ip_addr_set_ip4_u32_val(peer.endpoint_ip, p.standin);
    peer.endport_port = 1;
    if (wireguardif_add_peer(&L.nif, &peer, &p.index) != ERR_OK || p.index == WIREGUARDIF_INVALID_INDEX) {
        log_line("tunnel: can't add %s", p.name.c_str());
        return L.peers.end();
    }
    L.standins[p.standin] = key;
    return L.peers.emplace(key, p).first;
}

// What came through the client: WireGuard's packets.
void receive(Loop &L) {
    uint8_t from[32];
    std::string d;
    for (int k = 0; k < 512 && L.ts->receive(from, d); k++) {
        if (!L.up) continue;
        // WireGuard's messages are types 1 to 4, then three zero bytes.
        if (d.size() < 4 || d[0] < 1 || d[0] > 4 || d[1] || d[2] || d[3]) continue;
        const std::string key((const char *)from, 32);
        auto it = L.peers.find(key);
        if (it == L.peers.end() && (it = add_peer(L, key)) == L.peers.end()) continue;
        it->second.last = now_s();
        ws_wg_input(d.data(), d.size(), it->second.standin, 1);
        // Data for a session the PS5 doesn't have (it restarted since): a
        // handshake from here, rather than the device's own after 15 s.
        ip_addr_t at;
        u16_t port;
        wireguard_device *dev = (wireguard_device *)L.nif.state;
        if (d[0] == 4 && dev && it->second.index < WIREGUARD_MAX_PEERS &&
            wireguardif_peer_is_up(&L.nif, it->second.index, &at, &port) != ERR_OK)
            dev->peers[it->second.index].send_handshake = true;
    }
}

}  // namespace

Tunnel::~Tunnel() {
    quit_ = true;
    if (thread_.joinable()) thread_.join();
}

void Tunnel::start(ts::Client *client, const TunnelConfig &cfg) {
    client_ = client;
    cfg_ = cfg;
    thread_ = std::thread([this] { loop(); });
}

std::map<std::string, TunnelPeer> Tunnel::peers() {
    std::lock_guard<std::mutex> lk(mu_);
    return peers_;
}

bool Tunnel::caller(uint16_t local_port, Caller &out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = callers_.find(local_port);
    if (it == callers_.end()) return false;
    out = it->second.first;
    return true;
}

void Tunnel::loop() {
    Loop L;
    L.owner = this;
    L.ts = client_;
    L.cfg = cfg_;
    lwip_init();
    g_loop = &L;
    tcp_pcb *pcb = tcp_new();
    if (!pcb || tcp_bind(pcb, IP_ADDR_ANY, 1) != ERR_OK || !(L.listener = tcp_listen(pcb))) {
        log_line("tunnel: no listener");
        return;
    }
    tcp_arg(L.listener, &L);
    tcp_accept(L.listener, on_accept);

    double last_states = 0;
    std::vector<pollfd> fds;
    while (!quit_) {
        sync(L);
        receive(L);

        fds.clear();
        for (Conn *c : L.conns) {
            short ev = 0;
            if (c->connecting || (!c->to_sock.empty() && !c->sock_eof)) ev |= POLLOUT;
            if (c->pcb && !c->sock_eof && !c->connecting && c->to_dev.size() < 65536) ev |= POLLIN;
            fds.push_back({c->fd, ev, 0});
        }
        for (auto &[k, f] : L.flows) fds.push_back({f->fd, POLLIN, 0});
        u32_t sleep_ms = sys_timeouts_sleeptime();
        if (sleep_ms > 50) sleep_ms = 50;
        // With anything open, more often: on the PS5 poll may not wake for a
        // socket that's ready, and the client's packets wait in a queue poll
        // can't see.
        if ((L.up || !L.conns.empty()) && sleep_ms > 2) sleep_ms = 2;
        if (fds.empty()) usleep(sleep_ms * 1000);
        else if (poll(fds.data(), (nfds_t)fds.size(), (int)sleep_ms) < 0 && errno != EINTR && !L.poll_failed) {
            log_line("tunnel: poll failed (errno %d)", errno);
            L.poll_failed = true;
            usleep(sleep_ms * 1000);
        }

        for (auto it = L.conns.begin(); it != L.conns.end();) {
            Conn *c = *it;
            if (pump(L, c)) {
                ++it;
            } else {
                delete c;
                it = L.conns.erase(it);
            }
        }
        pump_flows(L);
        receive(L);
        sys_check_timeouts();
        open_tcp_ = (int)L.conns.size();
        open_udp_ = (int)L.flows.size();

        const double t = now_s();
        if (t - last_states >= 0.5 && L.up) {
            last_states = t;
            std::map<std::string, TunnelPeer> states;
            const wireguard_device *dev = (const wireguard_device *)L.nif.state;
            const uint32_t now_ms = wireguard_sys_now();
            for (auto &[key, p] : L.peers) {
                TunnelPeer st;
                ip_addr_t from;
                u16_t port = 0;
                st.up = wireguardif_peer_is_up(&L.nif, p.index, &from, &port) == ERR_OK;
                st.direct = L.ts->direct((const uint8_t *)key.data());
                if (dev && p.index < WIREGUARD_MAX_PEERS && dev->peers[p.index].last_rx)
                    st.seen_s = (double)(uint32_t)(now_ms - dev->peers[p.index].last_rx) / 1000.0;
                if (st.up != p.was_up)
                    log_line(st.up ? "tunnel: %s (%s) connected" : "tunnel: %s (%s)'s session ended", p.name.c_str(),
                             p.address.c_str());
                p.was_up = st.up;
                states[p.address] = st;
            }
            std::lock_guard<std::mutex> lk(mu_);
            peers_ = std::move(states);
        }
    }
    g_loop = nullptr;
    drop_conns(L);
    drop_flows(L);
}

}  // namespace ws

using ws::g_loop;

extern "C" bool ws_ts_send(const void *data, size_t len, uint32_t ip_be, uint16_t port) {
    (void)port;
    ws::Loop *L = g_loop;
    if (!L || !L->ts) return false;
    auto it = L->standins.find(ip_be);
    if (it == L->standins.end()) return false;
    L->ts->send((const uint8_t *)it->second.data(), data, len);
    return true;
}

// lwIP (third_party/lwip, our change): packets for any address are taken on
// the tailnet's interface, TCP and UDP only (others, such as a ping to
// 8.8.8.8, would be answered as if from there).
static u8_t g_proto;  // the protocol of the packet lwIP is taking in

extern "C" int ws_ip4_seen(struct pbuf *p, struct netif *inp) {
    (void)inp;
    g_proto = p->len >= IP_HLEN ? IPH_PROTO((const struct ip_hdr *)p->payload) : 0;
    return 0;  // lwIP goes on with it
}

extern "C" int ws_accept_any(struct netif *netif) {
    ws::Loop *L = g_loop;
    if (!L || netif != &L->nif) return 0;
    return g_proto == IP_PROTO_TCP || g_proto == IP_PROTO_UDP;
}

extern "C" struct tcp_pcb *ws_tcp_catchall(void) {
    ws::Loop *L = g_loop;
    return L ? L->listener : nullptr;
}

// lwIP's udp.c isn't built: devices' UDP, through the PS5. One socket per
// flow (device address and port to where it's going), closed when quiet.
extern "C" void udp_input(struct pbuf *p, struct netif *inp) {
    ws::Loop *L = g_loop;
    (void)inp;
    if (!L || p->tot_len < UDP_HLEN) {
        pbuf_free(p);
        return;
    }
    udp_hdr h;
    pbuf_copy_partial(p, &h, UDP_HLEN, 0);
    const uint32_t src = ip4_addr_get_u32(ip4_current_src_addr()), dst = ip4_addr_get_u32(ip4_current_dest_addr());
    const uint16_t sport = ntohs(h.src), dport = ntohs(h.dest);
    size_t n = ntohs(h.len);
    if (n < UDP_HLEN || n > p->tot_len) n = p->tot_len;
    n -= UDP_HLEN;
    if (dst == L->ip || dst == 0xFFFFFFFFu) {  // nothing of the PS5's own takes UDP from the tailnet
        pbuf_free(p);
        return;
    }
    const std::string key = ws::flow_key(src, sport, dst, dport);
    auto it = L->flows.find(key);
    if (it == L->flows.end()) {
        const std::string what = ws::ip_text(src) + " to " + ws::ip_text(dst) + ":" + std::to_string(dport) + " (UDP)";
        if (const char *why = ws::forward_refusal(*L, ntohl(src), ntohl(dst), dport, true)) {
            ws::refused_log(*L, src, what, why);
            pbuf_free(p);
            return;
        }
        if (L->flows.size() >= ws::kMaxUdpFlows) {
            // The quietest makes room.
            auto quiet = L->flows.begin();
            for (auto f = L->flows.begin(); f != L->flows.end(); ++f)
                if (f->second->last < quiet->second->last) quiet = f;
            ws::close_flow(quiet->second);
            L->flows.erase(quiet);
        }
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = dst;
        to.sin_port = htons(dport);
        if (fd < 0 || connect(fd, (sockaddr *)&to, sizeof to) != 0) {
            if (fd >= 0) close(fd);
            pbuf_free(p);
            return;
        }
        ws::nonblocking(fd);
        ws::Flow *f = new ws::Flow;
        f->src = src;
        f->dst = dst;
        f->sport = sport;
        f->dport = dport;
        f->fd = fd;
        it = L->flows.emplace(key, f).first;
    }
    ws::Flow *f = it->second;
    f->last = ws::now_s();
    char buf[2048];
    if (n > sizeof buf) n = sizeof buf;
    pbuf_copy_partial(p, buf, (u16_t)n, UDP_HLEN);
    pbuf_free(p);
    send(f->fd, buf, n, MSG_DONTWAIT);
}
