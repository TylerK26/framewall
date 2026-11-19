// Live link to the panel editor on the PC (host/framewall-panels): a small
// window with sliders for the panel sizes and placement.
//
// framewall-vr connects out to the editor, at the framewall host's address
// and Sunshine port + 5, so the PC needs no address for the headset and its
// firewall only the port range framewall already uses. It keeps retrying in
// the background, so the editor can be opened and closed at any time.
// Protocol: see editor_snapshot() in settings.cpp.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

class ControlLink {
public:
    ControlLink(std::string host, int port);
    ~ControlLink();
    ControlLink(const ControlLink &) = delete;
    ControlLink &operator=(const ControlLink &) = delete;

    // Since the last call: whether the editor (re)connected, which then
    // needs a snapshot, and the settings it changed (latest value per key).
    void poll(bool &connected_now, std::map<std::string, std::string> &sets);
    // Sends whole lines; dropped while not connected.
    void send(const std::string &text);

private:
    void run();

    const std::string host_;
    const int port_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    int fd_ = -1;                // guarded by mu_
    bool fresh_ = false;         // guarded by mu_
    std::map<std::string, std::string> pending_;  // guarded by mu_
    std::thread thread_;
};
