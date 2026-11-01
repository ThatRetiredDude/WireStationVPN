#include "base.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#ifdef WS_PS5
extern "C" int sysctl(const int *name, unsigned namelen, void *old, size_t *oldlen, const void *new_, size_t newlen);
#endif

// The Mozilla CA bundle (ca_bundle.c, made at build time): the PS5 has none
// a payload can read.
extern "C" const char ws_ca_bundle[];
extern "C" const size_t ws_ca_bundle_len;

namespace ws {

namespace {

std::mutex log_mu;
FILE *log_file = nullptr;
double log_start = 0;

std::mutex dns_mu;
uint32_t dns_server = 0;  // network order

}  // namespace

double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t unix_now() { return (int64_t)time(nullptr); }

// UTC from seconds since 1970 (days to a civil date, after H. Hinnant):
// the PS5's libc may not have gmtime_r for payloads.
static void utc(int64_t t, struct tm &out) {
    memset(&out, 0, sizeof out);
    int64_t days = t / 86400, secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    out.tm_hour = (int)(secs / 3600);
    out.tm_min = (int)(secs % 3600 / 60);
    out.tm_sec = (int)(secs % 60);
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = yoe + era * 400 + (m <= 2);
    out.tm_year = (int)(y - 1900);
    out.tm_mon = (int)(m - 1);
    out.tm_mday = (int)d;
}

void log_open(const std::string &path) {
    std::lock_guard<std::mutex> lk(log_mu);
    FILE *f = fopen(path.c_str(), "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fclose(f);
        if (size > (1 << 20)) {
            const std::string old = path.substr(0, path.rfind('/') + 1) + "log.old.txt";
            rename(path.c_str(), old.c_str());
        }
    }
    log_file = fopen(path.c_str(), "a");
    log_start = now_s();
}

void log_line(const char *fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    // Local time isn't known to a payload; the Go build's lines are in the
    // PS5's time zone, these in UTC, marked so.
    struct tm t;
    utc(unix_now(), t);
    char line[2200];
    snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02dZ +%.0fs %s\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, now_s() - log_start, msg);
    std::lock_guard<std::mutex> lk(log_mu);
    fputs("tailstation: ", stdout);
    fputs(line, stdout);
    fflush(stdout);
    if (log_file) {
        fputs(line, log_file);
        fflush(log_file);
    }
}

bool random_bytes(unsigned char *out, size_t n) {
#ifdef WS_PS5
    static const int name[2] = {1, 37};  // CTL_KERN, KERN_ARND
    while (n > 0) {
        size_t k = n < 256 ? n : 256;  // the most one call gives
        if (sysctl(name, 2, out, &k, nullptr, 0) != 0 || k == 0) return false;
        out += k;
        n -= k;
    }
    return true;
#else
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (n > 0) {
        const ssize_t k = read(fd, out, n);
        if (k <= 0) {
            if (k < 0 && errno == EINTR) continue;
            close(fd);
            return false;
        }
        out += k;
        n -= (size_t)k;
    }
    close(fd);
    return true;
#endif
}

void nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    ioctl(fd, FIONBIO, &one);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

void set_dns(const std::string &server) {
    in_addr a{};
    if (inet_pton(AF_INET, server.c_str(), &a) != 1) return;
    std::lock_guard<std::mutex> lk(dns_mu);
    dns_server = a.s_addr;
}

