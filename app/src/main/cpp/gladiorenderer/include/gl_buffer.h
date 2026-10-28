#ifndef GLADIO_GL_BUFFER_H
#define GLADIO_GL_BUFFER_H

#include "gladio.h"

typedef struct GLBuffer {
    GLuint id;
    int size;
    void* mappedData;
    /* ES 2.0: Puffer, die in einem Bild mehrfach teilweise neu beschrieben werden (wined3d-Streaming),
     * zeichnet der Server als Client-Arrays aus einer eigenen Kopie, die er in der Reihenfolge der
     * Anfragen nachführt wie sonst den GL-Puffer (gl_es2.c). Teilweises Überschreiben eines Puffers, aus
     * dem im selben Bild schon gezeichnet wurde, kostet auf der Mali-400 eine Kopie des ganzen Puffers. */
    bool streaming;
    uint16_t uploadsInFrame;
    uint32_t uploadFrame;
    uint8_t* shadow;
} GLBuffer;

extern GLBuffer* GLBuffer_getBound(GLenum target);
extern void GLBuffer_bind(GLenum target, GLuint id);
extern int GLBuffer_allocateMemory(GLBuffer* buffer, int size);
extern void GLBuffer_delete(GLuint id);
extern void GLBuffer_onDestroy(GLClientState* clientState);

#endif