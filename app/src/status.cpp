#include "status.h"

#include <cstdlib>
#include <sstream>

namespace ws {

namespace {

std::vector<std::string> split(const std::string &line, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t at = line.find(sep, start);
        out.push_back(line.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) return out;
        start = at + 1;
    }
}

std::string trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string field(const std::vector<std::string> &f, size_t i) { return i < f.size() ? f[i] : ""; }

}  // namespace

int Status::online() const {
    int n = 0;
    for (const Peer &p : peers) n += p.online;
    return n;
}

Status parse_status(const std::string &tsv) {
    Status s;
    s.reachable = true;
    std::istringstream in(tsv);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = split(line, '\t');
        const std::string &k = f[0], v = field(f, 1);
        if (k == "version") s.version = v;
        else if (k == "uptime") s.uptime = v;
        else if (k == "hostname") s.hostname = v;
        else if (k == "backend") s.backend = v;
        else if (k == "login") s.login = v;
        else if (k == "error") s.error = v;
        else if (k == "name") s.name = v;
        else if (k == "ips") s.ips = v;
        else if (k == "exit") s.exit = v;
        else if (k == "tailnet_page") s.tailnet_page = v == "on";
        else if (k == "taildrop") s.taildrop = v == "on";
        else if (k == "received") s.received.push_back(v);
        else if (k == "server") s.server = v;
        else if (k == "joining") s.joining.push_back({v, field(f, 2)});
        else if (k == "https") s.https = v, s.https_until = field(f, 2);
        else if (k == "blocked") s.blocked = atoi(v.c_str());
        else if (k == "route") s.routes.push_back({v, field(f, 2) == "approved"});
        else if (k == "peer") {
            Peer p;
            p.name = v.substr(0, v.find('.'));
            p.ip = field(f, 2);
            p.online = field(f, 3) == "online";
            p.os = field(f, 4);
            p.seen = field(f, 5);
            p.path = field(f, 6);
            s.peers.push_back(p);
        }
    }
    return s;
}

std::string read_setting(const std::string &text, const std::string &key, const std::string &def) {
    std::istringstream in(text);
    std::string line, value = def;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq != std::string::npos && trim(t.substr(0, eq)) == key) value = trim(t.substr(eq + 1));
    }
    return value;
}

std::string write_settings(const std::string &text, const std::vector<std::pair<std::string, std::string>> &set) {
    std::vector<bool> done(set.size());
    std::istringstream in(text);
    std::string line, out;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        const size_t eq = t.find('=');
        bool replaced = false;
        if (!t.empty() && t[0] != '#' && eq != std::string::npos) {
            const std::string k = trim(t.substr(0, eq));
            for (size_t i = 0; i < set.size(); i++)
                if (set[i].first == k) {
                    // The last line for a key wins (settings.go), so every
                    // line for it gets the new value.
                    out += k + " = " + set[i].second + "\n";
                    done[i] = replaced = true;
                    break;
                }
        }
        if (!replaced) out += line + "\n";
    }
    for (size_t i = 0; i < set.size(); i++)
        if (!done[i]) out += set[i].first + " = " + set[i].second + "\n";
    return out;
}

}  // namespace ws
