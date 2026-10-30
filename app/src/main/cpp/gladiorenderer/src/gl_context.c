#include "gl_context.h"
#include "gl_renderer.h"
#include "gl_dsa.h"
#include "sysvshared_memory.h"
#include "request_handler.h"
#include "gl_es2.h"
#include "crash_report.h"
#include "gl_diag.h"

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

/* Daten in Puffer oder Texturen (Zeitanteil „Hochladen“ in der Statistik) */
static bool isUploadRequest(short requestCode) {
    switch (requestCode) {
        case REQUEST_CODE_GL_BUFFER_DATA:
        case REQUEST_CODE_GL_BUFFER_SUB_DATA:
        case REQUEST_CODE_GL_BUFFER_STORAGE:
        case REQUEST_CODE_GL_FLUSH_MAPPED_BUFFER_RANGE:
        case REQUEST_CODE_GL_COPY_BUFFER_SUB_DATA:
        case REQUEST_CODE_GL_TEX_IMAGE2D:
        case REQUEST_CODE_GL_TEX_SUB_IMAGE2D:
        case REQUEST_CODE_GL_COMPRESSED_TEX_IMAGE2D:
        case REQUEST_CODE_GL_COMPRESSED_TEX_SUB_IMAGE2D:
            return true;
        default:
            return false;
    }
}

static bool isDrawRequest(short requestCode) {
    switch (requestCode) {
        case REQUEST_CODE_GL_DRAW_ARRAYS:
        case REQUEST_CODE_GL_DRAW_ARRAYS_INSTANCED:
        case REQUEST_CODE_GL_DRAW_ELEMENTS:
        case REQUEST_CODE_GL_DRAW_ELEMENTS_BASE_VERTEX:
        case REQUEST_CODE_GL_DRAW_ELEMENTS_INSTANCED:
        case REQUEST_CODE_GL_DRAW_ELEMENTS_INSTANCED_BASE_VERTEX: /* wined3d: jedes DrawIndexedPrimitive (Instanzen = Strom-Frequenz, meist 1) */
        case REQUEST_CODE_GL_DRAW_RANGE_ELEMENTS:
        case REQUEST_CODE_GL_DRAW_RANGE_ELEMENTS_BASE_VERTEX:
        case REQUEST_CODE_GL_MULTI_DRAW_ELEMENTS_BASE_VERTEX:
        case REQUEST_CODE_GL_END:
            return true;
        default:
            return false;
    }
}

#define STATS_PERIOD_NS 5000000000LL

static uint32_t requestCodeCounts[GLADIO_REPLY_CODE_SLOTS];
static uint32_t flushCodeCounts[GLADIO_REPLY_CODE_SLOTS]; /* Anfragen, vor denen ein Stapel ausgegeben wurde */
/* Ausführzeit je Anfragecode, geschätzt aus jeder REQUEST_TIME_SAMPLE-ten Anfrage (zufällig gewählt, damit sich
 * kein gleichbleibendes Muster der Anfragen mit dem Abstand deckt; die Uhr kostet auf dem Telefon ~30 ns) */
#define REQUEST_TIME_SAMPLE 16
static uint64_t requestCodeNs[GLADIO_REPLY_CODE_SLOTS];
/* PC-Prüfstand (Datei "trace" im Ablageverzeichnis): die Anfragen eines Bilds protokollieren */
int gladio_traceRequests = 0;

