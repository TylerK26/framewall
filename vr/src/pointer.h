// The laser pointer: the headset's controllers drive the PC's mouse.
//
//   - The laser starts off. A controller's grip toggles it: on that controller
//     (moving it from the other one), or off if it's already there.
//   - Both grips together open/close the menu (see menu.h); while it's open,
//     both controllers point at it and nothing reaches the PC.
//   - The laser controller's trigger (a light pull) or bumper is the left
//     button, its thumbstick click the right button; the thumbstick scrolls.
//
// Where the laser meets a panel becomes an absolute mouse position in the
// wall stream, so the PC's cursor sits under the dot. Drawn as two OpenXR
// quad layers on top of the panels: a thin ray and a dot.
//
// With the laser off and the menu closed, the controllers are an Xbox
// controller on the PC instead (pad()): sticks, triggers, bumpers, stick
// clicks, face buttons, d-pad, menu = Start, view = Back. The grips stay here.
//
// The on-screen keyboard (keyboard.h): while it's shown, both controllers
// point and type on it, and the laser controller still drives the mouse when
// it points at the panels. While the laser is on (or the keyboard is shown),
// B or Y shows/hides it. A pull on its bar moves it, and that controller's
// stick pushes it away / pulls it closer.
//
// Main thread only (input is read once per display frame).
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include "keyboard.h"
#include "layout.h"
#include "menu.h"
#include "xr.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Pointer {
public:
    // What the pointer does to the PC's mouse.
    struct Mouse {
        std::function<void(int x, int y)> move;              // stream pixels
        std::function<void(bool right, bool down)> button;
        std::function<void(int vertical, int horizontal)> scroll;  // 120 = one notch
    };

    explicit Pointer(Xr &xr) : xr_(xr) {}
    ~Pointer();

    // Actions, bindings and the pointer images. Call before the session runs.
    bool init(std::string &err);

    // Once per display frame (only acts while focused). `poses`/`regions`:
    // the panels on the cylinder of `radius` and their rectangles in the
    // stream. Returns true when both grips were pressed together (the menu).
    bool update(XrTime t, const std::vector<PanelPose> &poses, const std::vector<Rect> &regions, float radius,
                const Mouse &mouse, Menu &menu, Keyboard &keyboard);

    // The Xbox controller as of the last update(): neutral while the laser is
    // on, the menu or keyboard is open or the app isn't focused. Buttons use Limelight's
    // *_FLAG values; sticks are -32768..32767 with up = +.
    struct Pad {
        int buttons = 0;
        uint8_t lt = 0, rt = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
        bool operator==(const Pad &o) const {
            return buttons == o.buttons && lt == o.lt && rt == o.rt && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry;
        }
    };
    const Pad &pad() const { return pad_; }
    // The PC's rumble (0..1 per motor): low on the left controller, high on
    // the right. Call every frame.
    void rumble(float low, float high);

    // Appends the ray(s) and dot, valid until the next call.
    void layers(std::vector<XrCompositionLayerBaseHeader *> &out);

private:
    struct Hand {
        XrPath path = XR_NULL_PATH;
        XrSpace aim = XR_NULL_HANDLE;
        bool grip = false, left = false, right = false;  // as of the last frame
        uint64_t grip_down_ms = 0;  // grip press waiting out the chord window (0 = none)
        bool pose_valid = false;
        XrVector3f origin{}, dir{}, end{};  // the ray this frame
        bool ray = false;                    // drawn this frame
        bool dot = false;                    // where it hits something, drawn this frame
        XrVector3f dot_pos{};
        XrQuaternionf dot_rot{0, 0, 0, 1};
        bool kb_button = false;              // B or Y (keyboard toggle), as of the last frame
    };

    bool make_actions(std::string &err);
    bool make_images(std::string &err);
    void suggest(const char *profile, const std::vector<std::pair<XrAction, std::vector<std::string>>> &candidates);
    void release_buttons(const Mouse &mouse);
    void haptic(int hand);

    Xr &xr_;
    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction aim_ = XR_NULL_HANDLE, grip_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE, bumper_ = XR_NULL_HANDLE,
             right_ = XR_NULL_HANDLE, stick_ = XR_NULL_HANDLE, buzz_ = XR_NULL_HANDLE;
    // Gamepad-only buttons.
    XrAction a_ = XR_NULL_HANDLE, b_ = XR_NULL_HANDLE, x_ = XR_NULL_HANDLE, y_ = XR_NULL_HANDLE, up_ = XR_NULL_HANDLE,
             down_ = XR_NULL_HANDLE, leftd_ = XR_NULL_HANDLE, rightd_ = XR_NULL_HANDLE, start_ = XR_NULL_HANDLE,
             back_ = XR_NULL_HANDLE;
    Pad pad_;
    float rumble_[2] = {0, 0};
    uint64_t rumble_sent_ms_[2] = {0, 0};
    XrSpace view_ = XR_NULL_HANDLE;
    Hand hands_[2];

    int laser_ = -1;              // hand with the laser, -1 = off
    bool held_[2] = {false, false};  // left, right button down on the PC
    bool trigger_down_[2] = {false, false};
    XrVector3f head_{};
    int last_x_ = -1, last_y_ = -1;
    double scroll_acc_[2] = {0, 0};
    XrTime last_t_ = 0;

    Xr::Swapchain dot_img_, ray_img_;
    XrCompositionLayerQuad ray_layers_[2], dot_layers_[2];
};
