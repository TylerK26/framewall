// framewall-vr: show a framewall monitor wall in VR as separate panels side
// by side around you.
//
// A native OpenXR app for the Steam Frame. It connects to the framewall host
// (Sunshine) like Moonlight does, reusing Moonlight's pairing, decodes the one
// wall stream on the headset's hardware decoder and shows each monitor of the
// wall as its own curved panel (an OpenXR cylinder layer), over a dark void.
// Mouse and keyboard stay on the PC.
//
//   framewall-vr                 stream the default preset (see below)
//   framewall-vr "APP"           stream this wall preset (app name, or part of one)
//   framewall-vr --list          list the host's apps
//   framewall-vr --quit          close the wall on the host (removes the virtual monitors)
//
//   options: --host NAME  --bitrate KBPS  --seconds N  --no-audio  --no-quit
//            --passthrough (the room instead of the void)
//
// Controllers (see pointer.h): a grip toggles the laser on that controller;
// trigger or bumper = left click, stick click = right click, stick = scroll.
// Both grips together open the menu (menu.h): passthrough, recenter, panel
// size, stream info, quit.
//
// Settings: ~/.config/framewall-vr.conf (KEY=VALUE, see framewall-vr.conf.example),
// created on the first run. Panel sizes and placement apply as soon as the file
// is saved, while streaming, or live from the panel editor on the PC
// (host/framewall-panels, see control.h), which saves them into the file.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "audio.h"
#include "control.h"
#include "layout.h"
#include "log.h"
#include "menu.h"
#include "moonlight_conf.h"
#include "nvhttp.h"
#include "pointer.h"
#include "settings.h"
#include "video.h"
#include "wallrender.h"
#include "xr.h"

#include <Limelight.h>
#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using clk = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};
std::atomic<int> g_terminated{1};  // 1 = still running, else the error code
std::atomic<bool> g_poor{false};   // the connection is losing packets
std::atomic<uint32_t> g_rumble{0}; // the PC's rumble for our pad: low << 16 | high
VideoPipeline *g_video = nullptr;

uint64_t now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(clk::now().time_since_epoch()).count());
}

// ---- settings -----------------------------------------------------------------

bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<PanelPose> place(const Wall &wall, const Settings &s) {
    std::vector<float> scales;
    for (const auto &p : wall.panels) scales.push_back(panel_scale(s, p.name));
    return place_panels(wall, s.place, scales);
}

bool parse_args(int argc, char **argv, Settings &s, bool &passthrough) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--list") s.list = true;
        else if (a == "--quit") s.quit = true;
        else if (a == "--host") s.host = next();
        else if (a == "--bitrate") s.bitrate_kbps = std::atoi(next().c_str());
        else if (a == "--seconds") s.seconds = std::atoi(next().c_str());
        else if (a == "--no-audio") s.audio = false;
        else if (a == "--no-quit") s.quit_on_exit = false;
        else if (a == "--passthrough") passthrough = true;
        else if (a == "-h" || a == "--help") {
            std::printf("usage: framewall-vr [APP] [--list] [--quit] [--host NAME] [--bitrate KBPS] [--seconds N]\n"
                        "                    [--no-audio] [--no-quit] [--passthrough]\n");
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
void cl_started() { fwlog("connection: started"); }
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
void cl_status(int status) {
    g_poor = status == CONN_STATUS_POOR;
    fwlog("connection: %s", status == CONN_STATUS_POOR ? "poor (packet loss or high latency)" : "okay");
}

void cl_rumble(unsigned short num, unsigned short low, unsigned short high) {
    if (num == 0) g_rumble = (uint32_t(low) << 16) | high;
}

void on_signal(int) { g_stop = true; }

// Frame pacing and conversion cost, logged every 5 s.
struct FrameStats {
    uint64_t frames = 0, missed = 0, converted = 0;
    double gpu_sum = 0, gpu_max = 0, lat_sum = 0;
    XrTime last_display = 0;
    clk::time_point since = clk::now();
    // The last logged interval, for the menu.
    struct Last { double fps = 0, shown = 0, latency = 0; uint64_t missed = 0; } last;

    void frame(const XrFrameState &fs) {
        frames++;
        if (last_display && fs.predictedDisplayTime - last_display > fs.predictedDisplayPeriod * 3 / 2)
            missed += uint64_t((fs.predictedDisplayTime - last_display + fs.predictedDisplayPeriod / 2) / fs.predictedDisplayPeriod) - 1;
        last_display = fs.predictedDisplayTime;
    }
    void maybe_log(const VideoPipeline &v, WallRenderer &r) {
        const auto now = clk::now();
        if (now - since < std::chrono::seconds(5)) return;
        const WallRenderer::Stats rs = r.take_stats();
        converted = rs.frames;
        gpu_sum = rs.gpu_sum;
        gpu_max = rs.gpu_max;
        lat_sum = rs.latency_sum;
        const double secs = std::chrono::duration<double>(now - since).count();
        fwlog("xr: %.0f fps, %llu missed | video: %.0f fps shown, decoded->on wall %.1f ms, convert gpu %.2f avg / %.2f max "
              "(received %llu, decoded %llu, errors %llu, dropped %llu)",
              frames / secs, (unsigned long long)missed, converted / secs, converted ? lat_sum / converted : 0.0,
              converted ? gpu_sum / converted : 0.0, gpu_max, (unsigned long long)v.stats.received.load(),
              (unsigned long long)v.stats.decoded.load(), (unsigned long long)v.stats.errors.load(),
              (unsigned long long)v.stats.dropped.load());
        const XrTime keep = last_display;
        const Last l{frames / secs, converted / secs, converted ? lat_sum / converted : 0.0, missed};
        *this = FrameStats{};
        last_display = keep;
        last = l;
    }
};

}  // namespace

