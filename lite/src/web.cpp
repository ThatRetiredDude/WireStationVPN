// The C++ build's web server: a small HTTP/1.1 server, a thread per
// connection, one request each.
//
//   status_port, all addresses   the status page and /api/status for the
//                                app, /stop, /logout (home network only)
//   127.0.0.1, a port of its own what the tunnel passes on from the PS5's
//                                tailnet address: port 80 (the page and the
//                                shared folders) and the PeerAPI (Taildrop)
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <thread>

#include "base.h"
#include "node.h"
#include "qrcodegen.hpp"

namespace ws {

namespace {

struct Request {
    int fd = -1;
    std::string method, raw_path, path, query;
    std::map<std::string, std::string> headers;  // names in lower case
    std::string body;  // what came with the head; the rest is read by read_body
    uint32_t from = 0;     // network order
    uint16_t from_port = 0;
    bool tailnet = false;  // came through the tunnel
    Caller caller;
    std::string header(const std::string &k) const {
        auto it = headers.find(k);
        return it == headers.end() ? "" : it->second;
    }
    std::string param(const std::string &k) const;
};

std::string url_decode(const std::string &s, bool plus) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (plus && s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string url_encode_path(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') out += (char)c;
        else {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        }
    }
    return out;
}

std::string html(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string Request::param(const std::string &k) const {
    std::string all = query;
    if (method == "POST" && header("content-type").find("application/x-www-form-urlencoded") == 0)
        all += (all.empty() ? "" : "&") + body;
    size_t i = 0;
    while (i <= all.size()) {
        size_t amp = all.find('&', i);
        if (amp == std::string::npos) amp = all.size();
        const std::string kv = all.substr(i, amp - i);
        const size_t eq = kv.find('=');
        if (url_decode(kv.substr(0, eq), true) == k) return eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1), true);
        i = amp + 1;
    }
    return "";
}

bool send_all(int fd, const char *p, size_t n) {
    const double until = now_s() + 30;
    while (n > 0) {
        const ssize_t k = ::send(fd, p, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            continue;
        }
        if (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
        if (now_s() > until) return false;
        pollfd pf{fd, POLLOUT, 0};
        poll(&pf, 1, 20);
    }
    return true;
}

const char *reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 303: return "See Other";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 421: return "Misdirected Request";
        case 500: return "Internal Server Error";
        case 507: return "Insufficient Storage";
        default: return "Status";
    }
}

void respond(const Request &r, int status, const std::string &type, const std::string &body,
             const std::string &extra = "") {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\nContent-Type: " + type +
                       "\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
                       "Referrer-Policy: no-referrer\r\n";
    if (type.find("text/html") == 0)
        head += "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; img-src 'self' data:; "
                "form-action 'self'; frame-ancestors 'none'; base-uri 'none'\r\n";
    head += extra + "\r\n";
    send_all(r.fd, head.data(), head.size());
    if (r.method != "HEAD") send_all(r.fd, body.data(), body.size());
}

void text(const Request &r, int status, const std::string &msg) { respond(r, status, "text/plain; charset=utf-8", msg + "\n"); }

void redirect(const Request &r, const std::string &to) {
    respond(r, 303, "text/plain; charset=utf-8", "", "Location: " + to + "\r\n");
}

// Reads the head of a request: false if there's none to take.
bool read_request(Request &r) {
    std::string in;
    const double until = now_s() + 10;
    size_t he;
    while ((he = in.find("\r\n\r\n")) == std::string::npos) {
        if (in.size() > 16384 || now_s() > until) return false;
        char buf[4096];
        const ssize_t n = recv(r.fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
            in.append(buf, (size_t)n);
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return false;
        pollfd pf{r.fd, POLLIN, 0};
        poll(&pf, 1, 20);
    }
    r.body = in.substr(he + 4);
    std::string head = in.substr(0, he);
    size_t eol = head.find("\r\n");
    const std::string first = head.substr(0, eol);
    const size_t s1 = first.find(' '), s2 = first.rfind(' ');
    if (s1 == std::string::npos || s2 == s1) return false;
    r.method = first.substr(0, s1);
    r.raw_path = first.substr(s1 + 1, s2 - s1 - 1);
    const size_t q = r.raw_path.find('?');
    r.path = url_decode(r.raw_path.substr(0, q), false);
    if (q != std::string::npos) r.query = r.raw_path.substr(q + 1);
    while (eol != std::string::npos) {
        const size_t next = head.find("\r\n", eol + 2);
        const std::string line = head.substr(eol + 2, next == std::string::npos ? std::string::npos : next - eol - 2);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon), v = line.substr(colon + 1);
            for (char &c : k) c = (char)tolower((unsigned char)c);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            r.headers[k] = v;
        }
        eol = next;
    }
    return true;
}