static const char* shortRequestName(int code) {
    if (code == GLADIO_REPLY_CODE_SLOTS - 1) return "Zustandswechsel"; /* aufgeschobene Änderungen (deferStateChange) */
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

/* die Anfragecodes mit der meisten Ausführzeit, in ms je Bild */
static void printTopRequestTimes(const char* title, const uint64_t* nanos, int maxShown, uint32_t frames) {
    char line[360];
    int pos = 0;
    int shown[10];
    uint64_t total = 0;
    for (int code = 0; code < GLADIO_REPLY_CODE_SLOTS; code++) {
        if (code != REQUEST_CODE_SWAP_DISPLAY_BUFFERS) total += nanos[code];
    }
    if (total == 0) return;
    if (maxShown > 10) maxShown = 10;
    pos = snprintf(line, sizeof(line), "gesamt %.1f", total / 1e6 / frames);
    for (int n = 0; n < maxShown; n++) {
        int best = -1;
        for (int code = 0; code < GLADIO_REPLY_CODE_SLOTS; code++) {
            if (code == REQUEST_CODE_SWAP_DISPLAY_BUFFERS || nanos[code] == 0) continue;
            bool done = false;
            for (int k = 0; k < n; k++) if (shown[k] == code) done = true;
            if (!done && (best < 0 || nanos[code] > nanos[best])) best = code;
        }
        if (best < 0) break;
        shown[n] = best;
        int written = snprintf(line + pos, sizeof(line) - pos, ", %s %.2f", shortRequestName(best), nanos[best] / 1e6 / frames);
        if (written < 0 || pos + written >= (int)sizeof(line)) break;
        pos += written;
    }
    println("gladio: %s: %s", title, line);
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
    println("gladio: Zeit je Bild: Warten auf den Client %.1f ms, Bildausgabe %.1f ms, Ausführen %.1f ms (davon Zeichnen %.1f ms, Hochladen %.1f ms); Shader-Programme neu %u, gesamt %u",
            st->waitNs / 1e6 / frames, st->swapNs / 1e6 / frames, execNs / 1e6 / frames, st->drawNs / 1e6 / frames, st->uploadNs / 1e6 / frames,
            (uint32_t)(gladio_statsPrograms - st->programsAtStart), (uint32_t)gladio_statsPrograms);
    GladioBatchStats* bs = &gladio_batchStats;
    GladioBatchStats* b0 = &st->batchAtStart;
    println("gladio: Stapel je Bild: %u ausgegeben mit %u Treiber-Zeichenaufrufen in %.1f ms (Shader, Uniforms, Vertexzeiger %.1f ms; Treiber-Zeichnen %.1f ms)",
            (uint32_t)((bs->batches - b0->batches) / frames), (uint32_t)((bs->driverDraws - b0->driverDraws) / frames),
            (bs->flushNs - b0->flushNs) / 1e6 / frames, (bs->setupNs - b0->setupNs) / 1e6 / frames, (bs->driverDrawNs - b0->driverDrawNs) / 1e6 / frames);

    /* welche Anfragen auf eine Antwort warten lassen (der Client steht so lange) und welche am häufigsten kommen */
    printTopRequestCodes("Antworten je Bild außer SWAP", gladio_statsReplyCodes, 4, frames);
    printTopRequestCodes("häufigste Anfragen je Bild", requestCodeCounts, 8, frames);
    printTopRequestTimes("Ausführzeit je Bild in ms (Stichprobe)", requestCodeNs, 8, frames);
    printTopRequestCodes("Stapel ausgegeben vor", flushCodeCounts, 6, frames);
    if (st->clientFrames > 0) {
        /* Sicht des Clients: wie lange er stand, statt Befehle zu erzeugen */
        uint32_t cf = st->clientFrames;
        println("gladio: Client je Bild: %.1f ms auf %u Antworten gewartet, %.1f ms auf Platz im Ring, %.1f ms auf das vorige Bild; "
                "ohne Rundreise: %u Abfragen (%u zurückgestellt), %u Uniform-Positionen, %u Texturen gelesen",
                st->client.replyWaitNs / 1e6 / cf, st->client.replies / cf, st->client.ringWaitNs / 1e6 / cf, st->client.swapWaitNs / 1e6 / cf,
                st->client.localQueries / cf, st->client.deferredQueries / cf, st->client.localUniforms / cf, st->client.localTexReads / cf);
    }
    GladioDiag_printStats();
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
    memset(requestCodeNs, 0, sizeof(requestCodeNs));
    memset(flushCodeCounts, 0, sizeof(flushCodeCounts));
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

    /* createDisplayBuffer hat die Anzeige an alle Ziele gebunden; wer die Bindungen des Programms braucht, stellt
     * sie danach wieder her (swapDisplayBuffers) */
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
        /* Das Fenster bekommt den Anzeigepuffer (die Java-Seite kopiert aus dem Lesepuffer), unabhängig davon, was das
         * Programm gerade gebunden hat. Danach gelten wieder dessen Bindungen – getrennt für Lesen und Zeichnen, denn
         * wined3d merkt sich beide und bindet nicht neu, was es für gebunden hält (DirectDraw-Spiele: jedes Bild ein
         * glBlitFramebuffer aus demselben Lese-FBO ins Fenster; vorher las der Blit ab dem zweiten Bild aus dem
         * Fenster selbst, GL_INVALID_OPERATION, Bild schwarz). Beim Neuanlegen der Anzeige (Fenstergröße) bekommt
         * die Bindung „Fenster“ den neuen Anzeigepuffer. */
        GLuint* slots = currentRenderer->clientState.framebuffer;
        GLuint savedRead = slots[indexOfGLTarget(GL_READ_FRAMEBUFFER)];
        GLuint savedDraw = slots[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)];
        GLuint oldDisplay = currentRenderer->displayBuffer;
        if (savedRead != oldDisplay) GLFramebuffer_bind(GL_READ_FRAMEBUFFER, oldDisplay);

        JMethods* jmethods = &context->jmethods;
        result = (*jmethods->env)->CallBooleanMethod(jmethods->env, jmethods->obj, jmethods->updateWindowContent, drawableId, currentRenderer->displaySize[0], currentRenderer->displaySize[1], JNI_TRUE);
        if (!result) {
            destroyDisplayBuffer();
            destroyDisplayBufAttachment(context);
            setCurrentRenderWindow(context, drawableId);
        }
        GLuint newDisplay = currentRenderer->displayBuffer;
        GLuint read = savedRead == oldDisplay || savedRead == 0 ? newDisplay : savedRead;
        GLuint draw = savedDraw == oldDisplay || savedDraw == 0 ? newDisplay : savedDraw;
        if (read == draw) {
            if (slots[indexOfGLTarget(GL_READ_FRAMEBUFFER)] != read || slots[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)] != draw) GLFramebuffer_bind(GL_FRAMEBUFFER, read);
        }
        else {
            if (slots[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)] != draw) GLFramebuffer_bind(GL_DRAW_FRAMEBUFFER, draw);
            if (slots[indexOfGLTarget(GL_READ_FRAMEBUFFER)] != read) GLFramebuffer_bind(GL_READ_FRAMEBUFFER, read);
        }
        if (result) {
            GLTexture* texture = GLTexture_getBound(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, texture ? texture->id : 0);
        }
        return;
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
    /* Der Wechsel der Textureinheit ändert nichts an der Zeichnung des Stapels (Texturzustand,
     * Matrizen und Arrays je Einheit stehen fest, die Anfragen dazu geben den Stapel aus). Worms 3D
     * schaltet vor jedem Zeichenaufruf alle acht Einheiten durch; ohne das entstand je Aufruf ein Stapel. */
    if (requestCode == REQUEST_CODE_GL_ACTIVE_TEXTURE || requestCode == REQUEST_CODE_GL_CLIENT_ACTIVE_TEXTURE) return false;
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
void gladio_printLastRequests(GLContext* context, const char* prefix) {
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
            println("%s: %s", prefix, line);
            pos = 0;
            continue;
        }
        pos += written;
        i += repeat;
    }
    if (pos > 0) println("%s (%u insgesamt): %s", prefix, lr->count, line);
}

