// OpenXR session on Vulkan: the native (scene app) side of framewall-vr.
//
// One Vulkan device, created through OpenXR (XR_KHR_vulkan_enable2) with what
// the wall renderer needs: dma-buf import of the decoder's NV12 frames and
// YCbCr sampling. A LOCAL reference space (seated, recentred by SteamVR).
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class Xr {
public:
    ~Xr();

    // Creates the instance, Vulkan device and session. False with `err` if
    // SteamVR isn't running or lacks something we need.
    bool init(std::string &err);

    // Pumps OpenXR events. Returns false once the session has ended for good
    // (exit requested, SteamVR quitting): then stop.
    bool poll();
    bool running() const { return running_; }     // frames should be submitted
    bool focused() const { return state_ == XR_SESSION_STATE_FOCUSED; }
    void request_exit();

    // Display refresh rate (Hz), and a request to change it (only what
    // SteamVR's settings allow for this app is accepted).
    float refresh_rate() const;
    void request_refresh_rate(float hz);

    bool passthrough_supported() const { return alpha_blend_; }

    // Swapchain of `fmt` images. With `static_image`, it's acquired once.
    struct Swapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        int width = 0, height = 0;
        std::vector<VkImage> images;
    };
    bool make_swapchain(int w, int h, XrSwapchainUsageFlags usage, bool static_image, Swapchain &out, std::string &err);
    void destroy_swapchain(Swapchain &s);
    // Copies RGBA8 pixels (sRGB, premultiplied alpha) into the swapchain's
    // next image and releases it. Main thread, not inside a frame.
    bool upload_image(Swapchain &s, const uint32_t *rgba, std::string &err);

    // Moves `space` so its origin is the head now, facing where it looks
    // (turned about the vertical axis only).
    void recenter(XrTime t);

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;  // where everything is placed: LOCAL, or recentred from it
    int64_t color_format = 0;        // sRGB RGBA swapchain format
    bool frame_controller = false;   // XR_VALVE_frame_controller_interaction enabled

    VkInstance vk_instance = VK_NULL_HANDLE;
    VkPhysicalDevice vk_phys = VK_NULL_HANDLE;
    VkDevice vk_device = VK_NULL_HANDLE;
    uint32_t vk_queue_family = 0;
    VkQueue vk_queue = VK_NULL_HANDLE;
    float vk_timestamp_period = 1;   // ns per timestamp tick

    // The Vulkan queue is shared with the runtime, which may use it in
    // xrBeginFrame/xrEndFrame and the swapchain calls: hold this around those
    // and around our own submits (frames are converted on their own thread).
    std::mutex queue_mu;

private:
    template <class T> T proc(const char *name) const;

    XrSpace local_ = XR_NULL_HANDLE, view_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    std::atomic<bool> running_{false};
    bool exiting_ = false;
    bool alpha_blend_ = false, have_refresh_ = false;
};

// "XR_ERROR_..." name of an OpenXR result, for messages.
std::string xr_result_name(XrInstance inst, XrResult r);
