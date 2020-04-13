#define GL_GLEXT_PROTOTYPES

#include "eglvid.h"

#include "streaming/session.h"
#include "path.h"

#include <QDir>

#include <Limelight.h>

#include <SDL_egl.h>
#include <SDL_opengl.h>
#include <SDL_syswm.h>
#include <libavutil/hwcontext_vaapi.h>
#include <va/va_drmcommon.h>

/* TODO:
 *  - draw something
 *  - draw the video
 *  - error handling
 *  - resources cleanup
 *  - code refacto/cleanup
 *  - remove now deadcode
 */

EGLRenderer::EGLRenderer()
    :
      m_SwPixelFormat(AV_PIX_FMT_NONE),
      m_egl_display(nullptr),
      m_has_dmabuf_import(false),
      m_textures{0, 0},
      m_context(0),
      m_window(nullptr)
{}

EGLRenderer::~EGLRenderer()
{}

bool EGLRenderer::prepareDecoderContext(AVCodecContext*, AVDictionary**)
{
    /* Nothing to do */

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Using EGL renderer!!");

    return true;
}

void EGLRenderer::notifyOverlayUpdated([[maybe_unused]] Overlay::OverlayType type)
{
    // TODO: FIXME
}

bool EGLRenderer::isRenderThreadSupported()
{
	// TODO: FIXME (maybe ?)
	return false;
}

bool EGLRenderer::isPixelFormatSupported(int, AVPixelFormat pixelFormat)
{
    // Remember to keep this in sync with EGLRenderer::renderFrame()!
    switch (pixelFormat)
    {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:
        return true;

    default:
        return false;
    }
}

bool EGLRenderer::initialize(PDECODER_PARAMETERS params)
{
    m_window = params->window;

    if (params->videoFormat == VIDEO_FORMAT_H265_MAIN10) {
        // SDL doesn't support rendering YUV 10-bit textures yet
        // TODO: FIXME
        return false;
    }

    if (params->enableVsync) {
	// TODO: enable VSYNC
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(params->window, &info)) {
	    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			 "!SDL_GetWindowWMInfo() failed: %s",
			 SDL_GetError());
	    return false;
    }
    if (!(m_egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, info.info.wl.display, nullptr))) {
	    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot get EGL display");
	    return false;
    }
#if 0
    EGLint numConfigs, majorVersion, minorVersion;
    eglInitialize(m_egl_display, &majorVersion, &minorVersion);

    EGLint attributes[] = {
	    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
	    EGL_RED_SIZE, 8,
	    EGL_GREEN_SIZE, 8,
	    EGL_BLUE_SIZE, 8,
	    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
	    EGL_NONE,
    };
    EGLint num_configs;
    void *egl_config = nullptr;
    if (!eglChooseConfig(m_egl_display, attributes, &egl_config, 0, &num_configs)) {
	    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "eglChooseConfig() fail ...");
	    return false;
    }

    m_egl_context = eglCreateContext(m_egl_display, &egl_config, EGL_NO_CONTEXT, nullptr);
#endif

    const auto extentionsstr = eglQueryString(m_egl_display, EGL_EXTENSIONS);
    if (!extentionsstr) {
	    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Unable to get EGL extensions");
	    return false;
    }
    const auto extensions = QByteArray::fromRawData(extentionsstr, qstrlen(extentionsstr));
    if (!extensions.contains("EGL_EXT_image_dma_buf_import")) {
	    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "EGL: DMABUF unsupported...");
	    return false;
    }
    m_has_dmabuf_import = extensions.contains("EGL_EXT_image_dma_buf_import_modifiers");

#if 0
    eglCreateWindowSurface(m_egl_display, &egl_config, (EGLNativeWindowType) info.info.wl.display, 0);
#endif


    m_context = SDL_GL_CreateContext(params->window);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SetSwapInterval(0);
    SDL_GL_SwapWindow(params->window);

    glGenTextures(2, m_textures);

    return true;
}

void EGLRenderer::renderOverlay([[maybe_unused]] Overlay::OverlayType type)
{
	// TODO: FIXME
}

static VADRMPRIMESurfaceDescriptor hwmap(AVFrame *frame) {
        auto hwFrameCtx = (AVHWFramesContext*)frame->hw_frames_ctx->data;
	AVVAAPIDeviceContext* vaDeviceContext = (AVVAAPIDeviceContext*)hwFrameCtx->device_ctx->hwctx;

	VASurfaceID surface_id = (VASurfaceID)(uintptr_t)frame->data[3];
	VADRMPRIMESurfaceDescriptor va_desc;
	VAStatus st = vaExportSurfaceHandle(vaDeviceContext->display,
			surface_id,
			VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
			VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
			&va_desc);
	if (st != VA_STATUS_SUCCESS) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				"vaExportSurfaceHandle failed: %d", st);
	}

	st = vaSyncSurface(vaDeviceContext->display, surface_id);
	if (st != VA_STATUS_SUCCESS) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				"vaSyncSurface failed: %d", st);
	}
	// Is it needed ?
	va_desc.width = frame->width;
	va_desc.height = frame->height;
	// EOIs it needed
	return va_desc;
}

void EGLRenderer::renderFrame(AVFrame* frame)
{
    if (frame->hw_frames_ctx != nullptr) {
        // If we are acting as the frontend for a hardware
        // accelerated decoder, we'll need to read the frame
        // back to render it.

        // Find the native read-back format
        if (m_SwPixelFormat == AV_PIX_FMT_NONE) {
            auto hwFrameCtx = (AVHWFramesContext*)frame->hw_frames_ctx->data;

            m_SwPixelFormat = hwFrameCtx->sw_format;
            SDL_assert(m_SwPixelFormat != AV_PIX_FMT_NONE);

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Selected read-back format: %d",
                        m_SwPixelFormat);
        }

	auto dma_img = hwmap(frame);
	for (size_t i = 0; i < dma_img.num_layers; ++i) {
		const auto &layer = dma_img.layers[i];
		const auto &object = dma_img.objects[layer.object_index[0]];

		EGLAttrib attribs[17] = {
			EGL_LINUX_DRM_FOURCC_EXT, (EGLint)layer.drm_format,
			EGL_WIDTH, (EGLint)dma_img.width,
			EGL_HEIGHT, (EGLint)dma_img.height,
			EGL_DMA_BUF_PLANE0_FD_EXT, object.fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)layer.offset[0],
			EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)layer.pitch[0],
			EGL_NONE,
		};
		if (m_has_dmabuf_import) {
			static const EGLAttrib extra[] = {
				EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
				(EGLint)object.drm_format_modifier,
				EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
				(EGLint)(object.drm_format_modifier >> 32),
				EGL_NONE
			};
			memcpy((void *)(&attribs[12]), (void *)extra, sizeof (extra));
		}
		const auto image = eglCreateImage(m_egl_display,
				EGL_NO_CONTEXT,
				EGL_LINUX_DMA_BUF_EXT,
				nullptr,
				attribs
			);
		if (!image) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "eglCreateImageKHR() FAILED !!!");
		}

		glBindTexture(GL_TEXTURE_2D, m_textures[i]);
		glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
		glFinish();

		// ???
		//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
#if 0
        GLuint fb;
	glGenFramebuffers(1, &fb);
	glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_textures[i], 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "glCheckFramebufferStatus()");
		glDeleteFramebuffers(1, &fb);
	}
#endif
		}

	} else {
		// TODO: load texture for SW decoding ?
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "EGL rendering only supports hw frames");
		return;
	}


    SDL_GL_SwapWindow(m_window);
}
