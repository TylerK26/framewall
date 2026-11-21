// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "pointer.h"

#include "log.h"

#include <Limelight.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

constexpr uint64_t kChordMs = 150;     // both grips within this = the menu chord
constexpr float kDotSize = 0.012f;     // m
constexpr float kRayWidth = 0.004f;    // m
constexpr float kRayLength = 3.0f;     // m, when it hits nothing
constexpr float kStickDeadzone = 0.2f;
constexpr float kTriggerDown = 0.35f, kTriggerUp = 0.2f;  // a light pull clicks
constexpr double kNotchesPerSecond = 14;  // scroll speed at full deflection
constexpr int kDotPx = 64, kRayW = 16, kRayH = 4;

uint64_t now_ms() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

XrVector3f operator+(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
XrVector3f operator-(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
XrVector3f operator*(XrVector3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
XrVector3f cross(XrVector3f a, XrVector3f b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
XrVector3f normalize(XrVector3f a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-6f ? a * (1 / l) : XrVector3f{0, 0, 1};
}
// Rotates v by unit quaternion q.
XrVector3f rotate(XrQuaternionf q, XrVector3f v) {
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f t = cross(u, v) * 2;
    return v + t * q.w + cross(u, t);
}
// Quaternion of the rotation whose columns are x, y, z.
XrQuaternionf from_axes(XrVector3f x, XrVector3f y, XrVector3f z) {
    const float tr = x.x + y.y + z.z;
    XrQuaternionf q;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1) * 2;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4};
    } else if (x.x > y.y && x.x > z.z) {
        const float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
        q = {s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        const float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
        q = {(y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s};
    }
    return q;
}

// Premultiplied RGBA (as uint32 little-endian R,G,B,A).
uint32_t rgba(float r, float g, float b, float a) {
    auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
    return c(r * a) | (c(g * a) << 8) | (c(b * a) << 16) | (c(a) << 24);
}

}  // namespace

Pointer::~Pointer() {
    if (dot_img_.handle) xr_.destroy_swapchain(dot_img_);
    if (ray_img_.handle) xr_.destroy_swapchain(ray_img_);
    for (auto &h : hands_)
        if (h.aim) xrDestroySpace(h.aim);
    if (view_) xrDestroySpace(view_);
    if (set_) xrDestroyActionSet(set_);
}

bool Pointer::init(std::string &err) {
    return make_actions(err) && make_images(err);
}

void Pointer::suggest(const char *profile, const std::vector<std::pair<XrAction, std::vector<std::string>>> &candidates) {
    XrPath prof;
    if (XR_FAILED(xrStringToPath(xr_.instance, profile, &prof))) return;
    // Try each candidate path alone (a suggestion with one unsupported path
    // fails as a whole), keep the first that works per action and hand.
    std::vector<XrActionSuggestedBinding> good;
    for (const auto &[action, paths] : candidates)
        for (const char *hand : {"/user/hand/left", "/user/hand/right"}) {
            for (const auto &p : paths) {
                XrPath path;
                if (XR_FAILED(xrStringToPath(xr_.instance, (std::string(hand) + p).c_str(), &path))) continue;
                XrActionSuggestedBinding b{action, path};
                XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                s.interactionProfile = prof;
                s.countSuggestedBindings = 1;
                s.suggestedBindings = &b;
                if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(xr_.instance, &s))) {
                    good.push_back(b);
                    break;
                }
            }
        }
    if (good.empty()) { fwlog("pointer: no bindings accepted for %s", profile); return; }
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = prof;
    s.countSuggestedBindings = uint32_t(good.size());
    s.suggestedBindings = good.data();
    const XrResult r = xrSuggestInteractionProfileBindings(xr_.instance, &s);
    fwlog("pointer: %zu bindings for %s%s", good.size(), profile, XR_FAILED(r) ? " (rejected)" : "");
}

