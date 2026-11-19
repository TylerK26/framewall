// The wall on screen: decoded NV12 frames -> one sRGB "wall" swapchain image
// (a single GPU pass: the decoder's dma-buf is sampled as YCbCr) -> one
// OpenXR cylinder layer per monitor, each showing its rectangle of the wall.
// SteamVR composites the cylinders itself (high-quality filtering, placed at
// display time, so head motion stays smooth whatever the stream does).
//
// Behind them, a static projection layer: the dark void, or, with
// passthrough, fully transparent (alpha 0, blended with the environment), so
// SteamVR shows the room through it (the OpenXR ALPHA_BLEND blend mode).
//
// Frames are converted on the renderer's own thread as soon as they are
// decoded (start()/stop()); layers() is called from the frame loop.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include "layout.h"
#include "video.h"
#include "xr.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class WallRenderer {
public:
    explicit WallRenderer(Xr &xr) : xr_(xr) {}
    ~WallRenderer();

    // `width` x `height`: the stream size.
    bool init(int width, int height, std::string &err);

    // Starts converting `video`'s frames as they are decoded; stop() before
    // `video` goes away.
    void start(VideoPipeline &video);
    void stop();

    // This frame's layers, valid until the next call. `poses` and `regions`:
    // one per panel (placement on the cylinder, rectangle in the stream).
    // Call with xr.queue_mu held (it's part of the frame's begin/end).
    void layers(XrTime display_time, bool passthrough, const std::vector<PanelPose> &poses,
                const std::vector<Rect> &regions, float radius, std::vector<XrCompositionLayerBaseHeader *> &out);

    // Frames converted since the last call: count, GPU time (sum, max) and
    // decoded -> on-the-wall time (sum), all in ms.
    struct Stats { uint64_t frames = 0; double gpu_sum = 0, gpu_max = 0, latency_sum = 0; };
    Stats take_stats();

private:
    struct Imported {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    bool make_conversion(std::string &err);
    bool make_pipeline(std::string &err);
    bool make_void(std::string &err);
    bool import(const DecodedBuffers &b, std::string &err);
    void run(VideoPipeline *video);
    bool convert(VideoPipeline &video, int idx, uint64_t gen, std::chrono::steady_clock::time_point decoded_at);
    void drop_imports();
    PFN_vkVoidFunction dev_proc(const char *name) const;

    Xr &xr_;
    int width_ = 0, height_ = 0;
    Xr::Swapchain wall_;
    std::vector<VkImageView> wall_views_;
    std::vector<VkFramebuffer> wall_fbs_;
    std::atomic<bool> has_frame_{false};
    std::thread thread_;
    std::atomic<bool> stop_{false};

    VkSamplerYcbcrConversion ycbcr_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkPipeline pipe_ = VK_NULL_HANDLE;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkQueryPool queries_ = VK_NULL_HANDLE;

    DecodedBuffers bufs_;  // fds are owned by the imported memory
    std::vector<Imported> imports_;
    std::vector<bool> initialized_;

    // The background: one static, single-colour image per eye, for the void
    // and for passthrough (transparent).
    Xr::Swapchain void_[2], clear_[2];

    XrCompositionLayerProjection proj_{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView proj_views_[2];
    std::vector<XrCompositionLayerCylinderKHR> cylinders_;

    std::mutex stats_mu_;
    Stats stats_;
};
