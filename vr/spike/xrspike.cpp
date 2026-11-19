// xrspike: feasibility test for a native (OpenXR scene app) framewall on the
// Steam Frame. No streaming: three curved panels showing a static 5760x1200
// test pattern, side by side like a "row" wall.
//
//   xrspike [--mode proj|cyl] [--passthrough] [--hz N] [--seconds N] [--scale S]
//
//   proj  the app draws the panels into the eye images (projection layer)
//   cyl   the panels are OpenXR cylinder layers composited by SteamVR; the
//         texture is copied into the layer's swapchain every frame, like a
//         video frame would be
//   --passthrough  ALPHA_BLEND: the room shows wherever no panel is drawn;
//                  default is OPAQUE with a dark void
//
// Logs every 5 s: frames, frames the app missed, GPU time of our work.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

[[noreturn]] void die(const char *what, long code) {
    std::fprintf(stderr, "xrspike: %s failed (%ld)\n", what, code);
    std::exit(1);
}
#define XR(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) die(#x, r_); } while (0)
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) die(#x, r_); } while (0)

volatile sig_atomic_t g_quit = 0;
void on_signal(int) { g_quit = 1; }

// ---- math (column-major 4x4) ------------------------------------------------
struct Mat { float m[16]; };
Mat mul(const Mat &a, const Mat &b) {
    Mat r{};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
Mat projection(const XrFovf &f, float n, float fa) {
    const float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), d = std::tan(f.angleDown), u = std::tan(f.angleUp);
    const float w = r - l, h = d - u;  // Vulkan: y down, z 0..1
    Mat m{};
    m.m[0] = 2 / w;
    m.m[8] = (r + l) / w;
    m.m[5] = 2 / h;
    m.m[9] = (u + d) / h;
    m.m[10] = -fa / (fa - n);
    m.m[14] = -(fa * n) / (fa - n);
    m.m[11] = -1;
    return m;
}
Mat view_from_pose(const XrPosef &p) {
    const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    // Rotation matrix R (pose -> space); view = R^T * translate(-pos).
    const float R[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                        2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                        2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
    // R stored column-major: column c = R[c*3 .. c*3+2]. R^T rows = R columns.
    Mat v{};
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) v.m[c * 4 + r] = R[r * 3 + c];
    const float px = p.position.x, py = p.position.y, pz = p.position.z;
    for (int r = 0; r < 3; r++) v.m[12 + r] = -(v.m[0 + r] * px + v.m[4 + r] * py + v.m[8 + r] * pz);
    v.m[15] = 1;
    return v;
}

// ---- the test wall ------------------------------------------------------------
constexpr int kTexW = 5760, kTexH = 1200;
struct Panel { int x, y, w, h; };
const Panel kPanels[3] = {{0, 60, 1920, 1080}, {1920, 0, 1920, 1200}, {3840, 60, 1920, 1080}};
constexpr float kRadius = 1.5f, kPxPerM = 1920 / 1.3f, kGap = 0.04f;

std::vector<uint32_t> test_pattern() {
    std::vector<uint32_t> px(size_t(kTexW) * kTexH);
    for (int y = 0; y < kTexH; y++)
        for (int x = 0; x < kTexW; x++) {
            const int panel = x / 1920, lx = x % 1920;
            uint8_t r = uint8_t(40 + panel * 50), g = uint8_t(lx * 255 / 1920), b = uint8_t(y * 255 / kTexH);
            if ((lx % 120) < 2 || (y % 120) < 2) r = g = b = 255;           // grid
            if (((lx / 8) + (y / 8)) % 2 == 0 && y > 500 && y < 700) r = g = b = 230;  // fine checker (text-like detail)
            px[size_t(y) * kTexW + x] = 0xff000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
        }
    return px;
}

struct Vtx { float x, y, z, u, v; };
std::vector<Vtx> cylinder_mesh(std::vector<float> &centers, std::vector<float> &angles) {
    std::vector<Vtx> out;
    float total = 0;
    for (auto &p : kPanels) total += p.w / kPxPerM / kRadius;
    total += 2 * kGap / kRadius;
    float a0 = -total / 2;
    for (auto &p : kPanels) {
        const float ang = p.w / kPxPerM / kRadius, hm = p.h / kPxPerM;
        centers.push_back(a0 + ang / 2);
        angles.push_back(ang);
        const int seg = 48;
        for (int i = 0; i < seg; i++) {
            auto vert = [&](int k, float yy, float vv) {
                const float a = a0 + ang * k / seg;
                return Vtx{kRadius * std::sin(a), yy, -kRadius * std::cos(a),
                           (p.x + p.w * float(k) / seg) / kTexW, vv};
            };
            const float vt = float(p.y) / kTexH, vb = float(p.y + p.h) / kTexH;
            Vtx a = vert(i, hm / 2, vt), b = vert(i + 1, hm / 2, vt), c = vert(i, -hm / 2, vb), d = vert(i + 1, -hm / 2, vb);
            out.insert(out.end(), {a, c, b, b, c, d});
        }
        a0 += ang + kGap / kRadius;
    }
    return out;
}

std::vector<uint32_t> read_spv(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::fprintf(stderr, "xrspike: can't read %s\n", path.c_str()); std::exit(1); }
    std::vector<uint32_t> d(size_t(f.tellg()) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char *>(d.data()), std::streamsize(d.size() * 4));
    return d;
}

