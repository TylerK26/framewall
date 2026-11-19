// The laser pointer: the headset's controllers drive the PC's mouse.
//
//   - The laser starts off. A controller's grip toggles it: on that controller
//     (moving it from the other one), or off if it's already there.
//   - Both grips together are reserved for the menu (nothing yet).
//   - The laser controller's trigger (a light pull) or bumper is the left
//     button, its thumbstick click the right button; the thumbstick scrolls.
//
// Where the laser meets a panel becomes an absolute mouse position in the
// wall stream, so the PC's cursor sits under the dot. Drawn as two OpenXR
// quad layers on top of the panels: a thin ray and a dot.
//
// Main thread only (input is read once per display frame).
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include "layout.h"
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

    // Once per display frame (any session state; only acts while focused).
    // `poses`/`regions`: the panels on the cylinder of `radius` and their
    // rectangles in the stream.
    void update(XrTime t, const std::vector<PanelPose> &poses, const std::vector<Rect> &regions, float radius,
                const Mouse &mouse);

    // Appends the ray and dot layers (if the laser is on), valid until the
    // next call.
    void layers(std::vector<XrCompositionLayerBaseHeader *> &out);

private:
    struct Hand {
        XrPath path = XR_NULL_PATH;
        XrSpace aim = XR_NULL_HANDLE;
        bool grip = false, left = false, right = false;  // as of the last frame
        uint64_t grip_down_ms = 0;  // grip press waiting out the chord window (0 = none)
        XrPosef pose{};
        bool pose_valid = false;
    };

    bool make_actions(std::string &err);
    bool make_images(std::string &err);
    bool upload(Xr::Swapchain &sc, const std::vector<uint32_t> &rgba, std::string &err);
    void suggest(const char *profile, const std::vector<std::pair<XrAction, std::vector<std::string>>> &candidates);
    void release_buttons(const Mouse &mouse);
    void haptic(int hand);

    Xr &xr_;
    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction aim_ = XR_NULL_HANDLE, grip_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE, bumper_ = XR_NULL_HANDLE,
             right_ = XR_NULL_HANDLE, stick_ = XR_NULL_HANDLE, buzz_ = XR_NULL_HANDLE;
    XrSpace view_ = XR_NULL_HANDLE;
    Hand hands_[2];

    int laser_ = -1;              // hand with the laser, -1 = off
    bool held_[2] = {false, false};  // left, right button down on the PC
    bool trigger_down_[2] = {false, false};
    bool hit_ = false;
    XrVector3f origin_{}, end_{}, hit_point_{}, head_{};
    float hit_angle_ = 0;
    int last_x_ = -1, last_y_ = -1;
    double scroll_acc_[2] = {0, 0};

    Xr::Swapchain dot_img_, ray_img_;
    XrCompositionLayerQuad ray_{XR_TYPE_COMPOSITION_LAYER_QUAD}, dot_{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
