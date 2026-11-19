// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "menu.h"

#include "log.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstring>

namespace {

constexpr int kW = 640, kH = 584;       // pixels
constexpr float kWidthM = 0.44f;        // m
constexpr float kDistance = 0.65f;      // m in front of the head
constexpr float kDrop = 0.12f;          // m below eye level
constexpr uint64_t kQuitArmMs = 3000;   // second pull must come within this

uint64_t now_ms() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

XrVector3f rotate(XrQuaternionf q, XrVector3f v) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float tx = 2 * (y * v.z - z * v.y), ty = 2 * (z * v.x - x * v.z), tz = 2 * (x * v.y - y * v.x);
    return {v.x + w * tx + (y * tz - z * ty), v.y + w * ty + (z * tx - x * tz), v.z + w * tz + (x * ty - y * tx)};
}

}  // namespace

Menu::~Menu() {
    if (ren_) SDL_DestroyRenderer(ren_);
    if (surface_) SDL_DestroySurface(surface_);
    if (img_.handle) xr_.destroy_swapchain(img_);
}

bool Menu::init(std::string &err) {
    surface_ = SDL_CreateSurface(kW, kH, SDL_PIXELFORMAT_ABGR8888);  // bytes R, G, B, A
    ren_ = surface_ ? SDL_CreateSoftwareRenderer(surface_) : nullptr;
    if (!ren_) { err = std::string("menu renderer: ") + SDL_GetError(); return false; }
    px_.assign(size_t(kW) * kH, 0);
    // Rows: 64 px buttons from y = 80, 12 px apart; the size row has - and +.
    const int x0 = 24, w = kW - 48, h = 64, step = 76;
    int y = 84;
    buttons_ = {{x0, y, w, h, Action::TogglePassthrough}};
    y += step;
    buttons_.push_back({x0, y, w, h, Action::Recenter});
    y += step;
    buttons_.push_back({x0, y, 130, h, Action::Smaller});
    buttons_.push_back({x0 + w - 130, y, 130, h, Action::Bigger});
    y += step;
    buttons_.push_back({x0, y, w, h, Action::Quit});
    y += step;
    buttons_.push_back({x0, y, w, h, Action::Close});
    return xr_.make_swapchain(kW, kH, XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, false, img_, err);
}

