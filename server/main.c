/* wirestation-server: a WireGuard server that runs as a PS5 payload.
 *
 * Build step 1 of WireStation (docs/PLAN.md): sent by hand to the
 * jailbreak's ELF loader (127.0.0.1:9021 on the console, or PS5_IP:9021
 * from a computer), it opens one UDP port and answers WireGuard on it.
 * Devices reach only the PS5 itself for now: ping 10.66.0.1 and a status
 * page at http://10.66.0.1/ inside the tunnel. No exit node yet.
 *
 * The PS5 gives a payload no network interface of its own to make, so the
 * tunnel's packets are taken apart here: WireGuard (third_party/
 * wireguard-lwip) on one UDP socket, and inside it lwIP (third_party/lwip),
 * driven by one thread. The same approach as TokenStation5's tunnel
 * (ps5/wg in PR #80), which this reuses.
 *
 * Its files live in WS_DIR (/data/wirestation on the console):
 *
 *   server.conf   optional: port, address, status_port, endpoint (below)
 *   server.key    its private key, made on the first start
 *   peers.conf    one device a line: "name public-key address"; a line with
 *                 only a name gets keys and an address at the next start,
 *                 and its config for the WireGuard app in NAME.conf
 *   log.txt       what it did, with a line a minute while it runs
 *
 * A status page on the LAN, http://PS5_IP:9039/ (status_port), says what it
 * is doing; /stop there stops it. That port is also how a second copy finds
 * the first and leaves.
 *
 * Copyright (C) 2026 ThatRetiredDude. GPL-3.0-or-later. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef WS_PS5
#include <sys/sysctl.h>
#include <ps5/kernel.h>
#endif

#include "lwip/init.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "crypto.h"
#include "wireguard.h"
#include "wireguardif.h"
#include "wg_udp.h"

#ifndef WS_VERSION
#define WS_VERSION "dev"
#endif
#ifndef WS_DEFAULT_DIR
#define WS_DEFAULT_DIR "/data/wirestation"
#endif

#define MAX_PEERS WIREGUARD_MAX_PEERS
#define KEY_B64 64  // a key in base64 is 44 characters and a NUL

/* ---- the log ---- */

static char dir[256];
static FILE *log_file;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

static double mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double started_s;

