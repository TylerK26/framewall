// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "video.h"

#include "log.h"

#include <linux/videodev2.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

constexpr unsigned kOutputBuffers = 8;
constexpr uint32_t kOutputSize = 8 << 20;  // largest access unit (an IDR at a high bitrate is a few MB)
constexpr size_t kMaxQueued = 6;           // decoder backlog before we drop and ask for an IDR

}  // namespace

bool VideoPipeline::start(int width, int height, std::string &err) {
    width_ = width;
    height_ = height;
    fd_ = open(opt_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) { err = "can't open the video decoder " + opt_.device + ": " + std::strerror(errno); return false; }
    v4l2_capability cap{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) == 0) fwlog("video: decoder %s (%s)", cap.card, cap.driver);

    v4l2_format ofmt{};
    ofmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ofmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
    ofmt.fmt.pix_mp.width = width;
    ofmt.fmt.pix_mp.height = height;
    ofmt.fmt.pix_mp.num_planes = 1;
    ofmt.fmt.pix_mp.plane_fmt[0].sizeimage = kOutputSize;
    if (xioctl(fd_, VIDIOC_S_FMT, &ofmt) < 0) { err = std::string("decoder rejects HEVC ") + std::to_string(width) + "x" + std::to_string(height) + ": " + std::strerror(errno); return false; }

    v4l2_requestbuffers oreq{};
    oreq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    oreq.memory = V4L2_MEMORY_MMAP;
    oreq.count = kOutputBuffers;
    if (xioctl(fd_, VIDIOC_REQBUFS, &oreq) < 0) { err = std::string("decoder input buffers: ") + std::strerror(errno); return false; }
    obufs_.assign(oreq.count, {});
    for (unsigned i = 0; i < oreq.count; i++) {
        v4l2_plane pl[1]{};
        v4l2_buffer b{};
        b.type = oreq.type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = pl;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &b) < 0) { err = "decoder QUERYBUF failed"; return false; }
        obufs_[i].len = pl[0].length;
        obufs_[i].ptr = mmap(nullptr, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, pl[0].m.mem_offset);
        if (obufs_[i].ptr == MAP_FAILED) { obufs_[i].ptr = nullptr; err = "decoder mmap failed"; return false; }
        ofree_.push_back(i);
    }

    v4l2_event_subscription sub{};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    xioctl(fd_, VIDIOC_SUBSCRIBE_EVENT, &sub);
    int otype = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd_, VIDIOC_STREAMON, &otype) < 0) { err = "decoder STREAMON failed"; return false; }

    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    running_ = true;
    thread_ = std::thread(&VideoPipeline::run, this);
    return true;
}

void VideoPipeline::wake() {
    const uint64_t one = 1;
    (void)!write(wake_fd_, &one, sizeof one);
}

int VideoPipeline::submit(PDECODE_UNIT du) {
    stats.received++;
    if (need_idr_.exchange(false)) {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.clear();
        if (du->frameType != FRAME_TYPE_IDR) return DR_NEED_IDR;
    }
    Au au;
    au.data.reserve(du->fullLength);
    for (PLENTRY e = du->bufferList; e; e = e->next)
        au.data.insert(au.data.end(), reinterpret_cast<uint8_t *>(e->data), reinterpret_cast<uint8_t *>(e->data) + e->length);
    au.frame = du->frameNumber;
    au.t = clk::now();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.size() >= kMaxQueued && du->frameType != FRAME_TYPE_IDR) {
            // The decoder fell behind: skipping frames breaks the references,
            // so start over from a fresh IDR.
            stats.dropped += queue_.size() + 1;
            queue_.clear();
            return DR_NEED_IDR;
        }
        if (du->frameType == FRAME_TYPE_IDR) queue_.clear();
        queue_.push_back(std::move(au));
    }
    wake();
    return DR_OK;
}

void VideoPipeline::stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    wake();
    if (thread_.joinable()) thread_.join();
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(fd_, VIDIOC_STREAMOFF, &t);
    for (auto &o : obufs_)
        if (o.ptr) munmap(o.ptr, o.len);
    obufs_.clear();
    if (fd_ >= 0) close(fd_);
    if (wake_fd_ >= 0) close(wake_fd_);
    fd_ = wake_fd_ = -1;
}