void Menu::open(XrTime t) {
    // In front of the head, turned to face it (about the vertical axis).
    XrSpace view;
    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    ci.poseInReferenceSpace.orientation.w = 1;
    if (XR_FAILED(xrCreateReferenceSpace(xr_.session, &ci, &view))) return;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    const bool ok = XR_SUCCEEDED(xrLocateSpace(view, xr_.space, t, &loc)) &&
                    (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                    (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    xrDestroySpace(view);
    if (!ok) return;
    const XrVector3f fwd = rotate(loc.pose.orientation, {0, 0, -1});
    const float yaw = std::atan2(-fwd.x, -fwd.z);
    const XrVector3f flat{-std::sin(yaw), 0, -std::cos(yaw)};
    quad_ = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad_.space = xr_.space;
    quad_.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad_.subImage.swapchain = img_.handle;
    quad_.subImage.imageRect = {{0, 0}, {kW, kH}};
    quad_.pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    quad_.pose.position = {loc.pose.position.x + flat.x * kDistance, loc.pose.position.y - kDrop,
                           loc.pose.position.z + flat.z * kDistance};
    quad_.size = {kWidthM, kWidthM * kH / kW};
    open_ = true;
    hovered_ = -1;
    quit_armed_ms_ = 0;
    pending_ = Action::None;
    dirty_ = true;
}

void Menu::close() { open_ = false; }

bool Menu::hit(XrVector3f o, XrVector3f d, float &u, float &v, float &dist) const {
    if (!open_) return false;
    const XrQuaternionf q = quad_.pose.orientation;
    const XrVector3f n = rotate(q, {0, 0, 1}), right = rotate(q, {1, 0, 0}), up = rotate(q, {0, 1, 0});
    const XrVector3f c = quad_.pose.position;
    const float denom = d.x * n.x + d.y * n.y + d.z * n.z;
    if (std::fabs(denom) < 1e-5f) return false;
    const float t = ((c.x - o.x) * n.x + (c.y - o.y) * n.y + (c.z - o.z) * n.z) / denom;
    if (t <= 0) return false;
    const XrVector3f p{o.x + d.x * t - c.x, o.y + d.y * t - c.y, o.z + d.z * t - c.z};
    const float lx = p.x * right.x + p.y * right.y + p.z * right.z;
    const float ly = p.x * up.x + p.y * up.y + p.z * up.z;
    u = lx / quad_.size.width + 0.5f;
    v = 0.5f - ly / quad_.size.height;
    dist = t;
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}

XrVector3f Menu::point(float u, float v) const {
    const XrQuaternionf q = quad_.pose.orientation;
    const XrVector3f right = rotate(q, {1, 0, 0}), up = rotate(q, {0, 1, 0}), n = rotate(q, {0, 0, 1});
    const float lx = (u - 0.5f) * quad_.size.width, ly = (0.5f - v) * quad_.size.height;
    const XrVector3f c = quad_.pose.position;
    return {c.x + right.x * lx + up.x * ly + n.x * 0.004f, c.y + right.y * lx + up.y * ly + n.y * 0.004f,
            c.z + right.z * lx + up.z * ly + n.z * 0.004f};
}

int Menu::button_at(float u, float v) const {
    const float x = u * kW, y = v * kH;
    for (size_t i = 0; i < buttons_.size(); i++) {
        const Button &b = buttons_[i];
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return int(i);
    }
    return -1;
}

void Menu::hover(float u, float v) {
    const int b = u < 0 ? -1 : button_at(u, v);
    if (b != hovered_) { hovered_ = b; dirty_ = true; }
}

void Menu::click(float u, float v) {
    const int b = button_at(u, v);
    if (b < 0) return;
    const Action a = buttons_[size_t(b)].action;
    if (a == Action::Quit) {
        const uint64_t now = now_ms();
        if (!quit_armed_ms_ || now - quit_armed_ms_ > kQuitArmMs) {
            quit_armed_ms_ = now;  // first pull: ask again
            dirty_ = true;
            return;
        }
    }
    quit_armed_ms_ = 0;
    pending_ = a;
    dirty_ = true;
}

Menu::Action Menu::take_action() {
    const Action a = pending_;
    pending_ = Action::None;
    return a;
}

void Menu::set_state(bool passthrough, float size, const std::vector<std::string> &info) {
    if (passthrough != passthrough_ || std::fabs(size - size_) > 1e-4f || info != info_) dirty_ = true;
    passthrough_ = passthrough;
    size_ = size;
    info_ = info;
}

void Menu::draw() {
    SDL_Renderer *r = ren_;
    auto fill = [&](int x, int y, int w, int h, Uint8 cr, Uint8 cg, Uint8 cb) {
        SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
        const SDL_FRect rc{float(x), float(y), float(w), float(h)};
        SDL_RenderFillRect(r, &rc);
    };
    // Text at `scale` x the 8x8 font, centred in [x, x+w) if w > 0.
    auto text = [&](const std::string &s, int x, int y, int w, float scale, Uint8 c) {
        SDL_SetRenderScale(r, scale, scale);
        SDL_SetRenderDrawColor(r, c, c, Uint8(std::min(255, c + 8)), 255);
        const float tw = float(s.size()) * 8 * scale;
        const float tx = w > 0 ? x + (w - tw) / 2 : float(x);
        SDL_RenderDebugText(r, tx / scale, y / scale, s.c_str());
        SDL_SetRenderScale(r, 1, 1);
    };
    fill(0, 0, kW, kH, 22, 24, 30);
    fill(0, 0, kW, 4, 90, 140, 220);  // accent line
    text("framewall", 24, 28, 0, 4, 235);

    const bool armed = quit_armed_ms_ && now_ms() - quit_armed_ms_ <= kQuitArmMs;
    for (size_t i = 0; i < buttons_.size(); i++) {
        const Button &b = buttons_[i];
        const bool hot = int(i) == hovered_;
        Uint8 base = hot ? 70 : 44;
        if (b.action == Action::Quit) {
            fill(b.x, b.y, b.w, b.h, Uint8(base + (armed ? 70 : 30)), Uint8(base - 10), Uint8(base - 10));
        } else {
            fill(b.x, b.y, b.w, b.h, base, Uint8(base + 3), Uint8(base + 10));
        }
        const int ty = b.y + (b.h - 24) / 2;
        switch (b.action) {
        case Action::TogglePassthrough: text(passthrough_ ? "Passthrough: ON" : "Passthrough: OFF", b.x, ty, b.w, 3, 235); break;
        case Action::Recenter: text("Recenter panels", b.x, ty, b.w, 3, 235); break;
        case Action::Smaller: text("-", b.x, ty, b.w, 3, 235); break;
        case Action::Bigger: text("+", b.x, ty, b.w, 3, 235); break;
        case Action::Quit: text(armed ? "Pull again to quit" : "Quit framewall", b.x, ty, b.w, 3, 235); break;
        case Action::Close: text("Close menu", b.x, ty, b.w, 3, 235); break;
        default: break;
        }
        if (b.action == Action::Smaller) {
            char s[32];
            std::snprintf(s, sizeof s, "Panels %d%%", int(std::lround(size_ * 100)));
            text(s, b.x + b.w, ty, kW - 48 - 2 * b.w, 3, 200);
        }
    }
    int y = buttons_.back().y + buttons_.back().h + 22;
    for (const auto &line : info_) {
        text(line, 28, y, 0, 2, 165);
        y += 22;
    }
    SDL_FlushRenderer(r);
    // Rows of the surface -> tightly packed RGBA (it's opaque: no premultiply).
    for (int row = 0; row < kH; row++)
        std::memcpy(&px_[size_t(row) * kW], static_cast<const uint8_t *>(surface_->pixels) + size_t(row) * surface_->pitch,
                    size_t(kW) * 4);
}

void Menu::prepare() {
    if (!open_) return;
    // The quit button falls back to its first state after a while.
    if (quit_armed_ms_ && now_ms() - quit_armed_ms_ > kQuitArmMs) { quit_armed_ms_ = 0; dirty_ = true; }
    if (!dirty_) return;
    draw();
    std::string err;
    if (!xr_.upload_image(img_, px_.data(), err)) fwlog("menu: %s", err.c_str());
    dirty_ = false;
}

void Menu::layers(std::vector<XrCompositionLayerBaseHeader *> &out) {
    if (!open_) return;
    quad_.space = xr_.space;
    out.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&quad_));
}
