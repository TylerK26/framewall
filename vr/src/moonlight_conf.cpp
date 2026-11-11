// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "moonlight_conf.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace {

// Undo Qt's INI value encoding: surrounding quotes, @ByteArray(...), and
// backslash escapes (\n, \\, \", \0, \xHH).
std::string decode_value(std::string v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    const std::string ba = "@ByteArray(";
    if (v.compare(0, ba.size(), ba) == 0 && !v.empty() && v.back() == ')')
        v = v.substr(ba.size(), v.size() - ba.size() - 1);
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] != '\\' || i + 1 >= v.size()) { out += v[i]; continue; }
        const char c = v[++i];
        switch (c) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case '0': out += '\0'; break;
        case 'x': {
            int n = 0, digits = 0;
            while (digits < 2 && i + 1 < v.size() && std::isxdigit(static_cast<unsigned char>(v[i + 1]))) {
                n = n * 16 + std::stoi(std::string(1, v[++i]), nullptr, 16);
                digits++;
            }
            out += char(n);
            break;
        }
        default: out += c; break;  // \\ \" and anything else
        }
    }
    return out;
}

}  // namespace

bool MoonlightConf::load(std::string &err) {
    const char *home = std::getenv("HOME");
    const std::string h = home ? home : "";
    const std::string candidates[] = {
        h + "/.var/app/com.moonlight_stream.Moonlight/config/Moonlight Game Streaming Project/Moonlight.conf",
        h + "/.config/Moonlight Game Streaming Project/Moonlight.conf",
    };
    std::ifstream in;
    for (const auto &c : candidates) {
        in.open(c);
        if (in) { path = c; break; }
        in.clear();
    }
    if (!in) {
        err = "Moonlight's settings not found: install Moonlight and pair the framewall host with it first";
        return false;
    }

    std::string section, line;
    std::map<int, std::map<std::string, std::string>> raw_hosts;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { section = line.substr(1, line.find(']') - 1); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (section == "General") {
            if (k == "certificate") certificate = decode_value(v);
            else if (k == "key") key = decode_value(v);
            else if (k == "uniqueid") uniqueid = decode_value(v);
        } else if (section == "hosts") {
            // "3\hostname" -> host 3, key "hostname" (nested apps\... ignored)
            const size_t bs = k.find('\\');
            if (bs == std::string::npos) continue;
            const std::string sub = k.substr(bs + 1);
            if (sub.find('\\') != std::string::npos) continue;
            raw_hosts[std::atoi(k.substr(0, bs).c_str())][sub] = decode_value(v);
        }
    }
    for (auto &[idx, kv] : raw_hosts) {
        MoonlightHost host;
        host.hostname = kv["hostname"];
        host.uuid = kv["uuid"];
        host.srvcert = kv["srvcert"];
        auto add = [&](const char *a, const char *p, int def) {
            const std::string addr = kv[a];
            if (addr.empty()) return;
            int port = std::atoi(kv[p].c_str());
            host.addresses.emplace_back(addr, port > 0 ? port : def);
        };
        add("manualaddress", "manualport", 47989);
        add("localaddress", "localport", 47989);
        add("remoteaddress", "remoteport", 47989);
        if (!host.hostname.empty()) hosts.push_back(std::move(host));
    }
    if (certificate.empty() || key.empty() || uniqueid.empty()) {
        err = "Moonlight's settings have no client certificate yet: pair the framewall host in Moonlight first";
        return false;
    }
    return true;
}
