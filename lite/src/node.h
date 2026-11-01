// The C++ build's program: settings, what it's doing, and its web pages.
// The same settings.conf, data folder, status page and /api/status as the
// Go build (node/), so the app works with either.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "ts/tailscale.h"
#include "tunnel.h"

namespace ws {

struct Settings {
    std::string hostname = "ps5";
    bool exit_node = true, home_network = true, tailnet_page = true, taildrop = true;
    bool fixed_port = false;
    int port = 0;  // Tailscale's UDP port; 0 picks one
    std::string auth_key, control_url, dns = "1.1.1.1";
    int status_port = 9039;
    std::vector<std::string> tags;
    std::string taildrop_dir;
    std::vector<std::string> shared = {"taildrop", "/mnt/usb0", "/mnt/usb1", "/mnt/ext0", "/mnt/ext1"};
    bool server = false;  // own tailnet: the Go build only

    int udp_port() const { return port == 0 && fixed_port ? 41641 : port; }
};

// Reads path over the defaults (a missing file is fine); problems: what it
// couldn't use, for the log.
Settings load_settings(const std::string &path, std::vector<std::string> &problems);

constexpr int kPeerAPIPort = 41642;  // where devices send files (the PS5's tailnet address)

struct Node {
    std::string version, dir;
    Settings cfg;
    double started = 0;
    ts::Client client;
    Tunnel tunnel;
    std::vector<std::string> routes;  // offered
    std::atomic<bool> quit{false};

    std::mutex mu;
    std::vector<std::string> received;  // the last files received with Taildrop
};

// The web server: the status page on the home network (status_port), and,
// on 127.0.0.1, the tailnet page and Taildrop for the tunnel. Returns the
// second's port, 0 if it couldn't start.
uint16_t serve_web(Node &n, int status_listener);

}  // namespace ws
