#include "gl_formats.h"
#include "texture_utils.h"
#include "gl_es2.h"

static GLFormatInfo textureFormats[] = {
    {GL_R8, GL_RED, GL_UNSIGNED_NORMALIZED, 8, 0, 0, 0, 0, true, true},
    {GL_R8_SNORM, GL_RED, GL_SIGNED_NORMALIZED, 8, 0, 0, 0, 0, false, true},
    {GL_R16F, GL_RED, GL_FLOAT, 16, 0, 0, 0, 0, false, true},
    {GL_R32F, GL_RED, GL_FLOAT, 32, 0, 0, 0, 0, false, false},
    {GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 8, 0, 0, 0, 0, true, false},
    {GL_R8I, GL_RED_INTEGER, GL_INT, 8, 0, 0, 0, 0, true, false},
    {GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 16, 0, 0, 0, 0, true, false},
    {GL_R16I, GL_RED_INTEGER, GL_INT, 16, 0, 0, 0, 0, true, false},
    {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 32, 0, 0, 0, 0, true, false},
    {GL_R32I, GL_RED_INTEGER, GL_INT, 32, 0, 0, 0, 0, true, false},
    {GL_RG8, GL_RG, GL_UNSIGNED_NORMALIZED, 8, 8, 0, 0, 0, true, true},
    {GL_RG8_SNORM, GL_RG, GL_SIGNED_NORMALIZED, 8, 8, 0, 0, 0, false, true},
    {GL_RG16F, GL_RG, GL_FLOAT, 16, 16, 0, 0, 0, false, true},
    {GL_RG32F, GL_RG, GL_FLOAT, 32, 32, 0, 0, 0, false, false},
    {GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 8, 8, 0, 0, 0, true, false},
    {GL_RG8I, GL_RG_INTEGER, GL_INT, 8, 8, 0, 0, 0, true, false},
    {GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 16, 16, 0, 0, 0, true, false},
    {GL_RG16I, GL_RG_INTEGER, GL_INT, 16, 16, 0, 0, 0, true, false},
    {GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 32, 32, 0, 0, 0, true, false},
    {GL_RG32I, GL_RG_INTEGER, GL_INT, 32, 32, 0, 0, 0, true, false},
    {GL_RGB8, GL_RGB, GL_UNSIGNED_NORMALIZED, 8, 8, 8, 0, 0, true, true},
    {GL_SRGB8, GL_RGB, GL_UNSIGNED_NORMALIZED, 8, 8, 8, 0, 0, false, true},
    {GL_RGB565, GL_RGB, GL_UNSIGNED_NORMALIZED, 5, 6, 5, 0, 0, true, true},
    {GL_RGB8_SNORM, GL_RGB, GL_SIGNED_NORMALIZED, 8, 8, 8, 0, 0, false, true},
    {GL_R11F_G11F_B10F, GL_RGB, GL_FLOAT, 11, 11, 10, 0, 0, false, true},
    {GL_RGB9_E5, GL_RGB, GL_UNSIGNED_NORMALIZED, 9, 9, 9, 0, 5, false, true},
    {GL_RGB16F, GL_RGB, GL_FLOAT, 16, 16, 16, 0, 0, false, true},
    {GL_RGB32F, GL_RGB, GL_FLOAT, 32, 32, 32, 0, 0, false, false},
    {GL_RGB8UI, GL_RGB_INTEGER, GL_UNSIGNED_INT, 8, 8, 8, 0, 0, false, false},
    {GL_RGB8I, GL_RGB_INTEGER, GL_INT, 8, 8, 8, 0, 0, false, false},
    {GL_RGB16UI, GL_RGB_INTEGER, GL_UNSIGNED_INT, 16, 16, 16, 0, 0, false, false},
    {GL_RGB16I, GL_RGB_INTEGER, GL_INT, 16, 16, 16, 0, 0, false, false},
    {GL_RGB32UI, GL_RGB_INTEGER, GL_UNSIGNED_INT, 32, 32, 32, 0, 0, false, false},
    {GL_RGB32I, GL_RGB_INTEGER, GL_INT, 32, 32, 32, 0, 0, false, false},
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_NORMALIZED, 8, 8, 8, 8, 0, true, true},
    {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_NORMALIZED, 8, 8, 8, 8, 0, true, true},
    {GL_RGBA8_SNORM, GL_RGBA, GL_SIGNED_NORMALIZED, 8, 8, 8, 8, 0, false, true},
    {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_NORMALIZED, 5, 5, 5, 1, 0, true, true},
    {GL_RGBA4, GL_RGBA, GL_UNSIGNED_NORMALIZED, 4, 4, 4, 4, 0, true, true},
    {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_NORMALIZED, 10, 10, 10, 2, 0, true, true},
    {GL_RGBA16F, GL_RGBA, GL_FLOAT, 16, 16, 16, 16, 0, false, true},
    {GL_RGBA32F, GL_RGBA, GL_FLOAT, 32, 32, 32, 32, 0, false, false},
    {GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 8, 8, 8, 8, 0, true, false},
    {GL_RGBA8I, GL_RGBA_INTEGER, GL_INT, 8, 8, 8, 8, 0, true, false},
    {GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 10, 10, 10, 2, 0, true, false},
    {GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 16, 16, 16, 16, 0, true, false},
    {GL_RGBA16I, GL_RGBA_INTEGER, GL_INT, 16, 16, 16, 16, 0, true, false},
    {GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, 32, 32, 32, 32, 0, true, false},
    {GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 32, 32, 32, 32, 0, true, false},
    {GL_BGRA, GL_BGRA, GL_UNSIGNED_NORMALIZED, 8, 8, 8, 8, 0, true, true},
    {GL_LUMINANCE8_ALPHA8, GL_RG, GL_UNSIGNED_NORMALIZED, 8, 8, 0, 0, 0, true, true},
    {GL_ALPHA8, GL_RED, GL_UNSIGNED_NORMALIZED, 8, 0, 0, 0, 0, true, true},
    {GL_LUMINANCE_LEGACY, GL_RED, GL_UNSIGNED_NORMALIZED, 8, 0, 0, 0, 0, true, true},
    {GL_LUMINANCE_ALPHA_LEGACY, GL_RG, GL_UNSIGNED_NORMALIZED, 8, 8, 0, 0, 0, true, true},
    {GL_RGB_LEGACY, GL_RGB, GL_UNSIGNED_NORMALIZED, 8, 8, 8, 0, 0, true, true},
    {GL_RGBA_LEGACY, GL_RGBA, GL_UNSIGNED_NORMALIZED, 8, 8, 8, 0, 0, true, true},
    {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_NORMALIZED, 16, 0, 0, 0, 0, false, false},
    {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_NORMALIZED, 24, 0, 0, 0, 0, false, false},
    {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 32, 0, 0, 0, 0, false, false},
    {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT, 24, 8, 0, 0, 0, false, false},
    {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT, 32, 8, 0, 0, 0, false, false},
};

