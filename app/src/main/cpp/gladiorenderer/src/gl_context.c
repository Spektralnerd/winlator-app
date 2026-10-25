#include "gl_context.h"
#include "gl_renderer.h"
#include "gl_dsa.h"
#include "sysvshared_memory.h"
#include "request_handler.h"
#include "gl_es2.h"

extern EGLContext globalEGLContext;
static void makeGLXContextCurrent(EGLDisplay eglDisplay, GLXContext* context);

uint64_t gladio_statsReplies = 0;
uint64_t gladio_statsTexBytes = 0;
uint64_t gladio_statsPrograms = 0;
GladioBatchStats gladio_batchStats = {0};
uint32_t gladio_statsReplyCodes[GLADIO_REPLY_CODE_SLOTS] = {0};

static inline int64_t nowNs(void) {
    return gladio_nowNs();
}

static bool isDrawRequest(short requestCode) {
    switch (requestCode) {
        case REQUEST_CODE_GL_DRAW_ARRAYS:
        case REQUEST_CODE_GL_DRAW_ARRAYS_INSTANCED:
        case REQUEST_CODE_GL_DRAW_ELEMENTS:
        case REQUEST_CODE_GL_DRAW_ELEMENTS_BASE_VERTEX:
        case REQUEST_CODE_GL_DRAW_ELEMENTS_INSTANCED:
        case REQUEST_CODE_GL_DRAW_RANGE_ELEMENTS:
        case REQUEST_CODE_GL_MULTI_DRAW_ELEMENTS_BASE_VERTEX:
        case REQUEST_CODE_GL_END:
            return true;
        default:
            return false;
    }
}

#define STATS_PERIOD_NS 5000000000LL

static uint32_t requestCodeCounts[GLADIO_REPLY_CODE_SLOTS];
/* PC-Prüfstand (Datei "trace" im Ablageverzeichnis): die Anfragen eines Bilds protokollieren */
int gladio_traceRequests = 0;

static const char* shortRequestName(int code) {
    if (code == REQUEST_CODE_SET_CURRENT_RENDER_WINDOW) return "MAKE_CURRENT";
    if (code == REQUEST_CODE_SWAP_DISPLAY_BUFFERS) return "SWAP";
    if (code < REQUEST_CODE_GL_CALL_START) return "DSA";
    const char* name = requestCodeToString(code);
    return name[0] == 'g' && name[1] == 'l' ? name + 2 : name;
}

/* die häufigsten Anfragecodes einer Zählung, je Bild */
static void printTopRequestCodes(const char* title, const uint32_t* counts, int maxShown, uint32_t frames) {
    char line[320];
    int pos = 0;
    int shown[8];
    if (maxShown > 8) maxShown = 8;
    for (int n = 0; n < maxShown; n++) {
        int best = -1;
        for (int code = 0; code < GLADIO_REPLY_CODE_SLOTS; code++) {
            if (code == REQUEST_CODE_SWAP_DISPLAY_BUFFERS || counts[code] == 0) continue;
            bool done = false;
            for (int k = 0; k < n; k++) if (shown[k] == code) done = true;
            if (!done && (best < 0 || counts[code] > counts[best])) best = code;
        }
        if (best < 0) break;
        shown[n] = best;
        int written = snprintf(line + pos, sizeof(line) - pos, "%s%s %.1f", n > 0 ? ", " : "", shortRequestName(best), counts[best] / (double)frames);
        if (written < 0 || pos + written >= (int)sizeof(line)) break;
        pos += written;
    }
    if (pos > 0) println("gladio: %s: %s", title, line);
}

static void printStats(GLContext* context, int64_t now) {
    typeof(context->stats)* st = &context->stats;
    double seconds = (now - st->periodStartNs) / 1e9;
    uint32_t frames = st->frames ? st->frames : 1;
    int64_t execNs = (now - st->periodStartNs) - st->waitNs - st->swapNs;
    if (execNs < 0) execNs = 0;
    println("gladio: %.1f s: %u Bilder (%.1f/s); je Bild %u Anfragen, %u Antworten, %u Zeichenaufrufe, %u KB (Vertexdaten %u KB), Texturen %u KB",
            seconds, st->frames, st->frames / seconds, st->requests / frames, (uint32_t)((gladio_statsReplies - st->repliesAtStart) / frames),
            st->draws / frames, (uint32_t)(st->bytes / frames / 1024), (uint32_t)(st->drawBytes / frames / 1024),
            (uint32_t)((gladio_statsTexBytes - st->texBytesAtStart) / frames / 1024));
    println("gladio: Zeit je Bild: Warten auf den Client %.1f ms, Bildausgabe %.1f ms, Ausführen %.1f ms (davon Zeichnen %.1f ms); Shader-Programme neu %u, gesamt %u",
            st->waitNs / 1e6 / frames, st->swapNs / 1e6 / frames, execNs / 1e6 / frames, st->drawNs / 1e6 / frames,
            (uint32_t)(gladio_statsPrograms - st->programsAtStart), (uint32_t)gladio_statsPrograms);
    GladioBatchStats* bs = &gladio_batchStats;
    GladioBatchStats* b0 = &st->batchAtStart;
    println("gladio: Stapel je Bild: %u ausgegeben mit %u Treiber-Zeichenaufrufen in %.1f ms (Shader, Uniforms, Vertexzeiger %.1f ms; Treiber-Zeichnen %.1f ms)",
            (uint32_t)((bs->batches - b0->batches) / frames), (uint32_t)((bs->driverDraws - b0->driverDraws) / frames),
            (bs->flushNs - b0->flushNs) / 1e6 / frames, (bs->setupNs - b0->setupNs) / 1e6 / frames, (bs->driverDrawNs - b0->driverDrawNs) / 1e6 / frames);

    /* welche Anfragen auf eine Antwort warten lassen (der Client steht so lange) und welche am häufigsten kommen */
    printTopRequestCodes("Antworten je Bild außer SWAP", gladio_statsReplyCodes, 4, frames);
    printTopRequestCodes("häufigste Anfragen je Bild", requestCodeCounts, 8, frames);
}