// The body, to sink as it comes: Content-Length or chunked. False when it
// broke off, or sink said stop.
template <class Sink>
bool read_body(Request &r, Sink sink) {
    std::string pending = std::move(r.body);
    r.body.clear();
    auto more = [&](std::string &into) {
        const double until = now_s() + 60;
        while (true) {
            char buf[65536];
            const ssize_t n = recv(r.fd, buf, sizeof buf, MSG_DONTWAIT);
            if (n > 0) {
                into.append(buf, (size_t)n);
                return true;
            }
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return false;
            if (now_s() > until) return false;
            pollfd pf{r.fd, POLLIN, 0};
            poll(&pf, 1, 20);
        }
    };
    if (r.header("transfer-encoding").find("chunked") != std::string::npos) {
        while (true) {
            size_t eol;
            while ((eol = pending.find("\r\n")) == std::string::npos)
                if (!more(pending)) return false;
            const unsigned long len = strtoul(pending.substr(0, eol).c_str(), nullptr, 16);
            pending.erase(0, eol + 2);
            if (len == 0) return true;
            while (pending.size() < len + 2)
                if (!more(pending)) return false;
            if (!sink(pending.data(), len)) return false;
            pending.erase(0, len + 2);
        }
    }
    const std::string cl = r.header("content-length");
    unsigned long long left = cl.empty() ? 0 : strtoull(cl.c_str(), nullptr, 10);
    while (left > 0) {
        if (pending.empty() && !more(pending)) return false;
        const size_t k = pending.size() < left ? pending.size() : (size_t)left;
        if (!sink(pending.data(), k)) return false;
        pending.erase(0, k);
        left -= k;
    }
    return true;
}

// A form small enough to keep (the status page's buttons).
void read_form(Request &r) {
    std::string body;
    read_body(r, [&](const char *p, size_t n) {
        body.append(p, n);
        return body.size() < 65536;
    });
    r.body = body;
}

// --- who may ask ---------------------------------------------------------

