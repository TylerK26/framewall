// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#include "glconvert.h"

#include "log.h"

#include <fcntl.h>
#include <unistd.h>

bool GlConverter::fail(const char *what) {
    fwlog("gl: %s failed (egl 0x%x, gl 0x%x)", what, eglGetError(), glGetError());
    return false;
}

bool GlConverter::init(int width, int height) {
    w_ = width;
    h_ = height;
    next_ = 0;
    last_ = -1;
    if (dpy_ == EGL_NO_DISPLAY) {
        drm_fd_ = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (drm_fd_ < 0) return fail("open /dev/dri/renderD128");
        gbm_ = gbm_create_device(drm_fd_);
        if (!gbm_) return fail("gbm_create_device");
        auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        EGLDisplay d = getPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_, nullptr);
        if (d == EGL_NO_DISPLAY || !eglInitialize(d, nullptr, nullptr)) return fail("eglInitialize");
        dpy_ = d;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    ctx_ = eglCreateContext(dpy_, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (ctx_ == EGL_NO_CONTEXT) return fail("eglCreateContext");
    if (!eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_)) return fail("eglMakeCurrent (surfaceless)");
    createImage_ = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    destroyImage_ = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    imageTarget_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    fwlog("gl: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    if (!build_program()) return false;
    glGenVertexArrays(1, &vao_);
    for (int i = 0; i < kRing; i++)
        if (!make_output(i)) return false;
    return true;
}

bool GlConverter::import_capture(int idx, int fd, int stride, int uv_offset) {
    if (int(src_.size()) <= idx) src_.resize(idx + 1);
    const EGLint attr[] = {
        EGL_WIDTH, w_, EGL_HEIGHT, h_,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
        EGL_DMA_BUF_PLANE1_FD_EXT, fd, EGL_DMA_BUF_PLANE1_OFFSET_EXT, uv_offset, EGL_DMA_BUF_PLANE1_PITCH_EXT, stride,
        EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT,
        EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
        EGL_NONE};
    EGLImageKHR img = createImage_(dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
    if (img == EGL_NO_IMAGE_KHR) return fail("eglCreateImage (NV12)");
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    imageTarget_(GL_TEXTURE_EXTERNAL_OES, img);
    src_[idx] = {img, tex};
    return glGetError() == GL_NO_ERROR || fail("bind NV12 EGLImage");
}

vr::SharedTextureHandle_t GlConverter::convert(int idx) {
    const int slot = next_;
    next_ = (next_ + 1) % kRing;
    glBindFramebuffer(GL_FRAMEBUFFER, out_[slot].fbo);
    glViewport(0, 0, w_, h_);
    glUseProgram(prog_);
    glUniform2f(scale_loc_, 1.0f / w_, 1.0f / h_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, src_[idx].tex);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    // Done before the decoder reuses its buffer and before SteamVR reads ours.
    glFinish();
    last_ = slot;
    return out_[slot].handle;
}

bool GlConverter::dump_last(const std::string &path) {
    if (last_ < 0) return false;
    std::vector<unsigned char> px(size_t(w_) * h_ * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, out_[last_].fbo);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w_, h_);
    // Row 0 in memory is the top of the image (see build_program).
    for (size_t i = 0; i < size_t(w_) * h_; i++) std::fwrite(&px[i * 4], 1, 3, f);
    std::fclose(f);
    return true;
}

bool GlConverter::build_program() {
    // Fullscreen triangle. Memory row y of the output = image row y (top
    // first, as SteamVR reads dma-bufs), and EGLImage t=0 is the first row
    // in memory, so sample at fragcoord with no flip.
    const char *vs = R"(#version 300 es
        void main() {
            vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
            gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
        })";
    const char *fs = R"(#version 300 es
        #extension GL_OES_EGL_image_external_essl3 : require
        precision mediump float;
        uniform samplerExternalOES tex;
        uniform vec2 scale;
        out vec4 color;
        void main() {
            color = vec4(texture(tex, gl_FragCoord.xy * scale).rgb, 1.0);
        })";
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return false;
    prog_ = glCreateProgram();
    glAttachShader(prog_, v);
    glAttachShader(prog_, f);
    glLinkProgram(prog_);
    GLint ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    if (!ok) return fail("shader link");
    scale_loc_ = glGetUniformLocation(prog_, "scale");
    glUseProgram(prog_);
    glUniform1i(glGetUniformLocation(prog_, "tex"), 0);
    return true;
}

