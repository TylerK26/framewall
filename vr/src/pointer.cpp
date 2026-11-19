// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "pointer.h"

#include "log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

constexpr uint64_t kChordMs = 150;     // both grips within this = the menu chord
constexpr float kDotSize = 0.012f;     // m
constexpr float kRayWidth = 0.004f;    // m
constexpr float kRayLength = 3.0f;     // m, when it hits nothing
constexpr float kStickDeadzone = 0.2f;
constexpr float kTriggerDown = 0.35f, kTriggerUp = 0.2f;  // a light pull clicks
constexpr double kNotchesPerSecond = 14;  // scroll speed at full deflection
constexpr int kDotPx = 64, kRayW = 16, kRayH = 4;

uint64_t now_ms() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

XrVector3f operator+(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
XrVector3f operator-(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
XrVector3f operator*(XrVector3f a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(XrVector3f a, XrVector3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
XrVector3f cross(XrVector3f a, XrVector3f b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
XrVector3f normalize(XrVector3f a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-6f ? a * (1 / l) : XrVector3f{0, 0, 1};
}
// Rotates v by unit quaternion q.
XrVector3f rotate(XrQuaternionf q, XrVector3f v) {
    const XrVector3f u{q.x, q.y, q.z};
    const XrVector3f t = cross(u, v) * 2;
    return v + t * q.w + cross(u, t);
}
// Quaternion of the rotation whose columns are x, y, z.
XrQuaternionf from_axes(XrVector3f x, XrVector3f y, XrVector3f z) {
    const float tr = x.x + y.y + z.z;
    XrQuaternionf q;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1) * 2;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, s / 4};
    } else if (x.x > y.y && x.x > z.z) {
        const float s = std::sqrt(1 + x.x - y.y - z.z) * 2;
        q = {s / 4, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (y.y > z.z) {
        const float s = std::sqrt(1 + y.y - x.x - z.z) * 2;
        q = {(y.x + x.y) / s, s / 4, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        const float s = std::sqrt(1 + z.z - x.x - y.y) * 2;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, s / 4, (x.y - y.x) / s};
    }
    return q;
}

// Premultiplied RGBA (as uint32 little-endian R,G,B,A).
uint32_t rgba(float r, float g, float b, float a) {
    auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
    return c(r * a) | (c(g * a) << 8) | (c(b * a) << 16) | (c(a) << 24);
}

}  // namespace

Pointer::~Pointer() {
    if (dot_img_.handle) xr_.destroy_swapchain(dot_img_);
    if (ray_img_.handle) xr_.destroy_swapchain(ray_img_);
    for (auto &h : hands_)
        if (h.aim) xrDestroySpace(h.aim);
    if (view_) xrDestroySpace(view_);
    if (set_) xrDestroyActionSet(set_);
}

bool Pointer::init(std::string &err) {
    return make_actions(err) && make_images(err);
}

void Pointer::suggest(const char *profile, const std::vector<std::pair<XrAction, std::vector<std::string>>> &candidates) {
    XrPath prof;
    if (XR_FAILED(xrStringToPath(xr_.instance, profile, &prof))) return;
    // Try each candidate path alone (a suggestion with one unsupported path
    // fails as a whole), keep the first that works per action and hand.
    std::vector<XrActionSuggestedBinding> good;
    for (const auto &[action, paths] : candidates)
        for (const char *hand : {"/user/hand/left", "/user/hand/right"}) {
            for (const auto &p : paths) {
                XrPath path;
                if (XR_FAILED(xrStringToPath(xr_.instance, (std::string(hand) + p).c_str(), &path))) continue;
                XrActionSuggestedBinding b{action, path};
                XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
                s.interactionProfile = prof;
                s.countSuggestedBindings = 1;
                s.suggestedBindings = &b;
                if (XR_SUCCEEDED(xrSuggestInteractionProfileBindings(xr_.instance, &s))) {
                    good.push_back(b);
                    break;
                }
            }
        }
    if (good.empty()) { fwlog("pointer: no bindings accepted for %s", profile); return; }
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = prof;
    s.countSuggestedBindings = uint32_t(good.size());
    s.suggestedBindings = good.data();
    const XrResult r = xrSuggestInteractionProfileBindings(xr_.instance, &s);
    fwlog("pointer: %zu bindings for %s%s", good.size(), profile, XR_FAILED(r) ? " (rejected)" : "");
}

bool Pointer::make_actions(std::string &err) {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(si.actionSetName, "framewall");
    std::strcpy(si.localizedActionSetName, "framewall");
    XrResult r = xrCreateActionSet(xr_.instance, &si, &set_);
    if (XR_FAILED(r)) { err = "action set: " + xr_result_name(xr_.instance, r); return false; }
    xrStringToPath(xr_.instance, "/user/hand/left", &hands_[0].path);
    xrStringToPath(xr_.instance, "/user/hand/right", &hands_[1].path);
    const XrPath both[2] = {hands_[0].path, hands_[1].path};
    auto action = [&](const char *name, const char *label, XrActionType type, XrAction &out) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ci.actionName, name);
        std::strcpy(ci.localizedActionName, label);
        ci.actionType = type;
        ci.countSubactionPaths = 2;
        ci.subactionPaths = both;
        return XR_SUCCEEDED(xrCreateAction(set_, &ci, &out));
    };
    if (!action("aim", "Laser aim", XR_ACTION_TYPE_POSE_INPUT, aim_) ||
        !action("laser", "Laser on/off (grip)", XR_ACTION_TYPE_BOOLEAN_INPUT, grip_) ||
        !action("click", "Left click (trigger)", XR_ACTION_TYPE_FLOAT_INPUT, trigger_) ||
        !action("click_bumper", "Left click (bumper)", XR_ACTION_TYPE_BOOLEAN_INPUT, bumper_) ||
        !action("right_click", "Right click (stick click)", XR_ACTION_TYPE_BOOLEAN_INPUT, right_) ||
        !action("scroll", "Scroll (stick)", XR_ACTION_TYPE_VECTOR2F_INPUT, stick_) ||
        !action("buzz", "Haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT, buzz_)) {
        err = "can't create the pointer's actions";
        return false;
    }
    // The Steam Frame's own profile (names tried in order), then the Index
    // controller's, which SteamVR can map onto any controller.
    if (xr_.frame_controller)
        suggest("/interaction_profiles/valve/frame_controller",
                {{aim_, {"/input/aim/pose"}},
                 {grip_, {"/input/squeeze/click", "/input/grip/click", "/input/squeeze/value", "/input/grip/value"}},
                 {trigger_, {"/input/trigger/value"}},
                 {bumper_, {"/input/bumper/click", "/input/shoulder/click"}},
                 {right_, {"/input/thumbstick/click", "/input/joystick/click"}},
                 {stick_, {"/input/thumbstick", "/input/joystick"}},
                 {buzz_, {"/output/haptic"}}});
    suggest("/interaction_profiles/valve/index_controller",
            {{aim_, {"/input/aim/pose"}},
             {grip_, {"/input/squeeze/force", "/input/squeeze/value"}},
             {trigger_, {"/input/trigger/value"}},
             {right_, {"/input/thumbstick/click"}},
             {stick_, {"/input/thumbstick"}},
             {buzz_, {"/output/haptic"}}});
    suggest("/interaction_profiles/khr/simple_controller",
            {{aim_, {"/input/aim/pose"}}, {bumper_, {"/input/select/click"}}, {buzz_, {"/output/haptic"}}});

    for (int i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = aim_;
        ci.subactionPath = hands_[i].path;
        ci.poseInActionSpace.orientation.w = 1;
        if (XR_FAILED(xrCreateActionSpace(xr_.session, &ci, &hands_[i].aim))) { err = "aim space"; return false; }
    }
    XrReferenceSpaceCreateInfo vci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vci.poseInReferenceSpace.orientation.w = 1;
    xrCreateReferenceSpace(xr_.session, &vci, &view_);
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &set_;
    r = xrAttachSessionActionSets(xr_.session, &ai);
    if (XR_FAILED(r)) { err = "attaching the pointer's actions: " + xr_result_name(xr_.instance, r); return false; }
    return true;
}

bool Pointer::make_images(std::string &err) {
    // The dot: white with a dark rim, so it shows on light and dark content.
    std::vector<uint32_t> dot(size_t(kDotPx) * kDotPx);
    for (int y = 0; y < kDotPx; y++)
        for (int x = 0; x < kDotPx; x++) {
            const float dx = (x + 0.5f) / kDotPx * 2 - 1, dy = (y + 0.5f) / kDotPx * 2 - 1;
            const float r = std::sqrt(dx * dx + dy * dy);
            const float edge = 2.0f / kDotPx;  // antialiasing width
            const float a = std::clamp((1 - r) / edge, 0.0f, 1.0f);
            const float inner = std::clamp((0.65f - r) / edge, 0.0f, 1.0f);
            const float g = 0.12f + 0.88f * inner;
            dot[size_t(y) * kDotPx + x] = rgba(g, g, g, a);
        }
    // The ray: bright core, soft sides, a little see-through.
    std::vector<uint32_t> ray(size_t(kRayW) * kRayH);
    for (int y = 0; y < kRayH; y++)
        for (int x = 0; x < kRayW; x++) {
            const float d = std::fabs((x + 0.5f) / kRayW * 2 - 1);
            ray[size_t(y) * kRayW + x] = rgba(0.85f, 0.92f, 1.0f, 0.7f * std::clamp(1.4f * (1 - d), 0.0f, 1.0f));
        }
    const XrSwapchainUsageFlags usage = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    return xr_.make_swapchain(kDotPx, kDotPx, usage, true, dot_img_, err) && upload(dot_img_, dot, err) &&
           xr_.make_swapchain(kRayW, kRayH, usage, true, ray_img_, err) && upload(ray_img_, ray, err);
}

bool Pointer::upload(Xr::Swapchain &sc, const std::vector<uint32_t> &rgba_px, std::string &err) {
    VkDevice d = xr_.vk_device;
    const VkDeviceSize bytes = rgba_px.size() * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer buf;
    if (vkCreateBuffer(d, &bci, nullptr, &buf) != VK_SUCCESS) { err = "pointer staging buffer"; return false; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(d, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(xr_.vk_phys, &mp);
    uint32_t type = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem;
    vkAllocateMemory(d, &mai, nullptr, &mem);
    vkBindBufferMemory(d, buf, mem, 0);
    void *map;
    vkMapMemory(d, mem, 0, bytes, 0, &map);
    std::memcpy(map, rgba_px.data(), bytes);
    vkUnmapMemory(d, mem);

    VkCommandPool pool;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = xr_.vk_queue_family;
    vkCreateCommandPool(d, &pci, nullptr, &pool);
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(d, &cai, &cmd);

    std::lock_guard<std::mutex> lk(xr_.queue_mu);
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(sc.handle, &ai, &idx);
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
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(xr_.vk_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(xr_.vk_queue);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &ri);
    vkDestroyCommandPool(d, pool, nullptr);
    vkDestroyBuffer(d, buf, nullptr);
    vkFreeMemory(d, mem, nullptr);
    return true;
}

void Pointer::haptic(int hand) {
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.duration = 15'000'000;  // 15 ms
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    v.amplitude = 0.4f;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = buzz_;
    hi.subactionPath = hands_[hand].path;
    xrApplyHapticFeedback(xr_.session, &hi, reinterpret_cast<XrHapticBaseHeader *>(&v));
}

void Pointer::release_buttons(const Mouse &mouse) {
    for (int b = 0; b < 2; b++)
        if (held_[b]) mouse.button(b == 1, false);
    held_[0] = held_[1] = false;
}

void Pointer::update(XrTime t, const std::vector<PanelPose> &poses, const std::vector<Rect> &regions, float radius,
                     const Mouse &mouse) {
    hit_ = false;
    if (!xr_.focused()) {  // the Steam menu or another app has the controllers
        release_buttons(mouse);
        for (auto &h : hands_) h.grip = h.left = h.right = false, h.grip_down_ms = 0;
        return;
    }
    XrActiveActionSet active{set_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(xr_.session, &sync))) return;

    const uint64_t now = now_ms();
    bool grip_now[2], left_now[2], right_now[2];
    XrVector2f stick[2] = {};
    for (int i = 0; i < 2; i++) {
        Hand &h = hands_[i];
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath = h.path;
        XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
        gi.action = grip_;
        xrGetActionStateBoolean(xr_.session, &gi, &b);
        grip_now[i] = b.isActive && b.currentState;
        bool bumper = false;
        {
            XrActionStateBoolean bb{XR_TYPE_ACTION_STATE_BOOLEAN};
            gi.action = bumper_;
            xrGetActionStateBoolean(xr_.session, &gi, &bb);
            bumper = bb.isActive && bb.currentState;
            bb = {XR_TYPE_ACTION_STATE_BOOLEAN};
            gi.action = right_;
            xrGetActionStateBoolean(xr_.session, &gi, &bb);
            right_now[i] = bb.isActive && bb.currentState;
        }
        // Trigger: analog, with hysteresis, so a light pull clicks.
        XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
        gi.action = trigger_;
        xrGetActionStateFloat(xr_.session, &gi, &f);
        const float tv = f.isActive ? f.currentState : 0;
        trigger_down_[i] = trigger_down_[i] ? tv > kTriggerUp : tv > kTriggerDown;
        left_now[i] = trigger_down_[i] || bumper;
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        gi.action = stick_;
        xrGetActionStateVector2f(xr_.session, &gi, &v);
        if (v.isActive) stick[i] = v.currentState;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        h.pose_valid = XR_SUCCEEDED(xrLocateSpace(h.aim, xr_.space, t, &loc)) &&
                       (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                       (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (h.pose_valid) h.pose = loc.pose;
    }

    // Grips: a press moves the laser to that hand, unless the other grip goes
    // down within kChordMs (both grips = the menu, reserved for now).
    for (int i = 0; i < 2; i++) {
        Hand &h = hands_[i], &o = hands_[1 - i];
        if (grip_now[i] && !h.grip) {
            if (o.grip_down_ms && now - o.grip_down_ms < kChordMs) {
                o.grip_down_ms = 0;  // chord: neither press counts
                fwlog("pointer: both grips (menu: not yet)");
            } else {
                h.grip_down_ms = now;  // decided once the chord window has passed
            }
        }
        h.grip = grip_now[i];
    }
    // A grip press (no chord): the laser onto that controller, or off if it's
    // already there.
    for (int i = 0; i < 2; i++) {
        Hand &h = hands_[i];
        if (h.grip_down_ms && now - h.grip_down_ms >= kChordMs) {
            h.grip_down_ms = 0;
            release_buttons(mouse);
            laser_ = laser_ == i ? -1 : i;
            fwlog("pointer: laser %s", laser_ < 0 ? "off" : i == 0 ? "on the left controller" : "on the right controller");
            haptic(i);
        }
    }
    // Buttons only count when pressed while the laser is on that controller.
    auto latch = [&] {
        for (int i = 0; i < 2; i++) hands_[i].left = left_now[i], hands_[i].right = right_now[i];
    };
    if (laser_ < 0) { latch(); return; }
    Hand &h = hands_[laser_];
    if (!h.pose_valid) { release_buttons(mouse); latch(); return; }

    // The ray, in the seated space, against the panels' cylinder (x² + z² = r²).
    origin_ = h.pose.position;
    const XrVector3f dir = normalize(rotate(h.pose.orientation, {0, 0, -1}));
    end_ = origin_ + dir * kRayLength;
    const float a = dir.x * dir.x + dir.z * dir.z;
    const float b = 2 * (origin_.x * dir.x + origin_.z * dir.z);
    const float c = origin_.x * origin_.x + origin_.z * origin_.z - radius * radius;
    const float disc = b * b - 4 * a * c;
    int panel = -1;
    float u = 0, v = 0;
    if (a > 1e-6f && disc >= 0) {
        const float tt = (-b + std::sqrt(disc)) / (2 * a);  // the far side: we're inside the cylinder
        if (tt > 0) {
            const XrVector3f p = origin_ + dir * tt;
            const float ang = std::atan2(p.x, -p.z);  // + = right of forward
            for (size_t i = 0; i < poses.size() && i < regions.size(); i++) {
                const float half = poses[i].width_m / radius / 2;
                float d = ang - poses[i].theta;
                while (d > float(M_PI)) d -= float(2 * M_PI);
                while (d < -float(M_PI)) d += float(2 * M_PI);
                if (std::fabs(d) > half || std::fabs(p.y - poses[i].y) > poses[i].height_m / 2) continue;
                panel = int(i);
                u = (d + half) / (2 * half);
                v = (poses[i].y + poses[i].height_m / 2 - p.y) / poses[i].height_m;
                hit_point_ = p;
                hit_angle_ = ang;
                end_ = p;
                break;
            }
        }
    }
    hit_ = panel >= 0;
    if (hit_) {
        const Rect &r = regions[size_t(panel)];
        const int x = r.x + std::clamp(int(u * r.w), 0, r.w - 1), y = r.y + std::clamp(int(v * r.h), 0, r.h - 1);
        if (x != last_x_ || y != last_y_) {
            mouse.move(x, y);
            last_x_ = x;
            last_y_ = y;
        }
    }

    // Left: trigger or bumper. Right: thumbstick click. A press needs the
    // laser on a panel; a release always goes through.
    const bool now_down[2] = {left_now[laser_], right_now[laser_]};
    const bool was_down[2] = {h.left, h.right};
    for (int b = 0; b < 2; b++) {
        if (now_down[b] && !was_down[b] && hit_ && !held_[b]) {
            held_[b] = true;
            mouse.button(b == 1, true);
        } else if (!now_down[b] && held_[b]) {
            held_[b] = false;
            mouse.button(b == 1, false);
        }
    }
    latch();

    // Stick: scroll, faster the further it's pushed.
    if (hit_) {
        const float s[2] = {stick[laser_].y, stick[laser_].x};
        int out[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            const float m = std::fabs(s[k]);
            if (m < kStickDeadzone) { scroll_acc_[k] = 0; continue; }
            const double speed = (m - kStickDeadzone) / (1 - kStickDeadzone);
            scroll_acc_[k] += (s[k] > 0 ? 1 : -1) * speed * speed * kNotchesPerSecond * 120 / 144.0;
            out[k] = int(scroll_acc_[k]);
            scroll_acc_[k] -= out[k];
        }
        if (out[0] || out[1]) mouse.scroll(out[0], out[1]);
    }

    // Head position, to turn the ray's flat quad towards the eyes.
    XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
    if (view_ && XR_SUCCEEDED(xrLocateSpace(view_, xr_.space, t, &hl)) && (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
        head_ = hl.pose.position;
}

void Pointer::layers(std::vector<XrCompositionLayerBaseHeader *> &out) {
    if (laser_ < 0 || !hands_[laser_].pose_valid) return;
    // Ray: a thin quad from the controller to the hit, its long axis along
    // the ray, its face turned to the head.
    const XrVector3f along = end_ - origin_;
    const float len = std::sqrt(dot(along, along));
    if (len > 0.01f) {
        const XrVector3f y = along * (1 / len);
        const XrVector3f mid = origin_ + along * 0.5f;
        XrVector3f z = head_ - mid;
        z = normalize(z - y * dot(z, y));
        const XrVector3f x = cross(y, z);
        ray_ = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        ray_.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        ray_.space = xr_.space;
        ray_.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        ray_.subImage.swapchain = ray_img_.handle;
        ray_.subImage.imageRect = {{0, 0}, {ray_img_.width, ray_img_.height}};
        ray_.pose.orientation = from_axes(x, y, z);
        ray_.pose.position = mid;
        ray_.size = {kRayWidth, len};
        out.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&ray_));
    }
    if (hit_) {
        // Dot: on the panel, a few mm towards you, facing the cylinder's axis.
        const XrVector3f inward{-std::sin(hit_angle_), 0, std::cos(hit_angle_)};
        const float a = -hit_angle_;
        dot_ = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        dot_.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        dot_.space = xr_.space;
        dot_.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        dot_.subImage.swapchain = dot_img_.handle;
        dot_.subImage.imageRect = {{0, 0}, {dot_img_.width, dot_img_.height}};
        dot_.pose.orientation = {0, std::sin(a / 2), 0, std::cos(a / 2)};
        dot_.pose.position = hit_point_ + inward * 0.005f;
        dot_.size = {kDotSize, kDotSize};
        out.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&dot_));
    }
}
