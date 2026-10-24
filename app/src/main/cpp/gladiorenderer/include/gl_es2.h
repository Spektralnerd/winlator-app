/*
 * GLES-2.0-Pfad des Gladio-Servers (armv7-Variante, z. B. Mali-400 im Galaxy Note 8.0).
 *
 * Der Server ist für OpenGL ES 3.2 geschrieben. Auf Geräten, die nur ES 2.0 können, wird
 * `gles.es2` gesetzt und die betroffenen Stellen weichen aus:
 *   - ES-2.0-Kontext und -Config, unsized Texturformate, Tiefe/Stencil als zwei Anhänge,
 *   - kein glDrawBuffers/glReadBuffer/glBlitFramebuffer/MapBufferRange, VAOs über
 *     OES_vertex_array_object (falls vorhanden),
 *   - Shader in GLSL ES 1.00 statt 3.20 (shader_material.c, shader_converter.c),
 *   - dem Client wird OpenGL 2.1 / GLSL 1.20 mit passender Erweiterungsliste gemeldet,
 *     damit wined3d den alten GLSL-Pfad (attribute/varying, gl_TexCoord, gl_FragData) nutzt.
 *
 * Erkennung: Umgebungsvariable GLADIO_GLES=2|3 erzwingt; sonst ES 2.0, wenn EGL keine
 * ES-3-Config liefert.
 */
#ifndef GLADIO_GL_ES2_H
#define GLADIO_GL_ES2_H

#include "gladio.h"

typedef struct GLESCaps {
    bool detected;
    bool es2;
    bool contextInit;

    /* Erweiterungen des ES-2.0-Treibers */
    bool vertexArrayObject;   /* GL_OES_vertex_array_object */
    bool mapBuffer;           /* GL_OES_mapbuffer */
    bool packedDepthStencil;  /* GL_OES_packed_depth_stencil */
    bool depthTexture;        /* GL_OES_depth_texture */
    bool depth24;             /* GL_OES_depth24 */
    bool rgb8rgba8;           /* GL_OES_rgb8_rgba8 oder GL_ARM_rgba8 */
    bool bgra8888;            /* GL_EXT_texture_format_BGRA8888 */
    bool elementIndexUint;    /* GL_OES_element_index_uint */
    bool textureNpot;         /* GL_OES_texture_npot */
    bool standardDerivatives; /* GL_OES_standard_derivatives */
    bool shaderTextureLod;    /* GL_EXT_shader_texture_lod */
    bool halfFloatVertex;     /* GL_OES_vertex_half_float */
    bool blendMinmax;         /* GL_EXT_blend_minmax */
    bool fragDepth;           /* GL_EXT_frag_depth */
    bool anisotropic;         /* GL_EXT_texture_filter_anisotropic */
    bool textureFloat;        /* GL_OES_texture_float */
    bool textureHalfFloat;    /* GL_OES_texture_half_float */
    bool fragmentHighp;       /* highp im Fragment-Shader verfügbar */
    bool unpackSubimage;      /* GL_EXT_unpack_subimage (UNPACK_ROW_LENGTH) */

    int maxVaryingVectors;
    int maxVertexUniformVectors;
    int maxFragmentUniformVectors;
    int maxVertexAttribs;
    int maxTextureUnits;
    int maxVertexTextureUnits;
    int maxTextureSize;
    int maxCubeMapSize;
    int maxRenderbufferSize;

    /* Zustand, den ES 2.0 nicht selbst führt */
    int unpackRowLength;
    int unpackSkipPixels;
    int unpackSkipRows;

    void (*glBindVertexArrayOES)(GLuint array);
    void (*glDeleteVertexArraysOES)(GLsizei n, const GLuint* arrays);
    void (*glGenVertexArraysOES)(GLsizei n, GLuint* arrays);
    GLboolean (*glIsVertexArrayOES)(GLuint array);
    void* (*glMapBufferOES)(GLenum target, GLenum access);
    GLboolean (*glUnmapBufferOES)(GLenum target);
} GLESCaps;

extern GLESCaps gles;

#define GLES2 (gles.es2)

/* Entscheidet vor dem ersten Kontext, ob der ES-2.0-Pfad genutzt wird. */
extern void GLES_detect(EGLDisplay eglDisplay);
/* Liest Erweiterungen und Grenzen; der Kontext muss aktuell sein. */
extern void GLES_initOnContext(void);
extern const char* GLES_versionString(void);
extern const char* GLES_shadingLanguageVersionString(void);
extern int GLES_majorVersion(void);
extern int GLES_minorVersion(void);
extern bool GLES_isCapabilitySupported(GLenum cap);
/* Bildet Typen der Vertexattribute auf ES 2.0 ab (z. B. GL_HALF_FLOAT -> GL_HALF_FLOAT_OES). */
extern GLenum GLES_vertexAttribType(GLenum type);
/* Erweiterungsliste, die der Server dem Client meldet. */
extern const char* GLES_clientExtensions(int* outNumExtensions);

/* Texturformate für ES 2.0 (unsized); liefert false, wenn das Format nicht abgebildet werden kann. */
extern bool GLES_convertTexImageFormat(uint32_t target, int* internalformat, uint32_t* format, uint32_t* type, void** imageData, int* imageSize, bool* ownsData);
extern GLenum GLES_renderbufferFormat(GLenum internalformat);
/* Packt Zeilen mit UNPACK_ROW_LENGTH eng zusammen; liefert einen neuen Puffer oder NULL. */
extern void* GLES_repackRows(const void* pixels, int width, int height, int bytesPerPixel);
/* glTexImage2D/glTexSubImage2D mit Formatumsetzung und Umpacken; tight = Daten ohne UNPACK_ROW_LENGTH. */
extern void GLES_texImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, void* data, int dataSize, bool tight);
extern void GLES_texSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, void* data, int dataSize, bool tight);
/* glDrawElements mit Basisvertex und 32-Bit-Indizes ohne OES_element_index_uint. */
extern void GLES_drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint basevertex);

#endif
