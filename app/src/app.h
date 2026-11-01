// The app on the TV: five pages (Home, Devices, Files, Settings, About)
// drawn on a CPU canvas, driven by the controller. It knows nothing about
// the console: main.cpp gives it the status and carries out what it asks
// for, and host/screens.cpp draws its pages on a PC.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "status.h"
#include "ui/canvas.h"

namespace ws {

// A setting the Settings page changes, as settings.conf names it.
struct Toggle {
    const char *key, *label, *help;
    bool on;
};

struct Action {
    enum Kind { None, Leave, Start, Stop, Logout, Apply, Approve, Deny } kind = None;
    std::vector<std::pair<std::string, std::string>> settings;  // Apply: key, value
    std::string id;                                             // Approve, Deny: the device's request
};

class App {
public:
    // logo_small and logo_big: "WSLG" images (ui/make_logo.py), or null.
    App(const ui::Fonts &fonts, const uint8_t *logo_small, size_t small_size, const uint8_t *logo_big, size_t big_size);

    enum Page { Home, Devices, Files, Settings, About, kPages };

    // The latest status; and while the background part is being started or
    // restarted, what is happening ("" when nothing is).
    void set_status(const Status &s) { status_ = s; }
    void set_busy(const std::string &what) { busy_ = what; }
    void set_problem(const std::string &what) { problem_ = what; }
    // The settings file as it is, for the Settings page's switches.
    void set_settings(const std::string &text);
    void set_version(const std::string &v) { version_ = v; }

    // A controller press (ui::Button bits); what main.cpp should do.
    Action press(uint32_t buttons);
    void show(Page p) { page_ = p; }
    Page page() const { return page_; }

    void draw(ui::Canvas &c, double t);
    // Redraw often (a spinner turns) or once a second.
    bool animating() const;

private:
    struct Image {
        int w = 0, h = 0;
        std::vector<uint32_t> px;
    };
    static Image load(const uint8_t *data, size_t size);

    void header(ui::Canvas &c);
    void tabs(ui::Canvas &c);
    void footer(ui::Canvas &c);
    void home(ui::Canvas &c, double t);
    void devices(ui::Canvas &c);
    void files(ui::Canvas &c);
    void settings(ui::Canvas &c);
    void about(ui::Canvas &c);
    void confirm(ui::Canvas &c);
    void qr(ui::Canvas &c, int x, int y, int size, const std::string &text);
    int paragraph(ui::Canvas &c, const ui::Font &f, int x, int y, int w, const std::string &text, ui::Color col);

    const ui::Fonts &fonts_;
    Image logo_small_, logo_big_;
    Status status_;
    std::string busy_, problem_, version_ = "dev";
    Page page_ = Home;
    std::vector<Toggle> toggles_, saved_;
    int sel_ = 0;     // the Settings page's row
    int scroll_ = 0;  // the Devices page's first row
    enum Confirm { NoConfirm, ConfirmStop, ConfirmLogout } confirm_ = NoConfirm;
};

}  // namespace ws
