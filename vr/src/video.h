// Wall video: HEVC access units from the stream -> the Steam Frame's hardware
// decoder (qcom iris, V4L2 stateful mem2mem) -> decoded NV12 frames handed to
// the renderer (see wallrender.h), which samples them straight from the
// decoder's buffers (dma-bufs).
//
// Threads: moonlight-common-c's decoder thread calls submit(); our video
// thread drives the decoder; the render thread takes the newest decoded frame
// with take() and gives it back with give_back() once the GPU is done with it.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include <Limelight.h>

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
};

// The decoder's output buffers. They change only when the decoder (re)starts
// (stream start, size change): `generation` tells the renderer to re-import.
struct DecodedBuffers {
    uint64_t generation = 0;  // 0 = none
    int width = 0, height = 0;      // visible picture
    int alloc_height = 0;           // rows allocated per plane (>= height)
    int stride = 0;                 // bytes per row, both planes
    int uv_offset = 0;              // chroma plane offset in the buffer
    std::vector<int> fds;           // one dma-buf per buffer (owned by VideoPipeline)
};

class VideoPipeline {
public:
    struct Options {
        std::string device = "/dev/video-dec0";
    };

    explicit VideoPipeline(Options o) : opt_(std::move(o)) {}
    ~VideoPipeline() { stop(); }

    // Opens the decoder for a WxH stream and starts the video thread.
    bool start(int width, int height, std::string &err);
    // From moonlight-common-c's decoder thread. Returns DR_OK / DR_NEED_IDR.
    int submit(PDECODE_UNIT du);
    void stop();

    // Render thread. The buffer set, duplicating its dma-buf fds (the caller
    // owns the copies); returns false if it hasn't changed since `generation`.
    bool buffers_if_changed(uint64_t generation, DecodedBuffers &out);
    // The newest decoded frame not yet taken: buffer index and generation,
    // or false. Older untaken frames go straight back to the decoder.
    bool take(int &index, uint64_t &generation, std::chrono::steady_clock::time_point &decoded_at);
    // Like take(), but waits up to `timeout` for a frame.
    bool wait_take(int &index, uint64_t &generation, std::chrono::steady_clock::time_point &decoded_at,
                   std::chrono::milliseconds timeout);
    // Hands a taken buffer back to the decoder.
    void give_back(int index, uint64_t generation);

    VideoStats stats;

private:
    using clk = std::chrono::steady_clock;
    struct Au { std::vector<uint8_t> data; int frame = 0; clk::time_point t; };
    struct OutBuf { void *ptr = nullptr; size_t len = 0; };

    void run();
    bool setup_capture();
    void teardown_capture();
    bool feed(Au &au);
    void requeue_capture(unsigned idx);
    void wake();

    Options opt_;
    int width_ = 0, height_ = 0;
    int fd_ = -1, wake_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> need_idr_{false};

    std::mutex mu_;           // guards the access-unit queue and the handoff below
    std::condition_variable ready_cv_;
    std::deque<Au> queue_;
    DecodedBuffers bufs_;     // current buffer set
    int ready_ = -1;          // newest decoded, not yet taken
    clk::time_point ready_at_{};
    std::vector<int> returned_;  // given back by the renderer, to requeue

    // Owned by the video thread.
    std::vector<OutBuf> obufs_;
    std::deque<unsigned> ofree_;
    bool capturing_ = false;
    uint64_t next_generation_ = 1;
};
