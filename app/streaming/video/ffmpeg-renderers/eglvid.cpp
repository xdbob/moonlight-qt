// vim: noai:ts=4:sw=4:softtabstop=4:expandtab
#define GL_GLEXT_PROTOTYPES

#include "eglvid.h"

#include "streaming/session.h"
#include "path.h"

#include <QDir>

#include <Limelight.h>
#include <unistd.h>

#include <SDL_egl.h>
#include <SDL_opengl.h>
#include <SDL_syswm.h>
#include <SDL_opengles2.h>
#include <SDL_opengles2_gl2ext.h>

/* TODO:
 *  - handle more pixel formats
 *  - code refacto/cleanup
 *  - handle software decoding
 *  - handle window resize
 */

/* DOC/misc:
 *  - https://kernel-recipes.org/en/2016/talks/video-and-colorspaces/
 *  - http://www.brucelindbloom.com/
 *  - https://learnopengl.com/Getting-started/Shaders
 *  - https://github.com/stunpix/yuvit
 *  - https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.601_conversion
 *  - https://www.renesas.com/eu/en/www/doc/application-note/an9717.pdf
 *  - https://www.xilinx.com/support/documentation/application_notes/xapp283.pdf
 *  - https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I!!PDF-E.pdf
 *  - https://www.khronos.org/registry/OpenGL/extensions/OES/OES_EGL_image_external.txt
 *  - https://gist.github.com/rexguo/6696123
 *  - https://wiki.libsdl.org/CategoryVideo
 */

static QStringList egl_get_extensions(EGLDisplay dpy) {
    const auto EGLExtensionsStr = eglQueryString(dpy, EGL_EXTENSIONS);
    if (!EGLExtensionsStr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Unable to get EGL extensions");
        return QStringList();
    }
    return QString(EGLExtensionsStr).split(" ");
}

EGLExtensions::EGLExtensions(EGLDisplay dpy) :
    m_Extensions(egl_get_extensions(dpy))
{}

bool EGLExtensions::isSupported(const QString &extension) const {
    return m_Extensions.contains(extension);
}

EGLRenderer::EGLRenderer(IFFmpegRenderer *backendRenderer)
    :
        m_SwPixelFormat(AV_PIX_FMT_NONE),
        m_EGLDisplay(nullptr),
        m_Textures{0},
        m_ShaderProgram(0),
        m_Context(0),
        m_Window(nullptr),
        m_Backend(backendRenderer),
        m_VAO(0),
        m_ColorSpace(AVCOL_SPC_NB),
        m_ColorFull(false),
        EGLImageTargetTexture2DOES(nullptr)
{
    SDL_assert(backendRenderer);
    SDL_assert(backendRenderer->canExportEGL());
}

EGLRenderer::~EGLRenderer()
{
    deinitialize();
}

bool EGLRenderer::prepareDecoderContext(AVCodecContext*, AVDictionary**)
{
    /* Nothing to do */

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Using EGL renderer");

    return true;
}

void EGLRenderer::notifyOverlayUpdated(Overlay::OverlayType)
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
    case AV_PIX_FMT_NV12:
        return true;
    default:
        return false;
    }
}

int EGLRenderer::loadAndBuildShader(int shaderType,
                                    const char *file) {
    GLuint shader = glCreateShader(shaderType);
    if (!shader || shader == GL_INVALID_ENUM)
        return 0;

    auto sourceData = Path::readDataFile(file);
    GLint len = sourceData.size();
    const char *buf = sourceData.data();

    glShaderSource(shader, 1, &buf, &len);
    glCompileShader(shader);
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char shaderLog[512];
        glGetShaderInfoLog(shader, sizeof (shaderLog), nullptr, shaderLog);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "EGLRenderer: cannot load shader \"%s\": %s",
                     file, shaderLog);
        return 0;
    }

    return shader;
}

bool EGLRenderer::compileShader() {
    if (m_ShaderProgram) {
        glDeleteProgram(m_ShaderProgram);
        m_ShaderProgram = 0;
    }
    SDL_assert(m_SwPixelFormat != AV_PIX_FMT_NONE);

    // XXX: TODO: other formats
    SDL_assert(m_SwPixelFormat != AV_PIX_FMT_NV12);

    bool ret = false;

    GLuint vertexShader = loadAndBuildShader(GL_VERTEX_SHADER, "egl.vert");
    if (!vertexShader)
        return false;

    GLuint fragmentShader = loadAndBuildShader(GL_FRAGMENT_SHADER, "egl.frag");
    if (!fragmentShader)
        goto fragError;

    m_ShaderProgram = glCreateProgram();
    if (!m_ShaderProgram) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "EGLRenderer: cannot create shader program");
        goto progFailCreate;
    }

    glAttachShader(m_ShaderProgram, vertexShader);
    glAttachShader(m_ShaderProgram, fragmentShader);
    glLinkProgram(m_ShaderProgram);
    int status;
    glGetProgramiv(m_ShaderProgram, GL_LINK_STATUS, &status);
    if (status) {
        ret = true;
    } else {
        char shader_log[512];
        glGetProgramInfoLog(m_ShaderProgram, sizeof (shader_log), nullptr, shader_log);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "EGLRenderer: cannot link shader program: %s",
                     shader_log);
        glDeleteProgram(m_ShaderProgram);
    }

