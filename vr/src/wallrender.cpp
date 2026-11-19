// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "wallrender.h"

#include "log.h"

#include <drm_fourcc.h>

#include <cmath>
#include <unistd.h>

namespace {

const uint32_t kWallVert[] = {
#include "wall.vert.inc"
};
const uint32_t kWallFrag[] = {
#include "wall.frag.inc"
};

// The void: very dark blue-grey, so the panels' edges still read.
constexpr VkClearColorValue kVoidColor = {{0.010f, 0.011f, 0.016f, 1.0f}};
constexpr int kVoidSize = 64;
constexpr VkFormat kNv12 = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

#define VKF(x, what) do { const VkResult r_ = (x); if (r_ != VK_SUCCESS) { err = std::string(what) + " failed (" + std::to_string(int(r_)) + ")"; return false; } } while (0)

uint32_t mem_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return 0;
}

}  // namespace

PFN_vkVoidFunction WallRenderer::dev_proc(const char *name) const { return vkGetDeviceProcAddr(xr_.vk_device, name); }

WallRenderer::~WallRenderer() {
    VkDevice d = xr_.vk_device;
    if (!d) return;
    vkDeviceWaitIdle(d);
    drop_imports();
    for (auto f : wall_fbs_) vkDestroyFramebuffer(d, f, nullptr);
    for (auto v : wall_views_) vkDestroyImageView(d, v, nullptr);
    xr_.destroy_swapchain(wall_);
    for (auto &v : void_) xr_.destroy_swapchain(v);
    if (queries_) vkDestroyQueryPool(d, queries_, nullptr);
    if (fence_) vkDestroyFence(d, fence_, nullptr);
    if (cmd_pool_) vkDestroyCommandPool(d, cmd_pool_, nullptr);
    if (pipe_) vkDestroyPipeline(d, pipe_, nullptr);
    if (pass_) vkDestroyRenderPass(d, pass_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (dsl_) vkDestroyDescriptorSetLayout(d, dsl_, nullptr);
    if (sampler_) vkDestroySampler(d, sampler_, nullptr);
    if (ycbcr_) vkDestroySamplerYcbcrConversion(d, ycbcr_, nullptr);
}

bool WallRenderer::init(int width, int height, std::string &err) {
    width_ = width;
    height_ = height;
    VkDevice d = xr_.vk_device;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = xr_.vk_queue_family;
    VKF(vkCreateCommandPool(d, &pci, nullptr, &cmd_pool_), "command pool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = cmd_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VKF(vkAllocateCommandBuffers(d, &cai, &cmd_), "command buffer");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKF(vkCreateFence(d, &fci, nullptr, &fence_), "fence");
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = 2;
    VKF(vkCreateQueryPool(d, &qci, nullptr, &queries_), "query pool");

    if (!xr_.make_swapchain(width, height, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT, false, wall_, err))
        return false;
    return make_conversion(err) && make_pipeline(err) && make_void(err);
}

bool WallRenderer::make_conversion(std::string &err) {
    VkDevice d = xr_.vk_device;
    // What the GPU can do with linear NV12: filter chroma linearly, use
    // co-sited chroma (HEVC's default: left/top-aligned horizontally).
    VkFormatFeatureFlags feats = 0;
    {
        VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
        VkFormatProperties2 fp{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        fp.pNext = &list;
        vkGetPhysicalDeviceFormatProperties2(xr_.vk_phys, kNv12, &fp);
        std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
        list.pDrmFormatModifierProperties = mods.data();
        vkGetPhysicalDeviceFormatProperties2(xr_.vk_phys, kNv12, &fp);
        for (auto &m : mods)
            if (m.drmFormatModifier == DRM_FORMAT_MOD_LINEAR) feats = m.drmFormatModifierTilingFeatures;
    }
    if (!(feats & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { err = "the GPU can't sample linear NV12 images"; return false; }
    const bool linear = feats & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
    const bool cosited = feats & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;
    const VkFilter filter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

    VkSamplerYcbcrConversionCreateInfo yci{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
    yci.format = kNv12;
    yci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    yci.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    yci.xChromaOffset = cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT;
    yci.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
    yci.chromaFilter = filter;
    VKF(vkCreateSamplerYcbcrConversion(d, &yci, nullptr, &ycbcr_), "YCbCr conversion");
    VkSamplerYcbcrConversionInfo yinfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    yinfo.conversion = ycbcr_;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.pNext = &yinfo;
    sci.magFilter = sci.minFilter = filter;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKF(vkCreateSampler(d, &sci, nullptr, &sampler_), "sampler");

    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler_};
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = 1;
    dci.pBindings = &b;
    VKF(vkCreateDescriptorSetLayout(d, &dci, nullptr, &dsl_), "descriptor set layout");
    // A YCbCr image can take several descriptors: be generous.
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32 * 3};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 32;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VKF(vkCreateDescriptorPool(d, &dpi, nullptr, &pool_), "descriptor pool");
    fwlog("wall: NV12 sampling with %s chroma filtering, %s chroma", linear ? "linear" : "nearest", cosited ? "co-sited" : "centred");
    return true;
}

bool WallRenderer::make_pipeline(std::string &err) {
    VkDevice d = xr_.vk_device;
    VkAttachmentDescription at{};
    at.format = VkFormat(xr_.color_format);
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;  // how OpenXR wants it back
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &at;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sp;
    VKF(vkCreateRenderPass(d, &rpi, nullptr, &pass_), "render pass");

    for (VkImage img : wall_.images) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VkFormat(xr_.color_format);
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        VKF(vkCreateImageView(d, &vci, nullptr, &v), "wall image view");
        wall_views_.push_back(v);
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = pass_;
        fci.attachmentCount = 1;
        fci.pAttachments = &v;
        fci.width = uint32_t(width_);
        fci.height = uint32_t(height_);
        fci.layers = 1;
        VkFramebuffer f;
        VKF(vkCreateFramebuffer(d, &fci, nullptr, &f), "wall framebuffer");
        wall_fbs_.push_back(f);
    }

    VkPushConstantRange pc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 2};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    VKF(vkCreatePipelineLayout(d, &pli, nullptr, &layout_), "pipeline layout");

    auto module = [&](const uint32_t *code, size_t bytes, VkShaderModule &m) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        return vkCreateShaderModule(d, &ci, nullptr, &m);
    };
    VkShaderModule vs, fs;
    VKF(module(kWallVert, sizeof kWallVert, vs), "vertex shader");
    VKF(module(kWallFrag, sizeof kWallFrag, fs), "fragment shader");
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, float(width_), float(height_), 0, 1};
    VkRect2D sc{{0, 0}, {uint32_t(width_), uint32_t(height_)}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = vps.scissorCount = 1;
    vps.pViewports = &vp;
    vps.pScissors = &sc;
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
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = st;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cbs;
    gpi.layout = layout_;
    gpi.renderPass = pass_;
    const VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe_);
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
    VKF(r, "wall pipeline");
    return true;
}