static GLFormatInfo renderbufferFormats[] = {
    {GL_R8, GL_RED, GL_UNSIGNED_NORMALIZED, 8, 0, 0, 0, 0, true, true},
    {GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 8, 0, 0, 0, 0, true, true},
    {GL_R8I, GL_RED_INTEGER, GL_INT, 8, 0, 0, 0, 0, true, true},
    {GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 16, 0, 0, 0, 0, true, true},
    {GL_R16I, GL_RED_INTEGER, GL_INT, 16, 0, 0, 0, 0, true, true},
    {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 32, 0, 0, 0, 0, true, true},
    {GL_R32I, GL_RED_INTEGER, GL_INT, 32, 0, 0, 0, 0, true, true},
    {GL_RG8, GL_RG, GL_UNSIGNED_NORMALIZED, 8, 8, 0, 0, 0, true, true},
    {GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 8, 8, 0, 0, 0, true, true},
    {GL_RG8I, GL_RG_INTEGER, GL_INT, 8, 8, 0, 0, 0, true, true},
    {GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 16, 16, 0, 0, 0, true, true},
    {GL_RG16I, GL_RG_INTEGER, GL_INT, 16, 16, 0, 0, 0, true, true},
    {GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 32, 32, 0, 0, 0, true, true},
    {GL_RG32I, GL_RG_INTEGER, GL_INT, 32, 32, 0, 0, 0, true, true},
    {GL_RGB8, GL_RGB, GL_UNSIGNED_NORMALIZED, 8, 8, 8, 0, 0, true, true},
    {GL_RGB565, GL_RGB, GL_UNSIGNED_NORMALIZED, 5, 6, 5, 0, 0, true, true},
    {GL_RGBA8, GL_RGBA, GL_UNSIGNED_NORMALIZED, 8, 8, 8, 8, 0, true, true},
    {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_NORMALIZED, 8, 8, 8, 8, 0, true, true},
    {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_NORMALIZED, 5, 5, 5, 1, 0, true, true},
    {GL_RGBA4, GL_RGBA, GL_UNSIGNED_NORMALIZED, 4, 4, 4, 4, 0, true, true},
    {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_NORMALIZED, 10, 10, 10, 2, 0, true, true},
    {GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 8, 8, 8, 8, 0, true, true},
    {GL_RGBA8I, GL_RGBA_INTEGER, GL_INT, 8, 8, 8, 8, 0, true, true},
    {GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 10, 10, 10, 2, 0, true, true},
    {GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 16, 16, 16, 16, 0, true, true},
    {GL_RGBA16I, GL_RGBA_INTEGER, GL_INT, 16, 16, 16, 16, 0, true, true},
    {GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, 32, 32, 32, 32, 0, true, true},
    {GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 32, 32, 32, 32, 0, true, true},
    {GL_R16F, GL_RED, GL_FLOAT, 16, 0, 0, 0, 0, false, false},
    {GL_RG16F, GL_RG, GL_FLOAT, 16, 16, 0, 0, 0, false, false},
    {GL_RGBA16F, GL_RGBA, GL_FLOAT, 16, 16, 16, 16, 0, false, false},
    {GL_R32F, GL_RED, GL_FLOAT, 32, 0, 0, 0, 0, false, false},
    {GL_RG32F, GL_RG, GL_FLOAT, 32, 32, 0, 0, 0, false, false},
    {GL_RGBA32F, GL_RGBA, GL_FLOAT, 32, 32, 32, 32, 0, false, false},
    {GL_R11F_G11F_B10F, GL_RGB, GL_FLOAT, 11, 11, 10, 0, 0, false, false},
    {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_NORMALIZED, 16, 0, 0, 0, 0, false, false},
    {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_NORMALIZED, 24, 0, 0, 0, 0, false, false},
    {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 32, 0, 0, 0, 0, false, false},
    {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT, 24, 8, 0, 0, 0, false, false},
    {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT, 32, 8, 0, 0, 0, false, false},
    {GL_STENCIL_INDEX8, GL_STENCIL, GL_UNSIGNED_NORMALIZED, 0, 8, 0, 0, 0, false, false},
};

