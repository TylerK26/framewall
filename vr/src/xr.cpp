// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "xr.h"

#include "log.h"

#include <cstring>

namespace {

bool has_ext(const std::vector<VkExtensionProperties> &all, const char *name) {
    for (auto &e : all)
        if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

}  // namespace

std::string xr_result_name(XrInstance inst, XrResult r) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (inst == XR_NULL_HANDLE || XR_FAILED(xrResultToString(inst, r, buf))) return std::to_string(int(r));
    return buf;
}

template <class T> T Xr::proc(const char *name) const {
    PFN_xrVoidFunction f = nullptr;
    xrGetInstanceProcAddr(instance, name, &f);
    return reinterpret_cast<T>(f);
}

Xr::~Xr() {
    if (vk_device) vkDeviceWaitIdle(vk_device);
    if (space) xrDestroySpace(space);
    if (session) xrDestroySession(session);
    if (vk_device) vkDestroyDevice(vk_device, nullptr);
    if (instance) xrDestroyInstance(instance);
    if (vk_instance) vkDestroyInstance(vk_instance, nullptr);
}

bool Xr::init(std::string &err) {
    auto fail = [&](const std::string &what, XrResult r) {
        err = what + ": " + xr_result_name(instance, r);
        return false;
    };
    std::vector<const char *> exts = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME};
    {
        uint32_t n = 0;
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) { err = "no OpenXR runtime (is SteamVR running?)"; return false; }
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
        bool vk2 = false, cyl = false;
        for (auto &p : props) {
            if (!std::strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) vk2 = true;
            if (!std::strcmp(p.extensionName, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)) cyl = true;
            if (!std::strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) have_refresh_ = true;
        }
        if (!vk2 || !cyl) { err = "the OpenXR runtime lacks Vulkan or cylinder layer support"; return false; }
        if (have_refresh_) exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "framewall");
    std::snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "framewall");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.enabledExtensionNames = exts.data();
    XrResult r = xrCreateInstance(&ici, &instance);
    if (XR_FAILED(r)) { instance = XR_NULL_HANDLE; return fail("OpenXR instance (is SteamVR running?)", r); }
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(r = xrGetSystem(instance, &sgi, &system))) return fail("no headset", r);
    {
        uint32_t n = 0;
        xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
        std::vector<XrEnvironmentBlendMode> modes(n);
        xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, modes.data());
        for (auto m : modes)
            if (m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) alpha_blend_ = true;
    }

    // ---- Vulkan, through the runtime ----
    auto getReq = proc<PFN_xrGetVulkanGraphicsRequirements2KHR>("xrGetVulkanGraphicsRequirements2KHR");
    auto createInst = proc<PFN_xrCreateVulkanInstanceKHR>("xrCreateVulkanInstanceKHR");
    auto getDev = proc<PFN_xrGetVulkanGraphicsDevice2KHR>("xrGetVulkanGraphicsDevice2KHR");
    auto createDev = proc<PFN_xrCreateVulkanDeviceKHR>("xrCreateVulkanDeviceKHR");
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (XR_FAILED(r = getReq(instance, system, &req))) return fail("Vulkan requirements", r);

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "framewall";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo vci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vci.pApplicationInfo = &ai;
    XrVulkanInstanceCreateInfoKHR xvci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xvci.systemId = system;
    xvci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xvci.vulkanCreateInfo = &vci;
    VkResult vr = VK_SUCCESS;
    if (XR_FAILED(r = createInst(instance, &xvci, &vk_instance, &vr)) || vr != VK_SUCCESS) {
        err = "Vulkan instance: " + (XR_FAILED(r) ? xr_result_name(instance, r) : std::to_string(int(vr)));
        return false;
    }
    XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gdi.systemId = system;
    gdi.vulkanInstance = vk_instance;
    if (XR_FAILED(r = getDev(instance, &gdi, &vk_phys))) return fail("Vulkan device", r);
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(vk_phys, &pp);
    vk_timestamp_period = pp.limits.timestampPeriod;
    fwlog("xr: Vulkan on %s", pp.deviceName);
    {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vk_phys, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(vk_phys, &nq, qf.data());
        for (uint32_t i = 0; i < nq; i++)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk_queue_family = i; break; }
    }
    // Importing the decoder's dma-bufs (linear NV12) and sampling them as YCbCr.
    const char *dev_exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                              VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME};
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(vk_phys, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> all(n);
        vkEnumerateDeviceExtensionProperties(vk_phys, nullptr, &n, all.data());
        for (auto *e : dev_exts)
            if (!has_ext(all, e)) { err = std::string("the GPU lacks ") + e; return false; }
    }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = vk_queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.samplerYcbcrConversion = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f11;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(std::size(dev_exts));
    dci.ppEnabledExtensionNames = dev_exts;
    XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdci.systemId = system;
    xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xdci.vulkanPhysicalDevice = vk_phys;
    xdci.vulkanCreateInfo = &dci;
    if (XR_FAILED(r = createDev(instance, &xdci, &vk_device, &vr)) || vr != VK_SUCCESS) {
        err = "Vulkan device: " + (XR_FAILED(r) ? xr_result_name(instance, r) : std::to_string(int(vr)));
        return false;
    }
    vkGetDeviceQueue(vk_device, vk_queue_family, 0, &vk_queue);

    // ---- session ----
    XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gb.instance = vk_instance;
    gb.physicalDevice = vk_phys;
    gb.device = vk_device;
    gb.queueFamilyIndex = vk_queue_family;
    gb.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = system;
    if (XR_FAILED(r = xrCreateSession(instance, &sci, &session))) return fail("OpenXR session", r);
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1;
    if (XR_FAILED(r = xrCreateReferenceSpace(session, &rsci, &space))) return fail("reference space", r);
    {
        uint32_t n = 0;
        xrEnumerateSwapchainFormats(session, 0, &n, nullptr);
        std::vector<int64_t> f(n);
        xrEnumerateSwapchainFormats(session, n, &n, f.data());
        for (auto x : f)
            if (x == VK_FORMAT_R8G8B8A8_SRGB) color_format = x;
        if (!color_format) { err = "no sRGB RGBA swapchain format"; return false; }
    }
    fwlog("xr: session ready; passthrough %s", alpha_blend_ ? "available" : "not available");
    return true;
}