progFailCreate:
    glDeleteShader(fragmentShader);
fragError:
    glDeleteShader(vertexShader);
    return ret;
}

bool EGLRenderer::initialize(PDECODER_PARAMETERS params)
{
    m_Window = params->window;

    if (params->videoFormat == VIDEO_FORMAT_H265_MAIN10) {
        // SDL doesn't support rendering YUV 10-bit textures yet
        // TODO: FIXME
        return false;
    }

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(params->window, &info)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "!SDL_GetWindowWMInfo() failed: %s",
                     SDL_GetError());
        return false;
    }
    switch (info.subsystem) {
    case SDL_SYSWM_WAYLAND:
        m_EGLDisplay = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,
                                              info.info.wl.display, nullptr);
        break;
    case SDL_SYSWM_X11:
        m_EGLDisplay = eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR,
                                              info.info.x11.display, nullptr);
        break;
    default:
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "EGLRenderer: not compatible with SYSWM");
        return false;
    }

    if (!m_EGLDisplay) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot get EGL display: ");
        return false;
    }

    if (!(m_Context = SDL_GL_CreateContext(params->window))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot create OpenGL context: %s",
                     SDL_GetError());
        return false;
    }
    if (SDL_GL_MakeCurrent(params->window, m_Context)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Cannot use created EGL context: %s",
                     SDL_GetError());
        return false;
    }

    const EGLExtensions egl_extensions(m_EGLDisplay);
    if (!egl_extensions.isSupported("EGL_KHR_image_base") &&
        !egl_extensions.isSupported("EGL_KHR_image")) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "EGL: KHR_image unsupported");
        deinitialize();
        return false;
    }

    if (!m_Backend->initializeEGL(m_EGLDisplay, egl_extensions)) {
        deinitialize();
        return false;
    }

    if (!(EGLImageTargetTexture2DOES = (EGLImageTargetTexture2DOES_t)eglGetProcAddress("glEGLImageTargetTexture2DOES"))) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "EGL: cannot retrieve `EGLImageTargetTexture2DOES` address");
        deinitialize();
        return false;
    }

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    if (params->enableVsync) {
        /* Try to use adaptive VSYNC */
        if (SDL_GL_SetSwapInterval(-1))
            SDL_GL_SetSwapInterval(1);
    } else {
        SDL_GL_SetSwapInterval(0);
    }

    SDL_GL_SwapWindow(params->window);

    glGenTextures(EGL_MAX_PLANES, m_Textures);
    for (size_t i = 0; i < EGL_MAX_PLANES; ++i) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_Textures[i]);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenGL error: %d", err);
        deinitialize();
    }

    return err == GL_NO_ERROR;
}

void EGLRenderer::deinitialize() {
    if (m_Context) {
        if (m_ShaderProgram) {
            glDeleteProgram(m_ShaderProgram);
            m_ShaderProgram = 0;
        }
        if (m_VAO) {
            glDeleteVertexArrays(1, &m_VAO);
            m_VAO = 0;
        }
        if (m_EGLDisplay) {
            // EGL context should be handled by SDL
            m_EGLDisplay = nullptr;
        }
        SDL_GL_DeleteContext(m_Context);
        m_Context = nullptr;
    }
}