// A line in log.txt and on stdout, with the time of day and the seconds
// since the start (the monotonic clock: a gap in one and not the other says
// the PS5 put the process to sleep).
static void say(const char *fmt, ...) {
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    char when[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    pthread_mutex_lock(&log_mu);
    printf("%s +%.0fs wirestation: %s\n", when, mono_s() - started_s, line);
    fflush(stdout);
    if (log_file) {
        fprintf(log_file, "%s +%.0fs %s\n", when, mono_s() - started_s, line);
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mu);
}

// A notification on the TV, as the payload SDK's samples send one.
static void notify(const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
#ifdef WS_PS5
    typedef struct notify_request {
        char useless1[45];
        char message[3075];
    } notify_request_t;
    int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
    notify_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "WireStation: %s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
#endif
    say("notified: %s", msg);
}

void ws_diag(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    say("lwip: %s", line);
}

void ws_assert(const char *what, const char *file, int line) {
    say("lwip assertion failed: %s (%s:%d)", what, file, line);
    abort();
}

/* ---- random numbers and keys ---- */

static bool fill_random(void *out, size_t n) {
    unsigned char *p = out;
#ifdef WS_PS5
    // kern.arandom, as TokenStation5 reads it on the PS5.
    static const int name[2] = {CTL_KERN, KERN_ARND};
    while (n > 0) {
        size_t k = n < 256 ? n : 256;
        if (sysctl(name, 2, p, &k, NULL, 0) != 0 || k == 0) return false;
        p += k;
        n -= k;
    }
    return true;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    while (n > 0) {
        ssize_t k = read(fd, p, n);
        if (k <= 0) {
            if (k < 0 && errno == EINTR) continue;
            close(fd);
            return false;
        }
        p += k;
        n -= (size_t)k;
    }
    close(fd);
    return true;
#endif
}

void ws_random(void *out, size_t n) {
    if (!fill_random(out, n)) {
        // No random numbers means no safe keys: stop rather than go on.
        say("the system gave no random numbers; stopping");
        abort();
    }
}

unsigned int ws_rand(void) {
    unsigned int r;
    ws_random(&r, sizeof r);
    return r;
}

static bool b64(const uint8_t key[32], char out[KEY_B64]) {
    size_t n = KEY_B64;
    memset(out, 0, KEY_B64);
    return wireguard_base64_encode(key, 32, out, &n);
}

static bool unb64(const char *s, uint8_t key[32]) {
    size_t n = 32;
    return s[0] && wireguard_base64_decode(s, key, &n) && n == 32;
}

static bool make_keys(char priv_b64[KEY_B64], char pub_b64[KEY_B64]) {
    uint8_t priv[32], pub[32];
    if (!fill_random(priv, sizeof priv)) return false;
    priv[0] &= 248;
    priv[31] = (uint8_t)((priv[31] & 127) | 64);
    x25519_base(pub, priv, 1);
    bool ok = b64(priv, priv_b64) && b64(pub, pub_b64);
    crypto_zero(priv, sizeof priv);
    return ok;
}

static bool public_key_of(const char *priv_b64, char pub_b64[KEY_B64]) {
    uint8_t priv[32], pub[32];
    if (!unb64(priv_b64, priv)) return false;
    x25519_base(pub, priv, 1);
    crypto_zero(priv, sizeof priv);
    return b64(pub, pub_b64);
}

/* ---- settings ---- */

struct peer {
    char name[32];
    char public_key[KEY_B64];
    char address[INET_ADDRSTRLEN];
    uint8_t index;  // wireguardif's
    bool added;
    bool was_up;
};

static struct {
    int port;                          // UDP, WireGuard
    char address[INET_ADDRSTRLEN];     // the PS5's in the tunnel, a /24
    int status_port;                   // TCP, the status page on the LAN
    char endpoint[128];                // what devices dial, for NAME.conf; "" = the LAN address
    char private_key[KEY_B64];
    char public_key[KEY_B64];
    struct peer peers[MAX_PEERS];
    int npeers;
} cfg = {.port = 9030, .address = "10.66.0.1", .status_port = 9039};

static void path_of(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", dir, name); }

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

static bool parse_ip(const char *s, uint32_t *be) {
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) return false;
    *be = a.s_addr;
    return true;
}

static void read_server_conf(void) {
    char path[300];
    path_of(path, sizeof path, "server.conf");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        if (!strcmp(k, "port")) cfg.port = atoi(v);
        else if (!strcmp(k, "status_port")) cfg.status_port = atoi(v);
        else if (!strcmp(k, "address")) snprintf(cfg.address, sizeof cfg.address, "%s", v);
        else if (!strcmp(k, "endpoint")) snprintf(cfg.endpoint, sizeof cfg.endpoint, "%s", v);
        else say("server.conf: %s isn't a setting", k);
    }
    fclose(f);
}

static bool write_file(const char *name, const char *text, mode_t mode) {
    char path[300], tmp[310];
    path_of(path, sizeof path, name);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;
    size_t n = strlen(text);
    bool ok = write(fd, text, n) == (ssize_t)n;
    ok = close(fd) == 0 && ok;
    return ok && rename(tmp, path) == 0;
}

// server.key, made the first time.
static bool load_key(void) {
    char path[300];
    path_of(path, sizeof path, "server.key");
    FILE *f = fopen(path, "r");
    if (f) {
        char line[128] = "";
        if (!fgets(line, sizeof line, f)) line[0] = 0;
        fclose(f);
        snprintf(cfg.private_key, sizeof cfg.private_key, "%s", trim(line));
        if (public_key_of(cfg.private_key, cfg.public_key)) return true;
        say("server.key isn't a WireGuard private key; leaving it and stopping");
        return false;
    }
    if (!make_keys(cfg.private_key, cfg.public_key)) return false;
    char text[KEY_B64 + 2];
    snprintf(text, sizeof text, "%s\n", cfg.private_key);
    if (!write_file("server.key", text, 0600)) {
        say("can't write %s (errno %d)", path, errno);
        return false;
    }
    say("made a key pair, public key %s", cfg.public_key);
    return true;
}