/* Farbpuffer mit Gleitkommaformaten: ES 3.0 kennt sie nicht als Renderziel (daher false in den Tabellen),
 * ES 3.2 und ES 3.0 mit GL_EXT_color_buffer_float schon (16 und 32 Bit, R11F_G11F_B10F),
 * GL_EXT_color_buffer_half_float nur die 16-Bit-Formate (dort auch RGB16F). Mischen in 32-Bit-Gleitkomma
 * braucht zusätzlich GL_EXT_float_blend. wined3d fragt das über ARB_internalformat_query2 ab; vorher
 * galten alle Gleitkommaformate als nicht renderbar, und D3DFMT_A16B16G16R16F war kein Renderziel –
 * Spiele mit der Unreal Engine 3 (Outlast) brechen dann mit „does not support alpha blending with
 * floating point render targets“ ab. ES 2.0 (Mali-400) hat keine Gleitkomma-Renderziele. */
static struct {
    bool detected;
    bool colorBufferFloat;
    bool colorBufferHalfFloat;
    bool floatBlend;
} floatCaps;

static bool hasExtension(const char* extensions, const char* name) {
    if (!extensions) return false;
    size_t len = strlen(name);
    for (const char* ptr = extensions; (ptr = strstr(ptr, name)); ptr += len) {
        if ((ptr == extensions || ptr[-1] == ' ') && (ptr[len] == '\0' || ptr[len] == ' ')) return true;
    }
    return false;
}