const float *EGLRenderer::getColorMatrix() {
    /* The conversion matrices are shamelessly stolen from linux:
     * drivers/media/platform/imx-pxp.c:pxp_setup_csc
     */
    static const float bt601Lim[] = {
        1.1644f, 1.1644f, 1.1644f,
        0.0f, -0.3917f, 2.0172f,
        1.5960f, -0.8129f, 0.0f
    };
    static const float bt601Full[] = {
        1.0f, 1.0f, 1.0f,
        0.0f, -0.3441f, 1.7720f,
        1.4020f, -0.7141f, 0.0f
    };
    static const float bt709Lim[] = {
        1.1644f, 1.1644f, 1.1644f,
        0.0f, -0.2132f, 2.1124f,
        1.7927f, -0.5329f, 0.0f
    };
    static const float bt709Full[] = {
        1.0f, 1.0f, 1.0f,
        0.0f, -0.1873f, 1.8556f,
        1.5748f, -0.4681f, 0.0f
    };
    static const float bt2020Lim[] = {
        1.1644f, 1.1644f, 1.1644f,
        0.0f, -0.1874f, 2.1418f,
        1.6781f, -0.6505f, 0.0f
    };
    static const float bt2020Full[] = {
        1.0f, 1.0f, 1.0f,
        0.0f, -0.1646f, 1.8814f,
        1.4746f, -0.5714f, 0.0f
    };

    switch (m_ColorSpace) {
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_BT470BG:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "EGLRenderer: BT-601 pixels");
            return m_ColorFull ? bt601Full : bt601Lim;
        case AVCOL_SPC_BT709:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "EGLRenderer: BT-709 pixels");
            return m_ColorFull ? bt709Full : bt709Lim;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "EGLRenderer: BT-2020 pixels");
            return m_ColorFull ? bt2020Full : bt2020Lim;
    };
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "EGLRenderer: unknown color space: %d, falling back to BT-601",
                m_ColorSpace);
    return bt601Lim;
}

bool EGLRenderer::specialize() {
    if (!compileShader())
        return false;
    if (m_VAO)
        glDeleteVertexArrays(1, &m_VAO);

    // XXX: Maybe we should keep the window ratio for the vertices
    static const float vertices[] = {
        // pos .... // tex coords
        1.0f, 1.0f, 1.0f, 0.0f,
        1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
        -1.0f, 1.0f, 0.0f, 0.0f,

    };
    static const unsigned int indices[] = {
        0, 1, 3,
        1, 2, 3,
    };

    glUseProgram(m_ShaderProgram);

    unsigned int VBO, EBO;
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);

    glBindVertexArray(m_VAO);

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof (vertices), vertices, GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof (indices), indices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof (float)));
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    int yuvmatLocation = glGetUniformLocation(m_ShaderProgram, "yuvmat");
    glUniformMatrix3fv(yuvmatLocation, 1, GL_FALSE, getColorMatrix());

    static const float limitedOffsets[] = { 16.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f };
    static const float fullOffsets[] = { 0.0f, 128.0f / 255.0f, 128.0f / 255.0f };

    int offLocation = glGetUniformLocation(m_ShaderProgram, "offset");
    glUniform3fv(offLocation, 1, m_ColorFull ? fullOffsets : limitedOffsets);

    int colorPlane = glGetUniformLocation(m_ShaderProgram, "plane1");
    glUniform1i(colorPlane, 0);
    colorPlane = glGetUniformLocation(m_ShaderProgram, "plane2");
    glUniform1i(colorPlane, 1);

    glDeleteBuffers(1, &VBO);
    glDeleteBuffers(1, &EBO);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "OpenGL error: %d", err);
    }

    return err == GL_NO_ERROR;
}

void EGLRenderer::renderFrame(AVFrame* frame)
{
    if (frame->hw_frames_ctx != nullptr) {
        // Find the native read-back format and load the shader
        if (m_SwPixelFormat == AV_PIX_FMT_NONE) {
            auto hwFrameCtx = (AVHWFramesContext*)frame->hw_frames_ctx->data;

            m_SwPixelFormat = hwFrameCtx->sw_format;
            SDL_assert(m_SwPixelFormat != AV_PIX_FMT_NONE);

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Selected read-back format: %d",
                        m_SwPixelFormat);
            // XXX: TODO: Handle other pixel formats
            SDL_assert(m_SwPixelFormat == AV_PIX_FMT_NV12);
            m_ColorSpace = frame->colorspace;
            m_ColorFull = frame->color_range == AVCOL_RANGE_JPEG;

            if (!specialize()) {
                m_SwPixelFormat = AV_PIX_FMT_NONE;
                return;
            }
        }

        EGLImage imgs[EGL_MAX_PLANES];
        ssize_t plane_count = m_Backend->exportEGLImages(frame, m_EGLDisplay, imgs);
        if (plane_count < 0)
            return;
        for (ssize_t i = 0; i < plane_count; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_Textures[i]);
            EGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, imgs[i]);
        }
    } else {
        // TODO: load texture for SW decoding ?
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "EGL rendering only supports hw frames");
        return;
    }

    glUseProgram(m_ShaderProgram);
    glBindVertexArray(m_VAO);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

    SDL_GL_SwapWindow(m_Window);
    m_Backend->freeEGLImages(m_EGLDisplay);
}
