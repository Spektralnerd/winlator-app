#ifndef GLADIO_GL_VAO_H
#define GLADIO_GL_VAO_H

#include "gladio.h"
#include "gl_buffer.h"

#define POSITION_ARRAY_INDEX 0
#define COLOR_ARRAY_INDEX 1
#define NORMAL_ARRAY_INDEX 2
#define TEXCOORD_ARRAY_INDEX 3
#define GENERIC_VERTEX_ARRAY_INDEX (TEXCOORD_ARRAY_INDEX + MAX_TEXTURES)

#define GL_GENERIC_VERTEX_ARRAY 0x9500

#define VERTEX_ATTRIB_DISABLED 0
#define VERTEX_ATTRIB_LEGACY_ENABLED 1
#define VERTEX_ATTRIB_ENABLED 2
/* nur Server: GL_BGRA-Attribut aus einem Puffer; der Client schickt die Daten nicht mit, der Server
 * stellt die Farben vor jedem Zeichenaufruf selbst um (GLContext_prepareBgraArrays) */
#define VERTEX_ATTRIB_BGRA_BUFFER 3
/* Kennung im normalized-Byte von glVertexAttribPointer: GL_BGRA aus dem gebundenen Puffer, ohne Daten
 * beim Zeichnen (Client mit armv7/scripts/patch-gladio-client.py; ältere Clients setzen sie nie) */
#define GLADIO_VERTEX_ATTRIB_BGRA_BUFFER 0x80

typedef struct GLVertexAttrib {
#ifdef GL_SERVER
    bool normalized;
    bool bgraBuffer; /* GL_BGRA aus boundArrayBuffer (VERTEX_ATTRIB_BGRA_BUFFER), auch während abgeschaltet */
    GLuint divisor;  /* glVertexAttribDivisor: > 0 = je Instanz statt je Eckpunkt (für die BGRA-Umstellung) */
    int type;
    GLuint boundArrayBuffer;
    int size;
    /* ES 2: an GL weitergereichtes Attribut aus einem Puffer (vboMask); ist der Puffer ein
     * Streaming-Puffer, zeigt es beim Zeichnen auf die Kopie des Servers (GLES_prepareStreamingArrays) */
    struct {
        GLuint buffer;
        int location, size, type, stride;
        bool normalized;
        uintptr_t offset;
    } vbo;
#endif
    void* pointer;
    short stride;
    uint8_t state;
} GLVertexAttrib;

typedef struct GLVertexArrayObject {
    uint8_t maxEnabledAttribs;
    GLVertexAttrib attribs[VERTEX_ATTRIB_COUNT];
    GLBuffer* buffer[MAX_BUFFER_TARGETS];
    GLuint bgraBuffers[VERTEX_ATTRIB_COUNT]; /* je Attribut eigener Puffer für umgestellte BGRA-Farben */
#ifdef GL_SERVER
    uint32_t vboMask; /* Attribute mit gültigem vbo (ES 2) */
    uint32_t streamMask; /* Attribute, die für den laufenden Zeichenaufruf auf eine Streaming-Kopie zeigen (ES 2) */
#endif
} GLVertexArrayObject;

#define GL_SEND_VERTEX_ARRAY(requestCode, arrayIdx) \
    GL_CALL_LOCK(); \
    GLClientState* clientState = currentGLContext->clientState; \
    GLuint index = arrayIdx == TEXCOORD_ARRAY_INDEX ? arrayIdx + clientState->activeTexCoord : arrayIdx; \
    GLBuffer* arrayBuffer = GLBuffer_getBound(GL_ARRAY_BUFFER); \
    if (arrayBuffer) { \
        if (clientState->program || clientState->arbProgram[0]) { \
            GLVertexArrayObject_setAttribState(clientState, index, VERTEX_ATTRIB_DISABLED, true); \
            GLboolean normalized = type != GL_FLOAT && type != GL_HALF_FLOAT ? GL_TRUE : GL_FALSE; \
            glVertexAttribPointer(INT32_MAX + index, size, type, normalized, stride, pointer); \
            goto unlock; \
        } \
        pointer = arrayBuffer->mappedData + (uint64_t)pointer; \
    } \
    GLVertexArrayObject_setAttribState(clientState, index, VERTEX_ATTRIB_LEGACY_ENABLED, false); \
    clientState->vao->attribs[index].stride = stride > 0 ? stride : (size * sizeofGLType(type)); \
    clientState->vao->attribs[index].pointer = pointer; \
    ArrayBuffer_rewind(&outputBuffer); \
    ArrayBuffer_put(&outputBuffer, size); \
    ArrayBuffer_putInt(&outputBuffer, type); \
    ArrayBuffer_putInt(&outputBuffer, stride); \
    gl_send(currentGLContext->serverRing, requestCode, outputBuffer.buffer, outputBuffer.size); \
    unlock: \
    GL_CALL_UNLOCK()

#define GL_READ_VERTEX_ARRAY(arrayIdx) \
    do { \
        GLbyte size = ArrayBuffer_get(&context->inputBuffer); \
        GLenum type = ArrayBuffer_getInt(&context->inputBuffer); \
        GLsizei stride = ArrayBuffer_getInt(&context->inputBuffer); \
        GLClientState* clientState = &currentRenderer->clientState; \
        bool hasBoundProgram = clientState->program || clientState->arbProgram[0]; \
        GLVertexArrayObject_setAttribState(clientState, arrayIdx, VERTEX_ATTRIB_LEGACY_ENABLED, false); \
        clientState->vao->attribs[arrayIdx].size = size; \
        clientState->vao->attribs[arrayIdx].type = type; \
        clientState->vao->attribs[arrayIdx].normalized = type != GL_FLOAT && type != GL_HALF_FLOAT && hasBoundProgram; \
        clientState->vao->attribs[arrayIdx].stride = stride > 0 || hasBoundProgram ? stride : (size * sizeofGLType(type)); \
    } \
    while (0)

extern void GLVertexArrayObject_setAttribState(GLClientState* clientState, GLuint index, uint8_t state, bool clear);
#ifdef GL_SERVER
extern void GLVertexArrayObject_setVboSource(GLClientState* clientState, GLuint index, GLuint buffer, int location, int size, int type, bool normalized, int stride, uintptr_t offset);
#endif
extern void GLVertexArrayObject_setBound(GLClientState* clientState, GLuint id);
extern void GLVertexArrayObject_delete(GLClientState* clientState, GLuint id);

#endif