// The LAN address devices at home dial: the one the PS5 would send from.
// Nothing is sent.
static void lan_address(char out[INET_ADDRSTRLEN]) {
    snprintf(out, INET_ADDRSTRLEN, "PS5_IP");
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(0x01010101);  // 1.1.1.1
    socklen_t len = sizeof a;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(fd, (struct sockaddr *)&a, &len) == 0 &&
        a.sin_addr.s_addr != 0)
        inet_ntop(AF_INET, &a.sin_addr, out, INET_ADDRSTRLEN);
    close(fd);
}

static bool address_taken(uint32_t be) {
    uint32_t mine;
    if (parse_ip(cfg.address, &mine) && mine == be) return true;
    for (int i = 0; i < cfg.npeers; i++) {
        uint32_t a;
        if (parse_ip(cfg.peers[i].address, &a) && a == be) return true;
    }
    return false;
}

// NAME.conf: the device's side, for the WireGuard app (import the file, or
// type it in). Endpoint is the LAN address unless server.conf names one;
// away from home it has to be the home's public address or name.
static bool write_device_conf(const struct peer *p, const char *device_private_key) {
    char endpoint[160];
    if (cfg.endpoint[0]) {
        if (strchr(cfg.endpoint, ':')) snprintf(endpoint, sizeof endpoint, "%s", cfg.endpoint);
        else snprintf(endpoint, sizeof endpoint, "%s:%d", cfg.endpoint, cfg.port);
    } else {
        char lan[INET_ADDRSTRLEN];
        lan_address(lan);
        snprintf(endpoint, sizeof endpoint, "%s:%d", lan, cfg.port);
    }
    char text[1024], name[64];
    snprintf(text, sizeof text,
             "# %s, for the WireGuard app. Made by WireStation %s.\n"
             "# Away from home, Endpoint must be your home's public address (or a\n"
             "# name for it) and your router must forward UDP %d to the PS5.\n"
             "[Interface]\n"
             "PrivateKey = %s\n"
             "Address = %s/32\n"
             "\n"
             "[Peer]\n"
             "PublicKey = %s\n"
             "Endpoint = %s\n"
             "AllowedIPs = %s/32\n"
             "PersistentKeepalive = 25\n",
             p->name, WS_VERSION, cfg.port, device_private_key, p->address, cfg.public_key, endpoint, cfg.address);
    snprintf(name, sizeof name, "%s.conf", p->name);
    return write_file(name, text, 0600);
}

static bool valid_name(const char *s) {
    if (!*s || strlen(s) >= sizeof(((struct peer *)0)->name)) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_'))
            return false;
    return true;
}

