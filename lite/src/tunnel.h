// The tailnet's side of the PS5 (C++ build): WireGuard and a small TCP/IP
// stack (lwIP) on one thread. What devices send to the PS5's tailnet
// address goes to this program's web server (port 80: the tailnet page;
// the PeerAPI port: Taildrop). What they send through it (exit node, home
// network) is opened again as the PS5's own TCP connections and UDP
// sockets, as Tailscale's own app does in its user-space mode.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace ts {
class Client;
}

namespace ws {

struct TunnelConfig {
    bool exit_node = true;     // forward to anywhere (the internet)
    bool home_network = true;  // forward to the home network
    std::vector<std::pair<uint32_t, uint32_t>> home;  // the home networks: address, mask (host order)
    uint16_t web_port = 0;     // this program's web server, on 127.0.0.1
    uint16_t tailnet_port = 80;  // 0: no tailnet page
    uint16_t peerapi_port = 0;   // 0: no Taildrop
};

struct TunnelPeer {
    bool up = false;       // has a WireGuard session now
    bool direct = false;   // over a direct path, not a relay
    double seen_s = -1;    // seconds since it last sent anything, -1 if never
};

// Who is behind a connection the tunnel made to the web server.
struct Caller {
    std::string address;  // the device's tailnet address
    uint16_t port = 0;    // the PS5's port it connected to (80 or the PeerAPI's)
};

class Tunnel {
public:
    ~Tunnel();
    void start(ts::Client *client, const TunnelConfig &cfg);
    // Devices with a session, by tailnet address.
    std::map<std::string, TunnelPeer> peers();
    // The device behind the web server's connection from local_port, and
    // which port it came to; false if it isn't one of the tunnel's.
    bool caller(uint16_t local_port, Caller &out);
    // Connections and UDP flows open now, for the status page.
    int open_tcp() const { return open_tcp_; }
    int open_udp() const { return open_udp_; }

    // For the tunnel's thread.
    std::mutex mu_;
    std::map<uint16_t, std::pair<Caller, double>> callers_;  // by local port: who, and when it lapses (0: open)
    std::map<std::string, TunnelPeer> peers_;
    std::atomic<int> open_tcp_{0}, open_udp_{0};

private:
    void loop();
    ts::Client *client_ = nullptr;
    TunnelConfig cfg_;
    std::atomic<bool> quit_{false};
    std::thread thread_;
};

}  // namespace ws