static void detectFloatCaps(void) {
    if (floatCaps.detected) return;
    floatCaps.detected = true;
    if (GLES2) return;

    const char* extensions = getenv("GLADIO_GLES_EXTENSIONS");
    if (!extensions) extensions = (const char*)glGetString(GL_EXTENSIONS);
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    while (glGetError() != GL_NO_ERROR);

    bool es32 = major > 3 || (major == 3 && minor >= 2);
    floatCaps.colorBufferFloat = es32 || hasExtension(extensions, "GL_EXT_color_buffer_float");
    floatCaps.colorBufferHalfFloat = hasExtension(extensions, "GL_EXT_color_buffer_half_float");
    floatCaps.floatBlend = hasExtension(extensions, "GL_EXT_float_blend");
    println("gladio: Gleitkomma-Renderziele: ES %d.%d, 16/32 Bit %s, nur 16 Bit %s, Mischen 32 Bit %s", major, minor,
            floatCaps.colorBufferFloat ? "ja" : "nein", floatCaps.colorBufferHalfFloat ? "ja" : "nein", floatCaps.floatBlend ? "ja" : "nein");
}

/* liefert für Gleitkommaformate, ob der Treiber in sie rendern (renderable) bzw. dabei mischen kann (blend);
 * false für alle anderen Formate (deren Angabe steht in der Tabelle) */
static bool floatColorRenderable(GLenum internalformat, bool blend) {
    switch (internalformat) {
        case GL_R16F:
        case GL_RG16F:
        case GL_RGBA16F:
            detectFloatCaps();
            return floatCaps.colorBufferFloat || floatCaps.colorBufferHalfFloat;
        case GL_RGB16F:
            detectFloatCaps();
            return floatCaps.colorBufferHalfFloat;
        case GL_R11F_G11F_B10F:
            detectFloatCaps();
            return floatCaps.colorBufferFloat;
        case GL_R32F:
        case GL_RG32F:
        case GL_RGBA32F:
            detectFloatCaps();
            return floatCaps.colorBufferFloat && (!blend || floatCaps.floatBlend);
        default:
            return false;
    }
}

/* Formate, die nicht in der Tabelle stehen, aber als Textur mit Filterung funktionieren:
 * komprimierte (werden entpackt) und die alten Größenvarianten von LUMINANCE/INTENSITY/ALPHA/RGB(A). */
static bool isFilterableLegacyFormat(GLenum internalformat) {
    if (isCompressedFormat(internalformat)) return true;
    switch (internalformat) {
        case GL_LUMINANCE4: case GL_LUMINANCE8: case GL_LUMINANCE12: case GL_LUMINANCE16:
        case GL_LUMINANCE4_ALPHA4: case GL_LUMINANCE6_ALPHA2: case GL_LUMINANCE12_ALPHA4: case GL_LUMINANCE12_ALPHA12: case GL_LUMINANCE16_ALPHA16:
        case GL_INTENSITY: case GL_INTENSITY4: case GL_INTENSITY8: case GL_INTENSITY12: case GL_INTENSITY16:
        case GL_ALPHA4: case GL_ALPHA12: case GL_ALPHA16:
        case GL_R3_G3_B2: case GL_RGB4: case GL_RGB5: case GL_RGB10: case GL_RGB12: case GL_RGB16:
        case GL_RGBA2: case GL_RGBA12: case GL_RGBA16:
        case GL_SLUMINANCE8: case GL_SLUMINANCE8_ALPHA8:
            return true;
        default:
            return false;
    }
}

