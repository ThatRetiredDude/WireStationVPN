// Draws each of the app's pages on a PC, with made-up and real statuses, to
// BMP files, so the screens can be looked at and checked without a PS5:
//
//   app/host/screens.sh OUT_DIR
//
// With a status file (what /api/status gave), or "live" to ask the
// background part running on this PC, draws that one's pages too.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "link.h"
#include "ui/canvas.h"
#include "ui/pad.h"

namespace {

std::vector<uint8_t> slurp(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

ws::Status running() {
    return ws::parse_status(
        "version\tv1.0.0\nuptime\t2h13m4s\nhostname\tps5\nbackend\tRunning\nname\tps5.tail1234.ts.net\n"
        "ips\t100.101.102.103, fd7a:115c:a1e0::1\nexit\tready\nroute\t192.168.1.0/24\tapproved\n"
        "tailnet_page\ton\ntaildrop\ton\n"
        "peer\tpixel-9.tail1234.ts.net\t100.88.1.2\tonline\tandroid\t\n"
        "peer\tlaptop.tail1234.ts.net\t100.88.1.3\tonline\tmacOS\t\n"
        "peer\tdesktop.tail1234.ts.net\t100.88.1.4\toffline\twindows\t3 hours ago\n"
        "peer\tiphone.tail1234.ts.net\t100.88.1.5\toffline\tiOS\t2 days ago\n"
        "received\tbeach.jpg\nreceived\tsave-backup.zip\n");
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: screens UI_DIR OUT_DIR [STATUS_FILE]\n");
        return 2;
    }
    const std::string ui = argv[1], out = argv[2];
    const std::vector<uint8_t> font = slurp(ui + "/font.bin"), small = slurp(ui + "/logo_small.bin"),
                               big = slurp(ui + "/logo_big.bin");
    ui::Fonts fonts;
    if (!fonts.load(font.data(), font.size())) {
        fprintf(stderr, "screens: can't read %s/font.bin\n", ui.c_str());
        return 1;
    }
    std::vector<std::string> said;
    ui::Canvas::record = &said;
    ui::Canvas canvas(1920, 1080);
    int failed = 0;
    auto shot = [&](ws::App &app, const std::string &name) {
        said.clear();
        app.draw(canvas, 0.3);
        const std::string path = out + "/" + name + ".bmp";
        if (!canvas.save_bmp(path)) failed++;
        std::ofstream(out + "/" + name + ".txt") << [&] {
            std::string s;
            for (auto &w : said) s += w + "\n";
            return s;
        }();
        printf("screens: %s\n", path.c_str());
    };
    auto make = [&] {
        ws::App app(fonts, small.data(), small.size(), big.data(), big.size());
        app.set_version("v1.0.0");
        return app;
    };

    {
        ws::App app = make();
        shot(app, "01-not-running");
        app.set_busy("Starting TailStation");
        shot(app, "02-starting");
    }
    {
        ws::App app = make();
        app.set_status(ws::parse_status(
            "version\tv1.0.0\nhostname\tps5\nbackend\tNeedsLogin\n"
            "login\thttps://login.tailscale.com/a/1a2b3c4d5e6f7\nexit\toff\n"));
        shot(app, "03-sign-in");
    }
    {
        ws::App app = make();
        ws::Status s = running();
        s.exit = "waiting";
        s.routes[0].approved = false;
        app.set_status(s);
        shot(app, "04-waiting-for-approval");
        app.set_status(running());
        shot(app, "05-home");
        for (const char *page : {"06-devices", "07-files", "08-settings", "09-about"}) {
            app.press(ui::kR1);
            shot(app, page);
        }
        app.show(ws::App::Settings);
        app.press(ui::kDown);
        app.press(ui::kCross);  // home network off: changed
        shot(app, "10-settings-changed");
        app.show(ws::App::Home);
        app.press(ui::kTriangle);
        shot(app, "11-confirm-stop");
    }
    {
        // The PS5 running its own tailnet: a phone asking to join, and the
        // steps before any device is in.
        ws::App app = make();
        ws::Status s = running();
        s.server = "http://203.0.113.7:8080";
        s.name = "ps5.ps5net";
        s.joining.push_back({"hskey-authreq-AbCdEf", "pixel-9"});
        app.set_status(s);
        shot(app, "12-own-server-home");
        app.show(ws::App::Devices);
        shot(app, "13-own-server-joining");
        s.peers.clear();
        s.joining.clear();
        app.set_status(s);
        shot(app, "14-own-server-add-a-device");
        app.show(ws::App::Settings);
        shot(app, "15-settings-with-server");
    }
    if (argc > 3) {
        // A saved /api/status, or "live": the background part running on
        // this PC, asked the way the app asks it (link.cpp).
        std::string text, err;
        if (std::string(argv[3]) == "live") {
            if (!ws::http("GET", "/api/status", text, err)) {
                fprintf(stderr, "screens: /api/status: %s\n", err.c_str());
                return 1;
            }
        } else {
            std::ifstream f(argv[3]);
            std::stringstream ss;
            ss << f.rdbuf();
            text = ss.str();
        }
        ws::App app = make();
        app.set_status(ws::parse_status(text));
        shot(app, "20-real-home");
        app.show(ws::App::Devices);
        shot(app, "21-real-devices");
    }
    return failed ? 1 : 0;
}
