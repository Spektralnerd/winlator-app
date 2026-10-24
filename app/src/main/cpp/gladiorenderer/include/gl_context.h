#ifndef GLADIO_GL_CONTEXT_H
#define GLADIO_GL_CONTEXT_H

#include "gladio.h"
#include "gl_renderer.h"
#include "gl_texture.h"

typedef struct GLXContext {
    EGLContext eglContext;
    EGLSurface eglSurface; /* 1x1-Pbuffer, wenn EGL keine oberflächenlosen Kontexte kann; sonst EGL_NO_SURFACE */
    GLRenderer renderer;
} GLXContext;

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
    } stats;
} GLContext;

/* Zähler über alle Kontexte (Antworten an den Client, Texturdaten, gelinkte Programme) */
extern uint64_t gladio_statsReplies;
extern uint64_t gladio_statsTexBytes;
extern uint64_t gladio_statsPrograms;

extern GLContext* createGLContext(JNIEnv* env, jobject obj, int clientFd);
extern void destroyGLContext(JNIEnv* env, GLContext* context);
extern GLXContext* createGLXContext(int contextId, GLXContext* sharedContext);
extern void destroyGLXContext(GLXContext* glxContext);
extern void readCommandBuffer(GLContext* context);
extern void readVertexArrayElement(GLContext* context, int arrayIdx, int elementIdx);
extern bool readUnboundVertexArrays(GLContext* context, GLenum drawMode, int drawCount, void** outIndices, GLenum indexType);
extern const char* getGLExtensions(int* numExtensions);

extern pthread_mutex_t glx_context_mutex;

#define GLX_CONTEXT_LOCK() pthread_mutex_lock(&glx_context_mutex)
#define GLX_CONTEXT_UNLOCK() pthread_mutex_unlock(&glx_context_mutex)

#endif