bool Pointer::make_actions(std::string &err) {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName, "framewall");
    std::strcpy(si.localizedActionSetName, "framewall");
    XrResult r = xrCreateActionSet(xr_.instance, &si, &set_);
    if (XR_FAILED(r)) { err = "action set: " + xr_result_name(xr_.instance, r); return false; }
    xrStringToPath(xr_.instance, "/user/hand/left", &hands_[0].path);
    xrStringToPath(xr_.instance, "/user/hand/right", &hands_[1].path);
    const XrPath both[2] = {hands_[0].path, hands_[1].path};
    auto action = [&](const char *name, const char *label, XrActionType type, XrAction &out) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ci.actionName, name);
        std::strcpy(ci.localizedActionName, label);
        ci.actionType = type;
        ci.countSubactionPaths = 2;
        ci.subactionPaths = both;
        return XR_SUCCEEDED(xrCreateAction(set_, &ci, &out));
    };
    if (!action("aim", "Laser aim", XR_ACTION_TYPE_POSE_INPUT, aim_) ||
        !action("laser", "Laser on/off (grip)", XR_ACTION_TYPE_BOOLEAN_INPUT, grip_) ||
        !action("click", "Left click (trigger)", XR_ACTION_TYPE_FLOAT_INPUT, trigger_) ||
        !action("click_bumper", "Left click (bumper)", XR_ACTION_TYPE_BOOLEAN_INPUT, bumper_) ||
        !action("right_click", "Right click (stick click)", XR_ACTION_TYPE_BOOLEAN_INPUT, right_) ||
        !action("scroll", "Scroll (stick)", XR_ACTION_TYPE_VECTOR2F_INPUT, stick_) ||
        !action("buzz", "Haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT, buzz_) ||
        !action("pad_a", "Gamepad A", XR_ACTION_TYPE_BOOLEAN_INPUT, a_) ||
        !action("pad_b", "Gamepad B", XR_ACTION_TYPE_BOOLEAN_INPUT, b_) ||
        !action("pad_x", "Gamepad X", XR_ACTION_TYPE_BOOLEAN_INPUT, x_) ||
        !action("pad_y", "Gamepad Y", XR_ACTION_TYPE_BOOLEAN_INPUT, y_) ||
        !action("pad_up", "Gamepad d-pad up", XR_ACTION_TYPE_BOOLEAN_INPUT, up_) ||
        !action("pad_down", "Gamepad d-pad down", XR_ACTION_TYPE_BOOLEAN_INPUT, down_) ||
        !action("pad_left", "Gamepad d-pad left", XR_ACTION_TYPE_BOOLEAN_INPUT, leftd_) ||
        !action("pad_right", "Gamepad d-pad right", XR_ACTION_TYPE_BOOLEAN_INPUT, rightd_) ||
        !action("pad_start", "Gamepad Start (menu)", XR_ACTION_TYPE_BOOLEAN_INPUT, start_) ||
        !action("pad_back", "Gamepad Back (view)", XR_ACTION_TYPE_BOOLEAN_INPUT, back_)) {
        err = "can't create the pointer's actions";
        return false;
    }
    // The Steam Frame's own profile (names tried in order), then the Index
    // controller's, which SteamVR can map onto any controller.
    if (xr_.frame_controller)
        suggest("/interaction_profiles/valve/frame_controller",
                {{aim_, {"/input/aim/pose"}},
                 {grip_, {"/input/squeeze/click", "/input/grip/click", "/input/squeeze/value", "/input/grip/value"}},
                 {trigger_, {"/input/trigger/value"}},
                 {bumper_, {"/input/bumper/click", "/input/shoulder/click"}},
                 {right_, {"/input/thumbstick/click", "/input/joystick/click"}},
                 {stick_, {"/input/thumbstick", "/input/joystick"}},
                 {buzz_, {"/output/haptic"}},
                 {a_, {"/input/a/click"}},
                 {b_, {"/input/b/click"}},
                 {x_, {"/input/x/click"}},
                 {y_, {"/input/y/click"}},
                 {up_, {"/input/dpad_up/click", "/input/dpad/up/click"}},
                 {down_, {"/input/dpad_down/click", "/input/dpad/down/click"}},
                 {leftd_, {"/input/dpad_left/click", "/input/dpad/left/click"}},
                 {rightd_, {"/input/dpad_right/click", "/input/dpad/right/click"}},
                 {start_, {"/input/menu/click", "/input/start/click"}},
                 {back_, {"/input/view/click", "/input/back/click", "/input/select/click"}}});
    suggest("/interaction_profiles/valve/index_controller",
            {{aim_, {"/input/aim/pose"}},
             {grip_, {"/input/squeeze/force", "/input/squeeze/value"}},
             {trigger_, {"/input/trigger/value"}},
             {right_, {"/input/thumbstick/click"}},
             {stick_, {"/input/thumbstick"}},
             {buzz_, {"/output/haptic"}},
             {a_, {"/input/a/click"}},
             {b_, {"/input/b/click"}}});
    suggest("/interaction_profiles/khr/simple_controller",
            {{aim_, {"/input/aim/pose"}}, {bumper_, {"/input/select/click"}}, {buzz_, {"/output/haptic"}}});

    for (int i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = aim_;
        ci.subactionPath = hands_[i].path;
        ci.poseInActionSpace.orientation.w = 1;
        if (XR_FAILED(xrCreateActionSpace(xr_.session, &ci, &hands_[i].aim))) { err = "aim space"; return false; }
    }
    XrReferenceSpaceCreateInfo vci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vci.poseInReferenceSpace.orientation.w = 1;
    xrCreateReferenceSpace(xr_.session, &vci, &view_);
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &set_;
    r = xrAttachSessionActionSets(xr_.session, &ai);
    if (XR_FAILED(r)) { err = "attaching the pointer's actions: " + xr_result_name(xr_.instance, r); return false; }
    return true;
}