static void resetStats(GLContext* context, int64_t now) {
    typeof(context->stats)* st = &context->stats;
    memset(st, 0, sizeof(*st));
    st->periodStartNs = now;
    st->repliesAtStart = gladio_statsReplies;
    st->texBytesAtStart = gladio_statsTexBytes;
    st->programsAtStart = gladio_statsPrograms;
    st->batchAtStart = gladio_batchStats;
    memset(gladio_statsReplyCodes, 0, sizeof(gladio_statsReplyCodes));
    memset(requestCodeCounts, 0, sizeof(requestCodeCounts));
}

pthread_mutex_t glx_context_mutex = PTHREAD_MUTEX_INITIALIZER;

static void loadJMethods(JMethods* jmethods) {
    JNIEnv* env;
    (*jmethods->jvm)->AttachCurrentThread(jmethods->jvm, &env, NULL);
    jmethods->env = env;

    jclass cls = (*env)->GetObjectClass(env, jmethods->obj);
    jmethods->getWindowSize = (*env)->GetMethodID(env, cls, "getWindowSize", "(I)[S");
    jmethods->clearWindowContent = (*env)->GetMethodID(env, cls, "clearWindowContent", "(I)V");
    jmethods->updateWindowContent = (*env)->GetMethodID(env, cls, "updateWindowContent", "(ISSZ)Z");
    jmethods->getGLXContextPtr = (*env)->GetMethodID(env, cls, "getGLXContextPtr", "(II)J");
    jmethods->presentWindowTexture = (*env)->GetMethodID(env, cls, "presentWindowTexture", "(ISSI)Z");
    jmethods->releaseWindowTexture = (*env)->GetMethodID(env, cls, "releaseWindowTexture", "(I)V");
}

static void getWindowSize(JMethods* jmethods, int windowId, short* outWidth, short* outHeight) {
    jshortArray windowSize = (*jmethods->env)->CallObjectMethod(jmethods->env, jmethods->obj, jmethods->getWindowSize, windowId);
    jshort* windowSizePtr = (*jmethods->env)->GetShortArrayElements(jmethods->env, windowSize, 0);
    *outWidth = windowSizePtr[0];
    *outHeight = windowSizePtr[1];
    (*jmethods->env)->ReleaseShortArrayElements(jmethods->env, windowSize, windowSizePtr, JNI_ABORT);
}

static void createDisplayBuffer(GLContext* context) {
    currentRenderer->displayBuffer = GLFramebuffer_create();
    GLFramebuffer_bind(GL_FRAMEBUFFER, currentRenderer->displayBuffer);
    GLFramebuffer_setAttachment(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, context->displayBufAttachment.texture, 0, NULL);
    GLFramebuffer_setAttachment(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, context->displayBufAttachment.renderbuffer, 0, NULL);
    if (GLES2) {
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) println("gladio:es2: Anzeigepuffer unvollständig (0x%x)", status);
    }
}

static void destroyDisplayBuffer() {
    if (currentRenderer && currentRenderer->displayBuffer > 0) {
        GLFramebuffer_delete(currentRenderer->displayBuffer);
        currentRenderer->displayBuffer = 0;
    }
}

static GLuint createDisplayTexture(short width, short height) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GLES2 ? GL_RGBA : PREFERRED_FRAMEBUFFER_FORMAT, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