// peers.conf. With none (no file, or no lines), one device, "device1", is
// made, so the first start leaves something to connect with.
static bool load_peers(void) {
    char path[300];
    path_of(path, sizeof path, "peers.conf");
    FILE *f = fopen(path, "r");
    char pending[MAX_PEERS][64];
    int npending = 0;
    bool rewrite = false;
    cfg.npeers = 0;
    if (f) {
        char line[256];
        int no = 0;
        while (fgets(line, sizeof line, f)) {
            no++;
            char *s = trim(line);
            if (!*s || *s == '#') continue;
            char name[64] = "", key[128] = "", addr[64] = "";
            int n = sscanf(s, "%63s %127s %63s", name, key, addr);
            if (!valid_name(name)) {
                say("peers.conf line %d: \"%s\" isn't a name (letters, digits, - and _, up to 31)", no, name);
                continue;
            }
            if (cfg.npeers + npending >= MAX_PEERS) {
                say("peers.conf line %d: more than %d devices; the rest are left out", no, MAX_PEERS);
                break;
            }
            if (n == 1) {
                snprintf(pending[npending++], 64, "%s", name);
                rewrite = true;
                continue;
            }
            uint8_t k[32];
            uint32_t be;
            if (n != 3 || !unb64(key, k) || !parse_ip(addr, &be)) {
                say("peers.conf line %d: want \"name public-key address\"", no);
                continue;
            }
            struct peer *p = &cfg.peers[cfg.npeers++];
            memset(p, 0, sizeof *p);
            snprintf(p->name, sizeof p->name, "%s", name);
            snprintf(p->public_key, sizeof p->public_key, "%s", key);
            snprintf(p->address, sizeof p->address, "%s", addr);
        }
        fclose(f);
    }
    if (cfg.npeers == 0 && npending == 0) {
        snprintf(pending[npending++], 64, "device1");
        rewrite = true;
    }
    // New devices: keys, the next free address in the /24, and NAME.conf.
    uint32_t base;
    parse_ip(cfg.address, &base);
    base = ntohl(base) & 0xFFFFFF00u;
    for (int i = 0; i < npending; i++) {
        uint32_t be = 0;
        if (!valid_name(pending[i])) continue;
        for (uint32_t host = 2; host < 255; host++) {
            if (!address_taken(htonl(base | host))) {
                be = htonl(base | host);
                break;
            }
        }
        char priv[KEY_B64];
        struct peer *p = &cfg.peers[cfg.npeers];
        memset(p, 0, sizeof *p);
        snprintf(p->name, sizeof p->name, "%s", pending[i]);
        struct in_addr a = {.s_addr = be};
        inet_ntop(AF_INET, &a, p->address, sizeof p->address);
        if (!be || !make_keys(priv, p->public_key)) {
            say("no room or no keys for %s", p->name);
            continue;
        }
        if (!write_device_conf(p, priv)) {
            say("can't write %s/%s.conf (errno %d)", dir, p->name, errno);
            crypto_zero(priv, sizeof priv);
            continue;
        }
        crypto_zero(priv, sizeof priv);
        cfg.npeers++;
        say("added %s at %s; its config for the WireGuard app is %s/%s.conf", p->name, p->address, dir, p->name);
    }
    if (rewrite) {
        char text[MAX_PEERS * 160 + 256];
        size_t at = (size_t)snprintf(text, sizeof text,
                                     "# WireStation's devices: name, public key, address in the tunnel.\n"
                                     "# Add a line with just a name and restart the server: it makes the\n"
                                     "# keys and writes NAME.conf for the WireGuard app next to this file.\n");
        for (int i = 0; i < cfg.npeers && at < sizeof text; i++)
            at += (size_t)snprintf(text + at, sizeof text - at, "%s %s %s\n", cfg.peers[i].name, cfg.peers[i].public_key,
                                   cfg.peers[i].address);
        if (!write_file("peers.conf", text, 0600)) say("can't write %s (errno %d)", path, errno);
    }
    return cfg.npeers > 0;
}

/* ---- sockets ---- */

