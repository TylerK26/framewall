// framewall-vr's settings: ~/.config/framewall-vr.conf (KEY=VALUE lines, see
// client/framewall-vr.conf.example), written with every setting on the first
// run. The panel settings can also change live, from the panel editor on the
// PC (see control.h); those changes are written back into the file.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include "layout.h"

#include <map>
#include <string>

struct Settings {
    std::string host, app;
    int bitrate_kbps = 0;  // 0 = automatic from the wall size
    bool audio = true;
    bool quit_on_exit = true;
    bool passthrough = false;  // the room around the panels instead of the dark void
    int seconds = 0;       // stop after this long (testing)
    bool list = false;
    bool quit = false;     // just quit the wall on the host
    float size = 1;        // SIZE: every panel at once
    Placement place;
    std::map<std::string, float> panel_size;  // panel name -> size multiplier
};

std::string settings_path();
// Reads the settings file, creating it from the template if it's missing.
void load_settings(Settings &s);
// The settings file's modification time (ns), to notice edits; 0 if missing.
long long settings_mtime();

// Applies one panel setting (SIZE, DISTANCE, HEIGHT, GAP, SCREENS_SIZE,
// VIRTUALn_SIZE) given as text. Returns false for any other key or a value
// out of range.
bool set_panel_setting(Settings &s, const std::string &key, const std::string &value);
// Writes these KEY=VALUE changes into the settings file, keeping everything
// else (comments, other settings, alignment). Missing keys are appended.
bool save_settings(const std::map<std::string, std::string> &changes);

// The settings key for a panel's size: "Screens" -> SCREENS_SIZE,
// "Virtual 2" -> VIRTUAL2_SIZE.
std::string panel_size_key(const std::string &panel_name);
// A panel's size multiplier (1 unless set).
float panel_scale(const Settings &s, const std::string &panel_name);

// What the panel editor gets on connecting and after the file changes: the
// wall, its panels (top row first, each row left to right) and the current
// panel settings.
std::string editor_snapshot(const std::string &app, const Wall &wall, const Settings &s);