static void createDisplayBufAttachment(GLContext* context) {
    short width = currentRenderer->displaySize[0];
    short height = currentRenderer->displaySize[1];

    if (context->displayBufAttachment.texture == 0) {
        if (GLES2) {
            for (int i = 0; i < DISPLAY_TEXTURE_COUNT; i++) context->displayBufAttachment.textures[i] = createDisplayTexture(width, height);
            context->displayBufAttachment.textureIndex = 0;
            context->displayBufAttachment.texture = context->displayBufAttachment.textures[0];
        }
        else context->displayBufAttachment.texture = createDisplayTexture(width, height);
    }

    if (context->displayBufAttachment.renderbuffer == 0) {
        glGenRenderbuffers(1, &context->displayBufAttachment.renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, context->displayBufAttachment.renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GLES2 ? GLES_renderbufferFormat(PREFERRED_RENDERBUFFER_FORMAT) : PREFERRED_RENDERBUFFER_FORMAT, width, height);
        if (GLES2) GLFramebuffer_noteRenderbufferSize(context->displayBufAttachment.renderbuffer, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
}

static void destroyDisplayBufAttachment(GLContext* context) {
    if (context->displayBufAttachment.texture > 0) {
        if (GLES2) {
            /* das Fenster darf die Textur nicht mehr zeigen, sobald sie weg ist */
            if (context->displayBufAttachment.presentedDrawableId) {
                JMethods* jmethods = &context->jmethods;
                (*jmethods->env)->CallVoidMethod(jmethods->env, jmethods->obj, jmethods->releaseWindowTexture, context->displayBufAttachment.presentedDrawableId);
                context->displayBufAttachment.presentedDrawableId = 0;
            }
            glDeleteTextures(DISPLAY_TEXTURE_COUNT, context->displayBufAttachment.textures);
            memset(context->displayBufAttachment.textures, 0, sizeof(context->displayBufAttachment.textures));
        }
        else glDeleteTextures(1, &context->displayBufAttachment.texture);
        context->displayBufAttachment.texture = 0;
    }

    if (context->displayBufAttachment.renderbuffer > 0) {
        glDeleteRenderbuffers(1, &context->displayBufAttachment.renderbuffer);
        context->displayBufAttachment.renderbuffer = 0;
    }
}

static void setCurrentRenderWindow(GLContext* context, int windowId) {
    if (windowId == 0) return;
    JMethods* jmethods = &context->jmethods;
    (*jmethods->env)->CallVoidMethod(jmethods->env, jmethods->obj, jmethods->clearWindowContent, windowId);
    if (currentRenderer->displayBuffer > 0) return;

    short width;
    short height;
    getWindowSize(&context->jmethods, windowId, &width, &height);
    if (width == 0 || height == 0) return;

    bool resized = currentRenderer->displaySize[0] != width || currentRenderer->displaySize[1] != height;
    currentRenderer->displaySize[0] = width;
    currentRenderer->displaySize[1] = height;

    if (resized) destroyDisplayBufAttachment(context);
    createDisplayBufAttachment(context);
    createDisplayBuffer(context);

    ARRAYS_FILL(currentRenderer->clientState.framebuffer, MAX_FRAMEBUFFER_TARGETS, 0);
    GLRenderer_setDrawBuffer(currentRenderer, GL_BACK);

    GLTexture* texture = GLTexture_getBound(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture ? texture->id : 0);

    glViewport(0, 0, width, height);
    glScissor(0, 0, width, height);
}

/*
 * ES 2.0: Das Fenster zeigt die gerade fertig gezeichnete Farbtextur des Anzeigepuffers direkt
 * (X-Server-Renderer und GLX-Kontexte teilen sich die Texturen); der Server zeichnet weiter in
 * die nächste von drei Texturen. Spart das Kopieren mit glCopyTexImage2D, das auf der Mali-400
 * über den Prozessor läuft (~300 ms je Bild bei 1024x768).
 */
static bool presentDisplayTexture(GLContext* context, int drawableId) {
    typeof(context->displayBufAttachment)* att = &context->displayBufAttachment;
    glFlush(); /* Zeichenauftrag abschicken, bevor der Renderer-Kontext die Textur liest */

    JMethods* jmethods = &context->jmethods;
    bool result = (*jmethods->env)->CallBooleanMethod(jmethods->env, jmethods->obj, jmethods->presentWindowTexture, drawableId, currentRenderer->displaySize[0], currentRenderer->displaySize[1], att->textures[att->textureIndex]);
    if (!result) return false;
    att->presentedDrawableId = drawableId;

    att->textureIndex = (att->textureIndex + 1) % DISPLAY_TEXTURE_COUNT;
    att->texture = att->textures[att->textureIndex];

    /* nächste Textur an den Anzeigepuffer hängen; Bindung der Anwendung danach wiederherstellen */
    GLuint* slots = currentRenderer->clientState.framebuffer;
    GLuint savedDraw = slots[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)];
    GLuint savedRead = slots[indexOfGLTarget(GL_READ_FRAMEBUFFER)];
    GLFramebuffer_bind(GL_FRAMEBUFFER, currentRenderer->displayBuffer);
    GLFramebuffer_setAttachment(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, att->texture, 0, NULL);
    if (savedDraw != currentRenderer->displayBuffer) GLFramebuffer_bind(GL_DRAW_FRAMEBUFFER, savedDraw);
    if (savedRead != savedDraw) GLFramebuffer_bind(GL_READ_FRAMEBUFFER, savedRead);
    return true;
}

static void swapDisplayBuffers(GLContext* context, int drawableId) {
    bool result;
    if (GLES2) {
        result = presentDisplayTexture(context, drawableId);
    }
    else {
        GLuint framebuffer = currentRenderer->clientState.framebuffer[indexOfGLTarget(GL_FRAMEBUFFER)];
        GLuint drawFramebuffer = currentRenderer->clientState.framebuffer[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)];
        if (framebuffer != drawFramebuffer) GLFramebuffer_bind(GL_FRAMEBUFFER, drawFramebuffer);

        JMethods* jmethods = &context->jmethods;
        result = (*jmethods->env)->CallBooleanMethod(jmethods->env, jmethods->obj, jmethods->updateWindowContent, drawableId, currentRenderer->displaySize[0], currentRenderer->displaySize[1], JNI_TRUE);
    }

    if (result) {
        GLTexture* texture = GLTexture_getBound(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture ? texture->id : 0);
    }
    else {
        destroyDisplayBuffer();
        destroyDisplayBufAttachment(context);
        setCurrentRenderWindow(context, drawableId);
    }
}

static bool isCanDrawImmediate(short requestCode) {
    return requestCode == REQUEST_CODE_GL_BEGIN ||
           requestCode == REQUEST_CODE_GL_END ||
           requestCode == REQUEST_CODE_GL_DRAW_ARRAYS ||
           requestCode == REQUEST_CODE_GL_DRAW_ELEMENTS ||
           requestCode == REQUEST_CODE_GL_ENABLE_CLIENT_STATE ||
           requestCode == REQUEST_CODE_GL_DISABLE_CLIENT_STATE ||
           requestCode == REQUEST_CODE_GL_VERTEX_POINTER ||
           requestCode == REQUEST_CODE_GL_COLOR_POINTER ||
           requestCode == REQUEST_CODE_GL_NORMAL_POINTER ||
           requestCode == REQUEST_CODE_GL_TEX_COORD_POINTER ||
           requestCode == REQUEST_CODE_GL_VERTEX4F ||
           requestCode == REQUEST_CODE_GL_COLOR4F ||
           requestCode == REQUEST_CODE_GL_NORMAL3F ||
           requestCode == REQUEST_CODE_GL_TEX_COORD4F ||
           requestCode == REQUEST_CODE_GL_MULTI_TEX_COORD4F ||
           requestCode == REQUEST_CODE_GL_ARRAY_ELEMENT ? false : true;
}

/* Beim Ende der Verbindung: die letzten Anfragen des Programms (nach einem Absturz des Programms
 * zeigt das, womit es zuletzt beschäftigt war); gleiche Anfragen hintereinander werden gezählt */
static void printLastRequests(GLContext* context) {
    typeof(context->lastRequests)* lr = &context->lastRequests;
    uint32_t n = lr->count < 64 ? lr->count : 64;
    if (n == 0) return;
    char line[1024];
    int pos = 0;
    uint32_t first = lr->count - n;
    for (uint32_t i = first; i < lr->count; ) {
        short code = lr->code[i % 64];
        int size = lr->size[i % 64];
        uint32_t repeat = 1;
        while (i + repeat < lr->count && lr->code[(i + repeat) % 64] == code) repeat++;
        const char* name;
        if (code == REQUEST_CODE_SWAP_DISPLAY_BUFFERS) name = "SWAP";
        else if (code == REQUEST_CODE_SET_CURRENT_RENDER_WINDOW) name = "MAKE_CURRENT";
        else if (code >= REQUEST_CODE_GL_DSA_START && code < REQUEST_CODE_GL_CALL_START) name = "DSA";
        else {
            name = requestCodeToString(code);
            if (name[0] == 'g' && name[1] == 'l') name += 2;
        }
        int written = repeat > 1 ? snprintf(line + pos, sizeof(line) - pos, "%s(%d)x%u ", name, size, repeat)
                                 : snprintf(line + pos, sizeof(line) - pos, "%s(%d) ", name, size);
        if (written < 0 || pos + written >= (int)sizeof(line) - 1) {
            println("gladio: letzte Anfragen: %s", line);
            pos = 0;
            continue;
        }
        pos += written;
        i += repeat;
    }
    if (pos > 0) println("gladio: letzte Anfragen (%u insgesamt): %s", lr->count, line);
}

