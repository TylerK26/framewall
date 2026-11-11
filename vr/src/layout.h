// Where each monitor sits inside the wall stream, and where its panel goes in
// VR.
//
// The host names each wall preset "Wall NxWxH@HZ LAYOUT +BWxBH (SWxSH)":
// N virtual monitors of WxH, the block of physical screens BWxBH, and the
// stream size SWxSH. That is enough to redo the host's layout (`plan` in
// host/framewall) and find every monitor's rectangle in the stream.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include <openvr.h>

#include <string>
#include <vector>

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

struct Panel {
    std::string name;  // "Virtual 1", "Screens"
    Rect src;          // rectangle in the stream
};

struct Wall {
    int count = 0, vw = 0, vh = 0, hz = 0;  // virtual monitors
    std::string shape;                      // top, row, grid
    int bw = 0, bh = 0;                     // physical-screen block
    int width = 0, height = 0;              // stream size
    std::vector<Panel> panels;
};

// Parses a wall app name and lays out its panels. Returns false with `err`
// if the name isn't a wall preset.
bool parse_wall(const std::string &app, Wall &wall, std::string &err);

struct Placement {
    float radius = 1.5f;       // m from the head to the panels
    float meters_1920 = 1.25f; // width of 1920 pixels, in m
    float height = -0.10f;     // m of the wall's centre relative to eye level
    float gap = 0.03f;         // m between neighbouring panels
};

struct PanelPose {
    vr::HmdMatrix34_t transform;  // in the seated universe
    float width_m = 0, height_m = 0;
    float theta = 0, y = 0;       // centre on the cylinder: angle (rad, + = right), height (m)
    // The point of the panel that stays put when it is resized (its edge
    // against its neighbour, or its centre), in panel units: -0.5..0.5, y up.
    float anchor_x = 0, anchor_y = 0;
};

// Panels on a vertical cylinder around the seated origin, keeping the
// monitors' arrangement: left/right becomes angle around you, up/down stays.
// `scales` (one per panel, default 1) resizes panels individually: the
// physical screens stay where they are and the other panels move outwards
// so that neighbours never overlap.
std::vector<PanelPose> place_panels(const Wall &wall, const Placement &p, const std::vector<float> &scales = {});