std::string exe_dir() {
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    std::string s(buf, n > 0 ? size_t(n) : 0);
    return s.substr(0, s.rfind('/'));
}

// ---- Vulkan helpers -------------------------------------------------------------
struct Vk {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem{};

    uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) return i;
        die("no memory type", long(want));
    }
    VkDeviceMemory alloc(VkMemoryRequirements req, VkMemoryPropertyFlags want) const {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mem_type(req.memoryTypeBits, want);
        VkDeviceMemory m;
        VK(vkAllocateMemory(dev, &ai, nullptr, &m));
        return m;
    }
    VkBuffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory &m, void **map) const {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        VkBuffer b;
        VK(vkCreateBuffer(dev, &bi, nullptr, &b));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, b, &req);
        m = alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK(vkBindBufferMemory(dev, b, m, 0));
        VK(vkMapMemory(dev, m, 0, size, 0, map));
        return b;
    }
};

void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
             VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

template <class T> T xr_proc(XrInstance inst, const char *name) {
    PFN_xrVoidFunction f = nullptr;
    XR(xrGetInstanceProcAddr(inst, name, &f));
    return reinterpret_cast<T>(f);
}

struct Swapchain {
    XrSwapchain sc = XR_NULL_HANDLE;
    int w = 0, h = 0;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> fbs;
};

}  // namespace