bool VideoPipeline::buffers_if_changed(uint64_t generation, DecodedBuffers &out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (bufs_.generation == generation) return false;
    out = bufs_;
    for (auto &fd : out.fds) fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    return true;
}

bool VideoPipeline::take(int &index, uint64_t &generation, clk::time_point &decoded_at) {
    std::lock_guard<std::mutex> lk(mu_);
    if (ready_ < 0) return false;
    index = ready_;
    generation = bufs_.generation;
    decoded_at = ready_at_;
    ready_ = -1;
    return true;
}

bool VideoPipeline::wait_take(int &index, uint64_t &generation, clk::time_point &decoded_at, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!ready_cv_.wait_for(lk, timeout, [&] { return ready_ >= 0; })) return false;
    index = ready_;
    generation = bufs_.generation;
    decoded_at = ready_at_;
    ready_ = -1;
    return true;
}

void VideoPipeline::give_back(int index, uint64_t generation) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (generation != bufs_.generation) return;  // that buffer set is gone
        returned_.push_back(index);
    }
    wake();
}

bool VideoPipeline::feed(Au &au) {
    if (au.data.size() > kOutputSize) {
        fwlog("video: frame %d is %zu bytes, larger than the decoder buffer; skipping", au.frame, au.data.size());
        need_idr_ = true;
        LiRequestIdrFrame();
        return true;
    }
    const unsigned idx = ofree_.front();
    ofree_.pop_front();
    std::memcpy(obufs_[idx].ptr, au.data.data(), au.data.size());
    v4l2_plane pl[1]{};
    pl[0].bytesused = uint32_t(au.data.size());
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = idx;
    b.length = 1;
    b.m.planes = pl;
    if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) {
        fwlog("video: queueing frame %d failed: %s", au.frame, std::strerror(errno));
        ofree_.push_back(idx);
        return false;
    }
    return true;
}

void VideoPipeline::requeue_capture(unsigned idx) {
    v4l2_plane pl[1]{};
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = idx;
    b.length = 1;
    b.m.planes = pl;
    if (xioctl(fd_, VIDIOC_QBUF, &b) < 0) fwlog("video: requeue %u failed: %s", idx, std::strerror(errno));
}

bool VideoPipeline::setup_capture() {
    if (capturing_) teardown_capture();
    v4l2_format cfmt{};
    cfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd_, VIDIOC_G_FMT, &cfmt);
    cfmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(fd_, VIDIOC_S_FMT, &cfmt) < 0 || xioctl(fd_, VIDIOC_G_FMT, &cfmt) < 0) {
        fwlog("video: decoder won't output NV12: %s", std::strerror(errno));
        return false;
    }
    const auto &pm = cfmt.fmt.pix_mp;
    DecodedBuffers nb;
    nb.stride = int(pm.plane_fmt[0].bytesperline);
    nb.alloc_height = int(pm.height);
    nb.uv_offset = nb.stride * nb.alloc_height;
    nb.width = int(pm.width);
    nb.height = int(pm.height);
    v4l2_selection sel{};
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_COMPOSE;
    if (xioctl(fd_, VIDIOC_G_SELECTION, &sel) == 0 && sel.r.width && sel.r.height) {
        nb.width = int(sel.r.width);
        nb.height = int(sel.r.height);
    }
    const uint32_t pf = pm.pixelformat;
    fwlog("video: decoder output %.4s %ux%u (visible %dx%d), stride %d, %u plane(s), %u bytes",
          reinterpret_cast<const char *>(&pf), pm.width, pm.height, nb.width, nb.height, nb.stride, pm.num_planes,
          pm.plane_fmt[0].sizeimage);
    if (pm.num_planes != 1) { fwlog("video: multi-plane NV12 isn't supported"); return false; }

    v4l2_control ctl{};
    ctl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    int minbufs = 4;
    if (xioctl(fd_, VIDIOC_G_CTRL, &ctl) == 0) minbufs = ctl.value;
    v4l2_requestbuffers creq{};
    creq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    creq.memory = V4L2_MEMORY_MMAP;
    creq.count = minbufs + 3;  // the decoder's own, the frame ready, the frame on the GPU, spare
    if (xioctl(fd_, VIDIOC_REQBUFS, &creq) < 0) { fwlog("video: decoder output buffers: %s", std::strerror(errno)); return false; }

    for (unsigned i = 0; i < creq.count; i++) {
        v4l2_exportbuffer exp{};
        exp.type = creq.type;
        exp.index = i;
        exp.plane = 0;
        exp.flags = O_RDONLY | O_CLOEXEC;
        if (xioctl(fd_, VIDIOC_EXPBUF, &exp) < 0) { fwlog("video: EXPBUF: %s", std::strerror(errno)); return false; }
        nb.fds.push_back(exp.fd);
        requeue_capture(i);
    }
    int ctype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd_, VIDIOC_STREAMON, &ctype) < 0) { fwlog("video: capture STREAMON: %s", std::strerror(errno)); return false; }
    fwlog("video: %u decoder output buffers (driver minimum %d)", creq.count, minbufs);
    {
        std::lock_guard<std::mutex> lk(mu_);
        nb.generation = next_generation_++;
        bufs_ = std::move(nb);
        ready_ = -1;
        returned_.clear();
    }
    capturing_ = true;
    return true;
}

