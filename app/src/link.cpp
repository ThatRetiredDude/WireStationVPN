#include "link.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ws {

namespace {

// A connection to 127.0.0.1:port with send and receive timeouts; -1 with err.
int dial(int port, int timeout_s, std::string &err) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        err = std::string("socket: ") + strerror(errno);
        return -1;
    }
    timeval tv{timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr *)&a, sizeof a) != 0) {
        err = "nothing on port " + std::to_string(port) + " (" + strerror(errno) + ")";
        close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        const ssize_t w = send(fd, p, n, 0);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

}  // namespace

bool http(const char *method, const std::string &path, std::string &body, std::string &err, int port) {
    body.clear();
    const int fd = dial(port, 3, err);
    if (fd < 0) return false;
    const std::string req = std::string(method) + " " + path +
                            " HTTP/1.0\r\nHost: 127.0.0.1\r\nUser-Agent: TailStation-app\r\nContent-Length: 0\r\n\r\n";
    std::string resp;
    if (send_all(fd, req.data(), req.size())) {
        char buf[4096];
        for (;;) {
            const ssize_t r = recv(fd, buf, sizeof buf, 0);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            resp.append(buf, (size_t)r);
            if (resp.size() > (1u << 20)) break;
        }
    }
    close(fd);
    const size_t head = resp.find("\r\n\r\n");
    if (resp.compare(0, 5, "HTTP/") != 0 || head == std::string::npos) {
        err = "no answer on port " + std::to_string(port);
        return false;
    }
    const size_t sp = resp.find(' ');
    const int code = sp == std::string::npos ? 0 : atoi(resp.c_str() + sp + 1);
    body = resp.substr(head + 4);
    if (code != 200) {
        err = "answer " + std::to_string(code);
        return false;
    }
    return true;
}

bool send_elf(const std::string &file, std::string &err, int port) {
    FILE *f = fopen(file.c_str(), "rb");
    if (!f) {
        err = "can't open " + file + ": " + strerror(errno);
        return false;
    }
    std::vector<char> data;
    char buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
    fclose(f);
    if (data.size() < 64 || memcmp(data.data(), "\x7f" "ELF", 4) != 0) {
        err = file + " isn't an ELF file";
        return false;
    }
    const int fd = dial(port, 10, err);
    if (fd < 0) {
        err = "the ELF loader isn't running: " + err;
        return false;
    }
    const bool ok = send_all(fd, data.data(), data.size());
    close(fd);
    if (!ok) err = "sending to the ELF loader failed";
    return ok;
}

}  // namespace ws
