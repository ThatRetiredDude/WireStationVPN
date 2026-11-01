// What the background part (node/, the payload) says about itself on
// http://127.0.0.1:9039/api/status, and the settings file both share.
#pragma once
#include <string>
#include <vector>

namespace ws {

struct Peer {
    std::string name, ip, os, seen;  // name: the device's own, without the tailnet's domain
    bool online = false;
};

struct Joining {
    std::string id, name;  // a device asking to join the PS5's own tailnet
};

struct Route {
    std::string prefix;
    bool approved = false;
};

struct Status {
    bool reachable = false;  // false: the background part isn't answering
    std::string version, uptime, hostname, backend, login, error, name, ips;
    std::string exit = "off";  // off, waiting (offered, not approved yet), ready
    std::vector<Route> routes;
    bool tailnet_page = false, taildrop = false;
    std::vector<Peer> peers;
    std::vector<std::string> received;
    std::string server;            // the PS5 runs its own tailnet: the address devices join at
    std::vector<Joining> joining;  // devices waiting for approval, oldest first

    bool running() const { return backend == "Running"; }
    int online() const;
};

// Parses /api/status's lines ("name<TAB>value<TAB>..."); unknown names are
// skipped, so a newer background part works with an older app.
Status parse_status(const std::string &tsv);

// settings.conf: "key = value" lines. Reads one setting (def when it isn't
// there), and writes some, keeping every other line (comments, the auth
// key) as it was.
std::string read_setting(const std::string &text, const std::string &key, const std::string &def);
std::string write_settings(const std::string &text, const std::vector<std::pair<std::string, std::string>> &set);

}  // namespace ws