GLuint GlConverter::compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        fwlog("gl: shader: %s", log);
        return 0;
    }
    return s;
}

bool GlConverter::make_output(int i) {
    Out &o = out_[i];
    // Adreno's compressed layout (UBWC): cheapest for the GPU to write and the
    // compositor to read, and SteamVR imports it. The modifier must be passed
    // explicitly: a bo made without modifiers reports DRM_FORMAT_MOD_INVALID,
    // which SteamVR rejects. Elsewhere (e.g. a desktop test) fall back to linear.
    uint64_t want = DRM_FORMAT_MOD_QCOM_COMPRESSED;
    o.bo = gbm_bo_create_with_modifiers2(gbm_, w_, h_, GBM_FORMAT_ABGR8888, &want, 1, GBM_BO_USE_RENDERING);
    if (!o.bo) {
        want = DRM_FORMAT_MOD_LINEAR;
        o.bo = gbm_bo_create_with_modifiers2(gbm_, w_, h_, GBM_FORMAT_ABGR8888, &want, 1, GBM_BO_USE_RENDERING);
    }
    if (!o.bo) return fail("gbm_bo_create_with_modifiers2");
    o.fd = gbm_bo_get_fd(o.bo);
    const uint32_t stride = gbm_bo_get_stride(o.bo);
    const uint32_t offset = gbm_bo_get_offset(o.bo, 0);
    const uint64_t mod = gbm_bo_get_modifier(o.bo);

    const EGLint attr[] = {
        EGL_WIDTH, w_, EGL_HEIGHT, h_,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, o.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(offset), EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(stride),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGLint(mod & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGLint(mod >> 32),
        EGL_NONE};
    o.img = createImage_(dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attr);
    if (o.img == EGL_NO_IMAGE_KHR) return fail("eglCreateImage (RGBA)");
    glGenTextures(1, &o.tex);
    glBindTexture(GL_TEXTURE_2D, o.tex);
    imageTarget_(GL_TEXTURE_2D, o.img);
    glGenFramebuffers(1, &o.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, o.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return fail("framebuffer");

    vr::DmabufAttributes_t a{};
    a.unWidth = w_;
    a.unHeight = h_;
    a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
    a.unFormat = DRM_FORMAT_ABGR8888;
    a.ulModifier = mod;
    a.unPlaneCount = 1;
    a.plane[0].nFd = o.fd;
    a.plane[0].unStride = stride;
    a.plane[0].unOffset = offset;
    if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &o.handle) ||
        !vr::VRIPCResourceManager()->RefResource(o.handle, nullptr))
        return fail("SteamVR ImportDmabuf");
    if (i == 0) fwlog("gl: output %dx%d ABGR8888, stride %u, modifier 0x%llx", w_, h_, stride, (unsigned long long)mod);
    return true;
}

void GlConverter::release_captures() {
    for (auto &s : src_) {
        if (s.tex) glDeleteTextures(1, &s.tex);
        if (s.img != EGL_NO_IMAGE_KHR) destroyImage_(dpy_, s.img);
    }
    src_.clear();
}

void GlConverter::destroy() {
    if (ctx_ == EGL_NO_CONTEXT) return;
    release_captures();
    for (auto &o : out_) {
        if (o.handle) vr::VRIPCResourceManager()->UnrefResource(o.handle);
        if (o.fbo) glDeleteFramebuffers(1, &o.fbo);
        if (o.tex) glDeleteTextures(1, &o.tex);
        if (o.img != EGL_NO_IMAGE_KHR) destroyImage_(dpy_, o.img);
        if (o.fd >= 0) close(o.fd);
        if (o.bo) gbm_bo_destroy(o.bo);
        o = Out{};
    }
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (prog_) glDeleteProgram(prog_);
    vao_ = prog_ = 0;
    eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx_ != EGL_NO_CONTEXT) eglDestroyContext(dpy_, ctx_);
    ctx_ = EGL_NO_CONTEXT;
    eglReleaseThread();
    // No eglTerminate / gbm_device_destroy: on Mesa (zink on Turnip) that
    // unloads the driver, whose thread-exit destructors then run as the video
    // thread ends and jump into unmapped code. The display stays open until
    // the process exits; init() reuses it.
}