int main(int argc, char **argv) {
    std::string mode = "proj";
    bool passthrough = false;
    float hz = 144, scale = 1.0f;
    int seconds = 60;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--mode") mode = next();
        else if (a == "--passthrough") passthrough = true;
        else if (a == "--hz") hz = std::stof(next());
        else if (a == "--seconds") seconds = std::stoi(next());
        else if (a == "--scale") scale = std::stof(next());
        else { std::fprintf(stderr, "usage: xrspike [--mode proj|cyl] [--passthrough] [--hz N] [--seconds N] [--scale S]\n"); return 2; }
    }
    if (mode != "proj" && mode != "cyl") { std::fprintf(stderr, "xrspike: --mode is proj or cyl\n"); return 2; }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // ---- OpenXR instance ----
    std::vector<const char *> exts = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME};
    bool have_refresh = false;
    {
        uint32_t n = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
        for (auto &p : props)
            if (!std::strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) have_refresh = true;
        if (have_refresh) exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "framewall xrspike");
    std::snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "framewall");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.enabledExtensionNames = exts.data();
    XrInstance xi;
    XR(xrCreateInstance(&ici, &xi));
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys;
    XR(xrGetSystem(xi, &sgi, &sys));

    // ---- Vulkan through OpenXR ----
    auto getReq = xr_proc<PFN_xrGetVulkanGraphicsRequirements2KHR>(xi, "xrGetVulkanGraphicsRequirements2KHR");
    auto createInst = xr_proc<PFN_xrCreateVulkanInstanceKHR>(xi, "xrCreateVulkanInstanceKHR");
    auto getDev = xr_proc<PFN_xrGetVulkanGraphicsDevice2KHR>(xi, "xrGetVulkanGraphicsDevice2KHR");
    auto createDev = xr_proc<PFN_xrCreateVulkanDeviceKHR>(xi, "xrCreateVulkanDeviceKHR");
    XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR(getReq(xi, sys, &req));

    Vk vk;
    {
        VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "framewall xrspike";
        ai.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &ai;
        XrVulkanInstanceCreateInfoKHR xci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        xci.systemId = sys;
        xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xci.vulkanCreateInfo = &ci;
        VkResult vr;
        XR(createInst(xi, &xci, &vk.inst, &vr));
        VK(vr);
        XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        gi.systemId = sys;
        gi.vulkanInstance = vk.inst;
        XR(getDev(xi, &gi, &vk.phys));
        vkGetPhysicalDeviceMemoryProperties(vk.phys, &vk.mem);
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &nq, qf.data());
        for (uint32_t i = 0; i < nq; i++)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk.qfam = i; break; }
        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = vk.qfam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        xdci.systemId = sys;
        xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xdci.vulkanPhysicalDevice = vk.phys;
        xdci.vulkanCreateInfo = &dci;
        XR(createDev(xi, &xdci, &vk.dev, &vr));
        VK(vr);
        vkGetDeviceQueue(vk.dev, vk.qfam, 0, &vk.queue);
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(vk.phys, &pp);
        std::printf("vulkan: %s\n", pp.deviceName);
    }
    VkPhysicalDeviceProperties pprops;
    vkGetPhysicalDeviceProperties(vk.phys, &pprops);

    // ---- session ----
    XrGraphicsBindingVulkan2KHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gb.instance = vk.inst;
    gb.physicalDevice = vk.phys;
    gb.device = vk.dev;
    gb.queueFamilyIndex = vk.qfam;
    gb.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = sys;
    XrSession session;
    XR(xrCreateSession(xi, &sci, &session));
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1;
    XrSpace space;
    XR(xrCreateReferenceSpace(session, &rsci, &space));

    const XrEnvironmentBlendMode blend = passthrough ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    // ---- swapchains ----
    int64_t fmt = 0;
    {
        uint32_t n = 0;
        XR(xrEnumerateSwapchainFormats(session, 0, &n, nullptr));
        std::vector<int64_t> f(n);
        XR(xrEnumerateSwapchainFormats(session, n, &n, f.data()));
        for (auto x : f) if (x == VK_FORMAT_R8G8B8A8_SRGB) fmt = x;
        if (!fmt) for (auto x : f) if (x == VK_FORMAT_B8G8R8A8_SRGB) fmt = x;
        if (!fmt) die("no sRGB swapchain format", 0);
    }
    VkRenderPass rp;
    {
        VkAttachmentDescription at{};
        at.format = VkFormat(fmt);
        at.samples = VK_SAMPLE_COUNT_1_BIT;
        at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        at.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1;
        ci.pAttachments = &at;
        ci.subpassCount = 1;
        ci.pSubpasses = &sp;
        VK(vkCreateRenderPass(vk.dev, &ci, nullptr, &rp));
    }
    auto make_swapchain = [&](int w, int h, XrSwapchainUsageFlags usage, bool fb) {
        Swapchain s;
        s.w = w;
        s.h = h;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = usage;
        ci.format = fmt;
        ci.sampleCount = 1;
        ci.width = uint32_t(w);
        ci.height = uint32_t(h);
        ci.faceCount = ci.arraySize = ci.mipCount = 1;
        XR(xrCreateSwapchain(session, &ci, &s.sc));
        uint32_t n = 0;
        XR(xrEnumerateSwapchainImages(s.sc, 0, &n, nullptr));
        std::vector<XrSwapchainImageVulkan2KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        XR(xrEnumerateSwapchainImages(s.sc, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader *>(imgs.data())));
        for (auto &im : imgs) {
            s.images.push_back(im.image);
            if (!fb) continue;
            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = im.image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VkFormat(fmt);
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView v;
            VK(vkCreateImageView(vk.dev, &vci, nullptr, &v));
            s.views.push_back(v);
            VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = rp;
            fci.attachmentCount = 1;
            fci.pAttachments = &v;
            fci.width = uint32_t(w);
            fci.height = uint32_t(h);
            fci.layers = 1;
            VkFramebuffer f;
            VK(vkCreateFramebuffer(vk.dev, &fci, nullptr, &f));
            s.fbs.push_back(f);
        }
        return s;
    };
    std::vector<XrViewConfigurationView> vcv(2, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    {
        uint32_t n = 0;
        XR(xrEnumerateViewConfigurationViews(xi, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv.data()));
    }
    Swapchain eye[2];
    Swapchain wall;
    if (mode == "proj") {
        for (int e = 0; e < 2; e++)
            eye[e] = make_swapchain(int(vcv[e].recommendedImageRectWidth * scale), int(vcv[e].recommendedImageRectHeight * scale),
                                    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, true);
        std::printf("eye images %dx%d (scale %.2f)\n", eye[0].w, eye[0].h, scale);
    } else {
        wall = make_swapchain(kTexW, kTexH, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, false);
        if (!passthrough)  // a dark void needs a projection layer to draw it
            for (int e = 0; e < 2; e++) eye[e] = make_swapchain(256, 256, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT, true);
    }

    // ---- command pool, test texture ----
    VkCommandPool pool;
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = vk.qfam;
        VK(vkCreateCommandPool(vk.dev, &ci, nullptr, &pool));
    }
    VkCommandBuffer cb;
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK(vkAllocateCommandBuffers(vk.dev, &ai, &cb));
    }
    VkFence fence;
    {
        VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK(vkCreateFence(vk.dev, &ci, nullptr, &fence));
    }
    auto submit_wait = [&] {
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VK(vkQueueSubmit(vk.queue, 1, &si, fence));
        VK(vkWaitForFences(vk.dev, 1, &fence, VK_TRUE, UINT64_MAX));
        VK(vkResetFences(vk.dev, 1, &fence));
    };
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkImage tex;
    VkImageView texview;
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_SRGB;
        ci.extent = {uint32_t(kTexW), uint32_t(kTexH), 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VK(vkCreateImage(vk.dev, &ci, nullptr, &tex));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(vk.dev, tex, &mr);
        VK(vkBindImageMemory(vk.dev, tex, vk.alloc(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0));
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = tex;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_SRGB;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK(vkCreateImageView(vk.dev, &vci, nullptr, &texview));

        const auto px = test_pattern();
        VkDeviceMemory sm;
        void *map;
        VkBuffer staging = vk.buffer(px.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, sm, &map);
        std::memcpy(map, px.data(), px.size() * 4);
        VK(vkBeginCommandBuffer(cb, &cbi));
        barrier(cb, tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy bic{};
        bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bic.imageExtent = {uint32_t(kTexW), uint32_t(kTexH), 1};
        vkCmdCopyBufferToImage(cb, staging, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        barrier(cb, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
        VK(vkEndCommandBuffer(cb));
        submit_wait();
        vkDestroyBuffer(vk.dev, staging, nullptr);
        vkFreeMemory(vk.dev, sm, nullptr);
    }

    // ---- pipeline (proj mode) ----
    std::vector<float> centers, angles;
    const auto mesh = cylinder_mesh(centers, angles);
    VkDeviceMemory vm;
    void *vmap;
    VkBuffer vbuf = vk.buffer(mesh.size() * sizeof(Vtx), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vm, &vmap);
    std::memcpy(vmap, mesh.data(), mesh.size() * sizeof(Vtx));

    VkSampler sampler;
    {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
        ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod = 0;
        VK(vkCreateSampler(vk.dev, &ci, nullptr, &sampler));
    }
    VkDescriptorSetLayout dsl;
    {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &b;
        VK(vkCreateDescriptorSetLayout(vk.dev, &ci, nullptr, &dsl));
    }
    VkDescriptorSet ds;
    {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 1;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = &ps;
        VkDescriptorPool dp;
        VK(vkCreateDescriptorPool(vk.dev, &ci, nullptr, &dp));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = dp;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &dsl;
        VK(vkAllocateDescriptorSets(vk.dev, &ai, &ds));
        VkDescriptorImageInfo ii{sampler, texview, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = ds;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(vk.dev, 1, &w, 0, nullptr);
    }
    VkPipelineLayout pl;
    {
        VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat)};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &dsl;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pc;
        VK(vkCreatePipelineLayout(vk.dev, &ci, nullptr, &pl));
    }
    VkPipeline pipe;
    {
        auto module = [&](const char *name) {
            const auto code = read_spv(exe_dir() + "/" + name);
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = code.size() * 4;
            ci.pCode = code.data();
            VkShaderModule m;
            VK(vkCreateShaderModule(vk.dev, &ci, nullptr, &m));
            return m;
        };
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = module("panel.vert.spv");
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = module("panel.frag.spv");
        st[1].pName = "main";
        VkVertexInputBindingDescription vb{0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription va[2] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions = va;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = 1;
        cbs.pAttachments = &cba;
        VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2;
        ci.pStages = st;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vps;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &cbs;
        ci.pDynamicState = &dy;
        ci.layout = pl;
        ci.renderPass = rp;
        VK(vkCreateGraphicsPipelines(vk.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe));
    }
    VkQueryPool qp;
    {
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = 2;
        VK(vkCreateQueryPool(vk.dev, &ci, nullptr, &qp));
    }

    // Void colour: dark, opaque. Passthrough: transparent.
    const VkClearValue clear = passthrough ? VkClearValue{{{0, 0, 0, 0}}} : VkClearValue{{{0.010f, 0.011f, 0.016f, 1}}};

    std::printf("mode %s, %s, target %.0f Hz\n", mode.c_str(), passthrough ? "passthrough (ALPHA_BLEND)" : "dark void (OPAQUE)", hz);

    // ---- frame loop ----
    bool running = false, refresh_set = false;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    const auto t_start = std::chrono::steady_clock::now();
    auto t_report = t_start;
    uint64_t frames = 0, missed = 0, rendered = 0;
    double gpu_sum = 0, gpu_max = 0;
    XrTime last_display = 0;
    while (!g_quit) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(xi, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                state = reinterpret_cast<XrEventDataSessionStateChanged &>(ev).state;
                std::printf("session state %d\n", int(state));
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR(xrBeginSession(session, &bi));
                    running = true;
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    XR(xrEndSession(session));
                    running = false;
                } else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
                    g_quit = 1;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                g_quit = 1;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) { usleep(10000); continue; }
        if (have_refresh && !refresh_set) {
            refresh_set = true;
            auto enumRates = xr_proc<PFN_xrEnumerateDisplayRefreshRatesFB>(xi, "xrEnumerateDisplayRefreshRatesFB");
            auto request = xr_proc<PFN_xrRequestDisplayRefreshRateFB>(xi, "xrRequestDisplayRefreshRateFB");
            auto getRate = xr_proc<PFN_xrGetDisplayRefreshRateFB>(xi, "xrGetDisplayRefreshRateFB");
            uint32_t n = 0;
            enumRates(session, 0, &n, nullptr);
            std::vector<float> rates(n);
            enumRates(session, n, &n, rates.data());
            std::printf("refresh rates:");
            for (float r : rates) std::printf(" %.0f", r);
            const XrResult rr = request(session, hz);
            float now = 0;
            getRate(session, &now);
            std::printf("; requested %.0f (%d), now %.0f\n", hz, int(rr), now);
        }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XR(xrWaitFrame(session, nullptr, &fs));
        XR(xrBeginFrame(session, nullptr));
        frames++;
        if (last_display && fs.predictedDisplayTime - last_display > fs.predictedDisplayPeriod * 3 / 2)
            missed += uint64_t((fs.predictedDisplayTime - last_display) / fs.predictedDisplayPeriod) - 1;
        last_display = fs.predictedDisplayTime;

        std::vector<XrCompositionLayerBaseHeader *> layers;
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerCylinderKHR cyl[3];

        if (fs.shouldRender) {
            XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.displayTime = fs.predictedDisplayTime;
            li.space = space;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t nv = 0;
            XR(xrLocateViews(session, &li, &vs, 2, &nv, views));

            VK(vkBeginCommandBuffer(cb, &cbi));
            vkCmdResetQueryPool(cb, qp, 0, 2);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
            uint32_t wall_idx = 0, eye_idx[2] = {0, 0};
            const bool draw_eyes = eye[0].sc != XR_NULL_HANDLE;
            if (mode == "cyl") {
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XR(xrAcquireSwapchainImage(wall.sc, &ai, &wall_idx));
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                XR(xrWaitSwapchainImage(wall.sc, &wi));
                VkImage dst = wall.images[wall_idx];
                barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkImageCopy ic{};
                ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                ic.extent = {uint32_t(kTexW), uint32_t(kTexH), 1};
                vkCmdCopyImage(cb, tex, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
                barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            if (draw_eyes) {
                for (int e = 0; e < 2; e++) {
                    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    XR(xrAcquireSwapchainImage(eye[e].sc, &ai, &eye_idx[e]));
                    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    wi.timeout = XR_INFINITE_DURATION;
                    XR(xrWaitSwapchainImage(eye[e].sc, &wi));
                    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                    rbi.renderPass = rp;
                    rbi.framebuffer = eye[e].fbs[eye_idx[e]];
                    rbi.renderArea = {{0, 0}, {uint32_t(eye[e].w), uint32_t(eye[e].h)}};
                    rbi.clearValueCount = 1;
                    rbi.pClearValues = &clear;
                    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
                    if (mode == "proj") {
                        VkViewport vp{0, 0, float(eye[e].w), float(eye[e].h), 0, 1};
                        VkRect2D sc{{0, 0}, {uint32_t(eye[e].w), uint32_t(eye[e].h)}};
                        vkCmdSetViewport(cb, 0, 1, &vp);
                        vkCmdSetScissor(cb, 0, 1, &sc);
                        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, nullptr);
                        const Mat mvp = mul(projection(views[e].fov, 0.05f, 100.0f), view_from_pose(views[e].pose));
                        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof mvp, &mvp);
                        VkDeviceSize off = 0;
                        vkCmdBindVertexBuffers(cb, 0, 1, &vbuf, &off);
                        vkCmdDraw(cb, uint32_t(mesh.size()), 1, 0, 0);
                    }
                    vkCmdEndRenderPass(cb);
                    pv[e].pose = views[e].pose;
                    pv[e].fov = views[e].fov;
                    pv[e].subImage.swapchain = eye[e].sc;
                    pv[e].subImage.imageRect = {{0, 0}, {eye[e].w, eye[e].h}};
                }
            }
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
            VK(vkEndCommandBuffer(cb));
            submit_wait();
            uint64_t ts[2];
            if (vkGetQueryPoolResults(vk.dev, qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                const double ms = double(ts[1] - ts[0]) * pprops.limits.timestampPeriod / 1e6;
                gpu_sum += ms;
                gpu_max = std::max(gpu_max, ms);
            }
            rendered++;
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if (mode == "cyl") XR(xrReleaseSwapchainImage(wall.sc, &ri));
            if (draw_eyes) {
                for (int e = 0; e < 2; e++) XR(xrReleaseSwapchainImage(eye[e].sc, &ri));
                proj.layerFlags = passthrough ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
                proj.space = space;
                proj.viewCount = 2;
                proj.views = pv;
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&proj));
            }
            if (mode == "cyl") {
                for (int i = 0; i < 3; i++) {
                    const Panel &p = kPanels[i];
                    cyl[i] = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
                    cyl[i].layerFlags = 0;
                    cyl[i].space = space;
                    cyl[i].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    cyl[i].subImage.swapchain = wall.sc;
                    cyl[i].subImage.imageRect = {{p.x, p.y}, {p.w, p.h}};
                    const float a = -centers[size_t(i)];
                    cyl[i].pose.orientation = {0, std::sin(a / 2), 0, std::cos(a / 2)};
                    cyl[i].radius = kRadius;
                    cyl[i].centralAngle = angles[size_t(i)];
                    cyl[i].aspectRatio = float(p.w) / float(p.h);
                    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&cyl[i]));
                }
            }
        }
        XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
        fe.displayTime = fs.predictedDisplayTime;
        fe.environmentBlendMode = blend;
        fe.layerCount = uint32_t(layers.size());
        fe.layers = layers.data();
        XR(xrEndFrame(session, &fe));

        const auto now = std::chrono::steady_clock::now();
        if (now - t_report >= std::chrono::seconds(5)) {
            const double secs = std::chrono::duration<double>(now - t_report).count();
            std::printf("%.0f fps (period %.2f ms), app missed %llu, gpu %.2f ms avg / %.2f max\n", frames / secs,
                        fs.predictedDisplayPeriod / 1e6, (unsigned long long)missed, rendered ? gpu_sum / rendered : 0.0, gpu_max);
            frames = missed = rendered = 0;
            gpu_sum = gpu_max = 0;
            t_report = now;
        }
        if (now - t_start > std::chrono::seconds(seconds)) {
            XR(xrRequestExitSession(session));
            seconds = 1 << 30;
        }
    }
    vkDeviceWaitIdle(vk.dev);
    xrDestroySession(session);
    xrDestroyInstance(xi);
    return 0;
}
