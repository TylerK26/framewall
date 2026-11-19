// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "settings.h"

#include "conf_template.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace {

bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
    s = b == std::string::npos ? "" : s.substr(b, e - b + 1);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
    return s;
}

// A number, all of the text (so "1.5m" or "" don't count).
bool parse_float(const std::string &v, float &out) {
    char *end = nullptr;
    out = std::strtof(v.c_str(), &end);
    return !v.empty() && end && *end == 0;
}

std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

}  // namespace

std::string settings_path() {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.config/framewall-vr.conf";
}

bool set_panel_setting(Settings &s, const std::string &k, const std::string &v) {
    float f;
    if (!parse_float(v, f)) return false;
    if (k == "DISTANCE") { if (f < 0.3f) return false; s.place.radius = f; }
    else if (k == "HEIGHT") s.place.height = f;
    else if (k == "GAP") { if (f < 0) return false; s.place.gap = f; }
    else if (k == "SIZE") { if (f <= 0.05f) return false; s.size = f; }
    else if (k == "SCREENS_SIZE") { if (f <= 0.05f) return false; s.panel_size["Screens"] = f; }
    else if (k.size() > 12 && k.compare(0, 7, "VIRTUAL") == 0 && ends_with(k, "_SIZE")) {
        if (f <= 0.05f) return false;
        s.panel_size["Virtual " + k.substr(7, k.size() - 12)] = f;
    } else return false;
    s.place.meters_1920 = 1.25f * s.size;  // SIZE=1: a 1080p monitor is 1.25 m wide
    return true;
}

void load_settings(Settings &s) {
    const std::string path = settings_path();
    std::ifstream in(path);
    if (!in) {
        // First run: write the file with every setting, so editing it is
        // just changing numbers.
        const std::string dir = path.substr(0, path.rfind('/'));
        mkdir(dir.c_str(), 0755);
        std::ofstream out(path);
        out << kConfTemplate;
        if (out) fwlog("settings: created %s", path.c_str());
        out.close();
        in.open(path);
    }
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "HOST") s.host = v;
        else if (k == "APP") s.app = v;
        else if (k == "BITRATE_KBPS") s.bitrate_kbps = std::atoi(v.c_str());
        else if (k == "AUDIO") s.audio = v != "0";
        else if (k == "QUIT_ON_EXIT") s.quit_on_exit = v != "0";
        else if (k == "PASSTHROUGH") s.passthrough = v == "1";
        else if (k == "SIZE" || k == "DISTANCE" || k == "HEIGHT" || k == "GAP" || ends_with(k, "_SIZE")) {
            if (!set_panel_setting(s, k, v)) fwlog("settings: ignoring %s=%s", k.c_str(), v.c_str());
        } else if (!k.empty()) fwlog("settings: unknown key %s", k.c_str());
    }
}

long long settings_mtime() {
    struct stat st;
    return stat(settings_path().c_str(), &st) == 0 ? (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec : 0;
}

bool save_settings(const std::map<std::string, std::string> &changes) {
    const std::string path = settings_path();
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    std::map<std::string, std::string> left = changes;
    for (auto &line : lines) {
        const auto key_start = line.find_first_not_of(" \t");
        if (key_start == std::string::npos || line[key_start] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || line.find('#') < eq) continue;
        const std::string k = trim(line.substr(0, eq));
        auto it = left.find(k);
        if (it == left.end()) continue;
        // Replace the value; keep the comment after it in the same column.
        const size_t v0 = eq + 1;
        size_t v1 = v0;
        while (v1 < line.size() && line[v1] != ' ' && line[v1] != '\t' && line[v1] != '#') v1++;
        size_t c0 = v1;
        while (c0 < line.size() && (line[c0] == ' ' || line[c0] == '\t')) c0++;
        std::string out = line.substr(0, v0) + it->second;
        if (c0 < line.size()) {  // a comment follows
            const size_t col = c0;
            out += std::string(out.size() + 1 < col ? col - out.size() : 1, ' ');
            out += line.substr(c0);
        }
        line = out;
        left.erase(it);
    }
    for (auto &[k, v] : left) lines.push_back(k + "=" + v);

    // Write a new file and rename it over the old one, so framewall-vr never
    // reads a half-written file.
    const std::string tmp = path + ".new";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (auto &l : lines) out << l << '\n';
        if (!out) { fwlog("settings: can't write %s", tmp.c_str()); return false; }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { fwlog("settings: can't replace %s", path.c_str()); return false; }
    return true;
}

std::string panel_size_key(const std::string &name) {
    if (name == "Screens") return "SCREENS_SIZE";
    if (name.compare(0, 8, "Virtual ") == 0) return "VIRTUAL" + name.substr(8) + "_SIZE";
    return "";
}

float panel_scale(const Settings &s, const std::string &name) {
    auto it = s.panel_size.find(name);
    return it != s.panel_size.end() ? it->second : 1.0f;
}

// Protocol (lines of UTF-8 text, newline-terminated):
//   headset -> editor:  hello 1 | wall APP | panel KEY ROW NAME | set KEY VALUE | ready
//   editor -> headset:  set KEY VALUE
// A snapshot is hello ... ready; the editor rebuilds from each one.
std::string editor_snapshot(const std::string &app, const Wall &wall, const Settings &s) {
    std::ostringstream o;
    o << "hello 1\n" << "wall " << app << "\n";
    // Panels in reading order: rows (panels whose heights overlap in the
    // wall, as in place_panels) top to bottom, each left to right.
    std::vector<const Panel *> order;
    for (const auto &p : wall.panels) order.push_back(&p);
    std::sort(order.begin(), order.end(), [](const Panel *a, const Panel *b) { return a->src.y < b->src.y; });
    std::vector<std::pair<int, const Panel *>> rows;  // row number, panel
    int row = 0, band_bottom = -1;
    for (const Panel *p : order) {
        if (band_bottom >= 0 && p->src.y >= band_bottom) row++;
        band_bottom = std::max(band_bottom, p->src.y + p->src.h);
        rows.emplace_back(row, p);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first < b.first : a.second->src.x < b.second->src.x;
    });
    for (const auto &[r, p] : rows) o << "panel " << panel_size_key(p->name) << " " << r + 1 << " " << p->name << "\n";
    o << "set SIZE " << num(s.size) << "\n"
      << "set DISTANCE " << num(s.place.radius) << "\n"
      << "set HEIGHT " << num(s.place.height) << "\n"
      << "set GAP " << num(s.place.gap) << "\n";
    for (const auto &p : wall.panels) o << "set " << panel_size_key(p.name) << " " << num(panel_scale(s, p.name)) << "\n";
    o << "ready\n";
    return o.str();
}
