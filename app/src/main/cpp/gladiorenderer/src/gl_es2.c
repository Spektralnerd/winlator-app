#include "gl_es2.h"
#include "gl_context.h"
#include "texture_utils.h"

GLESCaps gles = {0};

#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif
#ifndef GL_MAX_VARYING_VECTORS
#define GL_MAX_VARYING_VECTORS 0x8DFC
#endif
#ifndef GL_MAX_VERTEX_UNIFORM_VECTORS
#define GL_MAX_VERTEX_UNIFORM_VECTORS 0x8DFB
#endif
#ifndef GL_MAX_FRAGMENT_UNIFORM_VECTORS
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD
#endif
#ifndef GL_DEPTH_COMPONENT24_OES
#define GL_DEPTH_COMPONENT24_OES 0x81A6
#endif
#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif
#ifndef GL_RGBA8_OES
#define GL_RGBA8_OES 0x8058
#endif
#ifndef GL_RGB8_OES
#define GL_RGB8_OES 0x8051
#endif
#ifndef GL_DEPTH_STENCIL_OES
#define GL_DEPTH_STENCIL_OES 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8_OES
#define GL_UNSIGNED_INT_24_8_OES 0x84FA
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

static bool hasExtension(const char* extensions, const char* name) {
    if (!extensions) return false;
    size_t len = strlen(name);
    const char* ptr = extensions;
    while ((ptr = strstr(ptr, name))) {
        bool startOk = ptr == extensions || ptr[-1] == ' ';
        bool endOk = ptr[len] == '\0' || ptr[len] == ' ';
        if (startOk && endOk) return true;
        ptr += len;
    }
    return false;
}

/* Gibt eine lange Liste in Stücken aus (die Protokollzeilen sind begrenzt). */
static void printlnList(const char* prefix, const char* list) {
    if (!list) list = "(null)";
    const int chunk = 180;
    int len = strlen(list);
    for (int pos = 0; pos < len || pos == 0; pos += chunk) {
        char buf[200];
        snprintf(buf, sizeof(buf), "%.*s", chunk, list + pos);
        println("%s%s%s", prefix, pos ? "… " : "", buf);
    }
}