bool Xr::poll() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            state_ = reinterpret_cast<XrEventDataSessionStateChanged &>(ev).state;
            if (state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult r = xrBeginSession(session, &bi);
                if (XR_FAILED(r)) fwlog("xr: xrBeginSession: %s", xr_result_name(instance, r).c_str());
                else running_ = true;
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                xrEndSession(session);
                running_ = false;
            } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
                running_ = false;
                exiting_ = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            running_ = false;
            exiting_ = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !exiting_;
}

void Xr::request_exit() {
    if (running_) xrRequestExitSession(session);
    else exiting_ = true;
}

float Xr::refresh_rate() const {
    float hz = 0;
    if (have_refresh_) {
        auto get = proc<PFN_xrGetDisplayRefreshRateFB>("xrGetDisplayRefreshRateFB");
        if (get) get(session, &hz);
    }
    return hz;
}

void Xr::request_refresh_rate(float hz) {
    if (!have_refresh_) return;
    auto req = proc<PFN_xrRequestDisplayRefreshRateFB>("xrRequestDisplayRefreshRateFB");
    if (!req) return;
    const XrResult r = req(session, hz);
    if (XR_FAILED(r)) fwlog("xr: can't switch the headset to %.0f Hz (%s); set it in SteamVR's video settings for this app",
                            hz, xr_result_name(instance, r).c_str());
}

bool Xr::make_swapchain(int w, int h, XrSwapchainUsageFlags usage, bool static_image, Swapchain &out, std::string &err) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.createFlags = static_image ? XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT : 0;
    ci.usageFlags = usage;
    ci.format = color_format;
    ci.sampleCount = 1;
    ci.width = uint32_t(w);
    ci.height = uint32_t(h);
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(session, &ci, &out.handle);
    if (XR_FAILED(r)) { err = "swapchain " + std::to_string(w) + "x" + std::to_string(h) + ": " + xr_result_name(instance, r); return false; }
    out.width = w;
    out.height = h;
    uint32_t n = 0;
    xrEnumerateSwapchainImages(out.handle, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    xrEnumerateSwapchainImages(out.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(imgs.data()));
    out.images.clear();
    for (auto &im : imgs) out.images.push_back(im.image);
    return true;
}

void Xr::destroy_swapchain(Swapchain &s) {
    if (s.handle) xrDestroySwapchain(s.handle);
    s = Swapchain{};
}
