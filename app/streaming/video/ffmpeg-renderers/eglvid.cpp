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
 *  - color convertion (handle color plane)
 *  - error handling
 *  - resources cleanup
 *  - code refacto/cleanup
 *  - remove now deadcode
 *  - handle more pix FMTs
 *  - handle software decoding
 */

EGLRenderer::EGLRenderer()
    :
      m_SwPixelFormat(AV_PIX_FMT_NONE),
      m_egl_display(nullptr),
      m_has_dmabuf_import(false),
      m_textures{0, 0},
      m_shader_program(0),
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

static GLuint load_and_build_shader(GLenum shader_type,
				    const char *file,
				    const QVector<QByteArray>& extra_code) {
	GLuint shader = glCreateShader(shader_type);
	if (!shader || shader == GL_INVALID_ENUM)
		return 0;

	auto source_data = Path::readDataFile(file);
	QVector<const char *> data;
	data.reserve(extra_code.count() + 1);
	QVector<GLint> data_size;
	data_size.reserve(extra_code.count() + 1);
	for (const auto& a : extra_code) {
		data.append(a.data());
		data_size.append(a.size());
	}
	data.append(source_data);
	data_size.append(source_data.size());

	glShaderSource(shader, data.count(), data.data(), data_size.data());
	glCompileShader(shader);
	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		char shader_log[512];
		glGetShaderInfoLog(shader, sizeof (shader_log), nullptr, shader_log);
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "EGLRenderer: cannot load shader \"%s\": %s",
			     file, shader_log);
		return 0;
	}

	return shader;
}

bool EGLRenderer::compileShader() {
	if (m_shader_program) {
		glDeleteProgram(m_shader_program);
		m_shader_program = 0;
	}
	SDL_assert(m_SwPixelFormat != AV_PIX_FMT_NONE);

	// XXX: TODO: other formats
	SDL_assert(m_SwPixelFormat != AV_PIX_FMT_NV12);

	bool ret = false;

	QVector<QByteArray> shader_args;
	GLuint vertex_shader = load_and_build_shader(GL_VERTEX_SHADER,
						     "nv12.vert",
						     shader_args);
	if (!vertex_shader)
		return false;

	GLuint fragment_shader = load_and_build_shader(GL_FRAGMENT_SHADER,
						       "nv12.frag",
						       shader_args);
	if (!fragment_shader)
		goto frag_error;

	m_shader_program = glCreateProgram();
	if (!m_shader_program) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "EGLRenderer: cannot create shader program");
		goto prog_fail_create;
	}

	glAttachShader(m_shader_program, vertex_shader);
	glAttachShader(m_shader_program, fragment_shader);
	glLinkProgram(m_shader_program);
	int status;
	glGetProgramiv(m_shader_program, GL_LINK_STATUS, &status);
	if (status) {
		ret = true;
	} else {
		char shader_log[512];
		glGetProgramInfoLog(m_shader_program, sizeof (shader_log), nullptr, shader_log);
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "EGLRenderer: cannot link shader program: %s",
			     shader_log);
		glDeleteProgram(m_shader_program);
	}

prog_fail_create:
	glDeleteShader(fragment_shader);
frag_error:
	glDeleteShader(vertex_shader);
	return ret;
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
    // XXX: TODO: check all extentions
    m_has_dmabuf_import = extensions.contains("EGL_EXT_image_dma_buf_import_modifiers");

#if 0
    eglCreateWindowSurface(m_egl_display, &egl_config, (EGLNativeWindowType) info.info.wl.display, 0);
#endif



    /* Request opengl 3.2 context.
     * SDL doesn't have the ability to choose which profile at this time of writing,
     * but it should default to the core profile */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    m_context = SDL_GL_CreateContext(params->window);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SetSwapInterval(0);
    SDL_GL_SwapWindow(params->window);

    glGenTextures(2, m_textures);

#if 0
    // TODO: error handling
    static const float triangle_vertices[] = {
    #if 0
    	-0.5f, -0.5f, 0.0f,
    	0.5f, -0.5f, 0.0f,
    	0.5f, 0.5f, 0.0f,
    	-1.0f, -1.0f, 0.0f,
    	-1.0f, 1.0f, 0.0f,
    	1.0f, -1.0f, 0.0f,
    	1.0f, 1.0f, 0.0f,
    #endif
    	-0.9f, -0.9f, 0.0f,
    	-0.9f, 0.9f, 0.0f,
    	0.9f, -0.9f, 0.0f,
    	0.9f, 0.9f, 0.0f,
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof (triangle_vertices), triangle_vertices, GL_STATIC_DRAW);

    glGenVertexArrays(1, &m_vertices_frame);
    glBindVertexArray(m_vertices_frame);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof (float), nullptr);
    glEnableVertexAttribArray(0);
#endif

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
    static unsigned int VAO;
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
	    // XXX: TODO: Handle other pixel formats
	    SDL_assert(m_SwPixelFormat == AV_PIX_FMT_NV12);
	    if (!compileShader())
		    return;
#if 0
	    // XXX: TODO: error check + cleanup
	    m_Position_loc = glGetAttribLocation(m_shader_program, "aPosition");
	    m_tex_coord_loc = glGetAttribLocation(m_shader_program, "aTexCoord");
#endif
		static const float triangle_vertices[] = {
#if 0
			0.5f, 0.5f,
			0.5f, -0.5f,
			-0.5f, -0.5f,
			-0.5f, 0.5f,
#else
			// pos .... // tex coords
			1.0f, 1.0f, 1.0f, 0.0f,
			1.0f, -1.0f, 1.0f, 1.0f,
			-1.0f, -1.0f, 0.0f, 1.0f,
			-1.0f, 1.0f, 0.0f, 0.0f,

#endif
		};
		static const unsigned int indices[] = {
			0, 1, 3,
			1, 2, 3,
		};

		unsigned int VBO, EBO;
		glGenVertexArrays(1, &VAO);
		glGenBuffers(1, &VBO);
		glGenBuffers(1, &EBO);

		glBindVertexArray(VAO);

		glBindBuffer(GL_ARRAY_BUFFER, VBO);
		glBufferData(GL_ARRAY_BUFFER, sizeof (triangle_vertices), triangle_vertices, GL_STATIC_DRAW);

		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof (indices), indices, GL_STATIC_DRAW);

		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof (float)));
		glEnableVertexAttribArray(1);

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindVertexArray(0);
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

		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, m_textures[i]);
		glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

		// ???
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	} else {
		// TODO: load texture for SW decoding ?
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "EGL rendering only supports hw frames");
		return;
	}


	glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(m_shader_program);

#if 0
	int tmp = glGetUniformLocation(shaderProgram, "plane1");

	tmp = glGetUniformLocation(shaderProgram, "plane2");
#endif

	glBindVertexArray(VAO);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);


	SDL_GL_SwapWindow(m_window);
}
