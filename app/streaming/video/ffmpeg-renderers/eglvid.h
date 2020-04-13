#pragma once

#include "renderer.h"

class EGLRenderer : public IFFmpegRenderer {
public:
    EGLRenderer();
    virtual ~EGLRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool isRenderThreadSupported() override;
    virtual bool isPixelFormatSupported(int videoFormat, enum AVPixelFormat pixelFormat) override;

private:
    void renderOverlay(Overlay::OverlayType type);

    int m_SwPixelFormat;
    void *m_egl_display;
    bool m_has_dmabuf_import;
    unsigned m_textures[2];
    SDL_GLContext m_context;
    SDL_Window *m_window;
};

