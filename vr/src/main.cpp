// framewall-vr: show a framewall monitor wall in VR as separate panels side
// by side around you, instead of one flat screen in theater mode.
//
// It is a SteamVR overlay app for the Steam Frame. It connects to the
// framewall host (Sunshine) like Moonlight does, reusing Moonlight's pairing,
// decodes the one wall stream on the headset's hardware decoder and shows
// each monitor of the wall on its own panel. Mouse and keyboard stay on the
// PC; the headset's controllers are passed through as an Xbox controller
// when it is launched from Steam.
//
//   framewall-vr                 stream the default preset (see below)
//   framewall-vr "APP"           stream this wall preset (app name, or part of one)
//   framewall-vr --list          list the host's apps
//   framewall-vr --quit          close the wall on the host (removes the virtual monitors)
//
//   options: --host NAME  --bitrate KBPS  --seconds N  --window / --no-window
//            --dump FILE.ppm (save one converted frame, for testing)
//
// Settings: ~/.config/framewall-vr.conf (KEY=VALUE, see framewall-vr.conf.example),
// created on the first run. Panel sizes and placement apply as soon as the file
// is saved, while streaming.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "audio.h"
#include "layout.h"
#include "log.h"
#include "moonlight_conf.h"
#include "nvhttp.h"
#include "conf_template.h"
#include "video.h"

#include <Limelight.h>
#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <openssl/rand.h>
#include <openvr.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
std::atomic<int> g_terminated{1};  // 1 = still running, else the error code
VideoPipeline *g_video = nullptr;
std::atomic<const char *> g_status{"connecting"};  // set from several threads

struct Rumble { std::atomic<uint32_t> value{0}; std::atomic<bool> dirty{false}; };
Rumble g_rumble[4];

// ---- settings -----------------------------------------------------------------

struct Settings {
    std::string host, app;
    int bitrate_kbps = 0;  // 0 = automatic from the wall size
    bool audio = true;
    bool quit_on_exit = true;
    int window = -1;       // -1 = only when launched from Steam
    int seconds = 0;       // stop after this long (testing)
    std::string dump;
    bool list = false;
    bool quit = false;     // just quit the wall on the host
    Placement place;
    std::map<std::string, float> panel_size;  // panel name -> size multiplier
};

std::string settings_path() {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.config/framewall-vr.conf";
}

bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
    s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
    return s;
}

void load_settings(Settings &s) {
    const std::string path = settings_path();
    std::ifstream in(path);
    if (!in) {
        // First run: write the file with every setting, so editing it is
        // just changing numbers.
        const std::string dir = path.substr(0, path.rfind('/'));
        mkdir(dir.c_str(), 0755);
        std::ofstream out(path);
        out << kConfTemplate;
        if (out) fwlog("settings: created %s", path.c_str());
        out.close();
        in.open(path);
    }
    std::string line;
    float size = 1;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        auto f = [&] { return std::strtof(v.c_str(), nullptr); };
        if (k == "HOST") s.host = v;
        else if (k == "APP") s.app = v;
        else if (k == "BITRATE_KBPS") s.bitrate_kbps = std::atoi(v.c_str());
        else if (k == "AUDIO") s.audio = v != "0";
        else if (k == "QUIT_ON_EXIT") s.quit_on_exit = v != "0";
        else if (k == "DISTANCE" && f() >= 0.3f) s.place.radius = f();
        else if (k == "HEIGHT") s.place.height = f();
        else if (k == "GAP" && f() >= 0) s.place.gap = f();
        else if (k == "SIZE" && f() > 0.05f) size = f();
        else if (k == "SCREENS_SIZE" && f() > 0.05f) s.panel_size["Screens"] = f();
        else if (k.size() > 12 && k.compare(0, 7, "VIRTUAL") == 0 && ends_with(k, "_SIZE") && f() > 0.05f)
            s.panel_size["Virtual " + k.substr(7, k.size() - 12)] = f();
        else if (!k.empty()) fwlog("settings: unknown key %s", k.c_str());
    }
    s.place.meters_1920 = 1.25f * size;  // SIZE=1: a 1080p monitor is 1.25 m wide
}

