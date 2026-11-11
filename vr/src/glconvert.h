// NV12 dma-buf (from the V4L2 decoder) -> RGBA dma-buf (for SteamVR), on the
// GPU with EGL/GLES.
//
// Why: SteamVR accepts NV12 dma-bufs but draws them wrong in the eye-tracked
// (foveated) region on the Steam Frame, and the decoder's own RGBA output
// can't be imported. So each decoded frame goes through one GPU pass:
// the NV12 buffer is sampled as an EGLImage through samplerExternalOES (which
// does the BT.709 limited-range YUV->RGB conversion) and drawn into one of a
// small ring of ABGR8888 buffers, each imported into SteamVR once.
//
// Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
#pragma once

#include <openvr.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <drm_fourcc.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class GlConverter {
public:
    static constexpr int kRing = 4;

    ~GlConverter() { destroy(); }

    // Output size = the decoder's visible size.
    bool init(int width, int height);
    // Import decoder buffer `idx`: NV12 in one dma-buf, chroma at `uv_offset`.
    bool import_capture(int idx, int fd, int stride, int uv_offset);
    // Convert decoder buffer `idx`; returns the SteamVR handle to show. The
    // decoder buffer may be reused as soon as this returns.
    vr::SharedTextureHandle_t convert(int idx);
    // Save the last converted frame as a binary PPM (for testing).
    bool dump_last(const std::string &path);
    void release_captures();
    void destroy();

private:
    struct Out { gbm_bo *bo = nullptr; int fd = -1; EGLImageKHR img = EGL_NO_IMAGE_KHR; GLuint tex = 0, fbo = 0; vr::SharedTextureHandle_t handle = 0; };
    struct Src { EGLImageKHR img = EGL_NO_IMAGE_KHR; GLuint tex = 0; };

    bool fail(const char *what);
    bool build_program();
    GLuint compile(GLenum type, const char *src);
    bool make_output(int i);

    int w_ = 0, h_ = 0;
    int drm_fd_ = -1;
    gbm_device *gbm_ = nullptr;
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    EGLContext ctx_ = EGL_NO_CONTEXT;
    PFNEGLCREATEIMAGEKHRPROC createImage_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTarget_ = nullptr;
    GLuint prog_ = 0, vao_ = 0;
    GLint scale_loc_ = -1;
    std::vector<Src> src_;
    Out out_[kRing];
    int next_ = 0, last_ = -1;
};