// One A query to the DNS server, over UDP.
static bool dns_query(const std::string &host, std::vector<uint32_t> &addrs, std::string &err) {
    uint32_t server;
    {
        std::lock_guard<std::mutex> lk(dns_mu);
        server = dns_server;
    }
    if (!server) inet_pton(AF_INET, "1.1.1.1", &server);
    unsigned char id[2];
    random_bytes(id, 2);
    std::string q;
    q += (char)id[0];
    q += (char)id[1];
    q += std::string("\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10);  // recursion, 1 question
    size_t at = 0;
    while (at <= host.size()) {
        size_t dot = host.find('.', at);
        if (dot == std::string::npos) dot = host.size();
        const size_t len = dot - at;
        if (len == 0 || len > 63) {
            if (dot == host.size() && len == 0) break;  // a trailing dot
            err = "the name " + host + " isn't one";
            return false;
        }
        q += (char)len;
        q += host.substr(at, len);
        at = dot + 1;
    }
    q += std::string("\x00\x00\x01\x00\x01", 5);  // end, type A, class IN
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        err = "no socket for DNS";
        return false;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = server;
    unsigned char buf[1500];
    ssize_t n = -1;
    for (int attempt = 0; attempt < 3 && n < 0; attempt++) {
        sendto(fd, q.data(), q.size(), 0, (const sockaddr *)&to, sizeof to);
        pollfd p{fd, POLLIN, 0};
        const double until = now_s() + 2;
        while (now_s() < until) {
            poll(&p, 1, 100);
            n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
            if (n >= 12 && buf[0] == id[0] && buf[1] == id[1]) break;
            n = -1;
        }
    }
    close(fd);
    if (n < 12) {
        err = "no answer from the DNS server for " + host;
        return false;
    }
    if ((buf[3] & 0x0F) != 0) {
        err = (buf[3] & 0x0F) == 3 ? "no such name: " + host : "the DNS server failed to look up " + host;
        return false;
    }
    const int qd = (buf[4] << 8) | buf[5], an = (buf[6] << 8) | buf[7];
    size_t i = 12;
    auto skip_name = [&]() {
        while (i < (size_t)n) {
            const unsigned char l = buf[i];
            if (l == 0) {
                i++;
                return true;
            }
            if ((l & 0xC0) == 0xC0) {
                i += 2;
                return true;
            }
            i += 1 + l;
        }
        return false;
    };
    for (int k = 0; k < qd; k++) {
        if (!skip_name()) return false;
        i += 4;
    }
    for (int k = 0; k < an && i < (size_t)n; k++) {
        if (!skip_name() || i + 10 > (size_t)n) break;
        const int type = (buf[i] << 8) | buf[i + 1];
        const int rdlen = (buf[i + 8] << 8) | buf[i + 9];
        i += 10;
        if (i + (size_t)rdlen > (size_t)n) break;
        if (type == 1 && rdlen == 4) {
            uint32_t a;
            memcpy(&a, buf + i, 4);
            addrs.push_back(a);
        }
        i += (size_t)rdlen;
    }
    if (addrs.empty()) err = "no IPv4 address for " + host;
    return !addrs.empty();
}

bool resolve(const std::string &host, std::vector<uint32_t> &addrs, std::string &err) {
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) == 1) {
        addrs.push_back(a.s_addr);
        return true;
    }
    if (host == "localhost") {
        addrs.push_back(htonl(INADDR_LOOPBACK));
        return true;
    }
    return dns_query(host, addrs, err);
}

int dial_tcp(uint32_t addr, uint16_t port, int timeout_s, std::string &err) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = "no socket (errno " + std::to_string(errno) + ")";
        return -1;
    }
    nonblocking(fd);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = addr;
    if (connect(fd, (const sockaddr *)&to, sizeof to) != 0 && errno != EINPROGRESS) {
        err = strerror(errno);
        close(fd);
        return -1;
    }
    // Whether it connected: on the PS5 poll may not say, so ask the socket.
    const double until = now_s() + timeout_s;
    while (true) {
        pollfd p{fd, POLLOUT, 0};
        poll(&p, 1, 50);
        int e = 0;
        socklen_t len = sizeof e;
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &len);
        if (e != 0) {
            err = strerror(e);
            close(fd);
            return -1;
        }
        sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        if (getpeername(fd, (sockaddr *)&peer, &plen) == 0) break;
        if (now_s() > until) {
            err = "timed out";
            close(fd);
            return -1;
        }
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

}  // namespace ws

// Mbed TLS's hooks (ws_mbedtls_config.h): randomness from the system, and
// UTC dates without the C library's gmtime_r.
extern "C" int mbedtls_hardware_poll(void *, unsigned char *out, size_t len, size_t *olen) {
    if (!ws::random_bytes(out, len)) return -1;
    *olen = len;
    return 0;
}

extern "C" struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *out) {
    ws::utc((int64_t)*tt, *out);
    return out;
}

namespace ws {

namespace {

struct Tls {
    std::mutex mu;
    bool ready = false;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;