// The settings file's modification time, to notice edits.
long long settings_mtime() {
    struct stat st;
    return stat(settings_path().c_str(), &st) == 0 ? (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec : 0;
}

// Places the panels from the settings' placement and sizes.
void place_overlays(const Wall &wall, const Settings &s, const std::vector<vr::VROverlayHandle_t> &overlays) {
    std::vector<float> scales;
    for (const auto &p : wall.panels) {
        auto it = s.panel_size.find(p.name);
        scales.push_back(it != s.panel_size.end() ? it->second : 1.0f);
    }
    const auto poses = place_panels(wall, s.place, scales);
    for (size_t i = 0; i < overlays.size() && i < poses.size(); i++) {
        vr::VROverlay()->SetOverlayWidthInMeters(overlays[i], poses[i].width_m);
        vr::VROverlay()->SetOverlayTransformAbsolute(overlays[i], vr::TrackingUniverseSeated, &poses[i].transform);
    }
}

bool parse_args(int argc, char **argv, Settings &s) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") s.list = true;
        else if (a == "--quit") s.quit = true;
        else if (a == "--host") s.host = next();
        else if (a == "--bitrate") s.bitrate_kbps = std::atoi(next().c_str());
        else if (a == "--seconds") s.seconds = std::atoi(next().c_str());
        else if (a == "--dump") s.dump = next();
        else if (a == "--window") s.window = 1;
        else if (a == "--no-window") s.window = 0;
        else if (a == "--no-audio") s.audio = false;
        else if (a == "--no-quit") s.quit_on_exit = false;
        else if (a == "-h" || a == "--help") {
            std::printf("usage: framewall-vr [APP] [--list] [--quit] [--host NAME] [--bitrate KBPS] [--seconds N]\n"
                        "                    [--window|--no-window] [--no-audio] [--no-quit] [--dump FILE.ppm]\n");
            std::exit(0);
        } else if (!a.empty() && a[0] != '-') s.app = a;
        else { fwlog("unknown option %s", a.c_str()); return false; }
    }
    return true;
}

// ---- host and app -------------------------------------------------------------

std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}


std::unique_ptr<NvHttp> find_host(const MoonlightConf &mc, const std::string &want, std::string &name, std::string &err) {
    std::vector<std::string> tried;
    for (const auto &h : mc.hosts) {
        if (want.empty() ? !ends_with(h.hostname, " wall") : (h.hostname != want && lower(h.uuid) != lower(want))) continue;
        if (h.srvcert.empty()) { tried.push_back(h.hostname + " (not paired)"); continue; }
        for (const auto &[addr, port] : h.addresses) {
            auto http = std::make_unique<NvHttp>(addr, port, mc.certificate, mc.key, mc.uniqueid, h.srvcert);
            NvServerInfo info;
            std::string e;
            if (!http->server_info_http(info, e, 1500)) { tried.push_back(h.hostname + " at " + addr + ":" + std::to_string(port) + " (unreachable)"); continue; }
            // Moonlight may remember an older host at the same address.
            if (lower(info.uniqueid) != lower(h.uuid)) { tried.push_back(h.hostname + " at " + addr + " (a different host answers there)"); continue; }
            name = h.hostname;
            return http;
        }
    }
    err = want.empty() ? "no reachable framewall host (a Moonlight host whose name ends in \" wall\")"
                       : "host \"" + want + "\" isn't known to Moonlight or isn't reachable";
    if (!tried.empty()) {
        err += "; tried:";
        for (auto &t : tried) err += " " + t + ";";
    }
    return nullptr;
}

// Default preset: a side-by-side ("row") wall at the headset's refresh rate,
// else the closest refresh rate, else any wall.
const NvApp *pick_app(const std::vector<NvApp> &apps, const std::string &want, float hmd_hz, std::string &err) {
    if (!want.empty()) {
        for (auto &a : apps) if (a.title == want) return &a;
        for (auto &a : apps) if (a.title.find(want) != std::string::npos) return &a;
        err = "the host has no app matching \"" + want + "\" (see --list)";
        return nullptr;
    }
    const NvApp *best = nullptr;
    double best_score = 1e9;
    for (auto &a : apps) {
        Wall w;
        std::string e;
        if (!parse_wall(a.title, w, e)) continue;
        const double score = (w.shape == "row" ? 0 : 1000) + std::fabs(w.hz - hmd_hz);
        if (score < best_score) { best_score = score; best = &a; }
    }
    if (!best) err = "the host offers no wall presets (update framewall on the host?)";
    return best;
}

// ---- moonlight-common-c callbacks ---------------------------------------------

