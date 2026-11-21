// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "keyboard.h"

#include "log.h"

#include <Limelight.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

constexpr int kUnit = 64, kGap = 6, kMargin = 16;  // pixels: one key, between keys, around them
constexpr int kCols = 15, kRows = 6;               // in key units
constexpr int kW = kCols * kUnit + 2 * kMargin, kH = kRows * kUnit + 2 * kMargin;
constexpr float kWidthM = 0.56f;      // m
constexpr float kDistance = 0.6f;     // m in front of the head
constexpr float kDrop = 0.3f;         // m below eye level
constexpr float kTilt = 0.52f;        // rad (30°): the top edge leans away, like a desk keyboard
constexpr uint64_t kLockTapMs = 400;  // a second modifier tap within this locks it

uint64_t now_ms() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

XrVector3f rotate(XrQuaternionf q, XrVector3f v) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float tx = 2 * (y * v.z - z * v.y), ty = 2 * (z * v.x - x * v.z), tz = 2 * (x * v.y - y * v.x);
    return {v.x + w * tx + (y * tz - z * ty), v.y + w * ty + (z * tx - x * tz), v.z + w * tz + (x * ty - y * tx)};
}

// Win32 virtual-key codes (what Moonlight sends; Sunshine maps them to the
// PC's keys as a US layout).
enum : int {
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20,
    VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_DELETE = 0x2E, VK_LWIN = 0x5B, VK_F1 = 0x70,
    VK_LSHIFT = 0xA0, VK_LCONTROL = 0xA2, VK_LMENU = 0xA4, VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC,
    VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE, VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC,
    VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE,
};
enum { kShift, kCtrl, kAlt, kMeta };

}  // namespace

Keyboard::~Keyboard() {
    if (ren_) SDL_DestroyRenderer(ren_);
    if (surface_) SDL_DestroySurface(surface_);
    if (img_.handle) xr_.destroy_swapchain(img_);
}

bool Keyboard::init(std::string &err) {
    surface_ = SDL_CreateSurface(kW, kH, SDL_PIXELFORMAT_ABGR8888);  // bytes R, G, B, A
    ren_ = surface_ ? SDL_CreateSoftwareRenderer(surface_) : nullptr;
    if (!ren_) { err = std::string("keyboard renderer: ") + SDL_GetError(); return false; }
    px_.assign(size_t(kW) * kH, 0);
    mods_[kShift] = {VK_LSHIFT, MODIFIER_SHIFT};
    mods_[kCtrl] = {VK_LCONTROL, MODIFIER_CTRL};
    mods_[kAlt] = {VK_LMENU, MODIFIER_ALT};
    mods_[kMeta] = {VK_LWIN, MODIFIER_META};
    build();
    return xr_.make_swapchain(kW, kH, XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, false, img_, err);
}

