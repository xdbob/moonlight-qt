#pragma once

#include "renderer.h"

class EGLRenderer : public IFFmpegRenderer {
public:
    EGLRenderer(IFFmpegRenderer *frontend_renderer);
    virtual ~EGLRenderer() override;
    virtual bool initialize(PDECODER_PARAMETERS params) override;
    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary** options) override;
    virtual void renderFrame(AVFrame* frame) override;
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override;
    virtual bool isRenderThreadSupported() override;
    virtual bool isPixelFormatSupported(int videoFormat, enum AVPixelFormat pixelFormat) override;

private:
    void renderOverlay(Overlay::OverlayType type);

    bool compileShader();
    bool specialize();

    int m_SwPixelFormat;
    void *m_egl_display;
    unsigned m_textures[EGL_MAX_PLANES];
    unsigned m_shader_program;
    SDL_GLContext m_context;
    SDL_Window *m_window;
    IFFmpegRenderer *m_frontend;
    unsigned int m_vao;
};