int dr_setup(int format, int width, int height, int fps, void *ctx, int) {
    fwlog("video: stream %dx%d@%d, format 0x%x", width, height, fps, format);
    std::string err;
    if (!static_cast<VideoPipeline *>(ctx)->start(width, height, err)) {
        fwlog("video: %s", err.c_str());
        return -1;
    }
    return 0;
}
void dr_cleanup() { if (g_video) g_video->stop(); }
int dr_submit(PDECODE_UNIT du) { return g_video ? g_video->submit(du) : DR_OK; }

void cl_stage_failed(int stage, int err) { fwlog("connection: %s failed (error %d)", LiGetStageName(stage), err); }
void cl_started() { fwlog("connection: started"); g_status = "streaming"; }
void cl_terminated(int err) {
    fwlog("connection: ended (%s, %d)", err == ML_ERROR_GRACEFUL_TERMINATION ? "host closed the stream" : "error", err);
    g_terminated = err == 1 ? 2 : err;
    g_stop = true;
}
void cl_log(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = std::strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    fwlog("moonlight: %s", buf);
}
void cl_rumble(unsigned short num, unsigned short low, unsigned short high) {
    if (num >= 4) return;
    g_rumble[num].value = (uint32_t(low) << 16) | high;
    g_rumble[num].dirty = true;
}
void cl_status(int status) { fwlog("connection: %s", status == CONN_STATUS_POOR ? "poor (packet loss or high latency)" : "okay"); }

// ---- gamepads (SDL) -----------------------------------------------------------

struct Pad { SDL_Gamepad *g = nullptr; SDL_JoystickID id = 0; };
Pad g_pads[4];

uint16_t active_mask() {
    uint16_t m = 0;
    for (int i = 0; i < 4; i++) if (g_pads[i].g) m |= uint16_t(1 << i);
    return m;
}

void send_pad(int i) {
    SDL_Gamepad *g = g_pads[i].g;
    if (!g) { LiSendMultiControllerEvent(short(i), short(active_mask()), 0, 0, 0, 0, 0, 0, 0); return; }
    static const std::pair<SDL_GamepadButton, int> map[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, A_FLAG}, {SDL_GAMEPAD_BUTTON_EAST, B_FLAG},
        {SDL_GAMEPAD_BUTTON_WEST, X_FLAG}, {SDL_GAMEPAD_BUTTON_NORTH, Y_FLAG},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, UP_FLAG}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DOWN_FLAG},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, LEFT_FLAG}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RIGHT_FLAG},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, LB_FLAG}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, RB_FLAG},
        {SDL_GAMEPAD_BUTTON_START, PLAY_FLAG}, {SDL_GAMEPAD_BUTTON_BACK, BACK_FLAG},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, LS_CLK_FLAG}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, RS_CLK_FLAG},
        {SDL_GAMEPAD_BUTTON_GUIDE, SPECIAL_FLAG},
    };
    int buttons = 0;
    for (auto &[b, f] : map) if (SDL_GetGamepadButton(g, b)) buttons |= f;
    auto axis = [&](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(g, a)); };
    auto trig = [&](SDL_GamepadAxis a) { return (unsigned char)std::clamp(axis(a) * 255 / 32767, 0, 255); };
    auto inv = [&](SDL_GamepadAxis a) { return short(std::clamp(-axis(a), -32768, 32767)); };  // GameStream: up = +
    LiSendMultiControllerEvent(short(i), short(active_mask()), buttons,
                               trig(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), trig(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER),
                               short(axis(SDL_GAMEPAD_AXIS_LEFTX)), inv(SDL_GAMEPAD_AXIS_LEFTY),
                               short(axis(SDL_GAMEPAD_AXIS_RIGHTX)), inv(SDL_GAMEPAD_AXIS_RIGHTY));
}

int pad_index(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++) if (g_pads[i].g && g_pads[i].id == id) return i;
    return -1;
}

void handle_pad_event(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
        if (pad_index(e.gdevice.which) >= 0) return;
        int slot = -1;
        for (int i = 0; i < 4; i++) if (!g_pads[i].g) { slot = i; break; }
        if (slot < 0) return;
        SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which);
        if (!g) return;
        g_pads[slot] = {g, e.gdevice.which};
        fwlog("gamepad %d: %s", slot, SDL_GetGamepadName(g));
        LiSendControllerArrivalEvent(uint8_t(slot), active_mask(), LI_CTYPE_XBOX,
                                     A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG |
                                         LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | SPECIAL_FLAG,
                                     LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE);
        send_pad(slot);
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        const int i = pad_index(e.gdevice.which);
        if (i < 0) return;
        SDL_CloseGamepad(g_pads[i].g);
        g_pads[i] = {};
        fwlog("gamepad %d: removed", i);
        send_pad(i);
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const int i = pad_index(e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION ? e.gaxis.which : e.gbutton.which);
        if (i >= 0) send_pad(i);
        break;
    }
    default: break;
    }
}