static void printLastRequests(GLContext* context) {
    gladio_printLastRequests(context, "gladio: letzte Anfragen");
}

/*
 * Zustandsänderungen, die nur Server-Zustand betreffen (Matrizen, Licht, Material,
 * Texturumgebung, Alphatest, Schalter des festen Funktionsmodells), geben einen wartenden Stapel
 * nicht sofort aus. Der alte Inhalt der betroffenen Bereiche wird gemerkt; erst vor dem nächsten
 * Zeichenaufruf (oder einer Anfrage, die ausgeben muss) wird verglichen. Ist alles wieder wie
 * vorher – Worms 3D setzt vor fast jedem Objekt Matrix, Licht und Material neu, oft auf dieselben
 * Werte –, läuft der Stapel weiter; sonst wird er mit dem alten Zustand ausgegeben.
 * Änderungen, die gleich den Treiber erreichen (z. B. glEnable(GL_BLEND)), sind nur dann
 * aufschiebbar, wenn sie nichts ändern.
 */
#define DEFERRED_REGIONS 16
#define DEFERRED_REGION_MAX 128
typedef struct DeferredRegion {
    void* addr;
    int size;
    uint8_t old[DEFERRED_REGION_MAX];
} DeferredRegion;
static thread_local struct {
    DeferredRegion regions[DEFERRED_REGIONS];
    int count;
} deferred;

static bool deferRegion(void* addr, int size) {
    for (int i = 0; i < deferred.count; i++) {
        if (deferred.regions[i].addr == addr) return true;
    }
    if (deferred.count >= DEFERRED_REGIONS || size > DEFERRED_REGION_MAX) return false;
    DeferredRegion* region = &deferred.regions[deferred.count++];
    region->addr = addr;
    region->size = size;
    memcpy(region->old, addr, size);
    return true;
}

/* Gemerkte Änderungen auflösen: Stapel mit dem alten Zustand ausgeben, falls sich etwas geändert hat (true) */
static bool resolveDeferredState(GLRenderer* renderer) {
    if (deferred.count == 0) return false;
    bool changed = false;
    for (int i = 0; i < deferred.count && !changed; i++) {
        changed = memcmp(deferred.regions[i].addr, deferred.regions[i].old, deferred.regions[i].size) != 0;
    }
    if (changed && renderer && !ArrayDeque_isEmpty(&renderer->meshes)) {
        uint8_t tmp[DEFERRED_REGION_MAX];
        for (int i = 0; i < deferred.count; i++) {
            DeferredRegion* region = &deferred.regions[i];
            memcpy(tmp, region->addr, region->size);
            memcpy(region->addr, region->old, region->size);
            memcpy(region->old, tmp, region->size);
        }
        GLRenderer_drawImmediate(renderer);
        for (int i = 0; i < deferred.count; i++) {
            memcpy(deferred.regions[i].addr, deferred.regions[i].old, deferred.regions[i].size);
        }
        deferred.count = 0;
        return true;
    }
    deferred.count = 0;
    return false;
}