static void* requestHandlerThread(void* param) {
    GLContext* context = param;
    loadJMethods(&context->jmethods);
    short requestCode;

    resetStats(context, nowNs());
    while (context->running) {
        /* Zeit messen, wenn wirklich auf den Client gewartet wird (sonst keine Uhr lesen) */
        bool blocked = RingBuffer_size(context->serverRing) < HEADER_SIZE;
        int64_t waitStart = blocked ? nowNs() : 0;
        if (!gl_recv(context->serverRing, &requestCode, &context->inputBuffer)) break;
        if (blocked) context->stats.waitNs += nowNs() - waitStart;
        context->stats.requests++;
        requestCodeCounts[(unsigned short)requestCode % GLADIO_REPLY_CODE_SLOTS]++;
        if (gladio_traceRequests) {
            int a0 = context->inputBuffer.size >= 4 ? ((int*)context->inputBuffer.buffer)[0] : 0;
            int a1 = context->inputBuffer.size >= 8 ? ((int*)context->inputBuffer.buffer)[1] : 0;
            println("TRACE %s %d 0x%x 0x%x", shortRequestName(requestCode), context->inputBuffer.size, a0, a1);
            if (requestCode == REQUEST_CODE_SWAP_DISPLAY_BUFFERS) gladio_traceRequests = 0;
        }
        context->stats.bytes += HEADER_SIZE + context->inputBuffer.size;
        context->lastRequests.code[context->lastRequests.count % 64] = requestCode;
        context->lastRequests.size[context->lastRequests.count % 64] = context->inputBuffer.size;
        context->lastRequests.count++;
        if (isDrawRequest(requestCode)) {
            context->stats.draws++;
            context->stats.drawBytes += context->inputBuffer.size;
        }

        /* Zeichnen = Stapel ausgeben (aufgeschoben bis zur nächsten Nicht-Zeichen-Anfrage) + Zeichenanfragen verarbeiten */
        bool drawing = isDrawRequest(requestCode);
        bool flushBatch = isCanDrawImmediate(requestCode) && currentRenderer && !ArrayDeque_isEmpty(&currentRenderer->meshes);
        int64_t drawStart = (drawing || flushBatch) ? nowNs() : 0;
        if (flushBatch) {
            GLRenderer_drawImmediate(currentRenderer);
            if (!drawing) {
                context->stats.drawNs += nowNs() - drawStart;
                drawStart = 0;
            }
        }

        switch (requestCode) {
            case REQUEST_CODE_SET_CURRENT_RENDER_WINDOW: {
                int windowId = ArrayBuffer_getInt(&context->inputBuffer);
                int contextId = ArrayBuffer_getInt(&context->inputBuffer);
                JMethods* jmethods = &context->jmethods;
                GLXContext* glxContext = (GLXContext*)(*jmethods->env)->CallLongMethod(jmethods->env, jmethods->obj, jmethods->getGLXContextPtr, context->clientFd, contextId);

                if (glxContext && context->glxContext != glxContext) {
                    destroyDisplayBuffer();
                    makeGLXContextCurrent(eglGetDisplay(EGL_DEFAULT_DISPLAY), glxContext);
                    context->glxContext = glxContext;
                    currentRenderer = &glxContext->renderer;
                    GLRenderer_resetFrameCount(currentRenderer);
                }

                setCurrentRenderWindow(context, windowId);
                gl_send(context->clientRing, REQUEST_CODE_SET_CURRENT_RENDER_WINDOW, NULL, 0);
                break;
            }
            case REQUEST_CODE_SWAP_DISPLAY_BUFFERS: {
                int drawableId = ArrayBuffer_getInt(&context->inputBuffer);
                currentRenderer->frameCount++;
                int64_t swapStart = nowNs();
                swapDisplayBuffers(context, drawableId);
                gl_send(context->clientRing, REQUEST_CODE_SWAP_DISPLAY_BUFFERS, NULL, 0);
                int64_t now = nowNs();
                context->stats.swapNs += now - swapStart;
                context->stats.frames++;
                if (now - context->stats.periodStartNs >= STATS_PERIOD_NS) {
                    printStats(context, now);
                    resetStats(context, now);
                }
                break;
            }
            default: {
                if (requestCode >= REQUEST_CODE_GL_DSA_START && requestCode < REQUEST_CODE_GL_CALL_START) {
                    handleDSARequest(context, requestCode);
                    break;
                }

#if IS_DEBUG_ENABLED(DEBUG_MODE_HANDLE_REQUEST)
                println("handleRequest name=%s size=%d", requestCodeToString(requestCode), context->inputBuffer.size);
#endif

                HandleRequestFunc handleRequestFunc = getHandleRequestFunc(requestCode);
                if (handleRequestFunc) handleRequestFunc(context);
                break;
            }
        }

        if (drawStart) context->stats.drawNs += nowNs() - drawStart;

#if IS_DEBUG_ENABLED(DEBUG_MODE_GL_ERROR)
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) println("gladio: glError %s %x", requestCodeToString(requestCode), error);
#endif
    }

    printLastRequests(context);
    destroyDisplayBuffer();
    destroyDisplayBufAttachment(context);
    if (context->glxContext) {
        eglMakeCurrent(eglGetDisplay(EGL_DEFAULT_DISPLAY), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        context->glxContext = NULL;
    }
    (*context->jmethods.jvm)->DetachCurrentThread(context->jmethods.jvm);
    return NULL;
}

