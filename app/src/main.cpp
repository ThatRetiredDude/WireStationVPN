// WireStationVPN's PS5 app (PPSA99556): the screen for the background part.
//
// The background part (node/, built as wirestation.elf and shipped in the
// app's folder) is what keeps the PS5 on the tailnet. The app starts it
// through the jailbreak's ELF loader (127.0.0.1:9021) when it isn't running,
// shows what it says on http://127.0.0.1:9039/api/status, and starts it
// again if it stops while the app is open. "Run in background" closes the
// app; the background part keeps running.
//
// A title must never return from main or call exit(): the console reports
// "Something went wrong". It leaves with sceSystemServiceLoadExec("exit").
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "app.h"
#include "link.h"
#include "log.h"
#include "status.h"
#include "ui/canvas.h"
#include "ui/display.h"
#include "ui/pad.h"
#include "vk_context.h"

#ifndef WS_VERSION
#define WS_VERSION "dev"
#endif

extern "C" {
int sceSystemServiceLoadExec(const char *path, const char *const *args);
int sceSystemServiceHideSplashScreen(void);
int sceKernelSendNotificationRequest(int device, void *request, size_t size, int blocking);
int sceKernelUsleep(unsigned int microseconds);

// RADV is linked in whole and there is no Vulkan loader: its ICD entry point
// answers vkGetInstanceProcAddr for every command.
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
PFN_vkGetInstanceProcAddr ws_vk_entry() { return vk_icdGetInstanceProcAddr; }

// data.S
extern const uint8_t ws_font_data[], ws_font_data_end[], ws_logo_small[], ws_logo_small_end[], ws_logo_big[],
    ws_logo_big_end[];
}

namespace {

constexpr const char *kDir = "/data/wirestation", *kSettings = "/data/wirestation/settings.conf",
                     *kLog = "/data/wirestation/app-log.txt", *kPrevLog = "/data/wirestation/app-log.old.txt",
                     *kPayload = "/app0/wirestation.elf";
constexpr int kWidth = 1920, kHeight = 1080;

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// A notification on the TV: 45 bytes of header fields left at zero, then the
// text (the system's layout).
void notify(const std::string &text) {
    struct {
        char header[45];
        char text[3075];
    } request = {};
    snprintf(request.text, sizeof request.text, "%s", text.c_str());
    sceKernelSendNotificationRequest(0, &request, sizeof request, 0);
    printf("wirestation-app: notify: %s\n", text.c_str());
}

[[noreturn]] void leave() {
    printf("wirestation-app: leaving\n");
    fflush(nullptr);
    ws::log_drain();
    sceSystemServiceLoadExec("exit", nullptr);
    for (;;) sceKernelUsleep(1000000);
}

std::string read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(const char *path, const std::string &text) {
    const std::string tmp = std::string(path) + ".new";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (fclose(f) != 0 || !ok) return false;
    return rename(tmp.c_str(), path) == 0;
}

// The background part, looked after on a thread of its own: its status every
// two seconds, starting it, and the buttons' requests.
class Keeper {
public:
    void run() {
        bool first = true;
        for (;;) {
            ws::Action a;
            {
                std::lock_guard<std::mutex> l(mu_);
                a = pending_;
                pending_ = ws::Action();
            }
            handle(a);
            const bool up = poll();
            if (!up && !stopped_ && !starting_) {
                if (first) {
                    start("Starting WireStationVPN");
                } else if (was_up_) {
                    notify("WireStationVPN stopped. Starting it again.");
                    start("WireStationVPN stopped. Starting it again");
                }
            }
            if (up) {
                was_up_ = true;
                if (starting_) {
                    starting_ = false;
                    set_busy("");
                }
            } else if (starting_ && now() - started_at_ > 60) {
                starting_ = false;
                set_busy("");
                set_problem("WireStationVPN didn't start within a minute. Its log is /data/wirestation/log.txt. "
                            "Press Cross to try again.");
            }
            first = false;
            for (int i = 0; i < 20; i++) {  // two seconds, or sooner for a button
                sceKernelUsleep(100000);
                std::lock_guard<std::mutex> l(mu_);
                if (pending_.kind != ws::Action::None) break;
            }
        }
    }

    void ask(const ws::Action &a) {
        std::lock_guard<std::mutex> l(mu_);
        pending_ = a;
    }

    // What the screen shows; changed tells the main loop to redraw.
    bool take(ws::Status &s, std::string &busy, std::string &problem, std::string &settings) {
        std::lock_guard<std::mutex> l(mu_);
        if (!changed_) return false;
        s = status_;
        busy = busy_;
        problem = problem_;
        settings = settings_;
        changed_ = false;
        return true;
    }

private:
    bool poll() {
        std::string body, err;
        ws::Status s;
        if (ws::http("GET", "/api/status", body, err)) s = ws::parse_status(body);
        std::lock_guard<std::mutex> l(mu_);
        if (s.reachable != status_.reachable || body != last_body_) changed_ = true;
        status_ = s;
        last_body_ = body;
        return s.reachable;
    }