GLFormatInfo* GLFormats_queryInternalformat(GLenum target, GLenum internalformat, GLenum pname, int count, int* params) {
    target = parseTexTarget(target);
    int result = GL_NONE;

    GLFormatInfo* targetFormat = NULL;
    if (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP) {
        for (int i = 0; i < ARRAY_SIZE(textureFormats); i++) {
            if (textureFormats[i].internalformat == internalformat) {
                targetFormat = &textureFormats[i];
                break;
            }
        }
    }
    else if (target == GL_RENDERBUFFER) {
        for (int i = 0; i < ARRAY_SIZE(renderbufferFormats); i++) {
            if (renderbufferFormats[i].internalformat == internalformat) {
                targetFormat = &renderbufferFormats[i];
                break;
            }
        }
    }

    if (!targetFormat && (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP) && isFilterableLegacyFormat(internalformat)) {
        /* wined3d fragt FILTER über ARB_internalformat_query2 ab und tastet sonst nur noch
         * punktweise (DXT-Texturen, L8): diese Formate landen beim Server als 8-Bit-Texturen */
        switch (pname) {
            case GL_VERTEX_TEXTURE:
            case GL_FRAGMENT_TEXTURE:
            case GL_FILTER:
                result = GL_FULL_SUPPORT;
                break;
            default:
                result = GL_NONE;
                break;
        }
        if (params) *params = result;
        return NULL;
    }

    if (targetFormat) {
        switch (pname) {
            case GL_VERTEX_TEXTURE:
            case GL_FRAGMENT_TEXTURE:
            case GL_SHADER_IMAGE_STORE:
            case GL_SRGB_READ:
            case GL_SRGB_WRITE:
                result = GL_FULL_SUPPORT;
                break;
            case GL_FILTER:
                if (targetFormat->textureFilterable) result = GL_FULL_SUPPORT;
                break;
            case GL_FRAMEBUFFER_RENDERABLE:
            case GL_FRAMEBUFFER_BLEND:
                if (targetFormat->colorRenderable || floatColorRenderable(internalformat, pname == GL_FRAMEBUFFER_BLEND)) result = GL_FULL_SUPPORT;
                break;
            case GL_INTERNALFORMAT_RED_SIZE:
                result = targetFormat->redSize;
                break;
            case GL_INTERNALFORMAT_GREEN_SIZE:
                result = targetFormat->greenSize;
                break;
            case GL_INTERNALFORMAT_BLUE_SIZE:
                result = targetFormat->blueSize;
                break;
            case GL_INTERNALFORMAT_ALPHA_SIZE:
                result = targetFormat->alphaSize;
                break;
            case GL_INTERNALFORMAT_SHARED_SIZE:
                result = targetFormat->sharedSize;
                break;
            case GL_INTERNALFORMAT_DEPTH_SIZE:
                result = targetFormat->depthSize;
                break;
            case GL_INTERNALFORMAT_STENCIL_SIZE:
                result = targetFormat->stencilSize;
                break;
            case GL_INTERNALFORMAT_RED_TYPE:
                result = targetFormat->redSize > 0 ? targetFormat->componentType : 0;
                break;
            case GL_INTERNALFORMAT_GREEN_TYPE:
                result = targetFormat->greenSize > 0 ? targetFormat->componentType : 0;
                break;
            case GL_INTERNALFORMAT_BLUE_TYPE:
                result = targetFormat->blueSize > 0 ? targetFormat->componentType : 0;
                break;
            case GL_INTERNALFORMAT_ALPHA_TYPE:
                result = targetFormat->alphaSize > 0 ? targetFormat->componentType : 0;
                break;
            case GL_INTERNALFORMAT_DEPTH_TYPE:
                result = targetFormat->depthSize > 0 ? targetFormat->componentType : 0;
                break;
            case GL_INTERNALFORMAT_STENCIL_TYPE:
                result = targetFormat->stencilSize > 0 ? GL_UNSIGNED_NORMALIZED : 0;
                break;
            case GL_NUM_SAMPLE_COUNTS:
            case GL_SAMPLES:
                result = 0;
                break;
            default:
                if (pname > 0) println("gladio:queryInternalformat: unimplemented pname %x", pname);
                break;
        }
    }

    if (params) *params = result;
    return targetFormat;
}