GLContext* createGLContext(JNIEnv* env, jobject obj, int clientFd) {
    GLContext* context = calloc(1, sizeof(GLContext));
    context->clientFd = clientFd;

    int shmFds[2];
    shmFds[0] = ashmemCreateRegion("gl-server-ring", RingBuffer_getSHMemSize(SERVER_RING_BUFFER_SIZE));
    shmFds[1] = ashmemCreateRegion("gl-client-ring", RingBuffer_getSHMemSize(CLIENT_RING_BUFFER_SIZE));

    context->serverRing = RingBuffer_create(shmFds[0], SERVER_RING_BUFFER_SIZE);
    if (!context->serverRing) goto error;

    context->clientRing = RingBuffer_create(shmFds[1], CLIENT_RING_BUFFER_SIZE);
    if (!context->clientRing) goto error;

    int result = send_fds(clientFd, shmFds, 2, NULL, 0);
    if (result < 0) goto error;

    (*env)->GetJavaVM(env, &context->jmethods.jvm);
    context->jmethods.obj = (*env)->NewGlobalRef(env, obj);

    context->threadPool = ThreadPool_init(THREAD_POOL_NUM_THREADS);
    context->running = true;
    pthread_create(&context->requestHandlerThread, NULL, requestHandlerThread, context);

    CLOSEFD(shmFds[0]);
    CLOSEFD(shmFds[1]);
    return context;

error:
    CLOSEFD(shmFds[0]);
    CLOSEFD(shmFds[1]);
    MEMFREE(context);
    return NULL;
}

void destroyGLContext(JNIEnv* env, GLContext* context) {
    context->running = false;

    if (context->requestHandlerThread) {
        RingBuffer_setStatus(context->serverRing, RING_STATUS_EXIT);
        RingBuffer_setStatus(context->clientRing, RING_STATUS_EXIT);
        pthread_join(context->requestHandlerThread, NULL);

        ThreadPool_destroy(context->threadPool);
        context->threadPool = NULL;

        context->requestHandlerThread = 0;
        RingBuffer_free(context->serverRing);
        RingBuffer_free(context->clientRing);
    }

    if (context->jmethods.obj) {
        (*env)->DeleteGlobalRef(env, context->jmethods.obj);
        context->jmethods.obj = NULL;
    }

    ArrayBuffer_free(&context->inputBuffer);
    ArrayBuffer_free(&context->outputBuffer);

    free(context);
}

/* Macht den GLX-Kontext aktuell; mit seinem Pbuffer, wenn EGL keine oberflächenlosen Kontexte kann. */
static void makeGLXContextCurrent(EGLDisplay eglDisplay, GLXContext* context) {
    if (!eglMakeCurrent(eglDisplay, context->eglSurface, context->eglSurface, context->eglContext)) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            println("gladio: eglMakeCurrent fehlgeschlagen: 0x%x (Pbuffer %d)", eglGetError(), context->eglSurface != EGL_NO_SURFACE);
        }
    }
}

