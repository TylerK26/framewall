// The GameStream HTTP(S) API that Sunshine serves: server info, app list,
// launch/resume and quit. Authenticates with Moonlight's client certificate
// and pins the host certificate Moonlight saved when it paired.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include <string>
#include <vector>

struct NvApp {
    std::string title;
    int id = 0;
};

struct NvServerInfo {
    std::string hostname, uniqueid, appversion, gfeversion, state;
    int https_port = 0, codec_modes = 0, current_game = 0;
    bool paired = false;
};

class NvHttp {
public:
    NvHttp(std::string address, int http_port, std::string client_cert, std::string client_key,
           std::string uniqueid, std::string server_cert);

    // Unauthenticated (HTTP) server info: reachability, uuid, HTTPS port.
    bool server_info_http(NvServerInfo &out, std::string &err, long timeout_ms = 2500);
    // Authenticated server info (needs pairing): current game, codecs.
    bool server_info(NvServerInfo &out, std::string &err);
    bool app_list(std::vector<NvApp> &out, std::string &err);
    // Launch (or resume, if `resume`) an app. Returns the RTSP session URL.
    bool launch(bool resume, int appid, int width, int height, int fps, int audio_config,
                const unsigned char rikey[16], int rikeyid, std::string &rtsp_url, std::string &err);
    bool quit(std::string &err);

    const std::string &address() const { return address_; }

private:
    bool get(bool https, const std::string &path, const std::string &query, long timeout_ms,
             std::string &body, std::string &err);

    std::string address_;
    int http_port_, https_port_;
    std::string cert_, key_, uniqueid_, server_cert_;
};

// Tiny XML helpers for GameStream's flat responses.
std::string xml_tag(const std::string &xml, const std::string &tag, size_t from = 0, size_t *end = nullptr);
