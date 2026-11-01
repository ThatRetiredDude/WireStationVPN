#include "app.h"

#include <algorithm>
#include <cstring>

#include "qrcodegen.hpp"
#include "ui/pad.h"
#include "ui/style.h"

namespace ws {

using namespace ui;

namespace {

constexpr int kW = 1920, kLeft = 96, kRight = 1824, kTop = 250;

const char *const kPageNames[] = {"Home", "Devices", "Files", "Settings", "About"};

const std::vector<Toggle> kToggles = {
    {"exit_node", "Exit node",
     "Your phone or computer can send its internet traffic through your home connection, from anywhere.", true},
    {"home_network", "Home network",
     "Away from home, your devices can reach the other things on your home network through the PS5.", true},
    {"tailnet_page", "This page on your tailnet",
     "Open http://ps5/ from any of your devices to see this status and browse the shared folders.", true},
    {"taildrop", "Receive files (Taildrop)",
     "Send photos and files to the PS5 from the Tailscale app. They go to /data/wirestation/received.", true},
};

void panel(Canvas &c, int x, int y, int w, int h, const Font &title_font, const std::string &title) {
    c.round_rect((float)x, (float)y, (float)w, (float)h, 18, kPanel);
    c.round_rect_line((float)x, (float)y, (float)w, (float)h, 18, 2, kPanelLine);
    if (!title.empty()) c.text(title_font, x + 32, y + 52, title, kText2);
}

// A label and its value on one line of a panel.
void row(Canvas &c, const Font &label_font, const Font &value_font, int x, int y, int label_w, const std::string &label,
         const std::string &value, Color col, int max_w) {
    c.text(label_font, x, y, label, kMuted);
    c.text(value_font, x + label_w, y, value, col, max_w - label_w);
}

}  // namespace

App::Image App::load(const uint8_t *data, size_t size) {
    Image img;
    if (!data || size < 8 || memcmp(data, "WSLG", 4) != 0) return img;
    uint16_t w, h;
    memcpy(&w, data + 4, 2);
    memcpy(&h, data + 6, 2);
    if (size < 8 + (size_t)w * h * 4) return img;
    img.w = w;
    img.h = h;
    img.px.resize((size_t)w * h);
    memcpy(img.px.data(), data + 8, img.px.size() * 4);
    return img;
}

App::App(const Fonts &fonts, const uint8_t *logo_small, size_t small_size, const uint8_t *logo_big, size_t big_size)
    : fonts_(fonts), logo_small_(load(logo_small, small_size)), logo_big_(load(logo_big, big_size)) {
    set_settings("");
}

void App::set_settings(const std::string &text) {
    toggles_ = kToggles;
    for (Toggle &t : toggles_) t.on = read_setting(text, t.key, t.on ? "on" : "off") != "off";
    saved_ = toggles_;
}

bool App::animating() const { return !busy_.empty() || (status_.reachable && !status_.running() && status_.login.empty()); }

Action App::press(uint32_t b) {
    Action a;
    if (confirm_ != NoConfirm) {
        if (b & kCross) a.kind = confirm_ == ConfirmStop ? Action::Stop : Action::Logout;
        if (b & (kCross | kCircle)) confirm_ = NoConfirm;
        return a;
    }
    if (b & kR1) page_ = (Page)((page_ + 1) % kPages);
    if (b & kL1) page_ = (Page)((page_ + kPages - 1) % kPages);
    if (b & (kL1 | kR1)) return a;
    switch (page_) {
        case Home:
            if (b & kCross) a.kind = status_.reachable ? Action::Leave : Action::Start;
            if ((b & kTriangle) && status_.reachable) confirm_ = ConfirmStop;
            if ((b & kSquare) && status_.reachable && status_.running()) confirm_ = ConfirmLogout;
            break;
        case Devices: {
            const int most = std::max(0, (int)status_.peers.size() - 9);
            if (b & kDown) scroll_ = std::min(scroll_ + 1, most);
            if (b & kUp) scroll_ = std::max(scroll_ - 1, 0);
            break;
        }
        case Settings: {
            const int rows = (int)toggles_.size() + 1;  // the switches, then Apply
            if (b & kDown) sel_ = std::min(sel_ + 1, rows - 1);
            if (b & kUp) sel_ = std::max(sel_ - 1, 0);
            if (b & kCross) {
                if (sel_ < (int)toggles_.size()) {
                    toggles_[sel_].on = !toggles_[sel_].on;
                } else {
                    a.kind = Action::Apply;
                    for (const Toggle &t : toggles_) a.settings.push_back({t.key, t.on ? "on" : "off"});
                    saved_ = toggles_;
                }
            }
            if (b & kCircle) toggles_ = saved_;
            break;
        }
        default:
            if (b & kCross) a.kind = Action::Leave;
            break;
    }
    return a;
}

void App::draw(Canvas &c, double t) {
    c.gradient(0, 0, c.width(), c.height(), kBgTop, kBgBottom);
    header(c);
    tabs(c);
    switch (page_) {
        case Home: home(c, t); break;
        case Devices: devices(c); break;
        case Files: files(c); break;
        case Settings: settings(c); break;
        default: about(c); break;
    }
    footer(c);
    if (confirm_ != NoConfirm) confirm(c);
}

void App::header(Canvas &c) {
    if (logo_small_.w) c.image(kLeft, 36, logo_small_.w, logo_small_.h, logo_small_.px.data());
    const int x = kLeft + (logo_small_.w ? logo_small_.w + 28 : 0);
    c.text(fonts_.get("title"), x, 98, "WireStationVPN", kText);
    c.text(fonts_.get("small"), x + 2, 132, "Your PS5 on your Tailscale network", kText2);
    std::string label;
    Color col = kMuted;
    if (!status_.reachable) {
        label = busy_.empty() ? "Not running" : "Starting";
        col = busy_.empty() ? kRed : kAmber;
    } else if (status_.running()) {
        label = "Connected";
        col = kGreen;
    } else if (!status_.login.empty() || status_.backend == "NeedsLogin") {
        label = "Sign-in needed";
        col = kAmber;
    } else {
        label = status_.backend.empty() ? "Starting" : status_.backend;
        col = kAmber;
    }
    pill(c, fonts_.get("bodyb"), kRight, 62, label, col);
}

void App::tabs(Canvas &c) {
    const Font &f = fonts_.get("bodyb");
    int x = kLeft + key_cap(c, fonts_.get("smallb"), kLeft, 204, "L1") + 28;
    for (int i = 0; i < kPages; i++) {
        const bool on = i == page_;
        const int w = c.text(f, x, 204, kPageNames[i], on ? kText : kMuted);
        if (on) c.round_rect((float)x, 218, (float)w, 4, 2, kAccent);
        x += w + 48;
    }
    key_cap(c, fonts_.get("smallb"), x - 20, 204, "R1");
    c.fill(kLeft, 232, kRight - kLeft, 1, kPanelLine);
}

void App::footer(Canvas &c) {
    const Font &f = fonts_.get("small");
    const int y = 1040;
    int x = kLeft;
    auto hint = [&](PadGlyph g, const std::string &text) {
        glyph(c, (float)x + 16, (float)y - 7, g);
        x += 40 + c.text(f, x + 40, y, text, kText2) + 40;
    };
    if (confirm_ != NoConfirm) return;
    switch (page_) {
        case Home:
            if (status_.reachable) {
                hint(kGlyphCross, "Run in background (close the app)");
                hint(kGlyphTriangle, "Stop WireStationVPN");
                if (status_.running()) hint(kGlyphSquare, "Sign this PS5 out");
            } else if (busy_.empty()) {
                hint(kGlyphCross, "Start WireStationVPN");
            }
            break;
        case Devices:
            if (status_.peers.size() > 9) c.text(f, x, y, "Up and down to scroll", kText2);
            break;
        case Settings:
            hint(kGlyphCross, sel_ < (int)toggles_.size() ? "Switch on or off" : "Apply");
            hint(kGlyphCircle, "Undo changes");
            break;
        default:
            if (status_.reachable) hint(kGlyphCross, "Run in background (close the app)");
            break;
    }
    c.text_right(fonts_.get("small"), kRight, y, "L1 / R1: pages", kMuted);
}

int App::paragraph(Canvas &c, const Font &f, int x, int y, int w, const std::string &text, Color col) {
    std::string line;
    size_t i = 0;
    while (i <= text.size()) {
        size_t sp = text.find(' ', i);
        if (sp == std::string::npos) sp = text.size();
        const std::string word = text.substr(i, sp - i);
        const std::string next = line.empty() ? word : line + " " + word;
        if (!line.empty() && Canvas::measure(f, next) > w) {
            c.text(f, x, y, line, col);
            y += f.line_height;
            line = word;
        } else {
            line = next;
        }
        i = sp + 1;
    }
    if (!line.empty()) {
        c.text(f, x, y, line, col);
        y += f.line_height;
    }
    return y;
}

void App::qr(Canvas &c, int x, int y, int size, const std::string &text) {
    using qrcodegen::QrCode;
    const QrCode q = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
    const int n = q.getSize(), quiet = 4, cell = size / (n + 2 * quiet);
    const int side = cell * (n + 2 * quiet);
    c.round_rect((float)x, (float)y, (float)side, (float)side, 16, rgb(0xFFFFFF));
    for (int r = 0; r < n; r++)
        for (int col = 0; col < n; col++)
            if (q.getModule(col, r)) c.fill(x + (quiet + col) * cell, y + (quiet + r) * cell, cell, cell, rgb(0x000000));
}

void App::home(Canvas &c, double t) {
    const Font &h2 = fonts_.get("h2"), &body = fonts_.get("body"), &small = fonts_.get("small");
    const Status &s = status_;
    if (!s.reachable) {
        if (!busy_.empty()) {
            spinner(c, kLeft + 20, 352, t, kAccent);
            c.text(h2, kLeft + 56, 364, busy_, kText);
            paragraph(c, body, kLeft, 440, 1400,
                      "WireStationVPN runs in the background: you can close this app once it's connected, and play.",
                      kText2);
        } else {
            c.text(h2, kLeft, 364, "WireStationVPN isn't running", kText);
            int y = paragraph(c, body, kLeft, 430, 1400,
                              "It's the part that keeps the PS5 on your Tailscale network, in the background. Press "
                              "Cross to start it.",
                              kText2);
            if (!problem_.empty()) paragraph(c, body, kLeft, y + 24, 1400, problem_, kRed);
        }
        return;
    }
    if (!s.login.empty()) {
        qr(c, kLeft, kTop + 30, 600, s.login);
        const int x = kLeft + 680, w = kRight - x;
        c.text(h2, x, kTop + 90, "Sign this PS5 in to Tailscale", kText);
        int y = kTop + 160;
        const char *steps[] = {
            "Scan the code with your phone's camera.",
            "Sign in with your Tailscale account. The free plan is enough. No account yet? You can make one there.",
            "This screen changes by itself when the PS5 is signed in.",
        };
        for (int i = 0; i < 3; i++) {
            c.circle((float)x + 20, (float)y - 10, 20, kAccent);
            c.text(fonts_.get("bodyb"), x + 12, y, std::to_string(i + 1), rgb(0xFFFFFF));
            y = paragraph(c, body, x + 60, y, w - 60, steps[i], kText) + 22;
        }
        c.text(small, x, y + 20, "Or open this link on a computer:", kMuted);
        paragraph(c, fonts_.get("mono"), x, y + 56, w, s.login, kText2);
        return;
    }
    if (!s.running()) {
        spinner(c, kLeft + 20, 352, t, kAccent);
        const std::string what = s.backend == "NeedsLogin"    ? "Getting the sign-in code from Tailscale"
                                 : s.backend == "Starting"    ? "Connecting to your tailnet"
                                 : s.backend == "NeedsMachineAuth" ? "Waiting for the tailnet's admin to approve this PS5"
                                 : s.backend == "Stopped"     ? "Tailscale is stopped"
                                                              : "Starting Tailscale";
        c.text(h2, kLeft + 56, 364, what, kText);
        if (!s.error.empty()) paragraph(c, body, kLeft, 440, 1400, "Tailscale says: " + s.error, kAmber);
        return;
    }

    // Signed in: the PS5, what it offers, who is online.
    const Font &bodyb = fonts_.get("bodyb");
    const int lx = kLeft, lw = 1000, rx = kLeft + lw + 40, rw = kRight - rx;
    panel(c, lx, kTop + 20, lw, 250, small, "THIS PS5");
    const std::string name = s.name.empty() ? s.hostname : s.name.substr(0, s.name.find('.'));
    row(c, body, bodyb, lx + 32, kTop + 128, 280, "Name", name, kText, lw - 64);
    row(c, body, bodyb, lx + 32, kTop + 176, 280, "Tailscale address", s.ips.substr(0, s.ips.find(',')), kText, lw - 64);
    if (s.tailnet_page)
        row(c, body, bodyb, lx + 32, kTop + 224, 280, "From anywhere", "http://" + name + "/", kAccent, lw - 64);

    panel(c, lx, kTop + 290, lw, 390, small, "EXIT NODE AND HOME NETWORK");
    int y = kTop + 402;
    if (s.exit == "ready") {
        c.circle((float)lx + 46, (float)y - 10, 9, kGreen);
        c.text(bodyb, lx + 70, y, "Exit node ready", kGreen);
        y = paragraph(c, body, lx + 32, y + 48, lw - 64,
                      "On your phone or computer, in the Tailscale app, choose Exit node, then " + name + ".", kText2);
    } else if (s.exit == "waiting") {
        c.circle((float)lx + 46, (float)y - 10, 9, kAmber);
        c.text(bodyb, lx + 70, y, "Exit node waiting for your approval", kAmber);
        y = paragraph(c, body, lx + 32, y + 48, lw - 64,
                      "On a computer, open login.tailscale.com/admin/machines. Find " + name +
                          ", open its menu (...), choose Edit route settings, and tick Use as exit node" +
                          (s.routes.empty() ? "." : " and the home network."),
                      kText2);
    } else {
        c.text(bodyb, lx + 32, y, "Exit node off", kMuted);
        y += 48;
    }
    for (const Route &r : s.routes) {
        y += 8;
        c.circle((float)lx + 46, (float)y - 10, 9, r.approved ? kGreen : kAmber);
        c.text(body, lx + 70, y, "Home network " + r.prefix + (r.approved ? ": reachable" : ": waiting for approval"),
               r.approved ? kText : kAmber, lw - 102);
        y += 44;
    }

    panel(c, rx, kTop + 20, rw, 660, small, "DEVICES");
    c.text(h2, rx + 32, kTop + 132, std::to_string(s.online()) + " of " + std::to_string(s.peers.size()) + " online",
           kText);
    y = kTop + 196;
    int shown = 0;
    for (const Peer &p : s.peers) {
        if (shown++ == 7) {
            c.text(small, rx + 32, y, "More on the Devices page", kMuted);
            break;
        }
        c.circle((float)rx + 44, (float)y - 9, 7, p.online ? kGreen : kMuted);
        c.text(body, rx + 64, y, p.name, p.online ? kText : kText2, rw - 96);
        y += 46;
    }
    if (s.peers.empty())
        paragraph(c, body, rx + 32, y, rw - 64,
                  "No other devices yet. Install the Tailscale app on your phone and sign in with the same account.",
                  kText2);
    if (!s.received.empty()) c.text(small, lx, kTop + 730, "Last file received: " + s.received[0], kText2, lw);
}

void App::devices(Canvas &c) {
    const Font &body = fonts_.get("body"), &small = fonts_.get("small");
    const Status &s = status_;
    c.text(fonts_.get("h2"), kLeft, kTop + 70, "Devices on your tailnet", kText);
    if (!s.reachable || s.peers.empty()) {
        paragraph(c, body, kLeft, kTop + 140, 1400,
                  s.reachable ? "No other devices yet. Install the Tailscale app on your phone or computer and sign in "
                                "with the same Tailscale account."
                              : "WireStationVPN isn't running.",
                  kText2);
        return;
    }
    const int cols[] = {kLeft + 32, kLeft + 700, kLeft + 1100, kLeft + 1350};
    int y = kTop + 140;
    c.text(small, cols[0], y, "NAME", kMuted);
    c.text(small, cols[1], y, "TAILSCALE ADDRESS", kMuted);
    c.text(small, cols[2], y, "SYSTEM", kMuted);
    c.text(small, cols[3], y, "STATUS", kMuted);
    y += 24;
    for (size_t i = (size_t)scroll_; i < s.peers.size() && i < (size_t)scroll_ + 9; i++) {
        const Peer &p = s.peers[i];
        c.round_rect((float)kLeft, (float)y, (float)(kRight - kLeft), 60, 12, i % 2 ? kRow : kPanel);
        const int base = y + 40;
        c.text(body, cols[0], base, p.name, kText, cols[1] - cols[0] - 24);
        c.text(fonts_.get("code"), cols[1], base, p.ip, kText2);
        c.text(body, cols[2], base, p.os, kText2, cols[3] - cols[2] - 24);
        c.circle((float)cols[3] + 10, (float)base - 9, 7, p.online ? kGreen : kMuted);
        c.text(body, cols[3] + 30, base, p.online ? "online" : (p.seen.empty() ? "offline" : "seen " + p.seen),
               p.online ? kText : kText2);
        y += 66;
    }
}

void App::files(Canvas &c) {
    const Font &h2 = fonts_.get("h2"), &body = fonts_.get("body"), &small = fonts_.get("small");
    const Status &s = status_;
    const std::string name = s.name.empty() ? (s.hostname.empty() ? "ps5" : s.hostname) : s.name.substr(0, s.name.find('.'));
    const int lx = kLeft, lw = 840, rx = kLeft + lw + 48, rw = kRight - rx;
    panel(c, lx, kTop + 20, lw, 660, small, "SEND FILES TO THE PS5");
    int y = kTop + 132;
    if (!s.taildrop) {
        paragraph(c, body, lx + 32, y, lw - 64, "Receiving files is off. Turn on Receive files on the Settings page.",
                  kText2);
    } else {
        y = paragraph(c, body, lx + 32, y, lw - 64,
                      "On your phone: Share, then Tailscale, then " + name +
                          ". On a computer: right-click a file, Send with Tailscale (or tailscale file cp FILE " + name +
                          ":).",
                      kText);
        y = paragraph(c, body, lx + 32, y + 16, lw - 64, "Files go to /data/wirestation/received on the PS5.", kText2);
        c.text(small, lx + 32, y + 32, "RECEIVED", kMuted);
        y += 76;
        if (s.received.empty()) c.text(body, lx + 32, y, "Nothing yet.", kText2);
        for (size_t i = 0; i < s.received.size() && i < 7; i++, y += 44) c.text(body, lx + 32, y, s.received[i], kText, lw - 64);
    }
    panel(c, rx, kTop + 20, rw, 660, small, "YOUR PS5'S FILES, FROM ANYWHERE");
    y = kTop + 132;
    if (!s.tailnet_page) {
        paragraph(c, body, rx + 32, y, rw - 64,
                  "Off. Turn on This page on your tailnet on the Settings page to browse the PS5's shared folders "
                  "from your devices.",
                  kText2);
        return;
    }
    c.text(h2, rx + 32, y + 10, "http://" + name + "/files/", kAccent, rw - 64);
    y = paragraph(c, body, rx + 32, y + 70, rw - 64,
                  "Open it in a browser on any of your devices with Tailscale on, at home or away. Download anything "
                  "in the shared folders, and watch videos straight from the PS5.",
                  kText2);
    y = paragraph(c, body, rx + 32, y + 16, rw - 64,
                  "Shared: received files and USB drives. Add more folders (such as /data) with shared_folders in "
                  "/data/wirestation/settings.conf.",
                  kText2);
    paragraph(c, small, rx + 32, y + 16, rw - 64, "Read-only. The PS5's Tailscale keys and settings are never shared.",
              kMuted);
}

void App::settings(Canvas &c) {
    const Font &body = fonts_.get("body"), &bodyb = fonts_.get("bodyb"), &small = fonts_.get("small");
    c.text(fonts_.get("h2"), kLeft, kTop + 70, "Settings", kText);
    int y = kTop + 110;
    for (size_t i = 0; i <= toggles_.size(); i++) {
        const bool sel = (int)i == sel_;
        c.round_rect((float)kLeft, (float)y, 1100, 76, 14, sel ? kRowSel : kPanel);
        if (sel) c.round_rect_line((float)kLeft, (float)y, 1100, 76, 14, 2, kAccent);
        if (i < toggles_.size()) {
            const Toggle &t = toggles_[i];
            c.text(bodyb, kLeft + 32, y + 49, t.label, kText);
            // The switch.
            const float sx = kLeft + 1100 - 120, sy = (float)y + 22;
            c.round_rect(sx, sy, 76, 34, 17, t.on ? kAccent : kTrack);
            c.circle(t.on ? sx + 59 : sx + 17, sy + 17, 13, rgb(0xFFFFFF));
            const bool changed = t.on != saved_[i].on;
            if (changed) c.text_right(small, (int)sx - 24, y + 47, "changed", kAmber);
        } else {
            const bool dirty = [&] {
                for (size_t k = 0; k < toggles_.size(); k++)
                    if (toggles_[k].on != saved_[k].on) return true;
                return false;
            }();
            c.text(bodyb, kLeft + 32, y + 49, dirty ? "Apply (restarts WireStationVPN)" : "Apply", dirty ? kAccent : kMuted);
        }
        y += 92;
    }
    // What the chosen row does, beside the list.
    const int x = kLeft + 1160, w = kRight - x;
    if (sel_ < (int)toggles_.size()) {
        c.text(bodyb, x, kTop + 160, toggles_[sel_].label, kText);
        paragraph(c, body, x, kTop + 210, w, toggles_[sel_].help, kText2);
    } else {
        paragraph(c, body, x, kTop + 160, w,
                  "Saves the switches and restarts the background part: connections through the PS5 drop for a few "
                  "seconds.",
                  kText2);
    }
    paragraph(c, small, kLeft, kTop + 600, 1100,
              "More settings (the PS5's name, tags, an auth key, a Headscale server) are in "
              "/data/wirestation/settings.conf: edit it over FTP, then Apply.",
              kMuted);
}

void App::about(Canvas &c) {
    const Font &body = fonts_.get("body"), &small = fonts_.get("small"), &bodyb = fonts_.get("bodyb");
    if (logo_big_.w) c.image(kLeft, kTop + 30, logo_big_.w, logo_big_.h, logo_big_.px.data());
    const int x = kLeft + (logo_big_.w ? logo_big_.w + 64 : 0), w = kRight - x;
    c.text(fonts_.get("h2"), x, kTop + 70, "WireStationVPN", kText);
    int y = paragraph(c, body, x, kTop + 120, w,
                      "Puts your PS5 on your Tailscale network. Your phone and computer can use it as an exit node, "
                      "reach your home network, send it files and browse its folders, from anywhere, with no router "
                      "setup.",
                      kText2);
    c.text(bodyb, x, y + 24, "Works", kGreen);
    y = paragraph(c, body, x + 140, y + 24, w - 140,
                  "On the home screen and while you play. Web, apps and everything over TCP and UDP. Behind almost "
                  "any router.",
                  kText2);
    c.text(bodyb, x, y + 12, "Doesn't", kAmber);
    y = paragraph(c, body, x + 140, y + 12, w - 140,
                  "Rest mode. After a PS5 restart: run the jailbreak, then open this app again. Ping through the exit "
                  "node. The PS5's own games don't use the tailnet.",
                  kText2);
    c.text(bodyb, x, y + 12, "Needs", kText);
    y = paragraph(c, body, x + 140, y + 12, w - 140,
                  "A Tailscale account (the free plan is enough) and the Tailscale app on your other devices.",
                  kText2);
    std::string versions = "App " + version_;
    if (status_.reachable) versions += " · background part " + status_.version + ", up " + status_.uptime;
    y = paragraph(c, small, x, y + 36, w, versions, kMuted);
    y = paragraph(c, small, x, y + 4, w,
                  "Free software under the GPL-3.0. Source: github.com/ThatRetiredDude/WireStationVPN. Uses Tailscale's "
                  "open-source client (BSD-3-Clause). Not affiliated with Tailscale Inc.",
                  kMuted);
}

void App::confirm(Canvas &c) {
    c.fill(0, 0, c.width(), c.height(), alpha(rgb(0x000000), 190));
    const int w = 1000, h = 380, x = (kW - w) / 2, y = 330;
    c.round_rect((float)x, (float)y, (float)w, (float)h, 22, kPanel);
    c.round_rect_line((float)x, (float)y, (float)w, (float)h, 22, 2, kAccent);
    const bool stop = confirm_ == ConfirmStop;
    c.text(fonts_.get("h2"), x + 48, y + 84, stop ? "Stop WireStationVPN?" : "Sign this PS5 out of Tailscale?", kText);
    paragraph(c, fonts_.get("body"), x + 48, y + 144, w - 96,
              stop ? "Devices using the PS5 as their exit node lose their connection until you start it again."
                   : "It leaves your tailnet. To come back, sign in again with the code on the Home page.",
              kText2);
    const Font &f = fonts_.get("body");
    int hx = x + 48;
    glyph(c, (float)hx + 16, (float)y + h - 64, kGlyphCross);
    hx += 44 + c.text(f, hx + 44, y + h - 54, stop ? "Stop" : "Sign out", kText) + 56;
    glyph(c, (float)hx + 16, (float)y + h - 64, kGlyphCircle);
    c.text(f, hx + 44, y + h - 54, "Cancel", kText);
}

}  // namespace ws