GLXContext* createGLXContext(int contextId, GLXContext* sharedContext) {
    static const EGLint ctxAttribListES3[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    /* ES-2.0-Pfad (z. B. Mali-400): Config ohne ES3-Bit, Kontext der Version 2 */
    static const EGLint ctxAttribListES2[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    EGLBoolean success;

    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglDisplay) return NULL;

    EGLint major, minor;
    success = eglInitialize(eglDisplay, &major, &minor);
    if (!success) return NULL;

    GLES_detect(eglDisplay);
    const EGLint* ctxAttribList = GLES2 ? ctxAttribListES2 : ctxAttribListES3;

    /* Ohne EGL_KHR_surfaceless_context braucht die Config Pbuffer-Unterstützung. */
    EGLint confAttribList[16];
    int n = 0;
    confAttribList[n++] = EGL_RENDERABLE_TYPE; confAttribList[n++] = GLES2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES3_BIT;
    confAttribList[n++] = EGL_RED_SIZE; confAttribList[n++] = 8;
    confAttribList[n++] = EGL_GREEN_SIZE; confAttribList[n++] = 8;
    confAttribList[n++] = EGL_BLUE_SIZE; confAttribList[n++] = 8;
    confAttribList[n++] = EGL_ALPHA_SIZE; confAttribList[n++] = 8;
    if (!gles.surfaceless) { confAttribList[n++] = EGL_SURFACE_TYPE; confAttribList[n++] = EGL_PBUFFER_BIT; }
    confAttribList[n++] = EGL_NONE;

    int numConfigs;
    EGLConfig eglConfig;
    success = eglChooseConfig(eglDisplay, confAttribList, &eglConfig, 1, &numConfigs);
    if (!success || numConfigs != 1) {
        println("gladio: eglChooseConfig (ES %d) fehlgeschlagen: 0x%x", GLES2 ? 2 : 3, eglGetError());
        return NULL;
    }

    EGLContext eglContext = eglCreateContext(eglDisplay, eglConfig, sharedContext ? sharedContext->eglContext : globalEGLContext, ctxAttribList);
    if (eglContext == EGL_NO_CONTEXT) {
        println("gladio: eglCreateContext (ES %d) fehlgeschlagen: 0x%x", GLES2 ? 2 : 3, eglGetError());
        return NULL;
    }

    EGLSurface eglSurface = EGL_NO_SURFACE;
    if (!gles.surfaceless) {
        static const EGLint pbufferAttribList[] = {
            EGL_WIDTH, 1,
            EGL_HEIGHT, 1,
            EGL_NONE
        };
        eglSurface = eglCreatePbufferSurface(eglDisplay, eglConfig, pbufferAttribList);
        if (eglSurface == EGL_NO_SURFACE) println("gladio: eglCreatePbufferSurface fehlgeschlagen: 0x%x", eglGetError());
    }

    GLXContext* context = calloc(1, sizeof(GLXContext));
    context->eglContext = eglContext;
    context->eglSurface = eglSurface;
    context->renderer.contextId = contextId;
    GLVertexArrayObject_setBound(&context->renderer.clientState, 0);
    GLClientState_init(&context->renderer.clientState, sharedContext ? &sharedContext->renderer.clientState : NULL);

    GLX_CONTEXT_LOCK();
    makeGLXContextCurrent(eglDisplay, context);
    GLES_initOnContext();
    GLRenderer_initOnEGLContext(&context->renderer);
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    GLX_CONTEXT_UNLOCK();
    return context;
}

void destroyGLXContext(GLXContext* context) {
    GLX_CONTEXT_LOCK();
    GLClientState_destroy(&context->renderer.clientState);
    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    makeGLXContextCurrent(eglDisplay, context);
    GLRenderer_destroy(&context->renderer);
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    GLX_CONTEXT_UNLOCK();
    eglDestroyContext(eglDisplay, context->eglContext);
    if (context->eglSurface != EGL_NO_SURFACE) eglDestroySurface(eglDisplay, context->eglSurface);
}

static void internalReadVertexArrayElement(GLContext* context, int arrayIdx, int elementIdx, ArrayBuffer* dstBuffer, float* constValue) {
#define PARSE_VALUESF(glType, divVal) \
    const glType* values = srcValues; \
    for (int i = 0; i < vertexAttrib->size; i++) dstValues[i] = arrayIdx == COLOR_ARRAY_INDEX ? (float)values[i] / divVal : (float)values[i]

    GLClientState* clientState = &currentRenderer->clientState;
    if (!clientState->vao->attribs[arrayIdx].state) {
        if (constValue && dstBuffer->position > 0) {
            if (arrayIdx == NORMAL_ARRAY_INDEX) {
                ArrayBuffer_putBytes(dstBuffer, constValue, 3 * sizeof(float));
            }
            else ArrayBuffer_putBytes(dstBuffer, constValue, 4 * sizeof(float));
        }
        return;
    }

    GLVertexAttrib* vertexAttrib = &clientState->vao->attribs[arrayIdx];

    float* dstValues = NULL;
    if (arrayIdx == NORMAL_ARRAY_INDEX) {
        dstValues = ArrayBuffer_putFloat3(dstBuffer, 0.0f, 0.0f, 1.0f);
    }
    else dstValues = ArrayBuffer_putFloat4(dstBuffer, 0.0f, 0.0f, 0.0f, 1.0f);
    dstBuffer->position++;

    void* srcValues;
    if (vertexAttrib->pointer) {
        srcValues = vertexAttrib->pointer + (elementIdx - clientState->indexStart) * vertexAttrib->stride;
    }
    else srcValues = ArrayBuffer_getBytes(&context->inputBuffer, vertexAttrib->stride);

    switch (vertexAttrib->type) {
        case GL_FLOAT: {
            memcpy(dstValues, srcValues, vertexAttrib->size * sizeof(float));
            break;
        }
        case GL_HALF_FLOAT: {
            const GLhalf* values = srcValues;
            _Float16 value;
            for (int i = 0; i < vertexAttrib->size; i++) {
                memcpy(&value, &values[i], sizeof(value));
                dstValues[i] = (float)value;
            }
            break;
        }
        case GL_BYTE: {
            PARSE_VALUESF(GLbyte, INT8_MAX);
            break;
        }
        case GL_UNSIGNED_BYTE: {
            PARSE_VALUESF(GLubyte, UINT8_MAX);
            break;
        }
        case GL_SHORT: {
            PARSE_VALUESF(GLshort, INT16_MAX);
            break;
        }
        case GL_UNSIGNED_SHORT: {
            PARSE_VALUESF(GLushort, UINT16_MAX);
            break;
        }
        case GL_INT: {
            PARSE_VALUESF(GLint, (double)INT32_MAX);
            break;
        }
        case GL_UNSIGNED_INT: {
            PARSE_VALUESF(GLuint, (double)UINT32_MAX);
            break;
        }
        case GL_DOUBLE: {
            PARSE_VALUESF(GLdouble, 1);
            break;
        }
    }

#undef PARSE_VALUESF
}

void readCommandBuffer(GLContext* context) {
    if (ArrayBuffer_available(&context->inputBuffer) == 0) {
        GLRenderer_endImmediate(currentRenderer);
        return;
    }

    GLenum mode = ArrayBuffer_getInt(&context->inputBuffer);
    int indexStart = ArrayBuffer_getInt(&context->inputBuffer);
    int indexCount = ArrayBuffer_getInt(&context->inputBuffer);
    int bufferSize = ArrayBuffer_getInt(&context->inputBuffer);

    if (bufferSize == 0) return;

    GLClientState* clientState = &currentRenderer->clientState;
    clientState->indexStart = indexStart;
    if (indexCount > 0) {
        for (int i = 0; i < clientState->vao->maxEnabledAttribs; i++) {
            if (clientState->vao->attribs[i].state) {
                clientState->vao->attribs[i].pointer = context->inputBuffer.buffer + context->inputBuffer.position;
                context->inputBuffer.position += indexCount * clientState->vao->attribs[i].stride;
            }
        }
    }
    else ArrayBuffer_rewind(&context->inputBuffer);

    ENSURE_ARRAY_CAPACITY(context->inputBuffer.size + bufferSize, context->inputBuffer.capacity, context->inputBuffer.buffer, 1);
    RingBuffer_read(context->serverRing, context->inputBuffer.buffer + context->inputBuffer.position, bufferSize);
    context->inputBuffer.size += bufferSize;

    GLRenderer_beginImmediate(currentRenderer, mode);
    while (ArrayBuffer_available(&context->inputBuffer) > 0) {
        short requestCode = ArrayBuffer_getShort(&context->inputBuffer);
        HandleRequestFunc handleRequestFunc = getHandleRequestFunc(requestCode);
        if (!handleRequestFunc) break;
        handleRequestFunc(context);
    }
    GLRenderer_endImmediate(currentRenderer);

    for (int i = 0; i < clientState->vao->maxEnabledAttribs; i++) {
        if (clientState->vao->attribs[i].state) clientState->vao->attribs[i].pointer = NULL;
    }
    clientState->indexStart = 0;
}

void readVertexArrayElement(GLContext* context, int arrayIdx, int elementIdx) {
    switch (arrayIdx) {
        case POSITION_ARRAY_INDEX:
            internalReadVertexArrayElement(context, arrayIdx, elementIdx, &currentRenderer->geometry.vertices, NULL);
            break;
        case COLOR_ARRAY_INDEX:
            internalReadVertexArrayElement(context, arrayIdx, elementIdx, &currentRenderer->geometry.colors, currentRenderer->state.color);
            break;
        case NORMAL_ARRAY_INDEX:
            internalReadVertexArrayElement(context, arrayIdx, elementIdx, &currentRenderer->geometry.normals, currentRenderer->state.normal);
            break;
        default:
            if (arrayIdx >= TEXCOORD_ARRAY_INDEX) {
                int index = arrayIdx - TEXCOORD_ARRAY_INDEX;
                internalReadVertexArrayElement(context, arrayIdx, elementIdx, &currentRenderer->geometry.texCoords[index], currentRenderer->state.texCoords[index]);
            }
            break;
    }
}

/*
 * ES 2.0: ein Client-Array aus Floats im Block in den Geometriepuffer umsetzen statt je Element
 * über internalReadVertexArrayElement (bei Worms 3D ~80000 Elemente je Bild; auf dem Cortex-A9
 * spart das mehrere Millisekunden). Liefert false, wenn der Einzelweg nötig bleibt.
 */
static bool readLegacyFloatArrayBulk(GLContext* context, int arrayIdx, int drawCount, bool addIndices) {
    GLClientState* clientState = &currentRenderer->clientState;
    GLVertexAttrib* vertexAttrib = &clientState->vao->attribs[arrayIdx];
    if (vertexAttrib->type != GL_FLOAT || vertexAttrib->size < 1 || vertexAttrib->size > 4 || vertexAttrib->stride <= 0) return false;

    Geometry* geometry = &currentRenderer->geometry;
    ArrayBuffer* dst;
    int dstComps = 4;
    if (arrayIdx == POSITION_ARRAY_INDEX) dst = &geometry->vertices;
    else if (arrayIdx == COLOR_ARRAY_INDEX) dst = &geometry->colors;
    else if (arrayIdx == NORMAL_ARRAY_INDEX) {
        dst = &geometry->normals;
        dstComps = 3;
    }
    else if (arrayIdx >= TEXCOORD_ARRAY_INDEX && arrayIdx < TEXCOORD_ARRAY_INDEX + MAX_TEXTURES) dst = &geometry->texCoords[arrayIdx - TEXCOORD_ARRAY_INDEX];
    else return false;

    int stride = vertexAttrib->stride;
    int comps = MIN(vertexAttrib->size, dstComps);
    if (ArrayBuffer_available(&context->inputBuffer) < drawCount * stride) return false;
    const char* src = ArrayBuffer_getBytes(&context->inputBuffer, drawCount * stride);

    int targetSize = dst->size + drawCount * dstComps * (int)sizeof(float);
    ENSURE_ARRAY_CAPACITY(targetSize, dst->capacity, dst->buffer, 1);
    float* out = (float*)(dst->buffer + dst->size);
    for (int j = 0; j < drawCount; j++, out += dstComps) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = dstComps == 3 ? 1.0f : 0.0f;
        if (dstComps == 4) out[3] = 1.0f;
        memcpy(out, src + j * stride, comps * sizeof(float));
    }
    dst->size = targetSize;
    int base = dst->position;
    dst->position += drawCount;

    if (addIndices && arrayIdx == POSITION_ARRAY_INDEX) {
        for (int j = 0; j < drawCount; j++) GLRenderer_addArrayElement(currentRenderer, base + j);
    }
    return true;
}