    bool setup(std::string &err) {
        std::lock_guard<std::mutex> lk(mu);
        if (ready) return true;
        if (psa_crypto_init() != PSA_SUCCESS) {
            err = "the TLS library didn't start";
            return false;
        }
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_x509_crt_init(&ca);
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"tailstation", 11) != 0) {
            err = "no random numbers for TLS";
            return false;
        }
        mbedtls_x509_crt_parse(&ca, (const unsigned char *)ws_ca_bundle, ws_ca_bundle_len + 1);
        ready = true;
        return true;
    }
};

Tls &tls() {
    static Tls t;
    return t;
}

int send_cb(void *ctx, const unsigned char *buf, size_t len) {
    const int fd = *(int *)ctx;
    const ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

int recv_cb(void *ctx, unsigned char *buf, size_t len) {
    const int fd = *(int *)ctx;
    const ssize_t n = ::recv(fd, buf, len, MSG_DONTWAIT);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

std::string tls_error(int rc) {
    char b[160];
    mbedtls_strerror(rc, b, sizeof b);
    return b;
}

}  // namespace

struct Stream::Impl {
    int fd = -1;
    bool tls = false;
    int timeout_s = 15;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    ~Impl() {
        if (tls) {
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
        }
        if (fd >= 0) ::close(fd);
    }
};

Stream::Stream() : p_(nullptr) {}
Stream::~Stream() { close(); }

void Stream::close() {
    delete p_;
    p_ = nullptr;
}

bool Stream::is_open() const { return p_ != nullptr; }

bool Stream::open(const std::string &host, int port, bool use_tls, int timeout_s, std::string &err, bool tls12,
                  bool insecure) {
    close();
    std::vector<uint32_t> addrs;
    if (!resolve(host, addrs, err)) return false;
    int fd = -1;
    for (uint32_t a : addrs) {
        fd = dial_tcp(a, (uint16_t)port, timeout_s, err);
        if (fd >= 0) break;
    }
    if (fd < 0) {
        err = host + ":" + std::to_string(port) + ": " + err;
        return false;
    }
    Impl *p = new Impl;
    p->fd = fd;
    p->timeout_s = timeout_s;
    if (use_tls) {
        if (!tls().setup(err)) {
            delete p;
            return false;
        }
        p->tls = true;
        mbedtls_ssl_init(&p->ssl);
        mbedtls_ssl_config_init(&p->conf);
        int rc = mbedtls_ssl_config_defaults(&p->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                             MBEDTLS_SSL_PRESET_DEFAULT);
        if (rc == 0) {
            mbedtls_ssl_conf_rng(&p->conf, mbedtls_ctr_drbg_random, &tls().drbg);
            mbedtls_ssl_conf_ca_chain(&p->conf, &tls().ca, nullptr);
            mbedtls_ssl_conf_authmode(&p->conf, insecure ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);
            if (tls12) mbedtls_ssl_conf_max_tls_version(&p->conf, MBEDTLS_SSL_VERSION_TLS1_2);
            rc = mbedtls_ssl_setup(&p->ssl, &p->conf);
        }
        if (rc == 0) rc = mbedtls_ssl_set_hostname(&p->ssl, host.c_str());
        if (rc != 0) {
            err = "TLS: " + tls_error(rc);
            delete p;
            return false;
        }
        mbedtls_ssl_set_bio(&p->ssl, &p->fd, send_cb, recv_cb, nullptr);
        const double until = now_s() + timeout_s;
        while ((rc = mbedtls_ssl_handshake(&p->ssl)) != 0) {
            if ((rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) || now_s() > until) {
                err = host + ": TLS: " + (now_s() > until ? std::string("timed out") : tls_error(rc));
                delete p;
                return false;
            }
            pollfd pf{p->fd, (short)(rc == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT), 0};
            poll(&pf, 1, 20);
        }
    }
    p_ = p;
    return true;
}

bool Stream::write(const void *data, size_t len, std::string &err) {
    if (!p_) {
        err = "not connected";
        return false;
    }
    const unsigned char *b = (const unsigned char *)data;
    const double until = now_s() + p_->timeout_s;
    while (len > 0) {
        int n;
        if (p_->tls) {
            n = mbedtls_ssl_write(&p_->ssl, b, len);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) n = -2;
            else if (n < 0) {
                err = "TLS: " + tls_error(n);
                return false;
            }
        } else {
            const ssize_t k = ::send(p_->fd, b, len, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) n = -2;
            else if (k < 0) {
                err = strerror(errno);
                return false;
            } else {
                n = (int)k;
            }
        }
        if (n == -2) {
            if (now_s() > until) {
                err = "a write timed out";
                return false;
            }
            pollfd pf{p_->fd, POLLOUT, 0};
            poll(&pf, 1, 20);
            continue;
        }
        b += n;
        len -= (size_t)n;
    }
    return true;
}

