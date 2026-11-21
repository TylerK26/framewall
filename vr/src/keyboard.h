// The on-screen keyboard: a US keyboard floating low in front of you, typed
// on with the lasers, its keys sent to the PC as real key presses.
//
//   - Shown/hidden from the menu (both grips), with the B or Y button while
//     the laser is on, or with its own Hide key.
//   - While it's shown, both controllers get a ray; a trigger (or bumper)
//     pull on a key presses it, and the key is held until the trigger is let
//     go (the PC repeats it). Two hands can type at once.
//   - Shift, Ctrl, Alt and Meta are sticky: tap once for the next key only,
//     twice quickly to lock (held down until tapped again), e.g. for Alt+Tab.
//   - The laser controller keeps driving the mouse whenever it points at the
//     panels instead of the keyboard.
//   - The bar under the keys moves it: pull on the bar and the keyboard
//     follows that controller's ray, turned to face you; its stick pushes the
//     keyboard away (forward) or pulls it closer (back). Shown again, it comes
//     back where you left it, unless that's far off to the side of where
//     you're looking (then it opens low in front of you again).
//
// Drawn with SDL's software renderer and built-in 8x8 font, like the menu,
// and shown as an OpenXR quad layer. Main thread only.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include "xr.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_Surface;
struct SDL_Renderer;

class Keyboard {
public:
    // Sends one key event to the PC: a Win32 virtual-key code, down or up,
    // and the MODIFIER_* flags held at the time.
    using Send = std::function<void(int vk, bool down, int modifiers)>;

    Keyboard(Xr &xr, Send send) : xr_(xr), send_(std::move(send)) {}
    ~Keyboard();
    bool init(std::string &err);

    // Shows it in front of the head (at display time `t`), or hides it,
    // releasing every key on the PC.
    void open(XrTime t);
    void close();
    void toggle(XrTime t) { if (open_) close(); else open(t); }
    bool is_open() const { return open_; }

    // Like Menu::hit/point: where a ray meets the keyboard (u, v from the
    // top left), and a spot just in front of it.
    bool hit(XrVector3f origin, XrVector3f dir, float &u, float &v, float &dist) const;
    XrVector3f point(float u, float v) const;
    XrQuaternionf orientation() const { return quad_.pose.orientation; }

    // Per controller (0 = left, 1 = right). hover with u < 0: nothing.
    void hover(int hand, float u, float v);
    // A pull at (u, v): true if it landed on a key (for a haptic tick).
    bool press(int hand, float u, float v);
    // The pull ended (or the ray left the keyboard): lets its key go.
    void release(int hand);

    // Moving it by the bar under the keys.
    bool on_bar(float u, float v) const;
    // A pull on the bar at (u, v), `dist` along the controller's ray.
    void begin_move(int hand, float u, float v, float dist);
    // Each frame while the pull lasts: the bar's grabbed spot stays on the
    // ray, `push` (-1..1) moves it along the ray, the keyboard faces `head`.
    void move(XrVector3f origin, XrVector3f dir, XrVector3f head, float push, float dt);
    void end_move();
    int moving_hand() const { return move_hand_; }  // -1: not moving
    XrVector3f grab_point() const { return point(move_u_, move_v_); }
    // Lets every key go, the sticky modifiers included.
    void release_all();

    // Redraws and uploads the image if something changed. Before the frame's
    // begin/end (it takes xr.queue_mu).
    void prepare();
    // Appends the keyboard's layer, if shown.
    void layers(std::vector<XrCompositionLayerBaseHeader *> &out);

private:
    enum class Kind { Key, Modifier, Caps, Hide };
    struct Key {
        std::string label, shifted;  // shifted empty = same as label (letters: upper case)
        int vk;
        Kind kind;
        int mod;                     // Modifier: index into mods_
        int x, y, w, h;              // pixels
    };
    struct Mod {
        int vk, flag;
        int state = 0;               // 0 off, 1 the next key only, 2 locked
        uint64_t tapped_ms = 0;
        bool sent = false;           // its key is down on the PC
    };

    void build();
    int key_at(float u, float v) const;
    int modifiers() const;
    void sync_mods();                // presses/releases modifier keys to match their state
    void draw();
    // Faces the quad along `yaw` (about y), tilted back by `tilt`.
    void orient(float yaw, float tilt);

    Xr &xr_;
    Send send_;
    bool open_ = false, dirty_ = true;
    Xr::Swapchain img_;
    SDL_Surface *surface_ = nullptr;
    SDL_Renderer *ren_ = nullptr;
    std::vector<uint32_t> px_;
    std::vector<Key> keys_;
    Mod mods_[4];                    // shift, ctrl, alt, meta
    bool caps_ = false;              // as far as we know (only for the labels)
    int hovered_[2] = {-1, -1};
    int pressed_[2] = {-1, -1};      // key held by each controller
    bool bar_hot_[2] = {false, false};
    int move_hand_ = -1;
    float move_u_ = 0, move_v_ = 0, move_dist_ = 0;
    bool moved_ = false;             // placed by hand: reopen there

    XrCompositionLayerQuad quad_{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