int main(int argc, char **argv) {
    setvbuf(stderr, nullptr, _IOLBF, 0);
    Settings s;
    load_settings(s);
    bool passthrough = false;
    if (!parse_args(argc, argv, s, passthrough)) return 2;
    passthrough = passthrough || s.passthrough;
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

    if (s.audio && !SDL_Init(SDL_INIT_AUDIO)) {
        fwlog("SDL audio: %s; continuing without sound", SDL_GetError());
        s.audio = false;
    }

    Xr xr;
    if (!xr.init(err)) { fwlog("xr: %s", err.c_str()); return 1; }
    if (passthrough && !xr.passthrough_supported()) { fwlog("xr: this headset offers no passthrough"); passthrough = false; }
    float hmd_hz = xr.refresh_rate();
    if (!(hmd_hz > 1)) hmd_hz = 90;

    const NvApp *app = pick_app(apps, s.app, hmd_hz, err);
    Wall wall;
    if (!app || !parse_wall(app->title, wall, err)) { fwlog("%s", err.c_str()); return 1; }
    fwlog("wall: \"%s\", %zu panels, headset at %.0f Hz", app->title.c_str(), wall.panels.size(), hmd_hz);
    if (std::fabs(wall.hz - hmd_hz) > 1) xr.request_refresh_rate(float(wall.hz));
    std::vector<Rect> regions;
    for (size_t i = 0; i < wall.panels.size(); i++) {
        regions.push_back(wall.panels[i].src);
        const Rect &r = wall.panels[i].src;
        fwlog("panel %zu \"%s\": stream %dx%d+%d+%d", i, wall.panels[i].name.c_str(), r.w, r.h, r.x, r.y);
    }

    WallRenderer renderer(xr);
    if (!renderer.init(wall.width, wall.height, err)) { fwlog("wall: %s", err.c_str()); return 1; }
    Pointer pointer(xr);
    if (!pointer.init(err)) { fwlog("pointer: %s", err.c_str()); return 1; }
    Menu menu(xr);
    if (!menu.init(err)) { fwlog("menu: %s", err.c_str()); return 1; }
    // Absolute positions in the wall stream: Sunshine maps them onto the
    // captured region, so the PC's cursor lands under the laser's dot.
    const Pointer::Mouse mouse{
        [&](int x, int y) { LiSendMousePositionEvent(short(x), short(y), short(wall.width), short(wall.height)); },
        [](bool right, bool down) {
            LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, right ? BUTTON_RIGHT : BUTTON_LEFT);
        },
        [](int vertical, int horizontal) {
            if (vertical) LiSendHighResScrollEvent(short(vertical));
            if (horizontal) LiSendHighResHScrollEvent(short(horizontal));
        }};
    VideoPipeline video(VideoPipeline::Options{});
    g_video = &video;

    std::vector<PanelPose> poses = place(wall, s);
    long long conf_mtime = settings_mtime();
    uint64_t last_conf_check = 0;
    // The panel editor on the PC: Sunshine's port + 5 at the host's address.
    ControlLink editor(http->address(), http->http_port() + 5);
    std::map<std::string, std::string> unsaved;  // editor changes not yet in the file
    uint64_t last_edit = 0;
    auto save_edits = [&] {
        if (unsaved.empty()) return;
        if (save_settings(unsaved)) fwlog("settings: saved the panel editor's changes");
        unsaved.clear();
        conf_mtime = settings_mtime();  // our own write: no reload
    };

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
    std::string rtsp;
    if (!http->launch(resume, app->id, wall.width, wall.height, wall.hz, AUDIO_CONFIGURATION_STEREO, rikey, rikeyid, rtsp, err)) {
        fwlog("host: %s failed: %s", resume ? "resume" : "launch", err.c_str());
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
    cl.connectionStatusUpdate = cl_status;
    cl.rumble = cl_rumble;
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
    if (LiStartConnection(&si, &sc, &cl, &dr, &ar, &video, 0, nullptr, 0) != 0) {
        fwlog("connection failed");
        if (s.quit_on_exit && !http->quit(err)) fwlog("host: quit failed: %s", err.c_str());
        return 1;
    }

    // The controllers, as one Xbox controller (sent while the laser is off).
    LiSendControllerArrivalEvent(0, 1, LI_CTYPE_XBOX,
                                 A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG |
                                     RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG,
                                 LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE);
    Pointer::Pad sent_pad;
    uint64_t pad_sent_ms = 0;

    // ---- the frame loop ----
    // Frames go onto the wall as soon as they're decoded, on the renderer's
    // thread; this loop just hands SteamVR the layers every display frame.
    renderer.start(video);
    const uint64_t t0 = now_ms();
    bool exit_requested = false;
    FrameStats stats;
    std::vector<XrCompositionLayerBaseHeader *> layers;
    uint64_t last_info = 0;
    while (xr.poll()) {
        if (g_stop && !exit_requested) {
            exit_requested = true;
            xr.request_exit();
        }
        // The panel editor: send it the settings when it connects, and move
        // the panels as its sliders move. Its changes go into the settings
        // file once they settle.
        {
            bool fresh = false;
            std::map<std::string, std::string> sets;
            editor.poll(fresh, sets);
            if (fresh) editor.send(editor_snapshot(app->title, wall, s));
            bool moved = false;
            for (auto &[k, v] : sets) {
                if (set_panel_setting(s, k, v)) { unsaved[k] = v; moved = true; }
                else fwlog("panel editor: ignoring %s %s", k.c_str(), v.c_str());
            }
            if (moved) { poses = place(wall, s); last_edit = now_ms(); }
            if (!unsaved.empty() && now_ms() - last_edit > 1000) save_edits();
        }
        // Settings file saved: move the panels to match.
        if (unsaved.empty() && now_ms() - last_conf_check > 500) {
            last_conf_check = now_ms();
            const long long m = settings_mtime();
            if (m && m != conf_mtime) {
                conf_mtime = m;
                Settings fresh;
                load_settings(fresh);
                s.size = fresh.size;
                s.place = fresh.place;
                s.panel_size = fresh.panel_size;
                poses = place(wall, s);
                editor.send(editor_snapshot(app->title, wall, s));
                fwlog("settings: panels updated");
            }
        }
        if (s.seconds > 0 && now_ms() - t0 > uint64_t(s.seconds) * 1000) g_stop = true;

        if (!xr.running()) {
            usleep(10000);
            continue;
        }
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(xr.session, nullptr, &fs))) { usleep(10000); continue; }
        stats.frame(fs);
        if (pointer.update(fs.predictedDisplayTime, poses, regions, s.place.radius, mouse, menu)) {
            if (menu.is_open()) menu.close();
            else { menu.open(fs.predictedDisplayTime); last_info = 0; }
        }
        // The gamepad: on change, and every 100 ms so Sunshine never thinks
        // it's gone quiet.
        if (!(pointer.pad() == sent_pad) || now_ms() - pad_sent_ms > 100) {
            const Pointer::Pad &p = pointer.pad();
            LiSendMultiControllerEvent(0, 1, p.buttons, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry);
            sent_pad = p;
            pad_sent_ms = now_ms();
        }
        {
            const uint32_t r = g_rumble;
            pointer.rumble(float(r >> 16) / 65535.0f, float(r & 0xffff) / 65535.0f);
        }
        const Menu::Action act = menu.take_action();
        switch (act) {
        case Menu::Action::TogglePassthrough:
            if (xr.passthrough_supported()) {
                passthrough = !passthrough;
                s.passthrough = passthrough;
                save_settings({{"PASSTHROUGH", passthrough ? "1" : "0"}});
                conf_mtime = settings_mtime();  // our own write: no reload
                fwlog("menu: passthrough %s", passthrough ? "on" : "off");
            }
            break;
        case Menu::Action::Recenter:
            xr.recenter(fs.predictedDisplayTime);
            menu.close();
            break;
        case Menu::Action::Smaller:
        case Menu::Action::Bigger: {
            // Every panel at once, 10% a step; saved like the PC editor's changes.
            const float size = std::clamp(s.size * (act == Menu::Action::Bigger ? 1.1f : 1 / 1.1f), 0.3f, 3.0f);
            char v[16];
            std::snprintf(v, sizeof v, "%.2f", size);
            if (set_panel_setting(s, "SIZE", v)) {
                unsaved["SIZE"] = v;
                last_edit = now_ms();
                poses = place(wall, s);
                editor.send(editor_snapshot(app->title, wall, s));
                last_info = 0;
            }
            break;
        }
        case Menu::Action::Quit:
            fwlog("menu: quit");
            menu.close();
            g_stop = true;
            break;
        case Menu::Action::Close: menu.close(); break;
        case Menu::Action::None: break;
        }
        if (menu.is_open() && now_ms() - last_info > 500) {
            last_info = now_ms();
            char l1[64], l2[64], l3[64], l4[64];
            std::snprintf(l1, sizeof l1, "Wall %dx%d @ %d Hz", wall.width, wall.height, wall.hz);
            std::snprintf(l2, sizeof l2, "Video %.0f fps, headset %.0f fps", stats.last.shown, stats.last.fps);
            std::snprintf(l3, sizeof l3, "Decoded->wall %.1f ms, missed %llu", stats.last.latency,
                          (unsigned long long)stats.last.missed);
            std::snprintf(l4, sizeof l4, "Network %s, %d Mbps", g_poor ? "POOR (packet loss)" : "ok", sc.bitrate / 1000);
            menu.set_state(passthrough, s.size, {l1, l2, l3, l4});
        }
        menu.prepare();
        const XrEnvironmentBlendMode blend = passthrough ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        XrResult r;
        {
            std::lock_guard<std::mutex> lk(xr.queue_mu);
            xrBeginFrame(xr.session, nullptr);
            layers.clear();
            if (fs.shouldRender) {
                renderer.layers(fs.predictedDisplayTime, passthrough, poses, regions, s.place.radius, layers);
                menu.layers(layers);
                pointer.layers(layers);
            }
            XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
            fe.displayTime = fs.predictedDisplayTime;
            fe.environmentBlendMode = blend;
            fe.layerCount = uint32_t(layers.size());
            fe.layers = layers.data();
            r = xrEndFrame(xr.session, &fe);
        }
        if (XR_FAILED(r)) fwlog("xr: xrEndFrame: %s", xr_result_name(xr.instance, r).c_str());
        stats.maybe_log(video, renderer);
    }

    fwlog("stopping");
    renderer.stop();
    save_edits();
    LiStopConnection();
    g_video = nullptr;
    // Like Moonlight's --quit-after: quitting the app makes the host remove
    // the virtual monitors and restore the screen layout.
    if (s.quit_on_exit) {
        if (http->quit(err)) fwlog("host: wall closed");
        else fwlog("host: quit failed: %s", err.c_str());
    }
    if (s.audio) SDL_Quit();
    curl_global_cleanup();
    return g_terminated == 1 || g_terminated == ML_ERROR_GRACEFUL_TERMINATION ? 0 : 1;
}