int Stream::read(void *buf, size_t cap, int wait_ms, std::string &err) {
    if (!p_) {
        err = "not connected";
        return -1;
    }
    const double until = now_s() + wait_ms / 1000.0;
    while (true) {
        if (p_->tls) {
            const int n = mbedtls_ssl_read(&p_->ssl, (unsigned char *)buf, cap);
            if (n > 0) return n;
            if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            if (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE &&
                n != MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                err = "TLS: " + tls_error(n);
                return -1;
            }
            if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
        } else {
            const ssize_t n = ::recv(p_->fd, buf, cap, MSG_DONTWAIT);
            if (n > 0) return (int)n;
            if (n == 0) return 0;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                err = strerror(errno);
                return -1;
            }
        }
        const double left = until - now_s();
        if (left <= 0) return -2;
        pollfd pf{p_->fd, POLLIN, 0};
        // The PS5's poll may not wake for a ready socket: short waits.
        poll(&pf, 1, left * 1000 < 20 ? (int)(left * 1000) + 1 : 20);
    }
}

bool http_get(const std::string &url, HttpResponse &resp, std::string &err, size_t max_bytes, int timeout_s) {
    bool use_tls;
    std::string rest;
    if (url.rfind("https://", 0) == 0) {
        use_tls = true;
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        use_tls = false;
        rest = url.substr(7);
    } else {
        err = "not a web address: " + url;
        return false;
    }
    const size_t slash = rest.find('/');
    std::string hostport = rest.substr(0, slash), path = slash == std::string::npos ? "/" : rest.substr(slash);
    int port = use_tls ? 443 : 80;
    std::string host = hostport;
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = atoi(hostport.c_str() + colon + 1);
    }
    Stream s;
    if (!s.open(host, port, use_tls, timeout_s, err)) return false;
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + hostport +
                            "\r\nUser-Agent: TailStation\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";
    if (!s.write(req.data(), req.size(), err)) return false;
    std::string in;
    const double until = now_s() + timeout_s;
    while (in.size() < max_bytes + 65536) {
        char buf[16384];
        const int n = s.read(buf, sizeof buf, 200, err);
        if (n == 0) break;
        if (n == -1) return false;
        if (n == -2) {
            if (now_s() > until) {
                err = "timed out";
                return false;
            }
            // A whole answer with a length: done without waiting for close.
            const size_t he = in.find("\r\n\r\n");
            if (he != std::string::npos) {
                std::string head = in.substr(0, he);
                for (char &c : head) c = (char)tolower((unsigned char)c);
                const size_t cl = head.find("\r\ncontent-length:");
                if (cl != std::string::npos && in.size() - he - 4 >= (size_t)atol(head.c_str() + cl + 17)) break;
            }
            continue;
        }
        in.append(buf, (size_t)n);
    }
    const size_t he = in.find("\r\n\r\n");
    if (in.compare(0, 5, "HTTP/") != 0 || he == std::string::npos) {
        err = "not an HTTP answer";
        return false;
    }
    resp.status = atoi(in.c_str() + in.find(' ') + 1);
    std::string head = in.substr(0, he);
    for (char &c : head) c = (char)tolower((unsigned char)c);
    std::string body = in.substr(he + 4);
    if (head.find("transfer-encoding: chunked") != std::string::npos) {
        std::string out;
        size_t i = 0;
        while (i < body.size()) {
            const size_t eol = body.find("\r\n", i);
            if (eol == std::string::npos) break;
            const size_t len = strtoul(body.substr(i, eol - i).c_str(), nullptr, 16);
            if (len == 0) break;
            out += body.substr(eol + 2, len);
            i = eol + 2 + len + 2;
        }
        body = out;
    }
    if (body.size() > max_bytes) body.resize(max_bytes);
    resp.body = body;
    return true;
}

}  // namespace ws