// Non-blocking both ways: on the PS5, O_NONBLOCK through fcntl didn't take
// on a socket (TokenStation5, 2026-10-25), so FIONBIO too, and every call
// passes MSG_DONTWAIT.
static void nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    ioctl(fd, FIONBIO, &one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int bind_socket(int type, int port) {
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) return -1;
    int one = 1;
    if (type == SOCK_STREAM) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

// How the UDP port was bound, for the status page: the answer to "can a
// payload's UDP socket take this port".
static char bound_as[96] = "not bound";

static int open_udp(int port) {
    int fd = bind_socket(SOCK_DGRAM, port);
    if (fd >= 0) {
        snprintf(bound_as, sizeof bound_as, "bound as a payload");
        return fd;
    }
    int e = errno;
    say("UDP port %d: bind failed (errno %d%s)", port, e, e == EACCES ? ", refused" : e == EADDRINUSE ? ", in use" : "");
#ifdef WS_PS5
    if (e == EACCES) {
        // Titles get EACCES on some ports (51820 on TokenStation5). Try again
        // as the console's own services, as ts5-memprobe does, then put the
        // process's credentials back: the socket keeps its port.
        pid_t me = getpid();
        uint64_t authid = kernel_get_ucred_authid(me);
        uint8_t caps[16], all[16];
        memset(all, 0xff, sizeof all);
        if (authid && kernel_get_ucred_caps(me, caps) == 0 && kernel_set_ucred_authid(me, 0x4800000000010003l) == 0 &&
            kernel_set_ucred_caps(me, all) == 0) {
            fd = bind_socket(SOCK_DGRAM, port);
            e = errno;
            kernel_set_ucred_caps(me, caps);
            kernel_set_ucred_authid(me, authid);
            if (fd >= 0) {
                snprintf(bound_as, sizeof bound_as, "bound with system privileges (refused as a payload)");
                say("UDP port %d: bound with system privileges", port);
                return fd;
            }
            say("UDP port %d: bind failed with system privileges too (errno %d)", port, e);
        } else {
            say("UDP port %d: couldn't raise privileges", port);
        }
    }
#endif
    snprintf(bound_as, sizeof bound_as, "bind failed (errno %d)", e);
    errno = e;
    return -1;
}

/* ---- the tunnel ---- */

static struct netif nif;
static struct wireguardif_init_data wg_init;
static int udp_fd = -1, status_fd = -1;
static volatile sig_atomic_t quit;
static volatile double loop_at;  // when the loop last went round, for the watchdog
static unsigned long received, connects;
static double first_packet_s = -1;
static char first_from[64];

static void ip_text(uint32_t be, char out[INET_ADDRSTRLEN]) {
    struct in_addr a = {.s_addr = be};
    inet_ntop(AF_INET, &a, out, INET_ADDRSTRLEN);
}

static void add_peers(void) {
    for (int i = 0; i < cfg.npeers; i++) {
        struct peer *p = &cfg.peers[i];
        uint32_t ip;
        parse_ip(p->address, &ip);
        struct wireguardif_peer wp;
        wireguardif_peer_init(&wp);
        wp.public_key = p->public_key;
        wp.preshared_key = NULL;
        ip_addr_set_ip4_u32_val(wp.allowed_ip, ip);
        ip_addr_set_ip4_u32_val(wp.allowed_mask, 0xFFFFFFFFu);
        uint8_t index = WIREGUARDIF_INVALID_INDEX;
        if (wireguardif_add_peer(&nif, &wp, &index) != ERR_OK || index == WIREGUARDIF_INVALID_INDEX) {
            say("can't add %s (a bad key?)", p->name);
            continue;
        }
        p->index = index;
        p->added = true;
    }
}

// The status, as text: the LAN status page and http://10.66.0.1/ in the
// tunnel both show it.
static size_t status_text(char *out, size_t n) {
    const double up = mono_s() - started_s;
    size_t at = 0;
#define ADD(...) \
    if (at < n) at += (size_t)snprintf(out + at, n - at, __VA_ARGS__)
    ADD("WireStation server %s\n", WS_VERSION);
    ADD("up %dh %02dm %02ds\n", (int)up / 3600, (int)up / 60 % 60, (int)up % 60);
    ADD("UDP port %d: %s\n", cfg.port, bound_as);
    ADD("tunnel address %s, public key %s\n", cfg.address, cfg.public_key);
    if (first_packet_s >= 0) {
        ADD("first packet from %s, %.0f s after the start\n", first_from, first_packet_s);
    } else {
        ADD("no packets yet\n");
    }
    ADD("packets in %lu, out %lu, sends failed %lu (last errno %d)\n", received, ws_sent, ws_send_failed, ws_send_errno);
    ADD("status page visits through the tunnel: %lu\n", connects);
    ADD("devices:\n");
    const struct wireguard_device *dev = (const struct wireguard_device *)nif.state;
    const uint32_t now_ms = wireguard_sys_now();
    for (int i = 0; i < cfg.npeers; i++) {
        const struct peer *p = &cfg.peers[i];
        ip_addr_t from;
        u16_t port = 0;
        const bool on = p->added && wireguardif_peer_is_up(&nif, p->index, &from, &port) == ERR_OK;
        char where[64] = "";
        if (on && port) {
            char ip[INET_ADDRSTRLEN];
            ip_text(ip4_addr_get_u32(ip_2_ip4(&from)), ip);
            snprintf(where, sizeof where, ", from %s:%u", ip, port);
        }
        char seen[48] = ", never seen";
        if (p->added && dev && dev->peers[p->index].last_rx)
            snprintf(seen, sizeof seen, ", last heard %.0f s ago", (double)(uint32_t)(now_ms - dev->peers[p->index].last_rx) / 1000.0);
        ADD("  %-16s %-15s %s%s%s\n", p->name, p->address, !p->added ? "not added" : on ? "connected" : "not connected",
            where, seen);
    }
    ADD("files: %s\n", dir);
#undef ADD
    return at < n ? at : n - 1;
}

// http://10.66.0.1/ inside the tunnel: the status, then the connection
// closes. It waits for the request first, as lwIP resets a connection
// closed with data unread.
static err_t page_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (!p || err != ERR_OK) {
        if (p) pbuf_free(p);
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        return ERR_OK;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (arg) return ERR_OK;  // answered already
    static char body[4096], head[160];
    size_t n = status_text(body, sizeof body);
    snprintf(head, sizeof head,
             "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", n);
    tcp_arg(pcb, pcb);  // answered
    if (tcp_write(pcb, head, (u16_t)strlen(head), TCP_WRITE_FLAG_COPY) != ERR_OK ||
        tcp_write(pcb, body, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    tcp_output(pcb);
    // What the device sends after this is dropped; the reply goes before the FIN.
    tcp_recv(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

// A connection that sends nothing for 10 s is dropped.
static err_t page_poll(void *arg, struct tcp_pcb *pcb) {
    (void)arg;
    tcp_abort(pcb);
    return ERR_ABRT;
}

static err_t page_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;
    connects++;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, page_recv);
    tcp_poll(pcb, page_poll, 20);
    return ERR_OK;
}

/* ---- the status page on the LAN ---- */

static void serve_status(void) {
    for (int k = 0; k < 8; k++) {
        int c = accept(status_fd, NULL, NULL);
        if (c < 0) return;
        nonblocking(c);
        // The request line, if it has come: GET /stop stops the server.
        char req[512] = "";
        ssize_t got = -1;
        for (int tries = 0; tries < 50 && got < 0; tries++) {
            got = recv(c, req, sizeof req - 1, MSG_DONTWAIT);
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) usleep(2000);
            else break;
        }
        if (got > 0) req[got] = 0;
        else req[0] = 0;
        const bool stop = strncmp(req, "GET /stop", 9) == 0;
        static char body[4096];
        size_t n = stop ? (size_t)snprintf(body, sizeof body, "WireStation server: stopping\n") : status_text(body, sizeof body);
        char head[160];
        int h = snprintf(head, sizeof head,
                         "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", n);
        send(c, head, (size_t)h, MSG_DONTWAIT | MSG_NOSIGNAL);
        send(c, body, n, MSG_DONTWAIT | MSG_NOSIGNAL);
        close(c);
        if (stop) {
            say("asked to stop on the status page");
            quit = 1;
        }
    }
}

/* ---- the watchdog ---- */

// Says in the log when the tunnel's loop stops going round, and a line a
// minute while it does: gaps in those lines show when the PS5 held the
// process (a game starting, rest mode).
static void *watchdog(void *arg) {
    (void)arg;
    double last_line = mono_s(), stalled_since = 0;
    while (!quit) {
        sleep(1);
        const double t = mono_s(), since = t - loop_at;
        if (since > 10 && !stalled_since) {
            stalled_since = t;
            say("the tunnel's loop hasn't gone round for %.0f s", since);
        } else if (since <= 10 && stalled_since) {
            say("the tunnel's loop is going again, after %.0f s", t - stalled_since);
            stalled_since = 0;
        }
        if (t - last_line >= 60) {
            last_line = t;
            int up = 0;
            for (int i = 0; i < cfg.npeers; i++) up += cfg.peers[i].was_up;
            say("alive: packets in %lu, out %lu, %d of %d devices connected", received, ws_sent, up, cfg.npeers);
        }
    }
    return NULL;
}

static void on_signal(int sig) {
    (void)sig;
    quit = 1;
}

/* ---- main ---- */

int main(int argc, char **argv) {
    started_s = mono_s();
    loop_at = started_s;
    const char *d = getenv("WS_DIR");
    if (argc > 1) d = argv[1];
    snprintf(dir, sizeof dir, "%s", d && *d ? d : WS_DEFAULT_DIR);
    mkdir(dir, 0755);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    char path[300], old[310];
    path_of(path, sizeof path, "log.txt");
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > (1 << 20)) {
        snprintf(old, sizeof old, "%s/log.old.txt", dir);
        rename(path, old);
    }
    log_file = fopen(path, "a");
    say("WireStation server %s starting, files in %s", WS_VERSION, dir);
    read_server_conf();

    // One copy at a time: the status port is the lock.
    status_fd = bind_socket(SOCK_STREAM, cfg.status_port);
    if (status_fd < 0 || listen(status_fd, 8) != 0) {
        if (errno == EADDRINUSE) notify("already running (port %d is taken)", cfg.status_port);
        else notify("can't open the status port %d (errno %d)", cfg.status_port, errno);
        return 1;
    }
    nonblocking(status_fd);

    if (!load_key() || !load_peers()) {
        notify("couldn't start: see %s/log.txt", dir);
        return 1;
    }
    uint32_t ip = 0;
    if (!parse_ip(cfg.address, &ip)) {
        notify("server.conf: address %s isn't an IPv4 address", cfg.address);
        return 1;
    }

    udp_fd = open_udp(cfg.port);
    if (udp_fd < 0) {
        notify("can't use UDP port %d: %s", cfg.port, bound_as);
        return 1;
    }
    nonblocking(udp_fd);
    ws_udp_use(udp_fd);

    lwip_init();
    ip4_addr_t addr, mask, gw;
    ip4_addr_set_u32(&addr, ip);
    ip4_addr_set_u32(&mask, htonl(0xFFFFFF00u));
    ip4_addr_set_u32(&gw, 0);
    wg_init.private_key = cfg.private_key;
    wg_init.listen_port = (u16_t)cfg.port;
    wg_init.bind_netif = NULL;
    if (!netif_add(&nif, &addr, &mask, &gw, &wg_init, wireguardif_init, ip_input)) {
        notify("WireGuard didn't start");
        return 1;
    }
    netif_set_default(&nif);
    netif_set_up(&nif);
    netif_set_link_up(&nif);
    add_peers();
    struct tcp_pcb *pcb = tcp_new(), *listener = NULL;
    if (!pcb || tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK || !(listener = tcp_listen(pcb))) {
        notify("no status page in the tunnel");
        return 1;
    }
    tcp_accept(listener, page_accept);

    say("on: UDP port %d (%s), %s in the tunnel, public key %s, %d device%s, status page on TCP %d", cfg.port, bound_as,
        cfg.address, cfg.public_key, cfg.npeers, cfg.npeers == 1 ? "" : "s", cfg.status_port);
    notify("on, UDP port %d", cfg.port);

    pthread_t wd;
    if (pthread_create(&wd, NULL, watchdog, NULL) != 0) say("no watchdog thread (errno %d)", errno);

    double last_states = 0;
    while (!quit) {
        loop_at = mono_s();
        u32_t sleep_ms = sys_timeouts_sleeptime();
        if (sleep_ms > 20) sleep_ms = 20;
        struct pollfd fds[2] = {{udp_fd, POLLIN, 0}, {status_fd, POLLIN, 0}};
        // On the PS5 poll doesn't always wake for a socket that's ready
        // (TokenStation5), so the sockets are read whatever it says.
        if (poll(fds, 2, (int)sleep_ms) < 0 && errno != EINTR) usleep(sleep_ms * 1000);

        unsigned char buf[2048];
        for (int k = 0; k < 256; k++) {
            struct sockaddr_in from;
            socklen_t len = sizeof from;
            const ssize_t n = recvfrom(udp_fd, buf, sizeof buf, MSG_DONTWAIT, (struct sockaddr *)&from, &len);
            if (n <= 0) break;
            ws_udp_input(buf, (size_t)n, from.sin_addr.s_addr, ntohs(from.sin_port));
            if (++received == 1) {
                char ip_s[INET_ADDRSTRLEN];
                ip_text(from.sin_addr.s_addr, ip_s);
                snprintf(first_from, sizeof first_from, "%s:%u", ip_s, ntohs(from.sin_port));
                first_packet_s = mono_s() - started_s;
                say("the first packet came in, from %s", first_from);
            }
        }
        serve_status();
        sys_check_timeouts();

        const double t = mono_s();
        if (t - last_states >= 0.5) {
            last_states = t;
            for (int i = 0; i < cfg.npeers; i++) {
                struct peer *p = &cfg.peers[i];
                if (!p->added) continue;
                ip_addr_t from;
                u16_t port = 0;
                const bool on = wireguardif_peer_is_up(&nif, p->index, &from, &port) == ERR_OK;
                if (on != p->was_up) {
                    char ip_s[INET_ADDRSTRLEN] = "";
                    if (on) ip_text(ip4_addr_get_u32(ip_2_ip4(&from)), ip_s);
                    if (on) say("%s connected, from %s:%u", p->name, ip_s, port);
                    else say("%s's session ended", p->name);
                    if (on) notify("%s connected", p->name);
                }
                p->was_up = on;
            }
        }
    }
    say("stopping");
    close(udp_fd);
    close(status_fd);
    notify("off");
    return 0;
}