void Keyboard::build() {
    // A US layout, 15 units wide; widths in quarter units.
    struct K { const char *label, *shifted; int vk; int quarters; Kind kind = Kind::Key; int mod = 0; };
    auto mod = [](const char *label, int quarters, int m) { return K{label, "", 0, quarters, Kind::Modifier, m}; };
    std::vector<std::vector<K>> rows(kRows);
    rows[0].push_back({"Esc", "", VK_ESCAPE, 6});
    for (int i = 0; i < 12; i++) {
        static const char *f[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
        rows[0].push_back({f[i], "", VK_F1 + i, 4});
    }
    rows[0].push_back({"Del", "", VK_DELETE, 6});
    rows[1] = {{"`", "~", VK_OEM_3, 4}, {"1", "!", '1', 4}, {"2", "@", '2', 4}, {"3", "#", '3', 4}, {"4", "$", '4', 4},
               {"5", "%", '5', 4}, {"6", "^", '6', 4}, {"7", "&", '7', 4}, {"8", "*", '8', 4}, {"9", "(", '9', 4},
               {"0", ")", '0', 4}, {"-", "_", VK_OEM_MINUS, 4}, {"=", "+", VK_OEM_PLUS, 4}, {"Bksp", "", VK_BACK, 8}};
    auto letters = [](std::vector<K> &row, const char *s) {
        for (; *s; s++) row.push_back({nullptr, nullptr, std::toupper(static_cast<unsigned char>(*s)), 4});
    };
    rows[2] = {{"Tab", "", VK_TAB, 6}};
    letters(rows[2], "qwertyuiop");
    rows[2].insert(rows[2].end(), {{"[", "{", VK_OEM_4, 4}, {"]", "}", VK_OEM_6, 4}, {"\\", "|", VK_OEM_5, 6}});
    rows[3] = {{"Caps", "", VK_CAPITAL, 7, Kind::Caps}};
    letters(rows[3], "asdfghjkl");
    rows[3].insert(rows[3].end(), {{";", ":", VK_OEM_1, 4}, {"'", "\"", VK_OEM_7, 4}, {"Enter", "", VK_RETURN, 9}});
    rows[4] = {mod("Shift", 9, kShift)};
    letters(rows[4], "zxcvbnm");
    rows[4].insert(rows[4].end(), {{",", "<", VK_OEM_COMMA, 4}, {".", ">", VK_OEM_PERIOD, 4}, {"/", "?", VK_OEM_2, 4},
                                   mod("Shift", 7, kShift), {"^", "", VK_UP, 4}});
    rows[5] = {mod("Ctrl", 6, kCtrl), mod("Meta", 5, kMeta), mod("Alt", 5, kAlt), {"", "", VK_SPACE, 24},
               {"Hide", "", 0, 8, Kind::Hide}, {"<", "", VK_LEFT, 4}, {"v", "", VK_DOWN, 4}, {">", "", VK_RIGHT, 4}};

    keys_.clear();
    for (int r = 0; r < kRows; r++) {
        int q = 0;
        for (const K &k : rows[size_t(r)]) {
            Key key;
            if (k.label) {
                key.label = k.label;
                key.shifted = k.shifted;
            } else {  // a letter: lower case, upper case when shifted
                key.label = std::string(1, char(std::tolower(k.vk)));
                key.shifted = std::string(1, char(k.vk));
            }
            key.vk = k.vk;
            key.kind = k.kind;
            key.mod = k.mod;
            key.x = kMargin + q * kUnit / 4 + kGap / 2;
            key.y = kMargin + r * kUnit + kGap / 2;
            key.w = k.quarters * kUnit / 4 - kGap;
            key.h = kUnit - kGap;
            keys_.push_back(key);
            q += k.quarters;
        }
        if (q != kCols * 4) fwlog("keyboard: row %d is %d quarter units wide", r, q);
    }
}

void Keyboard::open(XrTime t) {
    // Low in front of the head, facing it, tilted back.
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
    // yaw (about y) * pitch (about x, negative: the top edge away from you)
    const float sy = std::sin(yaw / 2), cy = std::cos(yaw / 2), sp = std::sin(-kTilt / 2), cp = std::cos(-kTilt / 2);
    quad_ = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad_.space = xr_.space;
    quad_.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad_.subImage.swapchain = img_.handle;
    quad_.subImage.imageRect = {{0, 0}, {kW, kH}};
    quad_.pose.orientation = {cy * sp, cp * sy, -sy * sp, cy * cp};
    quad_.pose.position = {loc.pose.position.x + flat.x * kDistance, loc.pose.position.y - kDrop,
                           loc.pose.position.z + flat.z * kDistance};
    quad_.size = {kWidthM, kWidthM * kH / kW};
    open_ = true;
    hovered_[0] = hovered_[1] = -1;
    dirty_ = true;
    fwlog("keyboard: shown");
}

void Keyboard::close() {
    if (!open_) return;
    release_all();
    open_ = false;
    fwlog("keyboard: hidden");
}

bool Keyboard::hit(XrVector3f o, XrVector3f d, float &u, float &v, float &dist) const {
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

XrVector3f Keyboard::point(float u, float v) const {
    const XrQuaternionf q = quad_.pose.orientation;
    const XrVector3f right = rotate(q, {1, 0, 0}), up = rotate(q, {0, 1, 0}), n = rotate(q, {0, 0, 1});
    const float lx = (u - 0.5f) * quad_.size.width, ly = (0.5f - v) * quad_.size.height;
    const XrVector3f c = quad_.pose.position;
    return {c.x + right.x * lx + up.x * ly + n.x * 0.004f, c.y + right.y * lx + up.y * ly + n.y * 0.004f,
            c.z + right.z * lx + up.z * ly + n.z * 0.004f};
}

int Keyboard::key_at(float u, float v) const {
    // Gaps count as the nearest key in the row, so a pull never falls through.
    const float x = u * kW, y = v * kH;
    constexpr int g = kGap / 2;
    for (size_t i = 0; i < keys_.size(); i++) {
        const Key &k = keys_[i];
        if (x >= float(k.x - g) && x < float(k.x + k.w + g) && y >= float(k.y - g) && y < float(k.y + k.h + g))
            return int(i);
    }
    return -1;
}

int Keyboard::modifiers() const {
    int m = 0;
    for (const Mod &mod : mods_)
        if (mod.state) m |= mod.flag;
    return m;
}

void Keyboard::sync_mods() {
    for (Mod &m : mods_) {
        const bool want = m.state != 0;
        if (want == m.sent) continue;
        m.sent = want;
        send_(m.vk, want, modifiers());
    }
}

void Keyboard::hover(int hand, float u, float v) {
    const int k = u < 0 ? -1 : key_at(u, v);
    if (k != hovered_[hand]) { hovered_[hand] = k; dirty_ = true; }
}

bool Keyboard::press(int hand, float u, float v) {
    if (!open_) return false;
    const int i = key_at(u, v);
    if (i < 0) return false;
    release(hand);
    const Key &k = keys_[size_t(i)];
    dirty_ = true;
    switch (k.kind) {
    case Kind::Hide:
        close();
        return true;
    case Kind::Modifier: {
        // off -> next key only -> (a quick second tap) locked -> off
        Mod &m = mods_[k.mod];
        const uint64_t now = now_ms();
        if (m.state == 0) m.state = 1;
        else if (m.state == 1 && now - m.tapped_ms < kLockTapMs) m.state = 2;
        else m.state = 0;
        m.tapped_ms = now;
        sync_mods();
        return true;
    }
    case Kind::Caps:
        caps_ = !caps_;
        [[fallthrough]];
    case Kind::Key:
        send_(k.vk, true, modifiers());
        pressed_[hand] = i;
        return true;
    }
    return false;
}

void Keyboard::release(int hand) {
    const int i = pressed_[hand];
    if (i < 0) return;
    pressed_[hand] = -1;
    dirty_ = true;
    const Key &k = keys_[size_t(i)];
    send_(k.vk, false, modifiers());
    // A typed key uses up the one-shot modifiers, once no key is held.
    if (k.kind != Kind::Key || pressed_[0] >= 0 || pressed_[1] >= 0) return;
    for (Mod &m : mods_)
        if (m.state == 1) m.state = 0;
    sync_mods();
}

void Keyboard::release_all() {
    for (int h = 0; h < 2; h++) {
        if (pressed_[h] < 0) continue;
        send_(keys_[size_t(pressed_[h])].vk, false, modifiers());
        pressed_[h] = -1;
    }
    for (Mod &m : mods_) m.state = 0;
    sync_mods();
    dirty_ = true;
}

void Keyboard::draw() {
    SDL_Renderer *r = ren_;
    auto fill = [&](int x, int y, int w, int h, Uint8 cr, Uint8 cg, Uint8 cb) {
        SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
        const SDL_FRect rc{float(x), float(y), float(w), float(h)};
        SDL_RenderFillRect(r, &rc);
    };
    // Centred in the key: characters at 3x the 8x8 font, words at 2x.
    auto label = [&](const std::string &s, const Key &k, Uint8 c) {
        const int len = int(s.size());
        const float scale = len == 1 ? 3.0f : len * 16 <= k.w - 8 ? 2.0f : 1.0f;
        SDL_SetRenderScale(r, scale, scale);
        SDL_SetRenderDrawColor(r, c, c, Uint8(std::min(255, c + 8)), 255);
        const float tx = k.x + (k.w - len * 8 * scale) / 2, ty = k.y + (k.h - 8 * scale) / 2;
        SDL_RenderDebugText(r, tx / scale, ty / scale, s.c_str());
        SDL_SetRenderScale(r, 1, 1);
    };
    fill(0, 0, kW, kH, 18, 20, 26);
    fill(0, 0, kW, 3, 90, 140, 220);  // accent line, like the menu's
    const bool shift = mods_[kShift].state != 0;
    for (size_t i = 0; i < keys_.size(); i++) {
        const Key &k = keys_[i];
        const bool hot = int(i) == hovered_[0] || int(i) == hovered_[1];
        const bool down = int(i) == pressed_[0] || int(i) == pressed_[1];
        const int mod_state = k.kind == Kind::Modifier ? mods_[k.mod].state : 0;
        Uint8 cr = 44, cg = 47, cb = 54;
        if (k.kind == Kind::Hide) cr = 58, cg = 46, cb = 48;
        if (hot) cr += 26, cg += 26, cb += 26;
        if (down || mod_state == 2 || (k.kind == Kind::Caps && caps_)) cr = 90, cg = 140, cb = 220;
        else if (mod_state == 1) cr = 60, cg = 92, cb = 150;
        fill(k.x, k.y, k.w, k.h, cr, cg, cb);
        if (mod_state == 2) fill(k.x + 8, k.y + k.h - 7, k.w - 16, 3, 235, 240, 255);  // locked: underlined
        std::string s = k.label;
        if (!k.shifted.empty()) {
            const bool letter = k.vk >= 'A' && k.vk <= 'Z';
            if (letter ? shift != caps_ : shift) s = k.shifted;
        }
        label(s, k, 235);
    }
    SDL_FlushRenderer(r);
    for (int row = 0; row < kH; row++)
        std::memcpy(&px_[size_t(row) * kW], static_cast<const uint8_t *>(surface_->pixels) + size_t(row) * surface_->pitch,
                    size_t(kW) * 4);
}

void Keyboard::prepare() {
    if (!open_ || !dirty_) return;
    draw();
    std::string err;
    if (!xr_.upload_image(img_, px_.data(), err)) fwlog("keyboard: %s", err.c_str());
    dirty_ = false;
}

void Keyboard::layers(std::vector<XrCompositionLayerBaseHeader *> &out) {
    if (!open_) return;
    quad_.space = xr_.space;
    out.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&quad_));
}