bool WallRenderer::make_void(std::string &err) {
    VkDevice d = xr_.vk_device;
    for (auto &v : void_) {
        if (!xr_.make_swapchain(kVoidSize, kVoidSize, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT, true, v, err))
            return false;
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(v.handle, &ai, &idx))) { err = "void swapchain acquire"; return false; }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(v.handle, &wi);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_, &bi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = v.images[idx];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkCmdClearColorImage(cmd_, v.images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &kVoidColor, 1, &b.subresourceRange);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(cmd_);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        VKF(vkQueueSubmit(xr_.vk_queue, 1, &si, fence_), "void submit");
        vkWaitForFences(d, 1, &fence_, VK_TRUE, UINT64_MAX);
        vkResetFences(d, 1, &fence_);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(v.handle, &ri);
    }
    return true;
}

bool WallRenderer::import(const DecodedBuffers &b, std::string &err) {
    VkDevice d = xr_.vk_device;
    auto getFdProps = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(dev_proc("vkGetMemoryFdPropertiesKHR"));
    bufs_ = b;
    imports_.assign(b.fds.size(), {});
    initialized_.assign(b.fds.size(), false);
    for (size_t i = 0; i < b.fds.size(); i++) {
        Imported &im = imports_[i];
        int fd = b.fds[i];
        bufs_.fds[i] = -1;  // ours now: closed by Vulkan or below
        VkSubresourceLayout planes[2] = {};
        planes[0].offset = 0;
        planes[0].rowPitch = VkDeviceSize(b.stride);
        planes[1].offset = VkDeviceSize(b.uv_offset);
        planes[1].rowPitch = VkDeviceSize(b.stride);
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
        mod.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
        mod.drmFormatModifierPlaneCount = 2;
        mod.pPlaneLayouts = planes;
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.pNext = &mod;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &ext;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = kNv12;
        ici.extent = {uint32_t(b.width), uint32_t(b.alloc_height), 1};
        ici.mipLevels = ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(d, &ici, nullptr, &im.image);
        if (r != VK_SUCCESS) { close(fd); err = "decoder image (" + std::to_string(int(r)) + ")"; return false; }

        VkMemoryFdPropertiesKHR fdp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        getFdProps(d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp);
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(d, im.image, &mr);
        const off_t size = lseek(fd, 0, SEEK_END);
        VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        imp.fd = fd;
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        ded.pNext = &imp;
        ded.image = im.image;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &ded;
        mai.allocationSize = size > 0 ? VkDeviceSize(size) : mr.size;
        const uint32_t bits = fdp.memoryTypeBits ? (mr.memoryTypeBits & fdp.memoryTypeBits) : mr.memoryTypeBits;
        mai.memoryTypeIndex = mem_type(xr_.vk_phys, bits ? bits : mr.memoryTypeBits, 0);
        r = vkAllocateMemory(d, &mai, nullptr, &im.memory);
        if (r != VK_SUCCESS) { close(fd); err = "decoder memory import (" + std::to_string(int(r)) + ")"; return false; }
        VKF(vkBindImageMemory(d, im.image, im.memory, 0), "bind decoder image");

        VkSamplerYcbcrConversionInfo yinfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
        yinfo.conversion = ycbcr_;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.pNext = &yinfo;
        vci.image = im.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kNv12;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKF(vkCreateImageView(d, &vci, nullptr, &im.view), "decoder image view");

        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = pool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &dsl_;
        VKF(vkAllocateDescriptorSets(d, &dai, &im.set), "descriptor set");
        VkDescriptorImageInfo ii{VK_NULL_HANDLE, im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = im.set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(d, 1, &w, 0, nullptr);
    }
    fwlog("wall: imported %zu decoder buffers (%dx%d, stride %d)", imports_.size(), b.width, b.height, b.stride);
    return true;
}

void WallRenderer::drop_imports() {
    VkDevice d = xr_.vk_device;
    for (auto &im : imports_) {
        if (im.set) vkFreeDescriptorSets(d, pool_, 1, &im.set);
        if (im.view) vkDestroyImageView(d, im.view, nullptr);
        if (im.image) vkDestroyImage(d, im.image, nullptr);
        if (im.memory) vkFreeMemory(d, im.memory, nullptr);
    }
    imports_.clear();
    for (int fd : bufs_.fds)
        if (fd >= 0) close(fd);
    bufs_ = DecodedBuffers{};
}

bool WallRenderer::update(VideoPipeline &video) {
    VkDevice d = xr_.vk_device;
    DecodedBuffers nb;
    if (video.buffers_if_changed(bufs_.generation, nb)) {
        vkQueueWaitIdle(xr_.vk_queue);
        drop_imports();
        if (nb.generation) {
            std::string err;
            if (!import(nb, err)) {
                fwlog("wall: %s", err.c_str());
                drop_imports();
                for (int fd : nb.fds)
                    if (fd >= 0) close(fd);
            }
        }
    }
    int idx = -1;
    uint64_t gen = 0;
    std::chrono::steady_clock::time_point decoded_at;
    if (!video.take(idx, gen, decoded_at)) return false;
    if (gen != bufs_.generation || idx < 0 || size_t(idx) >= imports_.size()) {
        video.give_back(idx, gen);
        return false;
    }

    uint32_t wi = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(wall_.handle, &ai, &wi))) { video.give_back(idx, gen); return false; }
    XrSwapchainImageWaitInfo wwi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wwi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(wall_.handle, &wwi);

    const Imported &im = imports_[size_t(idx)];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    vkCmdResetQueryPool(cmd_, queries_, 0, 2);
    vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
    // The decoder wrote this buffer: take it over from the "foreign" owner.
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = initialized_[size_t(idx)] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    b.dstQueueFamilyIndex = xr_.vk_queue_family;
    b.image = im.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    initialized_[size_t(idx)] = true;

    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = pass_;
    rbi.framebuffer = wall_fbs_[wi];
    rbi.renderArea = {{0, 0}, {uint32_t(width_), uint32_t(height_)}};
    vkCmdBeginRenderPass(cmd_, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &im.set, 0, nullptr);
    // Wall pixel -> decoder texture coordinate (the decoder's buffer has
    // padding rows below the picture).
    const float pc[2] = {float(bufs_.width) / float(width_) / float(bufs_.width),
                         float(bufs_.height) / float(height_) / float(bufs_.alloc_height)};
    vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd_);

    // Give it back to the decoder.
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = xr_.vk_queue_family;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    const VkResult r = vkQueueSubmit(xr_.vk_queue, 1, &si, fence_);
    if (r == VK_SUCCESS) {
        vkWaitForFences(d, 1, &fence_, VK_TRUE, UINT64_MAX);
        vkResetFences(d, 1, &fence_);
        uint64_t ts[2];
        if (vkGetQueryPoolResults(d, queries_, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            gpu_ms_ = double(ts[1] - ts[0]) * xr_.vk_timestamp_period / 1e6;
    } else {
        fwlog("wall: submit failed (%d)", int(r));
    }
    video.give_back(idx, gen);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(wall_.handle, &ri);
    if (r != VK_SUCCESS) return false;
    has_frame_ = true;
    shown_at_ = decoded_at;
    video.stats.shown++;
    return true;
}

void WallRenderer::layers(XrTime display_time, bool passthrough, const std::vector<PanelPose> &poses,
                          const std::vector<Rect> &regions, float radius, std::vector<XrCompositionLayerBaseHeader *> &out) {
    out.clear();
    if (!passthrough) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = display_time;
        li.space = xr_.space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t n = 0;
        if (XR_SUCCEEDED(xrLocateViews(xr_.session, &li, &vs, 2, &n, views)) && n == 2) {
            for (int e = 0; e < 2; e++) {
                proj_views_[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                proj_views_[e].pose = views[e].pose;
                proj_views_[e].fov = views[e].fov;
                proj_views_[e].subImage.swapchain = void_[e].handle;
                proj_views_[e].subImage.imageRect = {{0, 0}, {kVoidSize, kVoidSize}};
            }
            proj_ = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            proj_.space = xr_.space;
            proj_.viewCount = 2;
            proj_.views = proj_views_;
            out.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&proj_));
        }
    }
    if (!has_frame_) return;
    cylinders_.assign(std::min(poses.size(), regions.size()), {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR});
    for (size_t i = 0; i < cylinders_.size(); i++) {
        const PanelPose &p = poses[i];
        const Rect &r = regions[i];
        XrCompositionLayerCylinderKHR &c = cylinders_[i];
        c.layerFlags = 0;
        c.space = xr_.space;
        c.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        c.subImage.swapchain = wall_.handle;
        c.subImage.imageRect = {{r.x, r.y}, {r.w, r.h}};
        // The cylinder's axis through the head, at the panel's height, turned
        // so the visible arc is centred on the panel (+theta = to the right).
        const float a = -p.theta;
        c.pose.orientation = {0, std::sin(a / 2), 0, std::cos(a / 2)};
        c.pose.position = {0, p.y, 0};
        c.radius = radius;
        c.centralAngle = p.width_m / radius;
        c.aspectRatio = p.width_m / p.height_m;
        out.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&c));
    }
}