bool Pointer::make_images(std::string &err) {
    // The dot: white with a dark rim, so it shows on light and dark content.
    std::vector<uint32_t> dot(size_t(kDotPx) * kDotPx);
    for (int y = 0; y < kDotPx; y++)
        for (int x = 0; x < kDotPx; x++) {
            const float dx = (x + 0.5f) / kDotPx * 2 - 1, dy = (y + 0.5f) / kDotPx * 2 - 1;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float edge = 2.0f / kDotPx;  // antialiasing width
            const float a = std::clamp((1 - r) / edge, 0.0f, 1.0f);
            const float inner = std::clamp((0.65f - r) / edge, 0.0f, 1.0f);
            const float g = 0.12f + 0.88f * inner;
            dot[size_t(y) * kDotPx + x] = rgba(g, g, g, a);
        }
    // The ray: bright core, soft sides, a little see-through.
    std::vector<uint32_t> ray(size_t(kRayW) * kRayH);
    for (int y = 0; y < kRayH; y++)
        for (int x = 0; x < kRayW; x++) {
            const float d = std::fabs((x + 0.5f) / kRayW * 2 - 1);
            ray[size_t(y) * kRayW + x] = rgba(0.85f, 0.92f, 1.0f, 0.7f * std::clamp(1.4f * (1 - d), 0.0f, 1.0f));
        }
    const XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    return xr_.make_swapchain(kDotPx, kDotPx, usage, true, dot_img_, err) && xr_.upload_image(dot_img_, dot.data(), err) &&
           xr_.make_swapchain(kRayW, kRayH, usage, true, ray_img_, err) && xr_.upload_image(ray_img_, ray.data(), err);
}

void Pointer::haptic(int hand) {
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.duration = 15'000'000;  // 15 ms
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    v.amplitude = 0.4f;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = buzz_;
    hi.subactionPath = hands_[hand].path;
    xrApplyHapticFeedback(xr_.session, &hi, reinterpret_cast<XrHapticBaseHeader *>(&v));
}

void Pointer::rumble(float low, float high) {
    // Each motor drives one controller; the vibration is renewed while it
    // lasts (OpenXR haptics have a duration) and stopped when it ends.
    const float want[2] = {low, high};
    const uint64_t now = now_ms();
    for (int i = 0; i < 2; i++) {
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = buzz_;
        hi.subactionPath = hands_[i].path;
        if (want[i] <= 0.01f) {
            if (rumble_[i] > 0.01f) xrStopHapticFeedback(xr_.session, &hi);
            rumble_[i] = 0;
            continue;
        }
        if (std::fabs(want[i] - rumble_[i]) < 0.02f && now - rumble_sent_ms_[i] < 80) continue;
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
        v.duration = 150'000'000;  // 150 ms, renewed every 80 ms while it lasts
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        v.amplitude = std::clamp(want[i], 0.0f, 1.0f);
        xrApplyHapticFeedback(xr_.session, &hi, reinterpret_cast<XrHapticBaseHeader *>(&v));
        rumble_[i] = want[i];
        rumble_sent_ms_[i] = now;
    }
}

void Pointer::release_buttons(const Mouse &mouse) {
    for (int b = 0; b < 2; b++)
        if (held_[b]) mouse.button(b == 1, false);
    held_[0] = held_[1] = false;
}