bool private_or_tailnet(uint32_t be) {
    const uint32_t h = ntohl(be);
    return (h >> 24) == 127 || (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8 || (h >> 16) == 0xA9FE ||
           (h & 0xFFC00000u) == 0x64400000u;
}

// The request names the PS5 itself (an address, localhost or its tailnet
// name), not another site's name pointed at it (DNS rebinding).
bool own_host(const std::string &host_header, const std::string &name) {
    std::string h = host_header;
    if (!h.empty() && h[0] == '[') return true;  // an IPv6 address
    const size_t colon = h.rfind(':');
    if (colon != std::string::npos) h = h.substr(0, colon);
    for (char &c : h) c = (char)tolower((unsigned char)c);
    while (!h.empty() && h.back() == '.') h.pop_back();
    in_addr a{};
    if (h.empty() || inet_pton(AF_INET, h.c_str(), &a) == 1 || h == "localhost") return true;
    std::string n = name;
    for (char &c : n) c = (char)tolower((unsigned char)c);
    if (h == n) return true;
    if (h.rfind(n + ".", 0) != 0) return false;
    const std::string rest = h.substr(n.size() + 1);
    return rest == "ts.net" || (rest.size() > 7 && rest.compare(rest.size() - 7, 7, ".ts.net") == 0);
}

// A button press on the page itself, not another site making the browser
// send it.
bool same_site(const Request &r) {
    std::string from = r.header("origin");
    if (from.empty()) from = r.header("referer");
    if (from.empty()) return true;
    const size_t s = from.find("://");
    if (s == std::string::npos) return false;
    std::string host = from.substr(s + 3);
    host = host.substr(0, host.find('/'));
    return host == r.header("host");
}

// --- the status -----------------------------------------------------------

struct PeerRow {
    std::string name, ip, os, seen, path;
    bool online = false, exit = false;
};

struct View {
    std::string backend, login, error, name, ips, exit = "off";
    std::vector<std::pair<std::string, std::string>> routes;  // route, approved/waiting
    std::vector<PeerRow> peers;
    std::vector<std::string> received;
    int udp_port = 0;
    std::string uptime;
};

std::string ago(double s) {
    if (s < 60) return "just now";
    int n;
    std::string unit;
    if (s < 3600) n = (int)(s / 60), unit = "minute";
    else if (s < 48 * 3600) n = (int)(s / 3600), unit = "hour";
    else n = (int)(s / 86400), unit = "day";
    return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
}

// Seconds since 1970 from RFC 3339 (UTC or with an offset); 0 if it isn't.
int64_t parse_time(const std::string &t) {
    int y, mo, d, h, mi, s;
    if (sscanf(t.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    // Days from civil (H. Hinnant).
    y -= mo <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t secs = (era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + s;
    const size_t tz = t.find_first_of("+-", 19);
    if (tz != std::string::npos && t.size() >= tz + 6) {
        const int oh = atoi(t.substr(tz + 1, 2).c_str()), om = atoi(t.substr(tz + 4, 2).c_str());
        secs -= (t[tz] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    }
    return secs;
}

std::string uptime(double s) {
    const long t = (long)s;
    char b[64];
    if (t >= 3600) snprintf(b, sizeof b, "%ldh%ldm%lds", t / 3600, t % 3600 / 60, t % 60);
    else if (t >= 60) snprintf(b, sizeof b, "%ldm%lds", t / 60, t % 60);
    else snprintf(b, sizeof b, "%lds", t);
    return b;
}

View collect(Node &n) {
    View v;
    const ts::Status s = n.client.status();
    v.uptime = uptime(now_s() - n.started);
    if (n.cfg.server) {
        v.backend = "Stopped";
        v.error = "Run my own tailnet needs the full build, which didn't run on this PS5. Turn it off to use a "
                  "Tailscale account.";
        return v;
    }
    static const char *names[] = {"Stopped", "Starting", "NeedsLogin", "Starting", "Running", "Starting"};
    v.backend = names[s.state];
    if (s.state == ts::Status::NeedsSignIn) v.login = s.sign_in_url;
    v.error = s.problem;
    v.name = s.name;
    v.ips = s.address;
    v.udp_port = n.cfg.udp_port();
    const ts::Netmap m = n.client.netmap();
    auto approved = [&](const std::string &r) { return std::find(m.allowed.begin(), m.allowed.end(), r) != m.allowed.end(); };
    for (const std::string &r : n.routes) {
        if (r == "::/0") continue;
        if (r == "0.0.0.0/0") v.exit = approved(r) ? "ready" : "waiting";
        else v.routes.push_back({r, approved(r) ? "approved" : "waiting"});
    }
    const auto tunnel = n.tunnel.peers();
    const int64_t now = unix_now();
    for (const ts::Peer &p : m.peers) {
        PeerRow row;
        row.name = p.name;
        row.ip = p.address;
        row.online = p.online;
        row.os = p.os;
        row.exit = p.exit_node;
        if (!p.online && !p.last_seen.empty()) {
            const int64_t t = parse_time(p.last_seen);
            if (t > 0 && t < now) row.seen = ago((double)(now - t));
        }
        auto it = tunnel.find(p.address);
        if (p.online && it != tunnel.end() && it->second.up) row.path = it->second.direct ? "direct" : "relay";
        v.peers.push_back(row);
    }
    std::sort(v.peers.begin(), v.peers.end(), [](const PeerRow &a, const PeerRow &b) {
        if (a.online != b.online) return a.online;
        return a.name < b.name;
    });
    std::lock_guard<std::mutex> lk(n.mu);
    v.received = n.received;
    return v;
}

std::string tsv(Node &n, const View &v) {
    std::string out;
    auto clean = [](std::string s) {
        for (char &c : s)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    };
    auto line = [&](const std::string &k, const std::vector<std::string> &vals) {
        out += k;
        out += '\t';
        for (size_t i = 0; i < vals.size(); i++) {
            if (i) out += '\t';
            out += clean(vals[i]);
        }
        out += '\n';
    };
    line("version", {n.version + " (C++ build)"});
    line("uptime", {v.uptime});
    line("hostname", {n.cfg.hostname});
    line("backend", {v.backend});
    line("build", {"lite"});
    if (!v.login.empty()) line("login", {v.login});
    if (!v.error.empty()) line("error", {v.error});
    if (!v.name.empty()) line("name", {v.name});
    if (!v.ips.empty()) line("ips", {v.ips});
    line("exit", {v.exit});
    for (const auto &[r, st] : v.routes) line("route", {r, st});
    if (v.udp_port) line("udp_port", {std::to_string(v.udp_port)});
    line("tailnet_page", {n.cfg.tailnet_page ? "on" : "off"});
    line("taildrop", {n.cfg.taildrop ? "on" : "off"});
    for (const PeerRow &p : v.peers) line("peer", {p.name, p.ip, p.online ? "online" : "offline", p.os, p.seen, p.path});
    for (const std::string &f : v.received) line("received", {f});
    return out;
}

// A QR code as an SVG picture, for an <img>.
std::string qr_svg(const std::string &text) {
    using qrcodegen::QrCode;
    const QrCode q = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
    const int size = q.getSize(), border = 2;
    std::string path;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            if (q.getModule(x, y)) path += "M" + std::to_string(x + border) + "," + std::to_string(y + border) + "h1v1h-1z";
    const std::string dim = std::to_string(size + 2 * border);
    const std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 " + dim + " " + dim +
                            "' shape-rendering='crispEdges'><rect width='100%' height='100%' fill='#fff'/><path d='" + path +
                            "' fill='#000'/></svg>";
    std::string uri = "data:image/svg+xml,";
    for (unsigned char c : svg) {
        if (isalnum(c) || strchr(" ,./:=-'_", c)) uri += c == ' ' ? std::string("%20") : std::string(1, (char)c);
        else {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            uri += b;
        }
    }
    return uri;
}

const char *kStyle = R"(<style>
:root { color-scheme: dark; --bg: #08090b; --fg: #f2f2f2; --dim: #9a9a9e; --accent: #e4eaf3; --line: #2c2f36; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 16px/1.5 system-ui, sans-serif; }
main { max-width: 640px; margin: 0 auto; padding: 24px 16px 48px; }
h1 { color: var(--accent); font-size: 28px; margin: 0 0 4px; letter-spacing: .5px; }
h2 { font-size: 18px; margin: 28px 0 8px; border-bottom: 1px solid var(--line); padding-bottom: 6px; }
.dim { color: var(--dim); }
.box { border: 1px solid var(--accent); border-radius: 10px; padding: 16px; margin: 16px 0; }
.qr { background: #fff; padding: 8px; border-radius: 8px; width: 256px; max-width: 100%; display: block; margin: 12px 0; }
a { color: var(--accent); word-break: break-all; }
table { width: 100%; border-collapse: collapse; }
td { padding: 6px 4px; border-bottom: 1px solid var(--line); vertical-align: top; }
.on { color: #4cd964; } .off { color: var(--dim); }
button { background: none; color: var(--dim); border: 1px solid var(--line); border-radius: 6px; padding: 6px 12px; font: inherit; cursor: pointer; }
</style>)";

std::string page(Node &n, const View &v) {
    std::string h = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                    "<meta http-equiv=\"refresh\" content=\"10\"><title>TailStation</title>";
    h += kStyle;
    h += "</head><body><main><h1>TailStation</h1><div class=\"dim\">" + html(n.version) +
         " (C++ build) · up " + v.uptime + " · Tailscale: " + html(v.backend) + "</div>";
    if (!v.error.empty()) h += "<div class=\"box\">" + html(v.error) + "</div>";
    if (!v.login.empty()) {
        h += "<div class=\"box\"><strong>Sign in to add this PS5 to your Tailscale network.</strong>"
             "<p>Scan the code with your phone, or open the link, and sign in with your Tailscale account.</p>"
             "<img class=\"qr\" src=\"" + html(qr_svg(v.login)) + "\" alt=\"QR code for the sign-in link\">"
             "<a href=\"" + html(v.login) + "\">" + html(v.login) + "</a></div>";
    } else if (v.backend == "Running") {
        h += "<h2>This PS5</h2><table><tr><td class=\"dim\">Name</td><td>" + html(v.name) + "</td></tr>"
             "<tr><td class=\"dim\">Tailscale address</td><td>" + html(v.ips) + "</td></tr>"
             "<tr><td class=\"dim\">Exit node</td><td>" +
             (v.exit == "ready" ? std::string("<span class=\"on\">ready</span>")
                                : v.exit == "waiting" ? std::string("offered, waiting for approval") : std::string("off")) +
             "</td></tr>";
        bool waiting = v.exit == "waiting";
        if (!v.routes.empty()) {
            h += "<tr><td class=\"dim\">Home network</td><td>";
            for (const auto &[r, st] : v.routes) {
                h += html(r) + (st == "waiting" ? " (waiting)" : "") + " ";
                waiting |= st == "waiting";
            }
            h += "</td></tr>";
        }
        h += "</table>";
        if (waiting)
            h += "<div class=\"box\"><strong>One step left:</strong> approve this PS5 in the "
                 "<a href=\"https://login.tailscale.com/admin/machines\">Tailscale admin page</a>. Find <em>" + html(v.name) +
                 "</em>, open its menu (⋯), choose <em>Edit route settings</em>, and tick <em>Use as exit node</em>" +
                 (v.routes.empty() ? "" : " and the home network route") + ".</div>";
        h += "<h2>Devices</h2>";
        if (v.peers.empty()) {
            h += "<p class=\"dim\">No other devices yet. Install the Tailscale app on your phone or computer and sign in "
                 "with the same account.</p>";
        } else {
            h += "<table>";
            for (const PeerRow &p : v.peers) {
                h += "<tr><td>" + html(p.name) + (p.exit ? " <span class=\"dim\">(exit node)</span>" : "") +
                     "</td><td class=\"dim\">" + html(p.ip) + "</td><td>";
                if (p.online) h += "<span class=\"on\">online</span>" + (p.path.empty() ? "" : " <span class=\"dim\">· " + p.path + "</span>");
                else h += "<span class=\"off\">offline" + (p.seen.empty() ? "" : ", " + html(p.seen)) + "</span>";
                h += "</td></tr>";
            }
            h += "</table>";
        }
        if (n.cfg.tailnet_page || n.cfg.taildrop) {
            const std::string host = html(n.cfg.hostname);
            h += "<h2>On your tailnet</h2>";
            if (n.cfg.tailnet_page)
                h += "<p>From any of your devices, anywhere: <a href=\"http://" + host + "/\">http://" + host +
                     "/</a> (this page) and <a href=\"http://" + host + "/files/\">shared folders</a> to browse and download.</p>";
            if (n.cfg.taildrop) {
                h += "<p>Send files to the PS5 with the Tailscale app: <em>Share</em> → <em>Tailscale</em> → <em>" + host + "</em>.</p>";
                if (!v.received.empty()) {
                    h += "<p class=\"dim\">Received: ";
                    for (const std::string &f : v.received) h += html(f) + " · ";
                    h += "</p>";
                }
            }
        }
        h += "<h2>Using it</h2><p>In the Tailscale app on your phone or computer, choose <em>Exit node</em> and pick <em>" +
             html(n.cfg.hostname) + "</em>. Your browsing then goes out through your home internet.</p>";
        h += "<p class=\"dim\">This is TailStation's C++ build, used because the full build didn't run on this PS5. "
             "It has everything but running your own tailnet.</p>";
        h += "<p class=\"dim\">The full guide: <a href=\"https://github.com/ThatRetiredDude/TailStation/blob/main/docs/GUIDE.md\">"
             "TailStation guide</a> (also on the app's Guide page).</p>";
        h += "<form method=\"post\" action=\"/logout\" style=\"display:inline\"><button>Sign this PS5 out</button></form> "
             "<a href=\"/stop\"><button>Stop TailStation</button></a>";
    } else {
        h += "<p>Starting Tailscale…</p>";
    }
    h += "</main></body></html>\n";
    return h;
}

// --- shared folders ------------------------------------------------------

struct Root {
    std::string name, path;
};

std::vector<Root> roots(const Node &n) {
    std::vector<Root> out;
    for (std::string f : n.cfg.shared) {
        while (f.size() > 1 && f.back() == '/') f.pop_back();
        struct stat st;
        if (stat(f.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        std::string name = f;
        while (!name.empty() && name[0] == '/') name.erase(0, 1);
        std::replace(name.begin(), name.end(), '/', '-');
        out.push_back({name.empty() ? "root" : name, f});
    }
    return out;
}

std::string real(const std::string &p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

// Never shown or served: the PS5's keys and settings.
bool hidden(const Node &n, const std::string &p) {
    for (const char *name : {"tailscale", "tailscale-own-server", "headscale", "settings.conf", "tailscale-lite.keys"}) {
        const std::string h = n.dir + "/" + name;
        for (const std::string &x : {h, real(h)})
            for (const std::string &q : {p, real(p)})
                if (q == x || q.rfind(x + "/", 0) == 0) return true;
    }
    return false;
}

std::string size_text(long long b) {
    char s[32];
    if (b < 1024) snprintf(s, sizeof s, "%lld B", b);
    else if (b < 1024 * 1024) snprintf(s, sizeof s, "%.1f KB", b / 1024.0);
    else if (b < 1024LL * 1024 * 1024) snprintf(s, sizeof s, "%.1f MB", b / 1048576.0);
    else snprintf(s, sizeof s, "%.2f GB", b / 1073741824.0);
    return s;
}

void files(Node &n, Request &r) {
    if (r.method != "GET" && r.method != "HEAD") return text(r, 405, "read-only");
    // A clean path under /files/.
    std::vector<std::string> parts;
    std::string rest = r.path.substr(6);  // after "/files"
    size_t i = 0;
    while (i <= rest.size()) {
        size_t s = rest.find('/', i);
        if (s == std::string::npos) s = rest.size();
        const std::string part = rest.substr(i, s - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        i = s + 1;
    }
    const std::vector<Root> rs = roots(n);
    auto listing = [&](const std::string &title, const std::string &up,
                       const std::vector<std::pair<std::string, std::string>> &entries) {
        std::string h = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>" + html(title) + "</title>";
        h += kStyle;
        h += "</head><body><main><h1>" + html(title) + "</h1>";
        if (!up.empty()) h += "<p><a href=\"" + html(up) + "\">↑ Up</a></p>";
        h += "<table>";
        for (const auto &[href, label] : entries) h += "<tr><td><a href=\"" + html(href) + "\">" + label + "</a></td></tr>";
        if (entries.empty()) h += "<tr><td class=\"dim\">Empty.</td></tr>";
        h += "</table></main></body></html>\n";
        respond(r, 200, "text/html; charset=utf-8", h);
    };
    if (parts.empty()) {
        std::vector<std::pair<std::string, std::string>> e;
        for (const Root &rt : rs) e.push_back({"/files/" + url_encode_path(rt.name) + "/", html(rt.path)});
        return listing("Shared folders", "/", e);
    }
    const Root *base = nullptr;
    for (const Root &rt : rs)
        if (rt.name == parts[0]) base = &rt;
    if (!base) return text(r, 404, "not found");
    std::string full = base->path, rel;
    for (size_t k = 1; k < parts.size(); k++) {
        full += "/" + parts[k];
        rel += "/" + parts[k];
    }
    if (hidden(n, full)) return text(r, 404, "not found");
    struct stat st;
    if (stat(full.c_str(), &st) != 0) return text(r, 404, "not found");
    const std::string self = "/files/" + url_encode_path(base->name + rel);
    if (S_ISDIR(st.st_mode)) {
        if (r.path.back() != '/') return redirect(r, self + "/");
        std::vector<std::pair<std::string, std::string>> e;
        std::vector<std::pair<std::string, bool>> names;
        if (DIR *d = opendir(full.c_str())) {
            while (dirent *de = readdir(d)) {
                const std::string name = de->d_name;
                if (name == "." || name == ".." || hidden(n, full + "/" + name)) continue;
                struct stat es;
                const bool dir = stat((full + "/" + name).c_str(), &es) == 0 && S_ISDIR(es.st_mode);
                names.push_back({name, dir});
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end(), [](const auto &a, const auto &b) {
            if (a.second != b.second) return a.second;
            return a.first < b.first;
        });
        for (const auto &[name, dir] : names) {
            struct stat es;
            stat((full + "/" + name).c_str(), &es);
            e.push_back({self + "/" + url_encode_path(name) + (dir ? "/" : ""),
                         html(name) + (dir ? "/" : " <span class=\"dim\">" + size_text(es.st_size) + "</span>")});
        }
        const std::string up = parts.size() == 1 ? "/files/" : "/files/" + url_encode_path(base->name + rel.substr(0, rel.rfind('/'))) + "/";
        return listing(base->name + rel, up, e);
    }
    // A file: whole, or the range asked for (video players seek).
    const int fd = open(full.c_str(), O_RDONLY);
    if (fd < 0) return text(r, 404, "not found");
    long long from = 0, to = st.st_size - 1;
    int status = 200;
    const std::string range = r.header("range");
    if (range.rfind("bytes=", 0) == 0 && range.find(',') == std::string::npos) {
        const std::string spec = range.substr(6);
        const size_t dash = spec.find('-');
        if (dash != std::string::npos) {
            if (dash == 0) from = st.st_size - atoll(spec.c_str() + 1);
            else {
                from = atoll(spec.c_str());
                if (dash + 1 < spec.size()) to = atoll(spec.c_str() + dash + 1);
            }
            if (from < 0) from = 0;
            if (to >= st.st_size) to = st.st_size - 1;
            if (from > to) {
                close(fd);
                return respond(r, 416, "text/plain", "", "Content-Range: bytes */" + std::to_string(st.st_size) + "\r\n");
            }
            status = 206;
        }
    }
    const long long len = st.st_size == 0 ? 0 : to - from + 1;
    std::string name = parts.back();
    for (char &c : name)
        if (c == '"' || c == '\\' || (unsigned char)c < 0x20) c = '_';
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) +
                       "\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(len) +
                       "\r\nAccept-Ranges: bytes\r\nContent-Disposition: inline; filename=\"" + name +
                       "\"\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n";
    if (status == 206)
        head += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" + std::to_string(st.st_size) + "\r\n";
    head += "\r\n";
    send_all(r.fd, head.data(), head.size());
    if (r.method == "GET") {
        lseek(fd, from, SEEK_SET);
        long long left = len;
        static thread_local char buf[1 << 16];
        while (left > 0) {
            const ssize_t k = read(fd, buf, left < (long long)sizeof buf ? (size_t)left : sizeof buf);
            if (k <= 0 || !send_all(r.fd, buf, (size_t)k)) break;
            left -= k;
        }
    }
    close(fd);
}

// --- Taildrop ------------------------------------------------------------

bool good_name(const std::string &s) {
    if (s.empty() || s.size() > 255 || s == "." || s == "..") return false;
    for (unsigned char c : s)
        if (c < 0x20 || c == '/' || c == '\\' || c == 0x7F) return false;
    return s[0] != '.' || s.size() > 2;
}

// PeerAPI's Taildrop: PUT /v0/put/NAME with the file; GET /v0/put/ lists
// parts to go on from (none: a file always starts over here).
void taildrop(Node &n, Request &r) {
    if (r.raw_path.rfind("/v0/put/", 0) != 0) return text(r, 404, "not found");
    // Only the PS5's owner's own devices, as Tailscale's apps allow.
    const ts::Netmap m = n.client.netmap();
    const ts::Peer *from = nullptr;
    for (const ts::Peer &p : m.peers)
        if (p.address == r.caller.address) from = &p;
    if (!from || from->tagged || m.tagged || from->user != m.user)
        return text(r, 403, "Taildrop disabled; only the PS5 owner's own devices may send it files");
    std::string raw = r.raw_path.substr(8);
    raw = raw.substr(0, raw.find('?'));
    const std::string name = url_decode(raw, false);
    if (r.method == "GET") return respond(r, 200, "application/json", raw.empty() ? "[]\n" : "");
    if (r.method != "PUT") return text(r, 405, "expected method GET or PUT");
    if (!good_name(name)) return text(r, 400, "invalid filename");
    if (!r.header("range").empty()) return text(r, 400, "invalid Range header");
    const std::string dir = n.cfg.taildrop_dir;
    mkdir(dir.c_str(), 0755);
    const std::string part = dir + "/.incoming-" + std::to_string(getpid()) + "-" + std::to_string(r.from_port);
    FILE *f = fopen(part.c_str(), "wb");
    if (!f) return text(r, 500, "can't write to " + dir);
    bool full = false;
    const bool ok = read_body(r, [&](const char *p, size_t k) {
        if (fwrite(p, 1, k, f) != k) {
            full = true;
            return false;
        }
        return true;
    });
    const bool closed = fclose(f) == 0;
    if (!ok || !closed) {
        unlink(part.c_str());
        return text(r, full ? 507 : 400, full ? "storage full" : "the file didn't arrive whole");
    }
    // Its name, or "name (2).ext" if that's taken.
    std::string dest = dir + "/" + name, shown = name;
    for (int k = 2; access(dest.c_str(), F_OK) == 0 && k < 1000; k++) {
        const size_t dot = name.rfind('.');
        shown = dot == std::string::npos || dot == 0 ? name + " (" + std::to_string(k) + ")"
                                                     : name.substr(0, dot) + " (" + std::to_string(k) + ")" + name.substr(dot);
        dest = dir + "/" + shown;
    }
    if (rename(part.c_str(), dest.c_str()) != 0) {
        unlink(part.c_str());
        return text(r, 500, "can't keep the file");
    }
    log_line("taildrop: received %s from %s", shown.c_str(), from->name.c_str());
    {
        std::lock_guard<std::mutex> lk(n.mu);
        n.received.insert(n.received.begin(), shown);
        if (n.received.size() > 10) n.received.resize(10);
    }
    respond(r, 200, "application/json", "{}\n");
}

// --- the requests ----------------------------------------------------------

void handle(Node &n, Request &r) {
    if (r.tailnet && r.caller.port == kPeerAPIPort) return taildrop(n, r);
    if (!r.tailnet && !private_or_tailnet(r.from)) return text(r, 403, "this page is for the home network");
    if (!own_host(r.header("host"), n.cfg.hostname)) return text(r, 421, "open this page by the PS5's address");
    if (r.tailnet && (r.path == "/files" || r.path.rfind("/files/", 0) == 0)) return files(n, r);
    if (r.path == "/stop") {
        if (r.method != "POST") {
            return respond(r, 200, "text/html; charset=utf-8",
                           std::string("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Stop</title>") + kStyle +
                               "</head><body><main><h1>Stop TailStation?</h1><form method=\"post\" action=\"/stop\">"
                               "<button>Stop it</button></form></main></body></html>\n");
        }
        read_form(r);
        if (!same_site(r)) return text(r, 403, "use the button on the status page");
        text(r, 200, "TailStation: stopping");
        log_line("asked to stop on the status page");
        n.quit = true;
        return;
    }
    if (r.path == "/logout") {
        read_form(r);
        if (r.method != "POST" || !same_site(r)) return text(r, 405, "use the button on the status page");
        n.client.sign_out();
        log_line("signed out on the status page");
        return redirect(r, "/");
    }
    if (r.path.rfind("/server/", 0) == 0) return text(r, 404, "your own tailnet needs the full build");
    if (r.path != "/" && r.path != "/api/status") return text(r, 404, "not found");
    const View v = collect(n);
    if (r.path == "/api/status") return respond(r, 200, "text/tab-separated-values; charset=utf-8", tsv(n, v));
    respond(r, 200, "text/html; charset=utf-8", page(n, v));
}

void serve(Node &n, int ln, bool tailnet) {
    while (!n.quit) {
        sockaddr_in from{};
        socklen_t len = sizeof from;
        const int fd = accept(ln, (sockaddr *)&from, &len);
        if (fd < 0) {
            usleep(10000);
            continue;
        }
        nonblocking(fd);
        std::thread([&n, fd, from, tailnet] {
            Request r;
            r.fd = fd;
            r.from = from.sin_addr.s_addr;
            r.from_port = ntohs(from.sin_port);
            r.tailnet = tailnet;
            bool ok = read_request(r);
            // Through the tunnel: who it is. Not one of the tunnel's: refused.
            if (ok && tailnet && !n.tunnel.caller(r.from_port, r.caller)) ok = false;
            if (ok) handle(n, r);
            shutdown(fd, SHUT_WR);
            // Let the answer go out before the close resets anything unread.
            char drain[4096];
            for (int k = 0; k < 200; k++) {
                const ssize_t got = recv(fd, drain, sizeof drain, MSG_DONTWAIT);
                if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
                if (got < 0) usleep(10000);
            }
            close(fd);
        }).detach();
    }
}

}  // namespace

uint16_t serve_web(Node &n, int status_listener) {
    const int ln = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (ln < 0 || bind(ln, (sockaddr *)&a, sizeof a) != 0 || listen(ln, 128) != 0 ||
        getsockname(ln, (sockaddr *)&a, &len) != 0)
        return 0;
    std::thread([&n, status_listener] { serve(n, status_listener, false); }).detach();
    std::thread([&n, ln] { serve(n, ln, true); }).detach();
    return ntohs(a.sin_port);
}

}  // namespace ws
