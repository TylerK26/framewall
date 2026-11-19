// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "xr.h"

#include "log.h"

#include <cmath>
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
    if (space && space != local_) xrDestroySpace(space);
    if (local_) xrDestroySpace(local_);
    if (view_) xrDestroySpace(view_);
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
            if (!std::strcmp(p.extensionName, "XR_VALVE_frame_controller_interaction")) frame_controller = true;
        }
        if (!vk2 || !cyl) { err = "the OpenXR runtime lacks Vulkan or cylinder layer support"; return false; }
        if (have_refresh_) exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        // The Steam Frame controller's own interaction profile.
        if (frame_controller) exts.push_back("XR_VALVE_frame_controller_interaction");
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
    if (XR_FAILED(r = xrCreateReferenceSpace(session, &rsci, &local_))) return fail("reference space", r);
    space = local_;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xrCreateReferenceSpace(session, &rsci, &view_);
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
                std::lock_guard<std::mutex> lk(queue_mu);
                const XrResult r = xrBeginSession(session, &bi);
                if (XR_FAILED(r)) fwlog("xr: xrBeginSession: %s", xr_result_name(instance, r).c_str());
                else running_ = true;
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                std::lock_guard<std::mutex> lk(queue_mu);
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

void Xr::recenter(XrTime t) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(view_, local_, t, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return;
    // Yaw of the head: where its -Z points, flattened.
    const XrQuaternionf q = loc.pose.orientation;
    const float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    const float yaw = std::atan2(-fx, -fz);
    XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    ci.poseInReferenceSpace.position = loc.pose.position;
    XrSpace s;
    if (XR_FAILED(xrCreateReferenceSpace(session, &ci, &s))) return;
    if (space != local_) xrDestroySpace(space);
    space = s;
    fwlog("xr: recentred");
}

bool Xr::upload_image(Swapchain &sc, const uint32_t *rgba, std::string &err) {
    VkDevice d = vk_device;
    const VkDeviceSize bytes = VkDeviceSize(sc.width) * sc.height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf;
    if (vkCreateBuffer(d, &bci, nullptr, &buf) != VK_SUCCESS) { err = "staging buffer"; return false; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(d, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(vk_phys, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { type = i; break; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem;
    if (vkAllocateMemory(d, &mai, nullptr, &mem) != VK_SUCCESS) { vkDestroyBuffer(d, buf, nullptr); err = "staging memory"; return false; }
    vkBindBufferMemory(d, buf, mem, 0);
    void *map;
    vkMapMemory(d, mem, 0, bytes, 0, &map);
    std::memcpy(map, rgba, bytes);
    vkUnmapMemory(d, mem);

    VkCommandPool pool;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = vk_queue_family;
    vkCreateCommandPool(d, &pci, nullptr, &pool);
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(d, &cai, &cmd);

    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(queue_mu);
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(sc.handle, &ai, &idx))) {
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(sc.handle, &wi);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &bi);
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = sc.images[idx];
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {uint32_t(sc.width), uint32_t(sc.height), 1};
            vkCmdCopyBufferToImage(cmd, buf, sc.images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;  // how OpenXR wants it back
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            ok = vkQueueSubmit(vk_queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
            vkQueueWaitIdle(vk_queue);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(sc.handle, &ri);
        }
    }
    vkDestroyCommandPool(d, pool, nullptr);
    vkDestroyBuffer(d, buf, nullptr);
    vkFreeMemory(d, mem, nullptr);
    if (!ok) err = "image upload";
    return ok;
}
