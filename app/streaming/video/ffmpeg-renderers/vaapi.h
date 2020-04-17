#pragma once

#include "renderer.h"

// Avoid X11 if SDL was built without it
#ifndef SDL_VIDEO_DRIVER_X11
#warning Unable to use libva-x11 without SDL support
#undef HAVE_LIBVA_X11
#endif

// Avoid Wayland if SDL was built without it
#ifndef SDL_VIDEO_DRIVER_WAYLAND
#warning Unable to use libva-wayland without SDL support
#undef HAVE_LIBVA_WAYLAND
#endif

extern "C" {
#include <va/va.h>
#ifdef HAVE_LIBVA_X11
#include <va/va_x11.h>
#endif
#ifdef HAVE_LIBVA_WAYLAND
#include <va/va_wayland.h>
#endif
#ifdef HAVE_LIBVA_DRM
#include <va/va_drm.h>
#endif
#include <libavutil/hwcontext_vaapi.h>
#ifdef HAVE_EGL
#include <va/va_drmcommon.h>
#endif
}

class VAAPIRenderer : public IFFmpegRenderer
{
public:
    VAAPIRenderer();
    virtual ~VAAPIRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual bool needsTestFrame() override;
    virtual bool isDirectRenderingSupported() override;
    virtual int getDecoderColorspace() override;
#ifdef HAVE_EGL
    virtual bool canExportEGL() override;
    virtual ssize_t exportEGLImages(AVFrame *frame, EGLDisplay dpy, EGLImage images[EGL_MAX_PLANES]) override;
    virtual void freeEGLImages(EGLDisplay dpy) override;
#endif

private:
    int m_WindowSystem;
    AVBufferRef* m_HwContext;
    int m_DrmFd;

#ifdef HAVE_LIBVA_X11
    Window m_XWindow;
#endif

    int m_VideoWidth;
    int m_VideoHeight;
    int m_DisplayWidth;
    int m_DisplayHeight;

#ifdef HAVE_EGL
    VADRMPRIMESurfaceDescriptor m_descriptor;
    EGLImage m_last_images[EGL_MAX_PLANES];
#endif
};