    void start(const std::string &why) {
        set_problem("");
        std::string err;
        printf("wirestation-app: starting %s (%s)\n", kPayload, why.c_str());
        if (!ws::send_elf(kPayload, err)) {
            printf("wirestation-app: %s\n", err.c_str());
            set_problem("Couldn't start it: " + err + ". Run the jailbreak (with its ELF loader), then press Cross.");
            stopped_ = true;  // until Cross: no loop of failed starts
            return;
        }
        starting_ = true;
        started_at_ = now();
        set_busy(why);
    }

    // Waits up to `seconds` for the background part to stop answering.
    void wait_gone(int seconds) {
        for (int i = 0; i < seconds * 4 && poll(); i++) sceKernelUsleep(250000);
    }

    void handle(const ws::Action &a) {
        std::string body, err;
        switch (a.kind) {
            case ws::Action::Start:
                stopped_ = false;
                start("Starting WireStationVPN");
                break;
            case ws::Action::Stop:
                stopped_ = true;
                was_up_ = false;
                if (!ws::http("POST", "/stop", body, err)) printf("wirestation-app: stop: %s\n", err.c_str());
                wait_gone(10);
                break;
            case ws::Action::Logout:
                if (!ws::http("POST", "/logout", body, err)) printf("wirestation-app: sign-out: %s\n", err.c_str());
                break;
            case ws::Action::Apply: {
                mkdir(kDir, 0755);
                const std::string text = ws::write_settings(read_file(kSettings), a.settings);
                if (!write_file(kSettings, text)) {
                    set_problem(std::string("Couldn't save ") + kSettings + ".");
                    break;
                }
                {
                    std::lock_guard<std::mutex> l(mu_);
                    settings_ = text;
                    changed_ = true;
                }
                stopped_ = true;  // no "it stopped" restart in between
                ws::http("POST", "/stop", body, err);
                wait_gone(15);
                stopped_ = false;
                start("Restarting WireStationVPN with the new settings");
                break;
            }
            default:
                break;
        }
    }

    void set_busy(const std::string &b) {
        std::lock_guard<std::mutex> l(mu_);
        busy_ = b;
        changed_ = true;
    }
    void set_problem(const std::string &p) {
        std::lock_guard<std::mutex> l(mu_);
        problem_ = p;
        changed_ = true;
    }

    std::mutex mu_;
    ws::Action pending_;
    ws::Status status_;
    std::string last_body_, busy_, problem_, settings_ = read_file(kSettings);
    bool changed_ = true;
    // Only the keeper's thread uses these.
    bool stopped_ = false, starting_ = false, was_up_ = false;
    double started_at_ = 0;
};

}  // namespace

int main(int, char **) {
    mkdir(kDir, 0755);
    rename(kLog, kPrevLog);
    if (freopen(kLog, "a", stdout)) setvbuf(stdout, nullptr, _IOLBF, 0);
    static char stderr_buf[1 << 16];
    if (freopen(kLog, "a", stderr)) setvbuf(stderr, stderr_buf, _IOFBF, sizeof stderr_buf);
    printf("wirestation-app: WireStationVPN app %s\n", WS_VERSION);
    sceSystemServiceHideSplashScreen();

    std::string err;
    ws::VkContext *ctx = ws::create_vk_context(0, true, err);
    if (!ctx) {
        printf("wirestation-app: no GPU: %s\n", err.c_str());
        notify("WireStationVPN couldn't show its screen and is closing. Open it again.");
        sceKernelUsleep(3000000);
        leave();
    }
    ui::Fonts fonts;
    fonts.load(ws_font_data, (size_t)(ws_font_data_end - ws_font_data));
    ui::Display display;
    const bool screen = ctx->display && display.open(ctx, kWidth, kHeight, err);
    if (!screen) printf("wirestation-app: no picture on the TV: %s\n", ctx->display ? err.c_str() : "no display");
    ui::Pad pad;
    pad.open();

    ws::App app(fonts, ws_logo_small, (size_t)(ws_logo_small_end - ws_logo_small), ws_logo_big,
                (size_t)(ws_logo_big_end - ws_logo_big));
    app.set_version(WS_VERSION);
    Keeper keeper;
    std::thread([&keeper] { keeper.run(); }).detach();

    ui::Canvas canvas(kWidth, kHeight);
    double last_draw = 0;
    bool dirty = true;
    std::string settings_seen = "\x01";
    for (;;) {
        const double t = now();
        if (const uint32_t b = pad.poll()) {
            const ws::Action a = app.press(b);
            if (a.kind == ws::Action::Leave) {
                notify("WireStationVPN keeps running in the background.");
                leave();
            }
            if (a.kind != ws::Action::None) keeper.ask(a);
            dirty = true;
        }
        ws::Status s;
        std::string busy, problem, settings;
        if (keeper.take(s, busy, problem, settings)) {
            app.set_status(s);
            app.set_busy(busy);
            app.set_problem(problem);
            if (settings != settings_seen) {
                app.set_settings(settings);
                settings_seen = settings;
            }
            dirty = true;
        }
        const double period = app.animating() ? 1.0 / 15 : 1.0;
        if (dirty || t - last_draw >= period) {
            app.draw(canvas, t);
            if (screen && !display.present(canvas.pixels(), err))
                printf("wirestation-app: present: %s\n", err.c_str());
            last_draw = t;
            dirty = false;
        }
        sceKernelUsleep(16000);
    }
}