/* Vor einer Anfrage, die sonst den Stapel ausgeben würde: aufschiebbar? (Bereiche werden dabei gemerkt) */
static bool deferStateChange(GLContext* context, GLRenderer* renderer, short requestCode) {
    const int* args = (const int*)context->inputBuffer.buffer;
    int size = context->inputBuffer.size;
    switch (requestCode) {
        case REQUEST_CODE_GL_MATRIX_MODE:
            /* wählt nur den Stapel für folgende Anfragen */
            return true;
        case REQUEST_CODE_GL_LOAD_IDENTITY:
        case REQUEST_CODE_GL_LOAD_MATRIXF:
        case REQUEST_CODE_GL_LOAD_TRANSPOSE_MATRIXF:
        case REQUEST_CODE_GL_MULT_MATRIXF:
        case REQUEST_CODE_GL_MULT_TRANSPOSE_MATRIXF:
        case REQUEST_CODE_GL_TRANSLATEF:
        case REQUEST_CODE_GL_ROTATEF:
        case REQUEST_CODE_GL_SCALEF:
        case REQUEST_CODE_GL_ORTHO:
        case REQUEST_CODE_GL_FRUSTUM:
            if (renderer->matrixIndex > TEXTURE_MATRIX_INDEX) return false;
            return deferRegion(GLRenderer_getCurrentMatrix(renderer), MAT4_SIZE);
        case REQUEST_CODE_GL_LIGHTF:
        case REQUEST_CODE_GL_LIGHTFV: {
            if (size < 4) return false;
            int index = args[0] - GL_LIGHT0;
            if (index < 0) return false;
            return index >= MAX_LIGHTS || deferRegion(&renderer->lights[index], sizeof(GLLight));
        }
        case REQUEST_CODE_GL_MATERIALF:
        case REQUEST_CODE_GL_MATERIALFV:
            return renderer->materials && deferRegion(renderer->materials, 2 * sizeof(GLMaterial));
        case REQUEST_CODE_GL_TEX_ENVF:
        case REQUEST_CODE_GL_TEX_ENVFV: {
            if (size < 8 || args[0] != GL_TEXTURE_ENV) return false;
            switch (args[1]) {
                case GL_TEXTURE_ENV_MODE: case GL_TEXTURE_ENV_COLOR: case GL_RGB_SCALE: case GL_ALPHA_SCALE:
                case GL_COMBINE_RGB: case GL_COMBINE_ALPHA: case GL_SOURCE0_RGB: case GL_SOURCE1_RGB:
                case GL_SOURCE0_ALPHA: case GL_SOURCE1_ALPHA: case GL_OPERAND0_RGB: case GL_OPERAND1_RGB:
                case GL_OPERAND0_ALPHA: case GL_OPERAND1_ALPHA:
                    return deferRegion(&renderer->state.texEnv[renderer->clientState.activeTexture], sizeof(TexEnv));
                default:
                    return false;
            }
        }
        case REQUEST_CODE_GL_ALPHA_FUNC:
            return deferRegion(&renderer->state.alphaTest, sizeof(renderer->state.alphaTest));
        case REQUEST_CODE_GL_ENABLE:
        case REQUEST_CODE_GL_DISABLE: {
            if (size < 4) return false;
            GLenum cap = args[0];
            bool state = requestCode == REQUEST_CODE_GL_ENABLE;
            switch (cap) {
                case GL_LIGHTING: return deferRegion(&renderer->state.lighting, sizeof(renderer->state.lighting));
                case GL_COLOR_MATERIAL: return deferRegion(&renderer->state.colorMaterial.enabled, sizeof(bool));
                case GL_ALPHA_TEST: return deferRegion(&renderer->state.alphaTest.enabled, sizeof(bool));
                case GL_FOG: return deferRegion(&renderer->state.fog.enabled, sizeof(bool));
                case GL_TEXTURE_1D:
                case GL_TEXTURE_2D:
                case GL_TEXTURE_3D:
                case GL_TEXTURE_CUBE_MAP:
                case GL_TEXTURE_RECTANGLE:
                    return deferRegion(&renderer->state.enabledTextures[renderer->clientState.activeTexture], sizeof(uint8_t));
                case GL_LINE_SMOOTH:
                case GL_LINE_STIPPLE:
                case GL_POLYGON_STIPPLE:
                case GL_POLYGON_OFFSET_LINE:
                case GL_VERTEX_PROGRAM_POINT_SIZE:
                case GL_NORMALIZE:
                case GL_TEXTURE_CUBE_MAP_SEAMLESS:
                case GL_DEPTH_CLAMP:
                    return true; /* setCapabilityState ignoriert sie */
                case GL_POINT_SMOOTH:
                case GL_POINT_SPRITE:
                case GL_VERTEX_PROGRAM_ARB:
                case GL_FRAGMENT_PROGRAM_ARB:
                case GL_VERTEX_PROGRAM_TWO_SIDE:
                case GL_PRIMITIVE_RESTART:
                    return false;
                default:
                    if (cap >= GL_LIGHT0 && cap <= GL_LIGHT7) {
                        int index = cap - GL_LIGHT0;
                        return index >= MAX_LIGHTS || deferRegion(&renderer->lights[index], sizeof(GLLight));
                    }
                    if ((cap >= GL_TEXTURE_GEN_S && cap <= GL_TEXTURE_GEN_Q) || (cap >= GL_CLIP_PLANE0 && cap <= GL_CLIP_PLANE5)) return true;
                    if (!GLES_isCapabilitySupported(cap)) return true;
                    /* Treiberzustand: nur aufschiebbar, wenn er schon so ist */
                    return (glIsEnabled(cap) != GL_FALSE) == state;
            }
        }
        case REQUEST_CODE_GL_BIND_TEXTURE: {
            /* dieselbe Textur noch einmal gebunden: nichts zu tun */
            if (size < 8 || args[0] != GL_TEXTURE_2D) return false;
            GLTexture* bound = renderer->clientState.texture[renderer->clientState.activeTexture][indexOfGLTarget(GL_TEXTURE_2D)];
            if (args[1] == 0) return bound == NULL;
            if (!bound || bound->type != GL_TEXTURE_2D || bound->normalizeCoords) return false;
            GLX_CONTEXT_LOCK();
            GLTexture* texture = SparseArray_get(renderer->clientState.textures, (GLuint)args[1]);
            GLX_CONTEXT_UNLOCK();
            return texture == bound;
        }
        default:
            return false;
    }
}

