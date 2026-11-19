// The wall on screen: decoded NV12 frames -> one sRGB "wall" swapchain image
// (a single GPU pass: the decoder's dma-buf is sampled as YCbCr) -> one
// OpenXR cylinder layer per monitor, each showing its rectangle of the wall.
// SteamVR composites the cylinders itself (high-quality filtering, placed at
// display time, so head motion stays smooth whatever the stream does).
//
// Behind them: a dark void (a static projection layer) or, with passthrough,
// nothing (the room shows through).
//
// All calls from the render (main) thread.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include "layout.h"
#include "video.h"
#include "xr.h"

#include <chrono>
#include <string>
#include <vector>

class WallRenderer {
public:
    explicit WallRenderer(Xr &xr) : xr_(xr) {}
    ~WallRenderer();

    // `width` x `height`: the stream size.
    bool init(int width, int height, std::string &err);

    // Converts the newest decoded frame, if there is one. Before each frame's
    // layers. Returns true if a new frame was converted.
    bool update(VideoPipeline &video);

    // This frame's layers, valid until the next call. `poses` and `regions`:
    // one per panel (placement on the cylinder, rectangle in the stream).
    void layers(XrTime display_time, bool passthrough, const std::vector<PanelPose> &poses,
                const std::vector<Rect> &regions, float radius, std::vector<XrCompositionLayerBaseHeader *> &out);

    double last_gpu_ms() const { return gpu_ms_; }
    // When the frame now on the wall was decoded.
    std::chrono::steady_clock::time_point shown_decoded_at() const { return shown_at_; }

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
    void drop_imports();
    PFN_vkVoidFunction dev_proc(const char *name) const;

    Xr &xr_;
    int width_ = 0, height_ = 0;
    Xr::Swapchain wall_;
    std::vector<VkImageView> wall_views_;
    std::vector<VkFramebuffer> wall_fbs_;
    bool has_frame_ = false;

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

    // The void: one static, single-colour image per eye.
    Xr::Swapchain void_[2];

    XrCompositionLayerProjection proj_{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView proj_views_[2];
    std::vector<XrCompositionLayerCylinderKHR> cylinders_;

    double gpu_ms_ = 0;
    std::chrono::steady_clock::time_point shown_at_{};
};
