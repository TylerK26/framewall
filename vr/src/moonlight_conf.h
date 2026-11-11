// Read the Moonlight client's settings (Moonlight.conf, a Qt INI file) to
// reuse its pairing: the client certificate/key, its unique id, and the
// paired hosts with their server certificates.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include <string>
#include <vector>

struct MoonlightHost {
    std::string hostname, uuid, srvcert;
    // Candidate addresses ("ip", port), most specific first.
    std::vector<std::pair<std::string, int>> addresses;
};

struct MoonlightConf {
    std::string path;
    std::string certificate, key, uniqueid;  // PEM, PEM, hex
    std::vector<MoonlightHost> hosts;

    // Loads the flatpak's config, or the native one. Returns false with `err`.
    bool load(std::string &err);
};