static void* requestHandlerThread(void* param) {
    GLContext* context = param;
    loadJMethods(&context->jmethods);
    GladioCrash_setThreadContext(context);
    short requestCode;
    uint32_t sampleState = 0x9e3779b9u;

    resetStats(context, nowNs());
    while (context->running) {
        /* Zeit messen, wenn wirklich auf den Client gewartet wird (sonst keine Uhr lesen) */
        bool blocked = RingBuffer_size(context->serverRing) < HEADER_SIZE;
        int64_t waitStart = blocked ? nowNs() : 0;
        if (!gl_recv(context->serverRing, &requestCode, &context->inputBuffer)) break;
        int64_t sampleStart = 0;
        if (blocked) {
            sampleStart = nowNs();
            context->stats.waitNs += sampleStart - waitStart;
        }
        sampleState = sampleState * 1664525u + 1013904223u;
        if ((sampleState >> 28) != 0) sampleStart = 0;
        else if (!sampleStart) sampleStart = nowNs();
        context->stats.requests++;
        requestCodeCounts[(unsigned short)requestCode % GLADIO_REPLY_CODE_SLOTS]++;
        if (gladio_traceRequests) {
            char args[128];
            int pos = 0, n = context->inputBuffer.size / 4 < 10 ? context->inputBuffer.size / 4 : 10;
            args[0] = 0;
            for (int i = 0; i < n && pos < (int)sizeof(args) - 12; i++)
                pos += snprintf(args + pos, sizeof(args) - pos, " 0x%x", ((int*)context->inputBuffer.buffer)[i]);
            println("TRACE %s %d%s", shortRequestName(requestCode), context->inputBuffer.size, args);
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
        if (currentRenderer) {
            /* gemerkte Zustandsänderungen (deferStateChange): aufschieben, auflösen oder ausgeben */
            if (flushBatch && deferStateChange(context, currentRenderer, requestCode)) {
                flushBatch = false;
                drawStart = drawing ? drawStart : 0;
            }
            else if (drawing || flushBatch || deferred.count > 0) {
                if (deferred.count > 0 && (drawing || isCanDrawImmediate(requestCode))) {
                    if (!drawStart) drawStart = nowNs();
                    if (resolveDeferredState(currentRenderer)) flushCodeCounts[GLADIO_REPLY_CODE_SLOTS - 1]++;
                }
                flushBatch = isCanDrawImmediate(requestCode) && !ArrayDeque_isEmpty(&currentRenderer->meshes);
                if (flushBatch && requestCode == REQUEST_CODE_GL_PUSH_MATRIX) {
                    /* Push ändert die oberste Matrix nicht */
                    flushBatch = false;
                }
                else if (flushBatch && requestCode == REQUEST_CODE_GL_POP_MATRIX && currentRenderer->matrixIndex <= TEXTURE_MATRIX_INDEX) {
                    /* Pop auf eine gleiche Matrix ändert nichts */
                    ArrayList* stack = &currentRenderer->matrixStack[currentRenderer->matrixIndex];
                    if (stack->size > 1 && memcmp(stack->elements[stack->size-1], stack->elements[stack->size-2], MAT4_SIZE) == 0) flushBatch = false;
                }
            }
        }
        if (flushBatch) {
            flushCodeCounts[(unsigned short)requestCode % GLADIO_REPLY_CODE_SLOTS]++;
            GLRenderer_drawImmediate(currentRenderer);
        }
        if (drawStart && !drawing) {
            context->stats.drawNs += nowNs() - drawStart;
            drawStart = 0;
        }

        GladioDiag_beforeRequest(context, requestCode, drawing);

        switch (requestCode) {
            case REQUEST_CODE_SET_CURRENT_RENDER_WINDOW: {
                int windowId = ArrayBuffer_getInt(&context->inputBuffer);
                int contextId = ArrayBuffer_getInt(&context->inputBuffer);
                JMethods* jmethods = &context->jmethods;
                GLXContext* glxContext = (GLXContext*)(*jmethods->env)->CallLongMethod(jmethods->env, jmethods->obj, jmethods->getGLXContextPtr, context->clientFd, contextId);
                if (!glxContext) {
                    static int reports = 0;
                    if (reports++ < 20) println("gladio: MAKE_CURRENT: GLX-Kontext %d unbekannt (Fenster 0x%x, fd %d)", contextId, windowId, context->clientFd);
                }

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
                if (context->inputBuffer.size >= (int)(sizeof(int) + sizeof(GladioClientStats))) {
                    GladioClientStats clientStats;
                    memcpy(&clientStats, (char*)context->inputBuffer.buffer + sizeof(int), sizeof(clientStats));
                    if (clientStats.magic == GLADIO_CLIENT_STATS_MAGIC) {
                        context->stats.client.replies += clientStats.replies;
                        context->stats.client.replyWaitNs += clientStats.replyWaitNs;
                        context->stats.client.ringWaitNs += clientStats.ringWaitNs;
                        context->stats.client.swapWaitNs += clientStats.swapWaitNs;
                        context->stats.client.localQueries += clientStats.localQueries;
                        context->stats.client.localUniforms += clientStats.localUniforms;
                        context->stats.client.localTexReads += clientStats.localTexReads;
                        context->stats.client.deferredQueries += clientStats.deferredQueries;
                        context->stats.clientFrames++;
                    }
                }
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
                int64_t uploadStart = isUploadRequest(requestCode) ? nowNs() : 0;
                if (handleRequestFunc) handleRequestFunc(context);
                if (uploadStart) context->stats.uploadNs += nowNs() - uploadStart;
                break;
            }
        }

        if (drawStart) context->stats.drawNs += nowNs() - drawStart;

        /* GL-Fehler, Zeichenziel, Aufzeichnung (gl_diag.c; im Bau mit DEBUG_MODE_GL_ERROR nach jeder Anfrage) */
        GladioDiag_afterRequest(context, requestCode, drawing);
        if (sampleStart) requestCodeNs[(unsigned short)requestCode % GLADIO_REPLY_CODE_SLOTS] += (uint64_t)(nowNs() - sampleStart) * REQUEST_TIME_SAMPLE;
    }

    printLastRequests(context);
    GladioDiag_endContext(context);
    GladioCrash_setPhase("Ende der Verbindung");
    destroyDisplayBuffer();
    destroyDisplayBufAttachment(context);
    if (context->glxContext) {
        eglMakeCurrent(eglGetDisplay(EGL_DEFAULT_DISPLAY), EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        context->glxContext = NULL;
    }
    GladioCrash_setThreadContext(NULL);
    GladioCrash_setPhase(NULL);
    (*context->jmethods.jvm)->DetachCurrentThread(context->jmethods.jvm);
    return NULL;
}

GLContext* createGLContext(JNIEnv* env, jobject obj, int clientFd) {
    GladioCrash_install();
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
        static int reports = 0;
        if (reports++ < 20) {
            println("gladio: eglMakeCurrent fehlgeschlagen: 0x%x (GLX-Kontext %d, Pbuffer %d, Thread %d)", eglGetError(),
                    context->renderer.contextId, context->eglSurface != EGL_NO_SURFACE, (int)currentThreadId());
        }
    }
}

static GLXContext* createGLXContextInternal(int contextId, GLXContext* sharedContext);

GLXContext* createGLXContext(int contextId, GLXContext* sharedContext) {
    GladioCrash_install();
    GladioCrash_setPhase("createGLXContext");
    GLXContext* context = createGLXContextInternal(contextId, sharedContext);
    GladioCrash_setPhase(NULL);
    return context;
}

static GLXContext* createGLXContextInternal(int contextId, GLXContext* sharedContext) {
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
    GladioCrash_setPhase("destroyGLXContext");
    GLX_CONTEXT_LOCK();
    GLClientState_destroy(&context->renderer.clientState);
    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    makeGLXContextCurrent(eglDisplay, context);
    GLRenderer_destroy(&context->renderer);
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    GLX_CONTEXT_UNLOCK();
    eglDestroyContext(eglDisplay, context->eglContext);
    if (context->eglSurface != EGL_NO_SURFACE) eglDestroySurface(eglDisplay, context->eglSurface);
    GladioCrash_setPhase(NULL);
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
            if (clientState->vao->attribs[i].state && clientState->vao->attribs[i].state != VERTEX_ATTRIB_BGRA_BUFFER) {
                clientState->vao->attribs[i].pointer = context->inputBuffer.buffer + context->inputBuffer.position;
                context->inputBuffer.position += indexCount * clientState->vao->attribs[i].stride;
            }
        }
    }
    else ArrayBuffer_rewind(&context->inputBuffer);

    ENSURE_ARRAY_CAPACITY(context->inputBuffer.size + bufferSize, context->inputBuffer.capacity, context->inputBuffer.buffer, 1);
    RingBuffer_read(context->serverRing, context->inputBuffer.buffer + context->inputBuffer.position, bufferSize);
    context->inputBuffer.size += bufferSize;

    /* 16-Bit-Indizes – höchstens ein Vertex je 6 Byte Befehle (glArrayElement) */
    GLRenderer_reserveBatchVertices(currentRenderer, bufferSize / 6);
    GLRenderer_beginImmediate(currentRenderer, mode);
    while (ArrayBuffer_available(&context->inputBuffer) > 0) {
        short requestCode = ArrayBuffer_getShort(&context->inputBuffer);
        HandleRequestFunc handleRequestFunc = getHandleRequestFunc(requestCode);
        if (!handleRequestFunc) break;
        handleRequestFunc(context);
    }
    GLRenderer_endImmediate(currentRenderer);

    for (int i = 0; i < clientState->vao->maxEnabledAttribs; i++) {
        if (clientState->vao->attribs[i].state && clientState->vao->attribs[i].state != VERTEX_ATTRIB_BGRA_BUFFER) clientState->vao->attribs[i].pointer = NULL;
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
 * Ein Client-Array aus Floats (oder 8-Bit-Farben) im Block in den Geometriepuffer umsetzen statt je Element
 * über internalReadVertexArrayElement (bei Worms 3D ~80000 Elemente je Bild; auf dem Cortex-A9
 * spart das mehrere Millisekunden). Liefert false, wenn der Einzelweg nötig bleibt.
 */
static bool readLegacyFloatArrayBulk(GLContext* context, int arrayIdx, int drawCount, bool addIndices) {
    GLClientState* clientState = &currentRenderer->clientState;
    GLVertexAttrib* vertexAttrib = &clientState->vao->attribs[arrayIdx];
    /* Farben als 8-Bit-Werte (D3D-artig, z. B. Worms 3D) ebenfalls im Block */
    bool ubyteColor = arrayIdx == COLOR_ARRAY_INDEX && vertexAttrib->type == GL_UNSIGNED_BYTE && (vertexAttrib->size == 3 || vertexAttrib->size == 4);
    if ((vertexAttrib->type != GL_FLOAT && !ubyteColor) || vertexAttrib->size < 1 || vertexAttrib->size > 4 || vertexAttrib->stride <= 0) return false;

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
    if (ubyteColor) {
        const float scale = 1.0f / 255.0f;
        bool hasAlpha = vertexAttrib->size == 4;
        for (int j = 0; j < drawCount; j++, out += 4) {
            const uint8_t* c = (const uint8_t*)(src + j * stride);
            out[0] = c[0] * scale;
            out[1] = c[1] * scale;
            out[2] = c[2] * scale;
            out[3] = hasAlpha ? c[3] * scale : 1.0f;
        }
    }
    else for (int j = 0; j < drawCount; j++, out += dstComps) {
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
    /* ohne Vertex-Array entsteht nichts (die übrigen Arrays dürfen den Stapel nicht verschieben) */
    if (renderer->clientState.vao->attribs[POSITION_ARRAY_INDEX].state != VERTEX_ATTRIB_LEGACY_ENABLED) return true;
    GLRenderer_prepareBatchArrays(renderer, vertexCount);

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
    bool skipLegacy = false;
    {
        /* Stapel ausrichten (GLRenderer_prepareBatchArrays); ohne Vertex-Array entsteht nichts */
        bool anyLegacy = false;
        for (int i = 0; i < clientState->vao->maxEnabledAttribs; i++) {
            if (clientState->vao->attribs[i].state == VERTEX_ATTRIB_LEGACY_ENABLED && !GLClientState_isLegacyEnabledWithProgram(clientState, i)) anyLegacy = true;
        }
        if (anyLegacy) {
            if (clientState->vao->attribs[POSITION_ARRAY_INDEX].state != VERTEX_ATTRIB_LEGACY_ENABLED) skipLegacy = true;
            else {
                GLRenderer_reserveBatchVertices(currentRenderer, drawCount);
                GLRenderer_prepareBatchArrays(currentRenderer, drawCount);
            }
        }
    }
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
                /* eigener Puffer je Attribut: Diffus- und Glanzfarbe (wined3d: beide GL_BGRA) lesen sonst
                 * beide aus dem zuletzt hochgeladenen Block, in dem nur das zweite Attribut umgestellt ist */
                GLuint* bgraBuffer = &clientState->vao->bgraBuffers[i];
                if (*bgraBuffer == 0) glGenBuffers(1, bgraBuffer);
                glBindBuffer(GL_ARRAY_BUFFER, *bgraBuffer);
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
            if (skipLegacy) continue;
            if (!meshCreated) {
                GLRenderer_beginImmediate(currentRenderer, drawMode);
                meshCreated = true;
            }

            if (readLegacyFloatArrayBulk(context, i, drawCount, true)) continue;
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

/* ---- GL_BGRA-Farben aus einem Puffer -------------------------------------------------------------
 * wined3d gibt D3DCOLOR-Eckpunktfarben bei GL >= 3.2 als GL_BGRA an (ARB_vertex_array_bgra ist dort
 * Kern). ES kennt GL_BGRA nicht als Attributgröße. Früher schickte der Client dafür bei jedem
 * Zeichenaufruf den Puffer vom Anfang bis zum letzten Eckpunkt mit – wined3d zeichnet aus einem großen
 * Streaming-Puffer mit wachsendem Startpunkt: bis über 100 MB je Bild (LEGO Racers im ES-3.0-Pfad
 * 1–3 Bilder/s). Jetzt liest der Server die Farben selbst aus dem geteilten Speicher des Puffers und
 * stellt nur die Eckpunkte des Zeichenaufrufs auf RGBA um (4 Byte je Eckpunkt).
 *
 * Ziel ist ein Ringpuffer, der nur angehängt wird (unsynchronisiert abgebildet, kein Warten auf die
 * GPU, keine Kopie des Puffers im Treiber). Der Attributzeiger muss für Eckpunkt v auf seine Farbe
 * zeigen: Zeiger = Platz - erster Eckpunkt * 4, also liegt der Platz mindestens bei erster Eckpunkt * 4
 * (bei wachsendem Startpunkt deckt sich das mit dem Anhängen). Ist der Ring voll, wird er neu angelegt
 * (ältere Zeichenaufrufe behalten den alten Speicher). */

#define BGRA_STREAM_MIN_SIZE (1024 * 1024)
#define BGRA_STREAM_MAX_SIZE (256 * 1024 * 1024)

typedef struct BgraJob {
    int location;
    bool normalized;
    int stride;
    const uint8_t* src;
    int64_t first;
    int count;
} BgraJob;

static int bgraAttribLocation(GLClientState* clientState, int index) {
    if (clientState->arbProgram[0]) {
        if (clientState->program) return clientState->program->location.attributes[index];
        return clientState->arbProgram[0]->material->location.attributes[index];
    }
    return index;
}

static void restoreArrayBufferBinding(void) {
    GLBuffer* bound = GLBuffer_getBound(GL_ARRAY_BUFFER);
    glBindBuffer(GL_ARRAY_BUFFER, bound ? bound->id : 0);
}

static bool hasBgraBufferArrays(GLClientState* clientState) {
    for (int i = 0; i < clientState->vao->maxEnabledAttribs; i++) {
        if (clientState->vao->attribs[i].state == VERTEX_ATTRIB_BGRA_BUFFER) return true;
    }
    return false;
}

static void convertBgraColors(uint8_t* dst, const uint8_t* src, int stride, int count) {
    /* B G R A -> R G B A (little endian); memcpy wird zu einfachen Lade- und Speicherbefehlen */
    for (int v = 0; v < count; v++, src += stride, dst += 4) {
        uint32_t c;
        memcpy(&c, src, 4);
        c = (c & 0xff00ff00) | ((c >> 16) & 0xff) | ((c & 0xff) << 16);
        memcpy(dst, &c, 4);
    }
}

static void prepareBgraArrays(GLClientState* clientState, int64_t first, int64_t last, GLsizei instancecount) {
    static bool warned = false;
    GLRenderer* renderer = currentRenderer;
    GLVertexArrayObject* vao = clientState->vao;
    BgraJob jobs[VERTEX_ATTRIB_COUNT];
    int numJobs = 0;

    for (int i = 0; i < vao->maxEnabledAttribs; i++) {
        GLVertexAttrib* attrib = &vao->attribs[i];
        if (attrib->state != VERTEX_ATTRIB_BGRA_BUFFER) continue;

        int location = bgraAttribLocation(clientState, i);
        if (location < 0) continue;
        if (clientState->arbProgram[0]) GLRenderer_enableVertexAttribute(renderer, location);

        GLX_CONTEXT_LOCK();
        GLBuffer* source = SparseArray_get(clientState->buffers, attrib->boundArrayBuffer);
        GLX_CONTEXT_UNLOCK();
        if (!source) continue;

        int stride = attrib->stride > 0 ? attrib->stride : 4;
        uint64_t offset = (uint64_t)attrib->pointer;
        /* Attribute je Instanz (Teiler > 0) lesen die Instanzen 0 .. (Anzahl-1)/Teiler, ohne Basis-
         * Eckpunkt; die Eckpunkte des Zeichenaufrufs gelten nur für Attribute je Eckpunkt. Vorher
         * wurde auch hier der Eckpunktbereich umgestellt, die Instanzfarben (D3DCOLOR aus
         * SetStreamSourceFreq) kamen aus dem falschen Bereich. */
        int64_t attribFirst = first, attribLast = last;
        if (attrib->divisor > 0) {
            attribFirst = 0;
            attribLast = instancecount > 1 ? (instancecount - 1) / attrib->divisor : 0;
        }
        /* Bereich auf den Puffer begrenzen (Neustart-Index, falsche Bereiche) */
        int64_t maxVertex = ((int64_t)source->size - (int64_t)offset - 4) / stride;
        int64_t lo = MAX(attribFirst, 0), hi = MIN(attribLast, maxVertex);

        if (!source->mappedData || attrib->type != GL_UNSIGNED_BYTE || hi < lo || (hi + 1) * 4 > BGRA_STREAM_MAX_SIZE / 2) {
            /* Notbehelf: Puffer unverändert (Rot und Blau vertauscht) */
            if (!warned) {
                println("gladio: GL_BGRA-Attribut %d nicht umstellbar (Puffer %u, Typ %s, Eckpunkte %lld-%lld) – Farben unverändert",
                        i, attrib->boundArrayBuffer, glEnumToString(attrib->type), (long long)attribFirst, (long long)attribLast);
                warned = true;
            }
            glBindBuffer(GL_ARRAY_BUFFER, source->id);
            glVertexAttribPointer(location, 4, GLES_vertexAttribType(attrib->type), attrib->normalized, stride, (const void*)offset);
            restoreArrayBufferBinding();
            continue;
        }

        BgraJob* job = &jobs[numJobs++];
        job->location = location;
        job->normalized = attrib->normalized;
        job->stride = stride;
        job->src = (const uint8_t*)source->mappedData + offset + (size_t)lo * stride;
        job->first = lo;
        job->count = (int)(hi - lo + 1);
    }
    if (numJobs == 0) return;

    GLBgraStream* stream = &renderer->bgraStream;
    if (stream->buffer == 0) glGenBuffers(1, &stream->buffer);
    glBindBuffer(GL_ARRAY_BUFFER, stream->buffer);

    /* alle Attribute eines Zeichenaufrufs im selben Speicher (Neuanlegen verwirft den alten) */
    int64_t end = stream->cursor, needed = 0;
    for (int j = 0; j < numJobs; j++) {
        end = MAX(end, jobs[j].first * 4) + jobs[j].count * 4;
        needed = MAX(needed, jobs[j].first * 4) + jobs[j].count * 4;
    }
    if (end > stream->capacity) {
        int64_t size = MAX(BGRA_STREAM_MIN_SIZE, stream->capacity);
        while (size < needed) size *= 2;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)size, NULL, GL_STREAM_DRAW);
        stream->capacity = size;
        stream->cursor = 0;
    }

    for (int j = 0; j < numJobs; j++) {
        BgraJob* job = &jobs[j];
        int64_t position = MAX(stream->cursor, job->first * 4);
        int bytes = job->count * 4;

        uint8_t* dst = NULL;
        if (!GLES2) dst = glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)position, bytes, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
        if (dst) {
            convertBgraColors(dst, job->src, job->stride, job->count);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        else {
            dst = malloc(bytes);
            if (!dst) continue;
            convertBgraColors(dst, job->src, job->stride, job->count);
            glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)position, bytes, dst);
            free(dst);
        }
        stream->cursor = position + bytes;
        glVertexAttribPointer(job->location, 4, GL_UNSIGNED_BYTE, job->normalized, 4, (const void*)(uintptr_t)(position - job->first * 4));
    }
    restoreArrayBufferBinding();
}

