// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "nvhttp.h"

#include <Limelight.h>
#include <curl/curl.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

size_t collect(char *p, size_t size, size_t n, void *ud) {
    static_cast<std::string *>(ud)->append(p, size * n);
    return size * n;
}

// Base64 body of a PEM certificate, without headers or whitespace, so two
// encodings of the same certificate compare equal.
std::string pem_body(const std::string &pem) {
    std::string out;
    size_t i = pem.find("-----BEGIN");
    if (i == std::string::npos) return out;
    i = pem.find('\n', i);
    const size_t end = pem.find("-----END", i);
    if (i == std::string::npos || end == std::string::npos) return out;
    for (size_t k = i; k < end; k++)
        if (!std::isspace(static_cast<unsigned char>(pem[k]))) out += pem[k];
    return out;
}

std::string random_hex(size_t bytes) {
    std::vector<unsigned char> b(bytes);
    RAND_bytes(b.data(), int(bytes));
    std::string s;
    char h[3];
    for (unsigned char c : b) { std::snprintf(h, sizeof h, "%02x", c); s += h; }
    return s;
}

bool check_status(const std::string &xml, std::string &err) {
    const size_t root = xml.find("<root");
    if (root == std::string::npos) { err = "unexpected reply from host"; return false; }
    const size_t sc = xml.find("status_code=\"", root);
    if (sc == std::string::npos) return true;
    const int code = std::atoi(xml.c_str() + sc + 13);
    if (code == 200) return true;
    std::string msg;
    const size_t sm = xml.find("status_message=\"", root);
    if (sm != std::string::npos) msg = xml.substr(sm + 16, xml.find('"', sm + 16) - sm - 16);
    err = "host replied " + std::to_string(code) + (msg.empty() ? "" : ": " + msg);
    return false;
}

}  // namespace

std::string xml_tag(const std::string &xml, const std::string &tag, size_t from, size_t *end) {
    const std::string open = "<" + tag + ">", close = "</" + tag + ">";
    const size_t a = xml.find(open, from);
    if (a == std::string::npos) { if (end) *end = std::string::npos; return ""; }
    const size_t b = xml.find(close, a);
    if (b == std::string::npos) { if (end) *end = std::string::npos; return ""; }
    if (end) *end = b + close.size();
    return xml.substr(a + open.size(), b - a - open.size());
}

NvHttp::NvHttp(std::string address, int http_port, std::string client_cert, std::string client_key,
               std::string uniqueid, std::string server_cert)
    : address_(std::move(address)), http_port_(http_port), https_port_(http_port - 5),
      cert_(std::move(client_cert)), key_(std::move(client_key)), uniqueid_(std::move(uniqueid)),
      server_cert_(std::move(server_cert)) {}

bool NvHttp::get(bool https, const std::string &path, const std::string &query, long timeout_ms,
                 std::string &body, std::string &err) {
    CURL *c = curl_easy_init();
    if (!c) { err = "curl init failed"; return false; }
    const std::string url = std::string(https ? "https" : "http") + "://" + address_ + ":" +
                            std::to_string(https ? https_port_ : http_port_) + "/" + path +
                            "?uniqueid=" + uniqueid_ + "&uuid=" + random_hex(16) + (query.empty() ? "" : "&" + query);
    body.clear();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, std::min(timeout_ms, 3000L));
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (https) {
        curl_blob cert{const_cast<char *>(cert_.data()), cert_.size(), CURL_BLOB_COPY};
        curl_blob key{const_cast<char *>(key_.data()), key_.size(), CURL_BLOB_COPY};
        curl_easy_setopt(c, CURLOPT_SSLCERT_BLOB, &cert);
        curl_easy_setopt(c, CURLOPT_SSLCERTTYPE, "PEM");
        curl_easy_setopt(c, CURLOPT_SSLKEY_BLOB, &key);
        curl_easy_setopt(c, CURLOPT_SSLKEYTYPE, "PEM");
        // The host's certificate is self-signed; it is pinned below instead.
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_CERTINFO, 1L);
    }
    const CURLcode rc = curl_easy_perform(c);
    bool ok = rc == CURLE_OK;
    if (!ok) err = std::string("can't reach ") + address_ + ": " + curl_easy_strerror(rc);
    if (ok && https && !server_cert_.empty()) {
        curl_certinfo *ci = nullptr;
        bool match = false;
        if (curl_easy_getinfo(c, CURLINFO_CERTINFO, &ci) == CURLE_OK && ci && ci->num_of_certs > 0) {
            for (curl_slist *s = ci->certinfo[0]; s; s = s->next)
                if (std::strncmp(s->data, "Cert:", 5) == 0)
                    match = pem_body(s->data + 5) == pem_body(server_cert_);
        }
        if (!match) {
            ok = false;
            err = "the host's certificate doesn't match the one Moonlight paired with (re-pair in Moonlight)";
        }
    }
    curl_easy_cleanup(c);
    return ok;
}