/*
 * Indizierter Zeichenaufruf mit Client-Arrays des festen Funktionsmodells im Bereichsformat
 * (GLADIO_DRAW_RANGED, armv7-Client): jeder Vertex des Bereichs kommt einmal, die Indizes
 * verweisen hinein (Index + Versatz). Der alte Client schickte je Index einen Vertex – bei
 * Modellen mit gemeinsamen Ecken das Mehrfache an Daten, Umsetzung und Vertex-Shader-Arbeit.
 */
static bool readRangedLegacyArrays(GLContext* context, GLenum drawMode, int drawCount, const void* indices, GLenum indexType) {
    int indexBias = ArrayBuffer_getInt(&context->inputBuffer);
    int vertexCount = ArrayBuffer_getInt(&context->inputBuffer);
    GLRenderer* renderer = currentRenderer;
    if (vertexCount <= 0 || drawCount <= 0) return true;

    /* 16-Bit-Indizes des Stapels: vorher ausgeben, wenn der Bereich nicht mehr hineinpasst */
    if (renderer->geometry.vertices.position + vertexCount > 65535 && !ArrayDeque_isEmpty(&renderer->meshes)) {
        GLRenderer_drawImmediate(renderer);
    }

    GLRenderer_beginImmediate(renderer, drawMode);
    int base = renderer->geometry.vertices.position;
    GLClientState* clientState = &renderer->clientState;
    for (int i = 0; i < clientState->vao->maxEnabledAttribs; i++) {
        if (clientState->vao->attribs[i].state != VERTEX_ATTRIB_LEGACY_ENABLED) continue;
        if (readLegacyFloatArrayBulk(context, i, vertexCount, false)) continue;
        for (int j = 0; j < vertexCount; j++) readVertexArrayElement(context, i, -1);
    }

    int bias = base + indexBias;
    switch (indexType) {
        case GL_UNSIGNED_BYTE:
            for (int j = 0; j < drawCount; j++) GLRenderer_addArrayElement(renderer, ((const GLubyte*)indices)[j] + bias);
            break;
        case GL_UNSIGNED_SHORT:
            for (int j = 0; j < drawCount; j++) GLRenderer_addArrayElement(renderer, ((const GLushort*)indices)[j] + bias);
            break;
        case GL_UNSIGNED_INT:
            for (int j = 0; j < drawCount; j++) GLRenderer_addArrayElement(renderer, (int)((const GLuint*)indices)[j] + bias);
            break;
    }
    GLRenderer_endImmediate(renderer);
    return true;
}