void GLContext_prepareBgraArrays(GLint first, GLsizei count, GLsizei instancecount) {
    GLClientState* clientState = &currentRenderer->clientState;
    if (count <= 0 || !hasBgraBufferArrays(clientState)) return;
    prepareBgraArrays(clientState, first, (int64_t)first + count - 1, instancecount);
}

void GLContext_prepareBgraArraysIndexed(GLsizei count, GLenum type, const void* indices, GLint basevertex, GLsizei instancecount) {
    GLClientState* clientState = &currentRenderer->clientState;
    if (count <= 0 || !hasBgraBufferArrays(clientState)) return;

    int indexSize = type == GL_UNSIGNED_BYTE ? 1 : (type == GL_UNSIGNED_SHORT ? 2 : 4);
    const uint8_t* src = indices;
    GLBuffer* elementBuffer = GLBuffer_getBound(GL_ELEMENT_ARRAY_BUFFER);
    if (elementBuffer) {
        uint64_t offset = (uint64_t)indices;
        if (!elementBuffer->mappedData || offset + (uint64_t)count * indexSize > (uint64_t)elementBuffer->size) return;
        src = (const uint8_t*)elementBuffer->mappedData + offset;
    }
    if (!src) return;

    uint32_t minIndex = UINT32_MAX, maxIndex = 0;
    #define SCAN_INDICES(T, skip) \
        for (int i = 0; i < count; i++) { \
            uint32_t index = ((const T*)src)[i]; \
            if (skip) continue; \
            if (index < minIndex) minIndex = index; \
            if (index > maxIndex) maxIndex = index; \
        }
    uint32_t restart = type == GL_UNSIGNED_BYTE ? 0xff : (type == GL_UNSIGNED_SHORT ? 0xffff : 0xffffffff);
    switch (type) {
        case GL_UNSIGNED_BYTE: SCAN_INDICES(uint8_t, false); break;
        case GL_UNSIGNED_SHORT: SCAN_INDICES(uint16_t, false); break;
        default: SCAN_INDICES(uint32_t, false); break;
    }
    /* der feste Neustart-Index zählt nicht (GL_PRIMITIVE_RESTART wird zu GL_PRIMITIVE_RESTART_FIXED_INDEX) */
    if (maxIndex == restart && !GLES2 && glIsEnabled(GL_PRIMITIVE_RESTART_FIXED_INDEX)) {
        minIndex = UINT32_MAX;
        maxIndex = 0;
        switch (type) {
            case GL_UNSIGNED_BYTE: SCAN_INDICES(uint8_t, index == restart); break;
            case GL_UNSIGNED_SHORT: SCAN_INDICES(uint16_t, index == restart); break;
            default: SCAN_INDICES(uint32_t, index == restart); break;
        }
    }
    #undef SCAN_INDICES
    if (minIndex > maxIndex) return;
    prepareBgraArrays(clientState, (int64_t)minIndex + basevertex, (int64_t)maxIndex + basevertex, instancecount);
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