void apply_rumble() {
    for (int i = 0; i < 4; i++) {
        if (!g_rumble[i].dirty.exchange(false) || !g_pads[i].g) continue;
        const uint32_t v = g_rumble[i].value;
        SDL_RumbleGamepad(g_pads[i].g, uint16_t(v >> 16), uint16_t(v), v ? 10000 : 0);
    }
}

void draw_window(SDL_Renderer *r, const std::string &app) {
    SDL_SetRenderDrawColor(r, 16, 18, 24, 255);
    SDL_RenderClear(r);
    SDL_SetRenderScale(r, 2, 2);
    SDL_SetRenderDrawColor(r, 230, 232, 240, 255);
    SDL_RenderDebugText(r, 8, 8, "framewall");
    SDL_SetRenderDrawColor(r, 160, 168, 184, 255);
    char line[160];
    std::snprintf(line, sizeof line, "%s%s", g_status.load(),
                  g_video && g_video->stats.fps ? (", " + std::to_string(g_video->stats.fps.load()) + " fps").c_str() : "");
    SDL_RenderDebugText(r, 8, 24, line);
    SDL_RenderDebugText(r, 8, 36, app.substr(0, 48).c_str());
    std::snprintf(line, sizeof line, "gamepads: %d", __builtin_popcount(active_mask()));
    SDL_RenderDebugText(r, 8, 48, line);
    SDL_RenderDebugText(r, 8, 64, "Close this to stop the stream.");
    SDL_SetRenderScale(r, 1, 1);
    SDL_RenderPresent(r);
}

void on_signal(int) { g_stop = true; }

}  // namespace