bool readUnboundVertexArrays(GLContext* context, GLenum drawMode, int drawCount, void** outIndices, GLenum indexType) {
    bool ranged = (drawMode & GLADIO_DRAW_RANGED) != 0;
    drawMode &= ~GLADIO_DRAW_RANGED;
    if (indexType != GL_NONE) {
        if (GLBuffer_getBound(GL_ELEMENT_ARRAY_BUFFER)) {
            *outIndices = (void*)(uint64_t)ArrayBuffer_getInt(&context->inputBuffer);
        }
        else *outIndices = ArrayBuffer_getBytes(&context->inputBuffer, drawCount * sizeofGLType(indexType));
    }

    if (ArrayBuffer_available(&context->inputBuffer) == 0) return false;
    if (ranged && indexType != GL_NONE) return readRangedLegacyArrays(context, drawMode, drawCount, *outIndices, indexType);
    bool meshCreated = false;

    GLClientState* clientState = &currentRenderer->clientState;
    for (int i = 0, j; i < clientState->vao->maxEnabledAttribs; i++) {
        bool legacyEnabledWithProgram = GLClientState_isLegacyEnabledWithProgram(clientState, i);
        if (clientState->vao->attribs[i].state == VERTEX_ATTRIB_ENABLED || legacyEnabledWithProgram) {
            GLVertexAttrib* vertexAttrib = &clientState->vao->attribs[i];
            int size = MIN(4, vertexAttrib->size);

            int byteCount = ArrayBuffer_getInt(&context->inputBuffer);
            void* pointer = ArrayBuffer_getBytes(&context->inputBuffer, byteCount);

            GLBuffer* oldArrayBuffer = NULL;
            if (vertexAttrib->size == GL_BGRA) {
                uint64_t offset = (uint64_t)vertexAttrib->pointer;
                swapPixelsRedBlue(pointer + offset, vertexAttrib->stride, byteCount - offset);
                oldArrayBuffer = clientState->vao->buffer[indexOfGLTarget(GL_ARRAY_BUFFER)];
                if (clientState->vao->bgraBuffer == 0) glGenBuffers(1, &clientState->vao->bgraBuffer);
                glBindBuffer(GL_ARRAY_BUFFER, clientState->vao->bgraBuffer);
                glBufferData(GL_ARRAY_BUFFER, byteCount, pointer, GL_DYNAMIC_DRAW);
                pointer = vertexAttrib->pointer;
            }

            int location = i;
            if (legacyEnabledWithProgram || clientState->arbProgram[0]) {
                if (clientState->program) {
                    location = clientState->program->location.attributes[i];
                }
                else location = clientState->arbProgram[0]->material->location.attributes[i];
                GLRenderer_enableVertexAttribute(currentRenderer, location);
            }

            glVertexAttribPointer(location, size, GLES_vertexAttribType(vertexAttrib->type), vertexAttrib->normalized, vertexAttrib->stride, pointer);
            if (oldArrayBuffer) glBindBuffer(GL_ARRAY_BUFFER, oldArrayBuffer->id);
        }
        else if (clientState->vao->attribs[i].state == VERTEX_ATTRIB_LEGACY_ENABLED) {
            if (!meshCreated) {
                GLRenderer_beginImmediate(currentRenderer, drawMode);
                meshCreated = true;
            }

            if (GLES2 && readLegacyFloatArrayBulk(context, i, drawCount, true)) continue;
            for (j = 0; j < drawCount; j++) {
                readVertexArrayElement(context, i, -1);
                if (i == POSITION_ARRAY_INDEX) {
                    GLRenderer_addArrayElement(currentRenderer, currentRenderer->geometry.vertices.position-1);
                }
            }
        }
    }
    if (meshCreated) GLRenderer_endImmediate(currentRenderer);
    return meshCreated;
}

const char* getGLExtensions(int* outNumExtensions) {
    if (GLES2) return GLES_clientExtensions(outNumExtensions);
    static int numExtensions = 0;
    static const char* extensionNames = "GL_EXT_abgr GL_ARB_shadow GL_ARB_window_pos GL_EXT_packed_pixels GL_ARB_vertex_buffer_object GL_ARB_vertex_array_object GL_ARB_texture_border_clamp GL_ARB_texture_env_add GL_EXT_texture_env_add GL_EXT_draw_range_elements GL_EXT_bgra GL_ARB_texture_compression GL_EXT_texture_compression_s3tc GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_dxt3 GL_EXT_texture_compression_dxt5 GL_ARB_point_parameters GL_EXT_point_parameters GL_EXT_texture_edge_clamp GL_EXT_multi_draw_arrays GL_ARB_multisample GL_EXT_polygon_offset GL_ARB_draw_elements_base_vertex GL_ARB_texture_rectangle GL_EXT_vertex_array GL_ARB_vertex_array_bgra GL_ARB_texture_non_power_of_two GL_EXT_blend_color GL_EXT_blend_minmax GL_EXT_blend_equation_separate GL_EXT_blend_func_separate GL_EXT_blend_subtract GL_EXT_texture_filter_anisotropic GL_ARB_texture_mirrored_repeat GL_ARB_point_sprite GL_ARB_texture_cube_map GL_EXT_texture_cube_map GL_EXT_texture_rg GL_ARB_texture_rg GL_EXT_texture_float GL_ARB_texture_float GL_EXT_texture_half_float GL_EXT_color_buffer_float GL_EXT_color_buffer_half_float GL_EXT_depth_texture GL_ARB_depth_texture GL_ARB_depth_clamp GL_ARB_ES2_compatibility GL_ARB_fragment_shader GL_ARB_vertex_shader GL_ARB_shading_language_100 GL_ARB_draw_instanced GL_EXT_draw_instanced GL_ARB_instanced_arrays GL_EXT_instanced_arrays GL_ARB_framebuffer_object GL_EXT_framebuffer_object GL_EXT_packed_depth_stencil GL_EXT_framebuffer_blit GL_ARB_draw_buffers GL_ARB_internalformat_query GL_ARB_internalformat_query2 GL_ARB_map_buffer_range GL_ARB_draw_buffers_blend GL_ARB_multitexture GL_ARB_texture_env_combine GL_EXT_texture_env_combine GL_ARB_texture_env_dot3 GL_EXT_texture_env_dot3 GL_ARB_shader_objects GL_ARB_vertex_program GL_ARB_fragment_program GL_ARB_sync GL_ARB_sampler_objects GL_ARB_texture_multisample GL_ARB_color_buffer_float GL_ARB_occlusion_query GL_ARB_occlusion_query2 GL_EXT_direct_state_access GL_ARB_uniform_buffer_object GL_ARB_timer_query GL_EXT_timer_query GL_ARB_texture_swizzle GL_ARB_copy_buffer GL_ARB_depth_buffer_float GL_ARB_sample_shading GL_ARB_tessellation_shader GL_ARB_derivative_control GL_ARB_compute_shader GL_EXT_gpu_program_parameters";

    if (numExtensions == 0) {
        char* ptr = (char*)extensionNames;
        while ((ptr = strchr(ptr, ' '))) {
            while (*ptr == ' ') ptr++;
            numExtensions++;
        }
    }

    if (outNumExtensions) *outNumExtensions = numExtensions;
    return extensionNames;
}