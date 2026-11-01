// What the rest of the C++ build stands on: the log, random numbers, time,
// names to addresses, and connections (plain or TLS).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ws {

// The log: log.txt in the data folder, lines as the Go build writes them.
void log_open(const std::string &path);
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// n random bytes from the system; false if it has none.
bool random_bytes(unsigned char *out, size_t n);

// Seconds on a clock that only goes forward; and since 1970.
double now_s();
int64_t unix_now();

// The DNS server to ask (from settings.conf); names are looked up by this
// program itself, as the PS5 gives payloads no resolver to count on.
void set_dns(const std::string &server);
// IPv4 addresses (network order) for host, which may be an address already.
bool resolve(const std::string &host, std::vector<uint32_t> &addrs, std::string &err);

// Non-blocking, in a way that takes on the PS5 (FIONBIO as well as O_NONBLOCK).
void nonblocking(int fd);
// A TCP connection to addr:port (network order), within timeout_s; -1 and err.
int dial_tcp(uint32_t addr, uint16_t port, int timeout_s, std::string &err);

struct HttpResponse {
    int status = 0;
    std::string body;
};

// GET url (http:// or https://), at most max_bytes of its body.
bool http_get(const std::string &url, HttpResponse &resp, std::string &err, size_t max_bytes = 1 << 20,
              int timeout_s = 30);

// A connection held open both ways, plain or TLS (Tailscale's control server
// and relays). One thread at a time.
class Stream {
public:
    Stream();
    ~Stream();
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;
    // tls12: at most TLS 1.2. insecure: no certificate check (relays marked
    // InsecureForTests, in tests).
    bool open(const std::string &host, int port, bool tls, int timeout_s, std::string &err, bool tls12 = false,
              bool insecure = false);
    bool write(const void *data, size_t len, std::string &err);
    // Up to cap bytes, waiting at most wait_ms: how many; 0 once closed; -1
    // on an error; -2 when nothing came in time.
    int read(void *buf, size_t cap, int wait_ms, std::string &err);
    void close();
    bool is_open() const;

private:
    struct Impl;
    Impl *p_;
};

}  // namespace ws