bool Pointer::update(XrTime t, const std::vector<PanelPose> &poses, const std::vector<Rect> &regions, float radius,
                     const Mouse &mouse, Menu &menu, Keyboard &keyboard) {
    for (auto &h : hands_) h.ray = h.dot = false;
    pad_ = Pad{};
    if (!xr_.focused()) {  // the Steam menu or another app has the controllers
        release_buttons(mouse);
        keyboard.release_all();
        for (auto &h : hands_) h.grip = h.left = h.right = h.kb_button = false, h.grip_down_ms = 0;
        return false;
    }
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(xr_.session, &sync))) return false;

    const uint64_t now = now_ms();
    bool grip_now[2], left_now[2], right_now[2], bumper_now[2], kb_button_now[2];
    float trigger_value[2] = {0, 0};
    int pad_buttons = 0;
    XrVector2f stick[2] = {};
    for (int i = 0; i < 2; i++) {
        Hand &h = hands_[i];
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath = h.path;
        auto boolean = [&](XrAction a) {
            XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
            gi.action = a;
            xrGetActionStateBoolean(xr_.session, &gi, &b);
            return b.isActive && b.currentState;
        };
        grip_now[i] = boolean(grip_);
        const bool bumper = boolean(bumper_);
        bumper_now[i] = bumper;
        right_now[i] = boolean(right_);
        // Gamepad buttons, wherever SteamVR bound them.
        // (Checked in games: the Frame's X and Y are the Xbox X and Y. Some
        // browser gamepad testers show them swapped.)
        const std::pair<XrAction, int> pad_map[] = {{a_, A_FLAG}, {b_, B_FLAG}, {x_, X_FLAG}, {y_, Y_FLAG},
                                                    {up_, UP_FLAG}, {down_, DOWN_FLAG}, {leftd_, LEFT_FLAG},
                                                    {rightd_, RIGHT_FLAG}, {start_, PLAY_FLAG}, {back_, BACK_FLAG}};
        kb_button_now[i] = false;
        for (const auto &[act, flag] : pad_map)
            if (boolean(act)) {
                pad_buttons |= flag;
                if (flag == B_FLAG || flag == Y_FLAG) kb_button_now[i] = true;
            }
        // Trigger: analog, with hysteresis, so a light pull clicks.
        XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
        gi.action = trigger_;
        xrGetActionStateFloat(xr_.session, &gi, &f);
        const float tv = f.isActive ? f.currentState : 0;
        trigger_value[i] = tv;
        trigger_down_[i] = trigger_down_[i] ? tv > kTriggerUp : tv > kTriggerDown;
        left_now[i] = trigger_down_[i] || bumper;
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        gi.action = stick_;
        xrGetActionStateVector2f(xr_.session, &gi, &v);
        if (v.isActive) stick[i] = v.currentState;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        h.pose_valid = XR_SUCCEEDED(xrLocateSpace(h.aim, xr_.space, t, &loc)) &&
                       (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                       (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (h.pose_valid) {
            h.origin = loc.pose.position;
            h.dir = normalize(rotate(loc.pose.orientation, {0, 0, -1}));
        }
    }
    XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
    if (view_ && XR_SUCCEEDED(xrLocateSpace(view_, xr_.space, t, &hl)) && (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
        head_ = hl.pose.position;
    auto latch = [&] {
        for (int i = 0; i < 2; i++) hands_[i].left = left_now[i], hands_[i].right = right_now[i];
    };

    // Grips: both together (within kChordMs) = the menu; one alone, once the
    // window has passed, toggles the laser on that controller.
    bool chord = false;
    for (int i = 0; i < 2; i++) {
        Hand &h = hands_[i], &o = hands_[1 - i];
        if (grip_now[i] && !h.grip) {
            if (o.grip_down_ms && now - o.grip_down_ms < kChordMs) {
                o.grip_down_ms = 0;  // neither press counts on its own
                chord = true;
            } else {
                h.grip_down_ms = now;
            }
        }
        h.grip = grip_now[i];
    }
    if (chord) {
        release_buttons(mouse);
        keyboard.release_all();
    }

    // B or Y shows/hides the keyboard, while the controllers aren't a gamepad.
    for (int i = 0; i < 2; i++) {
        Hand &h = hands_[i];
        if (kb_button_now[i] && !h.kb_button && !menu.is_open() && (laser_ >= 0 || keyboard.is_open())) {
            if (!keyboard.is_open()) release_buttons(mouse);
            keyboard.toggle(t);
            haptic(i);
        }
        h.kb_button = kb_button_now[i];
    }

    // The Xbox controller, while the laser is off and the menu and keyboard closed.
    if (laser_ < 0 && !menu.is_open() && !keyboard.is_open()) {
        auto axis = [](float v) { return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767)); };
        auto trig = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
        pad_.buttons = pad_buttons | (bumper_now[0] ? LB_FLAG : 0) | (bumper_now[1] ? RB_FLAG : 0) |
                       (right_now[0] ? LS_CLK_FLAG : 0) | (right_now[1] ? RS_CLK_FLAG : 0);
        pad_.lt = trig(trigger_value[0]);
        pad_.rt = trig(trigger_value[1]);
        pad_.lx = axis(stick[0].x);
        pad_.ly = axis(stick[0].y);
        pad_.rx = axis(stick[1].x);
        pad_.ry = axis(stick[1].y);
    }
    for (int i = 0; i < 2; i++) {
        Hand &h = hands_[i];
        if (!h.grip_down_ms || now - h.grip_down_ms < kChordMs) continue;
        h.grip_down_ms = 0;
        if (menu.is_open()) continue;  // grips don't move the laser while the menu is up
        release_buttons(mouse);
        laser_ = laser_ == i ? -1 : i;
        fwlog("pointer: laser %s", laser_ < 0 ? "off" : i == 0 ? "on the left controller" : "on the right controller");
        haptic(i);
    }

    // The menu: both controllers point; trigger or bumper picks. Nothing
    // reaches the PC meanwhile.
    if (menu.is_open() && !chord) {
        release_buttons(mouse);
        keyboard.release_all();
        keyboard.hover(0, -1, -1);
        keyboard.hover(1, -1, -1);
        int hovering = -1;
        float hu = -1, hv = -1;
        for (int i = 0; i < 2; i++) {
            Hand &h = hands_[i];
            if (!h.pose_valid) continue;
            float u, v, dist;
            h.ray = true;
            if (menu.hit(h.origin, h.dir, u, v, dist)) {
                h.end = h.origin + h.dir * dist;
                if (hovering < 0 || i == laser_) { hovering = i; hu = u; hv = v; }
                if (left_now[i] && !h.left) menu.click(u, v);
            } else {
                h.end = h.origin + h.dir * kRayLength;
            }
        }
        menu.hover(hovering >= 0 ? hu : -1, hv);
        if (hovering >= 0) {
            Hand &h = hands_[hovering];
            h.dot = true;
            h.dot_pos = menu.point(hu, hv);
            h.dot_rot = menu.orientation();
        }
        latch();
        return chord;
    }

    // The keyboard: both controllers point at it; a pull holds a key until
    // it's let go (or the ray leaves the keyboard). The laser controller
    // goes on to the panels when it isn't pointing at the keyboard.
    bool laser_on_keyboard = false;
    if (keyboard.is_open() && !chord) {
        for (int i = 0; i < 2; i++) {
            Hand &h = hands_[i];
            float u, v, dist;
            if (!h.pose_valid || !keyboard.hit(h.origin, h.dir, u, v, dist)) {
                keyboard.hover(i, -1, -1);
                keyboard.release(i);
                if (h.pose_valid && i != laser_) {  // a ray to aim with
                    h.ray = true;
                    h.end = h.origin + h.dir * kRayLength;
                }
                continue;
            }
            if (i == laser_) laser_on_keyboard = true;
            h.ray = h.dot = true;
            h.end = h.origin + h.dir * dist;
            h.dot_pos = keyboard.point(u, v);
            h.dot_rot = keyboard.orientation();
            keyboard.hover(i, u, v);
            if (left_now[i] && !h.left) {
                if (keyboard.press(i, u, v)) haptic(i);
            } else if (!left_now[i]) {
                keyboard.release(i);
            }
        }
    }

    if (laser_ < 0) { latch(); return chord; }
    Hand &h = hands_[laser_];
    if (laser_on_keyboard) { release_buttons(mouse); latch(); return chord; }
    if (!h.pose_valid) { release_buttons(mouse); latch(); return chord; }

    // The ray, in the seated space, against the panels' cylinder (x² + z² = r²).
    h.ray = true;
    h.end = h.origin + h.dir * kRayLength;
    const XrVector3f o = h.origin, dir = h.dir;
    const float a = dir.x * dir.x + dir.z * dir.z;
    const float b = 2 * (o.x * dir.x + o.z * dir.z);
    const float c = o.x * o.x + o.z * o.z - radius * radius;
    const float disc = b * b - 4 * a * c;
    int panel = -1;
    float u = 0, v = 0;
    if (a > 1e-6f && disc >= 0) {
        const float tt = (-b + std::sqrt(disc)) / (2 * a);  // the far side: we're inside the cylinder
        if (tt > 0) {
            const XrVector3f p = o + dir * tt;
            const float ang = std::atan2(p.x, -p.z);  // + = right of forward
            for (size_t i = 0; i < poses.size() && i < regions.size(); i++) {
                const float half = poses[i].width_m / radius / 2;
                float d = ang - poses[i].theta;
                while (d > float(M_PI)) d -= float(2 * M_PI);
                while (d < -float(M_PI)) d += float(2 * M_PI);
                if (std::fabs(d) > half || std::fabs(p.y - poses[i].y) > poses[i].height_m / 2) continue;
                panel = int(i);
                u = (d + half) / (2 * half);
                v = (poses[i].y + poses[i].height_m / 2 - p.y) / poses[i].height_m;
                h.end = p;
                // The dot: on the panel, a few mm towards you, facing the axis.
                h.dot = true;
                h.dot_pos = p + XrVector3f{-std::sin(ang), 0, std::cos(ang)} * 0.005f;
                h.dot_rot = {0, std::sin(-ang / 2), 0, std::cos(-ang / 2)};
                break;
            }
        }
    }
    const bool hit = panel >= 0;
    if (hit) {
        const Rect &r = regions[size_t(panel)];
        const int x = r.x + std::clamp(int(u * r.w), 0, r.w - 1), y = r.y + std::clamp(int(v * r.h), 0, r.h - 1);
        if (x != last_x_ || y != last_y_) {
            mouse.move(x, y);
            last_x_ = x;
            last_y_ = y;
        }
    }

    // Left: trigger or bumper. Right: thumbstick click. A press needs the
    // laser on a panel; a release always goes through.
    const bool now_down[2] = {left_now[laser_], right_now[laser_]};
    const bool was_down[2] = {h.left, h.right};
    for (int k = 0; k < 2; k++) {
        if (now_down[k] && !was_down[k] && hit && !held_[k]) {
            held_[k] = true;
            mouse.button(k == 1, true);
        } else if (!now_down[k] && held_[k]) {
            held_[k] = false;
            mouse.button(k == 1, false);
        }
    }
    latch();

    // Stick: scroll, faster the further it's pushed.
    if (hit) {
        const float s[2] = {stick[laser_].y, stick[laser_].x};
        int out[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            const float m = std::fabs(s[k]);
            if (m < kStickDeadzone) { scroll_acc_[k] = 0; continue; }
            const double speed = (m - kStickDeadzone) / (1 - kStickDeadzone);
            scroll_acc_[k] += (s[k] > 0 ? 1 : -1) * speed * speed * kNotchesPerSecond * 120 / 144.0;
            out[k] = int(scroll_acc_[k]);
            scroll_acc_[k] -= out[k];
        }
        if (out[0] || out[1]) mouse.scroll(out[0], out[1]);
    }
    return chord;
}

void Pointer::layers(std::vector<XrCompositionLayerBaseHeader *> &out) {
    // Rays: thin quads from the controller to where they end, the long axis
    // along the ray, the face turned to the head.
    for (int i = 0; i < 2; i++) {
        const Hand &h = hands_[i];
        if (!h.ray || !h.pose_valid) continue;
        const XrVector3f along = h.end - h.origin;
        const float len = std::sqrt(dot(along, along));
        if (len < 0.01f) continue;
        const XrVector3f y = along * (1 / len);
        const XrVector3f mid = h.origin + along * 0.5f;
        XrVector3f z = head_ - mid;
        z = normalize(z - y * dot(z, y));
        const XrVector3f x = cross(y, z);
        XrCompositionLayerQuad &q = ray_layers_[i];
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.space = xr_.space;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = ray_img_.handle;
        q.subImage.imageRect = {{0, 0}, {ray_img_.width, ray_img_.height}};
        q.pose.orientation = from_axes(x, y, z);
        q.pose.position = mid;
        q.size = {kRayWidth, len};
        out.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&q));
    }
    for (int i = 0; i < 2; i++) {
        const Hand &h = hands_[i];
        if (!h.dot) continue;
        XrCompositionLayerQuad &q = dot_layers_[i];
        q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        q.space = xr_.space;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = dot_img_.handle;
        q.subImage.imageRect = {{0, 0}, {dot_img_.width, dot_img_.height}};
        q.pose.orientation = h.dot_rot;
        q.pose.position = h.dot_pos;
        q.size = {kDotSize, kDotSize};
        out.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&q));
    }
}