static void fill_info(const std::string &xml, NvServerInfo &out) {
    out.hostname = xml_tag(xml, "hostname");
    out.uniqueid = xml_tag(xml, "uniqueid");
    out.appversion = xml_tag(xml, "appversion");
    out.gfeversion = xml_tag(xml, "GfeVersion");
    out.state = xml_tag(xml, "state");
    out.https_port = std::atoi(xml_tag(xml, "HttpsPort").c_str());
    out.codec_modes = std::atoi(xml_tag(xml, "ServerCodecModeSupport").c_str());
    out.current_game = std::atoi(xml_tag(xml, "currentgame").c_str());
    out.paired = xml_tag(xml, "PairStatus") == "1";
}

bool NvHttp::server_info_http(NvServerInfo &out, std::string &err, long timeout_ms) {
    std::string body;
    if (!get(false, "serverinfo", "", timeout_ms, body, err) || !check_status(body, err)) return false;
    fill_info(body, out);
    if (out.https_port > 0) https_port_ = out.https_port;
    return true;
}

bool NvHttp::server_info(NvServerInfo &out, std::string &err) {
    std::string body;
    if (!get(true, "serverinfo", "", 5000, body, err) || !check_status(body, err)) return false;
    fill_info(body, out);
    return true;
}

bool NvHttp::app_list(std::vector<NvApp> &out, std::string &err) {
    std::string body;
    if (!get(true, "applist", "", 5000, body, err) || !check_status(body, err)) return false;
    out.clear();
    size_t pos = 0, end = 0;
    for (;;) {
        const std::string app = xml_tag(body, "App", pos, &end);
        if (end == std::string::npos) break;
        out.push_back({xml_tag(app, "AppTitle"), std::atoi(xml_tag(app, "ID").c_str())});
        pos = end;
    }
    return true;
}

bool NvHttp::launch(bool resume, int appid, int width, int height, int fps, int audio_config,
                    const unsigned char rikey[16], int rikeyid, std::string &rtsp_url, std::string &err) {
    char key_hex[33];
    for (int i = 0; i < 16; i++) std::snprintf(key_hex + 2 * i, 3, "%02x", rikey[i]);
    const std::string q = "appid=" + std::to_string(appid) +
                          "&mode=" + std::to_string(width) + "x" + std::to_string(height) + "x" + std::to_string(fps) +
                          "&additionalStates=1&sops=0" +
                          "&rikey=" + key_hex + "&rikeyid=" + std::to_string(rikeyid) +
                          "&localAudioPlayMode=0" +
                          "&surroundAudioInfo=" + std::to_string(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(audio_config)) +
                          "&remoteControllersBitmap=1&gcmap=1&gcpersist=0" + LiGetLaunchUrlQueryParameters();
    std::string body;
    // Launching runs framewall's prep command on the host (it creates the
    // virtual monitors), which takes a few seconds.
    if (!get(true, resume ? "resume" : "launch", q, 60000, body, err) || !check_status(body, err)) return false;
    rtsp_url = xml_tag(body, "sessionUrl0");
    const std::string gs = xml_tag(body, resume ? "resume" : "gamesession");
    if (gs == "0") { err = "host refused to start the stream"; return false; }
    return true;
}

bool NvHttp::quit(std::string &err) {
    std::string body;
    return get(true, "cancel", "", 30000, body, err) && check_status(body, err);
}