void VideoPipeline::teardown_capture() {
    int ctype = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd_, VIDIOC_STREAMOFF, &ctype);
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lk(mu_);
        fds = std::move(bufs_.fds);
        bufs_ = DecodedBuffers{};
        ready_ = -1;
        returned_.clear();
    }
    // The renderer holds its own (duplicated) fds, so the memory stays valid
    // for it until it lets go.
    for (int fd : fds)
        if (fd >= 0) close(fd);
    v4l2_requestbuffers creq{};
    creq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    creq.memory = V4L2_MEMORY_MMAP;
    creq.count = 0;
    xioctl(fd_, VIDIOC_REQBUFS, &creq);
    capturing_ = false;
}

void VideoPipeline::run() {
    int consecutive_errors = 0;
    while (running_) {
        pollfd p[2] = {{fd_, POLLIN | POLLOUT | POLLPRI, 0}, {wake_fd_, POLLIN, 0}};
        poll(p, 2, 100);
        if (p[1].revents & POLLIN) {
            uint64_t v;
            (void)!read(wake_fd_, &v, sizeof v);
        }
        if (!running_) break;

        if (p[0].revents & POLLPRI) {
            v4l2_event ev{};
            while (xioctl(fd_, VIDIOC_DQEVENT, &ev) == 0)
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE && !setup_capture()) {
                    fwlog("video: can't set up the decoder output; stopping video");
                    LiInterruptConnection();
                    running_ = false;
                }
            if (!running_) break;
        }
        // Buffers the renderer is done with.
        std::vector<int> back;
        {
            std::lock_guard<std::mutex> lk(mu_);
            back.swap(returned_);
        }
        if (capturing_)
            for (int i : back) requeue_capture(unsigned(i));
        // Reclaim decoder input buffers.
        for (;;) {
            v4l2_plane pl[1]{};
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            b.memory = V4L2_MEMORY_MMAP;
            b.length = 1;
            b.m.planes = pl;
            if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) break;
            ofree_.push_back(b.index);
        }
        // Feed queued access units.
        while (!ofree_.empty()) {
            Au au;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (queue_.empty()) break;
                au = std::move(queue_.front());
                queue_.pop_front();
            }
            feed(au);
        }
        // Decoded frames: offer the newest to the renderer, recycle the rest.
        if (!capturing_) continue;
        for (;;) {
            v4l2_plane pl[1]{};
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            b.memory = V4L2_MEMORY_MMAP;
            b.length = 1;
            b.m.planes = pl;
            if (xioctl(fd_, VIDIOC_DQBUF, &b) < 0) break;
            if (pl[0].bytesused == 0 || (b.flags & V4L2_BUF_FLAG_ERROR)) {
                if (b.flags & V4L2_BUF_FLAG_ERROR) {
                    stats.errors++;
                    if (++consecutive_errors == 3) LiRequestIdrFrame();
                }
                requeue_capture(b.index);
                continue;
            }
            consecutive_errors = 0;
            stats.decoded++;
            int replaced;
            {
                std::lock_guard<std::mutex> lk(mu_);
                replaced = ready_;
                ready_ = int(b.index);
                ready_at_ = clk::now();
            }
            ready_cv_.notify_one();
            if (replaced >= 0) requeue_capture(unsigned(replaced));  // never shown
        }
    }
    if (capturing_) teardown_capture();
}
