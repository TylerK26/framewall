// Wall video: HEVC access units from the stream -> the Steam Frame's hardware
// decoder (qcom iris, V4L2 stateful mem2mem) -> GPU NV12->RGBA pass ->
// SteamVR overlays, one per monitor, each showing its own region of the frame.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include "layout.h"

#include <Limelight.h>
#include <openvr.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct VideoStats {
    std::atomic<uint64_t> received{0}, decoded{0}, shown{0}, errors{0}, dropped{0};
    std::atomic<int> fps{0};                 // shown per second, last interval
    std::atomic<int> decode_us{0}, convert_us{0};
};

class VideoPipeline {
public:
    struct Options {
        std::vector<vr::VROverlayHandle_t> overlays;  // one per region
        std::vector<Rect> regions;                    // in stream pixels
        std::string device = "/dev/video-dec0";
        std::string dump_path;                        // save one frame (testing)
        int dump_frame = 300;
    };

    explicit VideoPipeline(Options o) : opt_(std::move(o)) {}
    ~VideoPipeline() { stop(); }

    // Opens the decoder for a WxH stream and starts the video thread.
    bool start(int width, int height, std::string &err);
    // From moonlight-common-c's decoder thread. Returns DR_OK / DR_NEED_IDR.
    int submit(PDECODE_UNIT du);
    void stop();

    VideoStats stats;

private:
    using clk = std::chrono::steady_clock;
    struct Au { std::vector<uint8_t> data; int frame = 0; clk::time_point t; };
    struct OutBuf { void *ptr = nullptr; size_t len = 0; };
    struct CapBuf { int dmabuf = -1; };

    void run();
    bool setup_capture();
    void teardown_capture();
    bool feed(Au &au);
    void requeue_capture(unsigned idx);

    Options opt_;
    int width_ = 0, height_ = 0;
    int fd_ = -1, wake_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> need_idr_{false};

    std::mutex mu_;
    std::deque<Au> queue_;

    // Owned by the video thread.
    std::vector<OutBuf> obufs_;
    std::deque<unsigned> ofree_;
    std::vector<CapBuf> cbufs_;
    bool capturing_ = false;
    int vis_w_ = 0, vis_h_ = 0;
    std::vector<clk::time_point> fed_at_ = std::vector<clk::time_point>(1024);
};
