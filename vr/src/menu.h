// The in-headset menu, opened and closed with both grips: a small panel in
// front of you, used with the lasers (trigger or bumper picks).
//
//   Passthrough on/off · Recenter panels · Panels smaller / bigger ·
//   Quit framewall (pull twice) · Close · stream info
//
// Drawn with SDL's software renderer and built-in 8x8 font into an image,
// shown as an OpenXR quad layer. Main thread only.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include "xr.h"

#include <cstdint>
#include <string>
#include <vector>

struct SDL_Surface;
struct SDL_Renderer;

class Menu {
public:
    enum class Action { None, TogglePassthrough, Recenter, Smaller, Bigger, Quit, Close };

    explicit Menu(Xr &xr) : xr_(xr) {}
    ~Menu();
    bool init(std::string &err);

    // Opens in front of the head (at display time `t`), or closes.
    void open(XrTime t);
    void close();
    bool is_open() const { return open_; }

    // Where a ray (in xr.space) meets the menu: u, v in 0..1 from the top
    // left, and the distance along the ray. False if it misses.
    bool hit(XrVector3f origin, XrVector3f dir, float &u, float &v, float &dist) const;
    XrVector3f point(float u, float v) const;  // a spot on the menu, a few mm in front
    XrQuaternionf orientation() const { return quad_.pose.orientation; }
    void hover(float u, float v);  // u < 0: nothing hovered
    // A pick at (u, v); the action, if any, comes out of take_action().
    void click(float u, float v);
    Action take_action();

    // What the rows show.
    void set_state(bool passthrough, float size, const std::vector<std::string> &info);

    // Redraws and uploads the image if something changed. Before the frame's
    // begin/end (it takes xr.queue_mu).
    void prepare();
    // Appends the menu's layer, if open.
    void layers(std::vector<XrCompositionLayerBaseHeader *> &out);

private:
    struct Button { int x, y, w, h; Action action; };
    int button_at(float u, float v) const;
    void draw();

    Xr &xr_;
    bool open_ = false, dirty_ = true;
    Xr::Swapchain img_;
    SDL_Surface *surface_ = nullptr;
    SDL_Renderer *ren_ = nullptr;
    std::vector<uint32_t> px_;
    std::vector<Button> buttons_;
    int hovered_ = -1;
    uint64_t quit_armed_ms_ = 0;
    Action pending_ = Action::None;

    bool passthrough_ = false;
    float size_ = 1;
    std::vector<std::string> info_;

    XrCompositionLayerQuad quad_{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