void GLES_detect(EGLDisplay eglDisplay) {
    if (gles.detected) return;
    gles.detected = true;

    const char* eglExtensions = eglQueryString(eglDisplay, EGL_EXTENSIONS);
    gles.surfaceless = hasExtension(eglExtensions, "EGL_KHR_surfaceless_context");
    const char* pbuffer = getenv("GLADIO_PBUFFER");
    if (pbuffer && pbuffer[0] == '1') {
        gles.surfaceless = false;
        println("gladio: Pbuffer-Oberflächen durch GLADIO_PBUFFER erzwungen");
    }
    println("gladio: EGL %s, %s", eglQueryString(eglDisplay, EGL_VERSION), eglQueryString(eglDisplay, EGL_VENDOR));
    printlnList("gladio: EGL-Erweiterungen: ", eglExtensions);
    if (!gles.surfaceless) println("gladio: kein EGL_KHR_surfaceless_context, Kontexte bekommen einen 1x1-Pbuffer");

    const char* force = getenv("GLADIO_GLES");
    if (force && force[0] == '2') {
        gles.es2 = true;
        println("gladio: GLES 2.0 durch GLADIO_GLES erzwungen");
    }
    else if (force && force[0] == '3') {
        gles.es2 = false;
    }
    else {
        static const EGLint confAttribList[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLint numConfigs = 0;
        EGLConfig eglConfig;
        EGLBoolean success = eglChooseConfig(eglDisplay, confAttribList, &eglConfig, 1, &numConfigs);
        gles.es2 = !success || numConfigs == 0;
        if (gles.es2) println("gladio: keine EGL-Config für OpenGL ES 3, nutze den ES-2.0-Pfad");
    }
}

static int getInteger(GLenum pname) {
    GLint value = 0;
    glGetIntegerv(pname, &value);
    return value;
}

void GLES_initOnContext() {
    if (!gles.es2 || gles.contextInit) return;
    gles.contextInit = true;

    const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
    const char* override = getenv("GLADIO_GLES_EXTENSIONS");
    if (override) {
        println("gladio: Erweiterungsliste aus GLADIO_GLES_EXTENSIONS: %s", override);
        extensions = override;
    }

    gles.vertexArrayObject = hasExtension(extensions, "GL_OES_vertex_array_object");
    gles.mapBuffer = hasExtension(extensions, "GL_OES_mapbuffer");
    gles.packedDepthStencil = hasExtension(extensions, "GL_OES_packed_depth_stencil");
    gles.depthTexture = hasExtension(extensions, "GL_OES_depth_texture");
    gles.depth24 = hasExtension(extensions, "GL_OES_depth24");
    gles.rgb8rgba8 = hasExtension(extensions, "GL_OES_rgb8_rgba8") || hasExtension(extensions, "GL_ARM_rgba8");
    gles.bgra8888 = hasExtension(extensions, "GL_EXT_texture_format_BGRA8888");
    gles.elementIndexUint = hasExtension(extensions, "GL_OES_element_index_uint");
    gles.textureNpot = hasExtension(extensions, "GL_OES_texture_npot");
    gles.standardDerivatives = hasExtension(extensions, "GL_OES_standard_derivatives");
    gles.shaderTextureLod = hasExtension(extensions, "GL_EXT_shader_texture_lod");
    gles.halfFloatVertex = hasExtension(extensions, "GL_OES_vertex_half_float");
    gles.blendMinmax = hasExtension(extensions, "GL_EXT_blend_minmax");
    gles.fragDepth = hasExtension(extensions, "GL_EXT_frag_depth");
    gles.anisotropic = hasExtension(extensions, "GL_EXT_texture_filter_anisotropic");
    gles.textureFloat = hasExtension(extensions, "GL_OES_texture_float");
    gles.textureHalfFloat = hasExtension(extensions, "GL_OES_texture_half_float");
    gles.unpackSubimage = hasExtension(extensions, "GL_EXT_unpack_subimage");

    GLint range[2] = {0, 0};
    GLint precision = 0;
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
    gles.fragmentHighp = precision > 0 && glGetError() == GL_NO_ERROR;

    gles.maxVaryingVectors = getInteger(GL_MAX_VARYING_VECTORS);
    gles.maxVertexUniformVectors = getInteger(GL_MAX_VERTEX_UNIFORM_VECTORS);
    gles.maxFragmentUniformVectors = getInteger(GL_MAX_FRAGMENT_UNIFORM_VECTORS);
    gles.maxVertexAttribs = getInteger(GL_MAX_VERTEX_ATTRIBS);
    gles.maxTextureUnits = getInteger(GL_MAX_TEXTURE_IMAGE_UNITS);
    gles.maxVertexTextureUnits = getInteger(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS);
    gles.maxTextureSize = getInteger(GL_MAX_TEXTURE_SIZE);
    gles.maxCubeMapSize = getInteger(GL_MAX_CUBE_MAP_TEXTURE_SIZE);
    gles.maxRenderbufferSize = getInteger(GL_MAX_RENDERBUFFER_SIZE);

    /* GLADIO_GLES_LIMITS="varyings=12 vs_uniforms=256 fs_uniforms=256 textures=8 vtextures=0 ..." ersetzt
     * einzelne Grenzen des Treibers (z. B. die der Mali-400, um sie auf dem PC mit Mesa nachzustellen). */
    const char* limits = getenv("GLADIO_GLES_LIMITS");
    if (limits) {
        static const struct { const char* name; int* value; } names[] = {
            {"varyings", &gles.maxVaryingVectors}, {"vs_uniforms", &gles.maxVertexUniformVectors},
            {"fs_uniforms", &gles.maxFragmentUniformVectors}, {"attribs", &gles.maxVertexAttribs},
            {"textures", &gles.maxTextureUnits}, {"vtextures", &gles.maxVertexTextureUnits},
            {"texture_size", &gles.maxTextureSize}, {"cube_size", &gles.maxCubeMapSize},
            {"renderbuffer_size", &gles.maxRenderbufferSize},
        };
        const char* p = limits;
        char name[32];
        int value, consumed;
        while (sscanf(p, " %31[a-z_]=%d%n", name, &value, &consumed) == 2) {
            if (strcmp(name, "highp_fragment") == 0) gles.fragmentHighp = value != 0;
            for (int i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
                if (strcmp(name, names[i].name) == 0) *names[i].value = value;
            }
            p += consumed;
        }
        println("gladio: Grenzen aus GLADIO_GLES_LIMITS: %s", limits);
    }

    if (gles.vertexArrayObject) {
        gles.glBindVertexArrayOES = (void*)eglGetProcAddress("glBindVertexArrayOES");
        gles.glDeleteVertexArraysOES = (void*)eglGetProcAddress("glDeleteVertexArraysOES");
        gles.glGenVertexArraysOES = (void*)eglGetProcAddress("glGenVertexArraysOES");
        gles.glIsVertexArrayOES = (void*)eglGetProcAddress("glIsVertexArrayOES");
        if (!gles.glBindVertexArrayOES || !gles.glGenVertexArraysOES || !gles.glDeleteVertexArraysOES) gles.vertexArrayObject = false;
    }
    if (gles.mapBuffer) {
        gles.glMapBufferOES = (void*)eglGetProcAddress("glMapBufferOES");
        gles.glUnmapBufferOES = (void*)eglGetProcAddress("glUnmapBufferOES");
        if (!gles.glMapBufferOES || !gles.glUnmapBufferOES) gles.mapBuffer = false;
    }

    println("gladio: ES 2.0 %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    println("gladio: ES 2.0 Erweiterungen: vao=%d mapbuffer=%d packed_depth_stencil=%d depth_texture=%d depth24=%d rgba8=%d bgra8888=%d index_uint=%d",
            gles.vertexArrayObject, gles.mapBuffer, gles.packedDepthStencil, gles.depthTexture, gles.depth24, gles.rgb8rgba8, gles.bgra8888, gles.elementIndexUint);
    println("gladio: ES 2.0 Erweiterungen: npot=%d derivatives=%d texture_lod=%d half_float=%d minmax=%d frag_depth=%d anisotropic=%d highp_fragment=%d",
            gles.textureNpot, gles.standardDerivatives, gles.shaderTextureLod, gles.halfFloatVertex, gles.blendMinmax, gles.fragDepth, gles.anisotropic, gles.fragmentHighp);
    println("gladio: ES 2.0 Grenzen: varyings=%d vs_uniforms=%d fs_uniforms=%d attribs=%d textures=%d/%d texture_size=%d renderbuffer_size=%d",
            gles.maxVaryingVectors, gles.maxVertexUniformVectors, gles.maxFragmentUniformVectors, gles.maxVertexAttribs,
            gles.maxTextureUnits, gles.maxVertexTextureUnits, gles.maxTextureSize, gles.maxRenderbufferSize);
}

const char* GLES_versionString() {
    return gles.es2 ? "2.1" : GL_STRING_VERSION;
}

const char* GLES_shadingLanguageVersionString() {
    return gles.es2 ? "1.20" : GL_STRING_SHADING_LANGUAGE_VERSION;
}

int GLES_majorVersion() {
    int major = 0, minor = 0;
    sscanf(GLES_versionString(), "%d.%d", &major, &minor);
    return major;
}

int GLES_minorVersion() {
    int major = 0, minor = 0;
    sscanf(GLES_versionString(), "%d.%d", &major, &minor);
    return minor;
}

bool GLES_isCapabilitySupported(GLenum cap) {
    if (!gles.es2) return true;
    switch (cap) {
        case GL_BLEND:
        case GL_CULL_FACE:
        case GL_DEPTH_TEST:
        case GL_DITHER:
        case GL_POLYGON_OFFSET_FILL:
        case GL_SAMPLE_ALPHA_TO_COVERAGE:
        case GL_SAMPLE_COVERAGE:
        case GL_SCISSOR_TEST:
        case GL_STENCIL_TEST:
            return true;
        default:
            return false;
    }
}

bool GLES_isBufferTarget(GLenum target) {
    if (!gles.es2) return true;
    return target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER;
}

GLenum GLES_vertexAttribType(GLenum type) {
    if (!gles.es2) return type;
    switch (type) {
        case GL_HALF_FLOAT:
            return GL_HALF_FLOAT_OES;
        case GL_DOUBLE:
            return GL_FLOAT;
        default:
            return type;
    }
}

const char* GLES_clientExtensions(int* outNumExtensions) {
    static char* extensionNames = NULL;
    static int numExtensions = 0;

    if (!extensionNames) {
        /* Grundstock, den der ES-2.0-Server (ggf. mit Software-Umwegen) erfüllt. Nicht dabei sind
         * z. B. GL_ARB_framebuffer_object (Multisample), GL_ARB_texture_rg, Float-Texturen,
         * Sync-Objekte, Sampler-Objekte, Instancing, UBOs und die ARB-Assembler-Programme.
         * Auch nicht GL_ARB_buffer_storage: wined3d legt damit alle Puffer als GL_PIXEL_UNPACK_BUFFER
         * an (64-MB-Stücke, dauerhaft gemappt), und für dieses Ziel führt der Server nur
         * Schattenspeicher ohne GL-Puffer – Zeichenaufrufe daraus blieben leer. Ohne die
         * Erweiterung nimmt wined3d glBufferData/glMapBufferRange am echten Ziel.
         * GL_ARB_framebuffer_object/GL_EXT_framebuffer_blit sind dabei, weil wined3d 10.x
         * glBlitFramebuffer und getrennte Lese-/Zeichen-Framebuffer ohnehin voraussetzt
         * (Bildausgabe) und ohne ARB_framebuffer_object für kleine Render-Targets eigene
         * Tiefen-Renderbuffer anlegt, die es nie löscht; gl_framebuffer.c bildet Blit, getrennte
         * Ziele und ungleich große Anhänge in ES 2.0 nach (Farb-Blit als texturiertes Viereck). */
        const char* base =
            "GL_ARB_window_pos GL_EXT_packed_pixels GL_ARB_vertex_buffer_object GL_ARB_pixel_buffer_object "
            "GL_ARB_texture_border_clamp GL_ARB_texture_env_add GL_EXT_texture_env_add GL_EXT_draw_range_elements GL_EXT_bgra "
            "GL_ARB_texture_compression GL_EXT_texture_compression_s3tc GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_dxt3 GL_EXT_texture_compression_dxt5 "
            "GL_ARB_point_parameters GL_EXT_point_parameters GL_EXT_texture_edge_clamp GL_EXT_multi_draw_arrays GL_ARB_multisample GL_EXT_polygon_offset "
            "GL_EXT_vertex_array GL_ARB_texture_non_power_of_two GL_EXT_blend_color GL_EXT_blend_equation_separate GL_EXT_blend_func_separate GL_EXT_blend_subtract "
            "GL_ARB_texture_mirrored_repeat GL_ARB_point_sprite GL_ARB_texture_cube_map GL_EXT_texture_cube_map "
            "GL_ARB_ES2_compatibility GL_ARB_fragment_shader GL_ARB_vertex_shader GL_ARB_shading_language_100 "
            "GL_ARB_framebuffer_object GL_EXT_framebuffer_object GL_EXT_framebuffer_blit GL_ARB_draw_buffers GL_ARB_internalformat_query GL_ARB_internalformat_query2 GL_ARB_map_buffer_range "
            "GL_ARB_multitexture GL_ARB_texture_env_combine GL_EXT_texture_env_combine GL_ARB_texture_env_dot3 GL_EXT_texture_env_dot3 "
            "GL_ARB_shader_objects GL_ARB_occlusion_query GL_ARB_occlusion_query2 "
            "GL_EXT_direct_state_access GL_ARB_timer_query GL_EXT_timer_query";

        ArrayBuffer buffer = {0};
        ArrayBuffer_putBytes(&buffer, base, strlen(base));
        if (gles.vertexArrayObject) ArrayBuffer_putString(&buffer, " GL_ARB_vertex_array_object");
        if (gles.packedDepthStencil) ArrayBuffer_putString(&buffer, " GL_EXT_packed_depth_stencil");
        if (gles.depthTexture) ArrayBuffer_putString(&buffer, " GL_EXT_depth_texture GL_ARB_depth_texture");
        if (gles.blendMinmax) ArrayBuffer_putString(&buffer, " GL_EXT_blend_minmax");
        if (gles.anisotropic) ArrayBuffer_putString(&buffer, " GL_EXT_texture_filter_anisotropic");
        if (gles.shaderTextureLod) ArrayBuffer_putString(&buffer, " GL_ARB_shader_texture_lod");
        ArrayBuffer_put(&buffer, '\0');
        extensionNames = (char*)buffer.buffer;

        numExtensions = 1;
        for (char* ptr = extensionNames; *ptr; ptr++) if (*ptr == ' ') numExtensions++;
    }

    if (outNumExtensions) *outNumExtensions = numExtensions;
    return extensionNames;
}

/* ------------------------------------------------------------------ Texturformate */

static int bytesPerPixelOfType(uint32_t format, uint32_t type) {
    return computeTexImageDataSize(format, type, 1, 1, 1);
}

static void* convertToRGBA8(uint32_t format, uint32_t type, const void* src, int pixelCount, bool* ownsData) {
    int channels = getTexImageFormatChannels(format);
    if (channels < 1 || channels > 4) return NULL;
    uint8_t* dst = malloc(pixelCount * channels);
    if (!dst) return NULL;

    switch (type) {
        case GL_UNSIGNED_SHORT: {
            const uint16_t* values = src;
            for (int i = 0; i < pixelCount * channels; i++) dst[i] = values[i] >> 8;
            break;
        }
        case GL_SHORT: {
            const int16_t* values = src;
            for (int i = 0; i < pixelCount * channels; i++) dst[i] = MAX(values[i], 0) >> 7;
            break;
        }
        case GL_UNSIGNED_INT: {
            const uint32_t* values = src;
            for (int i = 0; i < pixelCount * channels; i++) dst[i] = values[i] >> 24;
            break;
        }
        case GL_FLOAT: {
            const float* values = src;
            for (int i = 0; i < pixelCount * channels; i++) dst[i] = (uint8_t)(CLAMP(values[i], 0.0f, 1.0f) * 255.0f + 0.5f);
            break;
        }
        case GL_HALF_FLOAT: {
            const uint16_t* values = src;
            for (int i = 0; i < pixelCount * channels; i++) {
                _Float16 h;
                memcpy(&h, &values[i], sizeof(h));
                dst[i] = (uint8_t)(CLAMP((float)h, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            break;
        }
        case GL_UNSIGNED_INT_10_10_10_2:
        case GL_UNSIGNED_INT_2_10_10_10_REV: {
            const uint32_t* values = src;
            for (int i = 0; i < pixelCount; i++) {
                uint32_t v = values[i];
                uint32_t r, g, b, a;
                if (type == GL_UNSIGNED_INT_2_10_10_10_REV) {
                    r = v & 0x3ff; g = (v >> 10) & 0x3ff; b = (v >> 20) & 0x3ff; a = (v >> 30) & 0x3;
                }
                else {
                    r = (v >> 22) & 0x3ff; g = (v >> 12) & 0x3ff; b = (v >> 2) & 0x3ff; a = v & 0x3;
                }
                dst[i*4+0] = r >> 2;
                dst[i*4+1] = g >> 2;
                dst[i*4+2] = b >> 2;
                dst[i*4+3] = a * 85;
            }
            break;
        }
        default:
            free(dst);
            return NULL;
    }

    *ownsData = true;
    return dst;
}

static void swapRedBlue(uint8_t* pixels, int pixelCount, int channels) {
    for (int i = 0; i < pixelCount; i++, pixels += channels) {
        uint8_t tmp = pixels[0];
        pixels[0] = pixels[2];
        pixels[2] = tmp;
    }
}

bool GLES_convertTexImageFormat(uint32_t target, int* internalformat, uint32_t* format, uint32_t* type, void** imageData, int* imageSize, bool* ownsData) {
    int none = GL_NONE;
    if (!internalformat) internalformat = &none;
    if (ownsData) *ownsData = false;
    int dataSize = imageSize ? *imageSize : 0;
    void* data = imageData ? *imageData : NULL;

    /* Typen vereinheitlichen */
    if (*type == GL_UNSIGNED_INT_8_8_8_8 || *type == GL_UNSIGNED_INT_8_8_8_8_REV || *type == GL_BYTE) *type = GL_UNSIGNED_BYTE;

    /* Komprimierte Formate ohne Daten (Software-Dekodierung liefert BGRA) */
    if (isCompressedFormat(*internalformat) && dataSize == 0) {
        *format = GL_BGRA;
    }

    int pixelCount = 0;
    if (data && dataSize > 0) {
        int bpp = bytesPerPixelOfType(*format, *type);
        pixelCount = bpp > 0 ? dataSize / bpp : 0;
    }

    switch (*format) {
        case GL_BGRA:
            if (*type == GL_UNSIGNED_SHORT_1_5_5_5_REV) {
                *type = GL_UNSIGNED_SHORT_5_5_5_1;
                *format = GL_RGBA;
                if (data) {
                    GLushort* pixels = data;
                    for (int i = 0; i < dataSize / (int)sizeof(GLushort); i++) {
                        GLushort pixel = pixels[i];
                        GLushort r = pixel & 0x1f;
                        GLushort g = (pixel >> 5) & 0x1f;
                        GLushort b = (pixel >> 10) & 0x1f;
                        GLushort a = (pixel >> 15) & 0x01;
                        pixels[i] = (a & 0x01) | ((r & 0x1f) << 1) | ((g & 0x1f) << 6) | ((b & 0x1f) << 11);
                    }
                }
            }
            else if (*type == GL_UNSIGNED_SHORT_4_4_4_4_REV) {
                *type = GL_UNSIGNED_SHORT_4_4_4_4;
                *format = GL_RGBA;
                if (data) {
                    GLushort* pixels = data;
                    for (int i = 0; i < dataSize / (int)sizeof(GLushort); i++) {
                        GLushort pixel = pixels[i];
                        GLushort r = pixel & 0x0f;
                        GLushort g = (pixel >> 4) & 0x0f;
                        GLushort b = (pixel >> 8) & 0x0f;
                        GLushort a = (pixel >> 12) & 0x0f;
                        pixels[i] = ((b & 0x0f) << 12) | ((g & 0x0f) << 8) | ((r & 0x0f) << 4) | (a & 0x0f);
                    }
                }
            }
            else if (*type == GL_UNSIGNED_BYTE && !gles.bgra8888) {
                if (data) swapRedBlue(data, pixelCount, 4);
                *format = GL_RGBA;
            }
            else if (*type != GL_UNSIGNED_BYTE) {
                /* Kanalreihenfolge bleibt B,G,R,A; nur die Breite wird auf 8 Bit gebracht */
                void* converted = data ? convertToRGBA8(GL_RGBA, *type, data, pixelCount, ownsData) : NULL;
                if (data && !converted) return false;
                if (converted) {
                    data = converted;
                    dataSize = pixelCount * 4;
                }
                *type = GL_UNSIGNED_BYTE;
                *format = gles.bgra8888 ? GL_BGRA : GL_RGBA;
                if (!gles.bgra8888 && data) swapRedBlue(data, pixelCount, 4);
            }
            break;
        case GL_BGR:
            if (*type == GL_UNSIGNED_BYTE) {
                if (data) swapRedBlue(data, pixelCount, 3);
            }
            else {
                void* converted = data ? convertToRGBA8(GL_RGB, *type, data, pixelCount, ownsData) : NULL;
                if (data && !converted) return false;
                if (converted) {
                    swapRedBlue(converted, pixelCount, 3);
                    data = converted;
                    dataSize = pixelCount * 3;
                }
                *type = GL_UNSIGNED_BYTE;
            }
            *format = GL_RGB;
            break;
        case GL_RGBA:
        case GL_RGB:
        case GL_LUMINANCE:
        case GL_LUMINANCE_ALPHA:
        case GL_ALPHA: {
            bool okType = *type == GL_UNSIGNED_BYTE ||
                          (*format == GL_RGBA && (*type == GL_UNSIGNED_SHORT_4_4_4_4 || *type == GL_UNSIGNED_SHORT_5_5_5_1)) ||
                          (*format == GL_RGB && *type == GL_UNSIGNED_SHORT_5_6_5);
            if (!okType) {
                void* converted = data ? convertToRGBA8(*format, *type, data, pixelCount, ownsData) : NULL;
                if (data && !converted) return false;
                if (converted) {
                    data = converted;
                    dataSize = pixelCount * getTexImageFormatChannels(*format);
                }
                *type = GL_UNSIGNED_BYTE;
            }
            break;
        }
        case GL_INTENSITY: {
            /* (I, I, I, I) = LUMINANCE_ALPHA mit L = A = I */
            if (*type != GL_UNSIGNED_BYTE) {
                void* converted = data ? convertToRGBA8(GL_RED, *type, data, pixelCount, ownsData) : NULL;
                if (data && !converted) return false;
                if (converted) {
                    data = converted;
                    dataSize = pixelCount;
                }
                *type = GL_UNSIGNED_BYTE;
            }
            if (data) {
                uint8_t* la = malloc(pixelCount * 2);
                const uint8_t* src = data;
                for (int i = 0; i < pixelCount; i++) la[i*2] = la[i*2+1] = src[i];
                if (ownsData && *ownsData) free(data);
                data = la;
                dataSize = pixelCount * 2;
                if (ownsData) *ownsData = true;
            }
            *format = GL_LUMINANCE_ALPHA;
            break;
        }
        case GL_RED:
        case GL_GREEN:
        case GL_BLUE: {
            if (*type != GL_UNSIGNED_BYTE) {
                void* converted = data ? convertToRGBA8(GL_RED, *type, data, pixelCount, ownsData) : NULL;
                if (data && !converted) return false;
                if (converted) {
                    data = converted;
                    dataSize = pixelCount;
                }
                *type = GL_UNSIGNED_BYTE;
            }
            *format = GL_LUMINANCE;
            break;
        }
        case GL_RG: {
            if (*type != GL_UNSIGNED_BYTE) {
                void* converted = data ? convertToRGBA8(GL_RG, *type, data, pixelCount, ownsData) : NULL;
                if (data && !converted) return false;
                if (converted) {
                    data = converted;
                    dataSize = pixelCount * 2;
                }
                *type = GL_UNSIGNED_BYTE;
            }
            *format = GL_LUMINANCE_ALPHA;
            break;
        }
        case GL_DEPTH_COMPONENT:
            if (!gles.depthTexture) return false;
            if (*type != GL_UNSIGNED_SHORT) *type = GL_UNSIGNED_INT;
            break;
        case GL_DEPTH_STENCIL:
            if (!gles.depthTexture || !gles.packedDepthStencil) return false;
            *format = GL_DEPTH_STENCIL_OES;
            *type = GL_UNSIGNED_INT_24_8_OES;
            break;
        default:
            println("gladio:es2: unbekanntes Texturformat %s/%s", glEnumToString(*format), glEnumToString(*type));
            return false;
    }

    /* In ES 2.0 ist das interne Format gleich dem Datenformat. */
    *internalformat = *format == GL_BGRA ? GL_BGRA_EXT : *format;
    if (*format == GL_BGRA) *format = GL_BGRA_EXT;

    if (imageData) *imageData = data;
    if (imageSize) *imageSize = dataSize;
    return true;
}

GLenum GLES_renderbufferFormat(GLenum internalformat) {
    switch (internalformat) {
        case GL_DEPTH_COMPONENT:
        case GL_DEPTH_COMPONENT24:
        case GL_DEPTH_COMPONENT32:
        case GL_DEPTH_COMPONENT32F:
            return gles.depth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16;
        case GL_DEPTH_COMPONENT16:
            return GL_DEPTH_COMPONENT16;
        case GL_DEPTH_STENCIL:
        case GL_DEPTH24_STENCIL8:
        case GL_DEPTH32F_STENCIL8:
            if (gles.packedDepthStencil) return GL_DEPTH24_STENCIL8_OES;
            return gles.depth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16;
        case GL_STENCIL_INDEX:
        case GL_STENCIL_INDEX8:
            return GL_STENCIL_INDEX8;
        case GL_RGB565:
        case GL_RGBA4:
        case GL_RGB5_A1:
            return internalformat;
        case GL_RGB:
        case GL_RGB8:
        case GL_SRGB8:
            return gles.rgb8rgba8 ? GL_RGB8_OES : GL_RGB565;
        default:
            return gles.rgb8rgba8 ? GL_RGBA8_OES : GL_RGBA4;
    }
}

void* GLES_repackRows(const void* pixels, int width, int height, int bytesPerPixel) {
    if (!pixels || gles.unpackRowLength <= 0 || gles.unpackRowLength == width) return NULL;
    int srcStride = gles.unpackRowLength * bytesPerPixel;
    int dstStride = width * bytesPerPixel;
    uint8_t* dst = malloc((size_t)dstStride * height);
    const uint8_t* src = (const uint8_t*)pixels + gles.unpackSkipRows * srcStride + gles.unpackSkipPixels * bytesPerPixel;
    for (int y = 0; y < height; y++) memcpy(dst + y * dstStride, src + y * srcStride, dstStride);
    return dst;
}

/* ------------------------------------------------------------------ Hochladen */

/* Interne Formate ohne Alphakanal: GL liefert beim Abtasten A = 1 (GL_RGB5 für D3D-X1R5G5B5,
 * GL_RGB8 für X8R8G8B8, GL_RGB4 für X4R4G4B4 usw.). In ES 2.0 legen Format und Typ der Daten die
 * Textur fest, und die umgesetzten Daten (RGBA 5551/4444, RGBA/BGRA 8888) tragen im Alphakanal das
 * undefinierte X-Bit – meist 0. Sobald mit Alpha gemischt oder getestet wird, wäre die Textur dann
 * unsichtbar (LEGO Racers: Streckenlinie der Minikarte, X1R5G5B5 mit SRC_ALPHA gemischt). */
static bool isOpaqueInternalFormat(GLint internalformat) {
    switch (internalformat) {
        case 3: case GL_RGB: case GL_R3_G3_B2: case GL_RGB4: case GL_RGB5: case GL_RGB8:
        case GL_RGB10: case GL_RGB12: case GL_RGB16: case GL_SRGB: case GL_SRGB8: case GL_RGB565:
            return true;
        default:
            return false;
    }
}

/* setzt den Alphakanal umgesetzter Daten auf 1; fremde Daten (Ringpuffer, PBO) werden vorher kopiert */
static void* forceOpaqueAlpha(GLenum format, GLenum type, void* pixels, int dataSize, bool owned) {
    bool shorts = type == GL_UNSIGNED_SHORT_5_5_5_1 || type == GL_UNSIGNED_SHORT_4_4_4_4;
    bool bytes = type == GL_UNSIGNED_BYTE && (format == GL_RGBA || format == GL_BGRA_EXT);
    if (!pixels || dataSize <= 0 || (!shorts && !bytes)) return pixels;
    if (!owned) {
        void* copy = malloc(dataSize);
        memcpy(copy, pixels, dataSize);
        pixels = copy;
    }
    if (shorts) {
        GLushort mask = type == GL_UNSIGNED_SHORT_5_5_5_1 ? 0x0001 : 0x000f;
        GLushort* values = pixels;
        for (int i = 0; i < dataSize / (int)sizeof(GLushort); i++) values[i] |= mask;
    }
    else {
        uint8_t* values = pixels;
        for (int i = 3; i < dataSize; i += 4) values[i] = 0xff;
    }
    return pixels;
}

static void makeOpaque(GLenum format, GLenum type, void** pixels, int dataSize, bool* ownsConverted, bool ownsPacked, void* packed) {
    bool owned = *ownsConverted || (ownsPacked && *pixels == packed);
    void* opaque = forceOpaqueAlpha(format, type, *pixels, dataSize, owned);
    if (opaque != *pixels) {
        *pixels = opaque;
        *ownsConverted = true;
    }
}

static void* prepareUpload(int width, int height, GLenum format, GLenum type, void* data, int* dataSize, bool tight, bool* owns) {
    *owns = false;
    if (!tight && data && gles.unpackRowLength > 0 && gles.unpackRowLength != width) {
        int bpp = computeTexImageDataSize(format, type, 1, 1, 1);
        void* packed = GLES_repackRows(data, width, height, bpp);
        if (packed) {
            *owns = true;
            *dataSize = width * height * bpp;
            return packed;
        }
    }
    return data;
}

void GLES_texImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, void* data, int dataSize, bool tight) {
    bool ownsPacked, ownsConverted = false;
    if (data) gladio_statsTexBytes += dataSize;
    void* packed = prepareUpload(width, height, format, type, data, &dataSize, tight, &ownsPacked);
    void* pixels = packed;
    GLint ifmt = internalformat;
    GLenum fmt = format, typ = type;

    if (GLES_convertTexImageFormat(target, &ifmt, &fmt, &typ, &pixels, &dataSize, &ownsConverted)) {
        if (pixels && isOpaqueInternalFormat(internalformat)) makeOpaque(fmt, typ, &pixels, dataSize, &ownsConverted, ownsPacked, packed);
#if IS_DEBUG_ENABLED(DEBUG_MODE_TEXTURE_FORMAT)
        println("gladio:es2:texImage2D %s Stufe %d %dx%d %s/%s/%s -> %s/%s/%s %s", glEnumToString(target), level, width, height,
                glEnumToString(internalformat), glEnumToString(format), glEnumToString(type), glEnumToString(ifmt), glEnumToString(fmt), glEnumToString(typ), pixels ? "mit Daten" : "ohne Daten");
#endif
        glTexImage2D(target, level, ifmt, width, height, border, fmt, typ, pixels);
        GLTexture* texture = GLTexture_getBound(target);
        if (texture && level == 0) texture->es2Format = fmt;
    }
    else println("gladio:es2: glTexImage2D %s/%s nicht abbildbar", glEnumToString(format), glEnumToString(type));

    if (ownsConverted) free(pixels);
    if (ownsPacked) free(packed);
}

void GLES_texSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, void* data, int dataSize, bool tight) {
    bool ownsPacked, ownsConverted = false;
    if (data) gladio_statsTexBytes += dataSize;
    void* packed = prepareUpload(width, height, format, type, data, &dataSize, tight, &ownsPacked);
    void* pixels = packed;
    GLint ifmt = GL_NONE;
    GLenum fmt = format, typ = type;

    if (GLES_convertTexImageFormat(target, &ifmt, &fmt, &typ, &pixels, &dataSize, &ownsConverted)) {
        /* ES 2.0 verlangt dasselbe Format wie beim Anlegen: BGRA <-> RGBA notfalls tauschen */
        GLTexture* texture = GLTexture_getBound(target);
        if (texture && texture->es2Format != 0 && texture->es2Format != fmt && typ == GL_UNSIGNED_BYTE &&
            ((fmt == GL_BGRA_EXT && texture->es2Format == GL_RGBA) || (fmt == GL_RGBA && texture->es2Format == GL_BGRA_EXT))) {
            if (pixels && !ownsConverted) {
                void* copy = malloc(dataSize);
                memcpy(copy, pixels, dataSize);
                pixels = copy;
                ownsConverted = true;
            }
            if (pixels) swapRedBlue(pixels, dataSize / 4, 4);
            fmt = texture->es2Format;
        }
        if (pixels && texture && isOpaqueInternalFormat(texture->originFormat)) makeOpaque(fmt, typ, &pixels, dataSize, &ownsConverted, ownsPacked, packed);
#if IS_DEBUG_ENABLED(DEBUG_MODE_TEXTURE_FORMAT)
        println("gladio:es2:texSubImage2D %s Stufe %d %d,%d %dx%d %s/%s -> %s/%s (Textur %s)", glEnumToString(target), level, xoffset, yoffset, width, height,
                glEnumToString(format), glEnumToString(type), glEnumToString(fmt), glEnumToString(typ), texture ? glEnumToString(texture->es2Format) : "?");
#endif
        glTexSubImage2D(target, level, xoffset, yoffset, width, height, fmt, typ, pixels);
    }
    else println("gladio:es2: glTexSubImage2D %s/%s nicht abbildbar", glEnumToString(format), glEnumToString(type));

    if (ownsConverted) free(pixels);
    if (ownsPacked) free(packed);
}

/* ------------------------------------------------------------------ Texturparameter */

void GLES_applyMinFilter(GLenum target, GLTexture* texture) {
    GLenum filter = texture->minFilter;
    if (texture->maxLevel == 0) {
        if (filter == GL_NEAREST_MIPMAP_NEAREST || filter == GL_NEAREST_MIPMAP_LINEAR) filter = GL_NEAREST;
        else if (filter == GL_LINEAR_MIPMAP_NEAREST || filter == GL_LINEAR_MIPMAP_LINEAR) filter = GL_LINEAR;
    }
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
}

/* ------------------------------------------------------------------ Zeichnen */

void GLES_drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex) {
    bool needsConversion = basevertex != 0 || (type == GL_UNSIGNED_INT && !gles.elementIndexUint);
    if (!needsConversion) {
        glDrawElements(mode, count, type, indices);
        return;
    }

    GLBuffer* elementBuffer = GLBuffer_getBound(GL_ELEMENT_ARRAY_BUFFER);
    const uint8_t* src = NULL;
    if (elementBuffer) {
        if (elementBuffer->mappedData) src = (const uint8_t*)elementBuffer->mappedData + (uintptr_t)indices;
    }
    else src = indices;

    if (!src) {
        println("gladio:es2: Indizes nicht lesbar (basevertex %d, Typ %s)", basevertex, glEnumToString(type));
        glDrawElements(mode, count, type, indices);
        return;
    }

    uint32_t maxIndex = 0;
    uint32_t* values = malloc(count * sizeof(uint32_t));
    for (int i = 0; i < count; i++) {
        uint32_t index;
        switch (type) {
            case GL_UNSIGNED_BYTE: index = src[i]; break;
            case GL_UNSIGNED_SHORT: index = ((const uint16_t*)src)[i]; break;
            default: index = ((const uint32_t*)src)[i]; break;
        }
        index += basevertex;
        values[i] = index;
        if (index > maxIndex) maxIndex = index;
    }

    if (elementBuffer) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    if (maxIndex < 0x10000) {
        uint16_t* shortValues = malloc(count * sizeof(uint16_t));
        for (int i = 0; i < count; i++) shortValues[i] = (uint16_t)values[i];
        glDrawElements(mode, count, GL_UNSIGNED_SHORT, shortValues);
        free(shortValues);
    }
    else if (gles.elementIndexUint) {
        glDrawElements(mode, count, GL_UNSIGNED_INT, values);
    }
    else println("gladio:es2: Index %u passt nicht in 16 Bit und OES_element_index_uint fehlt", maxIndex);
    if (elementBuffer) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementBuffer->id);
    free(values);
}
