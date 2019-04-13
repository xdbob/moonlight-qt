#pragma once

#include <SDL.h>

#include "streaming/video/decoder.h"
#include "streaming/video/overlaymanager.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class IFFmpegRenderer : public Overlay::IOverlayRenderer {
public:
    enum FramePacingConstraint {
        PACING_FORCE_OFF,
        PACING_FORCE_ON,
        PACING_ANY
    };

    virtual bool initialize(PDECODER_PARAMETERS params) = 0;
    virtual bool prepareDecoderContext(AVCodecContext* context) = 0;
    virtual void renderFrame(AVFrame* frame) = 0;

    virtual bool needsTestFrame() {
        // No test frame required by default
        return false;
    }

    virtual int getDecoderCapabilities() {
        // No special capabilities by default
        return 0;
    }

    virtual FramePacingConstraint getFramePacingConstraint() {
        // No pacing preference
        return PACING_ANY;
    }

    virtual bool isRenderThreadSupported() {
        // Render thread is supported by default
        return true;
    }

    // IOverlayRenderer
    virtual void notifyOverlayUpdated(Overlay::OverlayType) override {
        // Nothing
    }
};