int main(int argc, char **argv) {
    setvbuf(stderr, nullptr, _IOLBF, 0);
    Settings s;
    load_settings(s);
    if (!parse_args(argc, argv, s)) return 2;
    if (s.window < 0) s.window = (std::getenv("SteamGameId") || std::getenv("SteamAppId")) ? 1 : 0;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGHUP, on_signal);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    MoonlightConf mc;
    std::string err, host_name;
    if (!mc.load(err)) { fwlog("%s", err.c_str()); return 1; }
    auto http = find_host(mc, s.host, host_name, err);
    if (!http) { fwlog("%s", err.c_str()); return 1; }
    NvServerInfo info;
    if (!http->server_info(info, err)) { fwlog("%s: %s", host_name.c_str(), err.c_str()); return 1; }
    if (!info.paired) { fwlog("%s: not paired with this headset; pair it in Moonlight", host_name.c_str()); return 1; }
    std::vector<NvApp> apps;
    if (!http->app_list(apps, err)) { fwlog("%s: %s", host_name.c_str(), err.c_str()); return 1; }
    if (s.list) {
        for (auto &a : apps) std::printf("%s\n", a.title.c_str());
        return 0;
    }
    if (s.quit) {
        if (!info.current_game) { fwlog("host: no wall is running"); return 0; }
        if (!http->quit(err)) { fwlog("host: quit failed: %s", err.c_str()); return 1; }
        fwlog("host: wall closed");
        return 0;
    }
    fwlog("host: %s at %s (Sunshine %s)", host_name.c_str(), http->address().c_str(), info.appversion.c_str());
    if (!(info.codec_modes & SCM_HEVC)) { fwlog("the host can't encode HEVC"); return 1; }

    // SDL before anything that might fail later, so errors can be shown.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_InitFlags sdl_flags = s.audio ? SDL_INIT_AUDIO : 0;
    // Only open a window (and take gamepads) when Steam launched us: Steam
    // gives controller input to the focused window of the game it launched.
    // From a shell (e.g. ssh) there is no window, so nothing is drawn into the
    // headset's session.
    if (s.window) sdl_flags |= SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
    if (sdl_flags && !SDL_Init(sdl_flags)) {
        fwlog("SDL: %s", SDL_GetError());
        if (s.window) { s.window = 0; SDL_Init(s.audio ? SDL_INIT_AUDIO : 0); }
    }

    vr::EVRInitError verr = vr::VRInitError_None;
    vr::VR_Init(&verr, vr::VRApplication_Overlay);
    if (verr != vr::VRInitError_None) { fwlog("SteamVR: %s", vr::VR_GetVRInitErrorAsEnglishDescription(verr)); return 1; }
    float hmd_hz = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float);
    if (!(hmd_hz > 1)) hmd_hz = 90;

    const NvApp *app = pick_app(apps, s.app, hmd_hz, err);
    Wall wall;
    if (!app || !parse_wall(app->title, wall, err)) { fwlog("%s", err.c_str()); vr::VR_Shutdown(); return 1; }
    fwlog("wall: \"%s\", %zu panels, headset at %.0f Hz", app->title.c_str(), wall.panels.size(), hmd_hz);

    // One overlay per monitor, all showing the same texture with different
    // bounds. Not forced interactive: that would take the controllers away
    // from gamepad mode (the laser reaches them with the Steam menu open).
    VideoPipeline::Options vopt;
    for (size_t i = 0; i < wall.panels.size(); i++) {
        vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
        const std::string key = "framewall." + std::to_string(getpid()) + "." + std::to_string(i);
        if (vr::VROverlay()->CreateOverlay(key.c_str(), wall.panels[i].name.c_str(), &h) != vr::VROverlayError_None) {
            fwlog("SteamVR: can't create overlay %s", key.c_str());
            vr::VR_Shutdown();
            return 1;
        }
        // Above other world overlays, e.g. the theater screen Steam makes for
        // our status window, which could otherwise cover the middle panel.
        vr::VROverlay()->SetOverlaySortOrder(h, 10);
        vopt.overlays.push_back(h);
        vopt.regions.push_back(wall.panels[i].src);
        const Rect &r = wall.panels[i].src;
        fwlog("panel %zu \"%s\": stream %dx%d+%d+%d", i, wall.panels[i].name.c_str(), r.w, r.h, r.x, r.y);
    }
    vopt.dump_path = s.dump;
    VideoPipeline video(vopt);
    g_video = &video;
    place_overlays(wall, s, vopt.overlays);
    long long conf_mtime = settings_mtime();
    uint64_t last_conf_check = 0;

    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    if (s.window) {
        win = SDL_CreateWindow("framewall", 640, 176, 0);
        // Software rendering: keeps SDL off the GPU and away from our EGL context.
        ren = win ? SDL_CreateRenderer(win, "software") : nullptr;
        if (!ren) fwlog("SDL window: %s", SDL_GetError());
    }

    // Start (or resume) the wall on the host.
    unsigned char rikey[16], riiv[4];
    RAND_bytes(rikey, sizeof rikey);
    RAND_bytes(riiv, sizeof riiv);
    const int rikeyid = int((uint32_t(riiv[0]) << 24) | (uint32_t(riiv[1]) << 16) | (uint32_t(riiv[2]) << 8) | riiv[3]);
    if (info.current_game && info.current_game != app->id) {
        fwlog("host: another app is running; quitting it first");
        if (!http->quit(err)) fwlog("host: quit failed: %s", err.c_str());
        info.current_game = 0;
    }
    const bool resume = info.current_game == app->id;
    g_status = resume ? "resuming the wall" : "starting the wall";
    if (ren) draw_window(ren, app->title);
    std::string rtsp;
    if (!http->launch(resume, app->id, wall.width, wall.height, wall.hz, AUDIO_CONFIGURATION_STEREO, rikey, rikeyid, rtsp, err)) {
        fwlog("host: %s failed: %s", resume ? "resume" : "launch", err.c_str());
        vr::VR_Shutdown();
        return 1;
    }

    STREAM_CONFIGURATION sc;
    LiInitializeStreamConfiguration(&sc);
    sc.width = wall.width;
    sc.height = wall.height;
    sc.fps = wall.hz;
    // Moonlight uses ~80 Mbps for 4K60; scale by pixel rate, cap at 150 Mbps.
    const double rate = double(wall.width) * wall.height * wall.hz / (3840.0 * 2160 * 60);
    sc.bitrate = s.bitrate_kbps > 0 ? s.bitrate_kbps : std::clamp(int(80000 * rate), 20000, 150000);
    sc.packetSize = 1392;
    sc.streamingRemotely = STREAM_CFG_AUTO;
    sc.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    sc.supportedVideoFormats = VIDEO_FORMAT_H265;
    sc.clientRefreshRateX100 = int(hmd_hz * 100);
    sc.colorSpace = COLORSPACE_REC_709;
    sc.colorRange = COLOR_RANGE_LIMITED;
    sc.encryptionFlags = ENCFLG_AUDIO;
    std::memcpy(sc.remoteInputAesKey, rikey, 16);
    std::memset(sc.remoteInputAesIv, 0, 16);
    std::memcpy(sc.remoteInputAesIv, riiv, 4);

    SERVER_INFORMATION si;
    LiInitializeServerInformation(&si);
    si.address = http->address().c_str();
    si.serverInfoAppVersion = info.appversion.c_str();
    si.serverInfoGfeVersion = info.gfeversion.c_str();
    si.rtspSessionUrl = rtsp.empty() ? nullptr : rtsp.c_str();
    si.serverCodecModeSupport = info.codec_modes;

    CONNECTION_LISTENER_CALLBACKS cl;
    LiInitializeConnectionCallbacks(&cl);
    cl.stageFailed = cl_stage_failed;
    cl.connectionStarted = cl_started;
    cl.connectionTerminated = cl_terminated;
    cl.logMessage = cl_log;
    cl.rumble = cl_rumble;
    cl.connectionStatusUpdate = cl_status;
    DECODER_RENDERER_CALLBACKS dr;
    LiInitializeVideoCallbacks(&dr);
    dr.setup = dr_setup;
    dr.cleanup = dr_cleanup;
    dr.submitDecodeUnit = dr_submit;
    dr.capabilities = 0;
    AUDIO_RENDERER_CALLBACKS ar;
    LiInitializeAudioCallbacks(&ar);
    if (s.audio) ar = audio_callbacks();

    fwlog("connecting: %dx%d@%d, %d kbps%s", sc.width, sc.height, sc.fps, sc.bitrate, s.audio ? "" : ", no audio");
    g_status = "connecting";
    if (LiStartConnection(&si, &sc, &cl, &dr, &ar, &video, 0, nullptr, 0) != 0) {
        fwlog("connection failed");
        if (s.quit_on_exit && !http->quit(err)) fwlog("host: quit failed: %s", err.c_str());
        vr::VR_Shutdown();
        return 1;
    }

    const auto t0 = SDL_GetTicks();
    uint64_t last_draw = 0;
    bool checked_visible = false;
    while (!g_stop) {
        if (s.window) {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 20)) {
                do {
                    if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) g_stop = true;
                    else handle_pad_event(e);
                } while (SDL_PollEvent(&e));
            }
            apply_rumble();
            if (ren && SDL_GetTicks() - last_draw > 500) { draw_window(ren, app->title); last_draw = SDL_GetTicks(); }
        } else {
            usleep(20000);
        }
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
            if (ev.eventType == vr::VREvent_Quit) {
                fwlog("SteamVR is quitting");
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                g_stop = true;
            }
        }
        // Settings file saved: move the panels to match.
        if (SDL_GetTicks() - last_conf_check > 500) {
            last_conf_check = SDL_GetTicks();
            const long long m = settings_mtime();
            if (m && m != conf_mtime) {
                conf_mtime = m;
                Settings fresh;
                load_settings(fresh);
                s.place = fresh.place;
                s.panel_size = fresh.panel_size;
                place_overlays(wall, s, vopt.overlays);
                fwlog("settings: panels updated");
            }
        }
        if (!checked_visible && video.stats.shown > 0 && SDL_GetTicks() - t0 > 3000) {
            checked_visible = true;
            int visible = 0;
            for (auto h : vopt.overlays) visible += vr::VROverlay()->IsOverlayVisible(h) ? 1 : 0;
            fwlog("panels visible in SteamVR: %d of %zu", visible, vopt.overlays.size());
        }
        if (s.seconds > 0 && SDL_GetTicks() - t0 > uint64_t(s.seconds) * 1000) g_stop = true;
    }

    fwlog("stopping");
    g_status = "stopping";
    if (ren) draw_window(ren, app->title);
    LiStopConnection();
    g_video = nullptr;
    // Like Moonlight's --quit-after: quitting the app makes the host remove
    // the virtual monitors and restore the screen layout.
    if (s.quit_on_exit) {
        if (http->quit(err)) fwlog("host: wall closed");
        else fwlog("host: quit failed: %s", err.c_str());
    }
    for (auto h : vopt.overlays) vr::VROverlay()->DestroyOverlay(h);
    for (auto &p : g_pads) if (p.g) SDL_CloseGamepad(p.g);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    vr::VR_Shutdown();
    SDL_Quit();
    curl_global_cleanup();
    return g_terminated == 1 || g_terminated == ML_ERROR_GRACEFUL_TERMINATION ? 0 : 1;
}
