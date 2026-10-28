#ifndef GLADIO_GL_CONTEXT_H
#define GLADIO_GL_CONTEXT_H

#include "gladio.h"
#include "gl_renderer.h"
#include "gl_texture.h"
#include <time.h>

static inline int64_t gladio_nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Ausgegebene Zeichenstapel (GLRenderer_drawImmediate), für die Laufstatistik */
typedef struct GladioBatchStats {
    uint64_t batches, driverDraws;
    int64_t flushNs, setupNs, driverDrawNs;
} GladioBatchStats;

typedef struct GLXContext {
    EGLContext eglContext;
    EGLSurface eglSurface; /* 1x1-Pbuffer, wenn EGL keine oberflächenlosen Kontexte kann; sonst EGL_NO_SURFACE */
    GLRenderer renderer;
} GLXContext;

#define DISPLAY_TEXTURE_COUNT 3

typedef struct GLContext {
    GLXContext* glxContext;
    int clientFd;
    RingBuffer* clientRing;
    RingBuffer* serverRing;
    JMethods jmethods;

    pthread_t requestHandlerThread;
    bool running;
    ArrayBuffer inputBuffer;
    ArrayBuffer outputBuffer;

    ThreadPool* threadPool;

    struct {
        GLuint renderbuffer;
        GLuint texture;
        /* ES 2.0: drei Farbtexturen im Wechsel; die fertige zeigt das Fenster direkt an
         * (glCopyTexImage2D kostet auf der Mali-400 ~300 ms je Bild), die nächste wird gezeichnet */
        GLuint textures[DISPLAY_TEXTURE_COUNT];
        int textureIndex;
        int presentedDrawableId;
    } displayBufAttachment;

    GLuint savedDSATarget;
    GLuint savedDSAId;

    /* Laufstatistik (alle 5 s ins Protokoll): wo die Zeit des Server-Threads bleibt */
    struct {
        int64_t periodStartNs;
        uint32_t frames, requests, draws;
        uint64_t bytes, drawBytes;
        int64_t waitNs, swapNs, drawNs;
        uint64_t repliesAtStart, texBytesAtStart, programsAtStart;
        GladioBatchStats batchAtStart;
    } stats;

    /* die letzten Anfragen (Ring), fürs Protokoll beim Ende der Verbindung – z. B. nach einem Absturz des Programms */
    struct {
        short code[64];
        int size[64];
        uint32_t count;
    } lastRequests;
} GLContext;

/* Zähler über alle Kontexte (Antworten an den Client, Texturdaten, gelinkte Programme) */
extern uint64_t gladio_statsReplies;
extern GladioBatchStats gladio_batchStats;
extern uint64_t gladio_statsTexBytes;
extern uint64_t gladio_statsPrograms;

extern GLContext* createGLContext(JNIEnv* env, jobject obj, int clientFd);
extern void destroyGLContext(JNIEnv* env, GLContext* context);
extern GLXContext* createGLXContext(int contextId, GLXContext* sharedContext);
extern void destroyGLXContext(GLXContext* glxContext);
extern void readCommandBuffer(GLContext* context);
extern void readVertexArrayElement(GLContext* context, int arrayIdx, int elementIdx);
extern bool readUnboundVertexArrays(GLContext* context, GLenum drawMode, int drawCount, void** outIndices, GLenum indexType);
/* GL_BGRA-Attribute aus Puffern vor dem Zeichnen umstellen (Eckpunkte [first, first + count) bzw. die Indizes) */
extern void GLContext_prepareBgraArrays(GLint first, GLsizei count);
extern void GLContext_prepareBgraArraysIndexed(GLsizei count, GLenum type, const void* indices, GLint basevertex);
extern const char* getGLExtensions(int* numExtensions);

extern pthread_mutex_t glx_context_mutex;

#define GLX_CONTEXT_LOCK() pthread_mutex_lock(&glx_context_mutex)
#define GLX_CONTEXT_UNLOCK() pthread_mutex_unlock(&glx_context_mutex)

#endif