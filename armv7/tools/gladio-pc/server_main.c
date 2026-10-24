/*
 * gladio-pc: PC-Testgeschirr für den Gladio-Server (app/src/main/cpp/gladiorenderer).
 *
 * Auf dem Gerät nimmt Winlators Java-X-Server (GLXExtension.java) die GLX-Anfragen
 * des Gladio-Clients (libGL in der Wine-Umgebung) entgegen und ruft den nativen
 * Server über JNI auf. Hier ersetzt ein kleiner X-Server-Nachbau in C diesen
 * Java-Teil:
 *
 *   - lauscht auf einem Unix-Socket (GLADIO_SOCKET, Vorgabe /tmp/gladio-pc.sock)
 *   - beantwortet die X11-Verbindungsaufnahme und die GLX-Anfragen mit dem
 *     Haupt-Opcode -106 byteidentisch zu GLXExtension.java
 *   - stellt dem Server eine JNI-Attrappe bereit: getWindowSize (fragt den echten
 *     X-Server unter $DISPLAY nach der Fenstergröße), clearWindowContent,
 *     updateWindowContent (liest das Bild mit glReadPixels aus und schreibt auf
 *     Wunsch PPM-Dateien) und getGLXContextPtr
 *   - erzeugt den „App-Kontext“ (globalEGLContext), mit dem die GLX-Kontexte
 *     ihre Objekte teilen, wie es auf dem Gerät der GLSurfaceView-Kontext tut
 *
 * Umgebungsvariablen:
 *   GLADIO_SOCKET        Pfad des Unix-Sockets (Client: dieselbe Variable)
 *   GLADIO_PC_ES         2 oder 3: Version des App-Kontexts (Vorgabe 3)
 *   GLADIO_PC_DUMP       Verzeichnis für PPM-Bilder der fertigen Frames
 *   GLADIO_PC_DUMP_FRAMES  Kommaliste von Framenummern (Vorgabe "1,10"), "all" = alle
 *   GLADIO_PC_SIZE       WxH als Ersatz, wenn kein $DISPLAY erreichbar ist (Vorgabe 640x480)
 *   EGL_PLATFORM=surfaceless  (Mesa) für einen Kontext ohne Fenster
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <X11/Xlib.h>

#include "gl_context.h"
#include "socket_utils.h"

#define GLX_OPCODE_CREATE_GL_CONTEXT 1
#define GLX_OPCODE_DESTROY_GL_CONTEXT 2
#define GLX_OPCODE_CREATE_CONTEXT 3
#define GLX_OPCODE_DESTROY_CONTEXT 4
#define GLX_OPCODE_QUERY_VERSION 7
#define GLX_OPCODE_QUERY_EXTENSIONS_STRING 18
#define GLX_OPCODE_QUERY_SERVER_STRING 19
#define GLX_OPCODE_GET_FB_CONFIGS 21
#define GLX_OPCODE_CREATE_CONTEXT_ATTRIBS_ARB 34

#define GLX_MAJOR_VERSION 1
#define GLX_MINOR_VERSION 4
#define DEFAULT_FBCONFIG_ID 1

/* GLX-Enums wie GLXEnums.java */
#define GLX_VENDOR 1
#define GLX_VERSION 2
#define GLX_EXTENSIONS 3
#define GLX_FBCONFIG_ID 0x8013
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_ALPHA_SIZE 11
#define GLX_DEPTH_SIZE 12
#define GLX_STENCIL_SIZE 13
#define GLX_BUFFER_SIZE 2
#define GLX_DOUBLEBUFFER 5
#define GLX_DRAWABLE_TYPE 0x8010
#define GLX_WINDOW_BIT 1
#define GLX_RENDER_TYPE 0x8011
#define GLX_RGBA_BIT 1
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092

#define MAX_CONTEXTS 64

/* vom Server erwartet (eigentlich aus gpu_helper.c / sysvshared_memory.c, die auf dem PC nicht gebaut werden) */
EGLContext globalEGLContext = EGL_NO_CONTEXT;

int ashmemCreateRegion(const char* name, int64_t size) {
    int fd = syscall(SYS_memfd_create, name, 0);
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int createMemoryFd(const char* name, int64_t size) {
    return ashmemCreateRegion(name, size);
}

typedef struct {
    int id;
    void* ptr;
} IdEntry;

typedef struct Client {
    int fd;
    uint16_t sequence;
    IdEntry glContexts[MAX_CONTEXTS];   /* Thread-Kontexte (GLContext*) */
    IdEntry glxContexts[MAX_CONTEXTS];  /* GLXContext* */
    short windowSize[2];
    int frames;
    struct timeval lastFpsTime;
    int lastFpsFrames;
} Client;

static pthread_mutex_t clientMutex = PTHREAD_MUTEX_INITIALIZER;
static const char* glxExtensions = "GLX_ARB_create_context GLX_ARB_get_proc_address";
static Display* xDisplay = NULL;
static int fallbackWidth = 640, fallbackHeight = 480;
static const char* dumpDir = NULL;
static const char* dumpFrames = "1,10";
static EGLDisplay eglDisplay;

static int xErrorHandler(Display* display, XErrorEvent* event) {
    /* z. B. BadDrawable, wenn das Fenster schon wieder weg ist: nicht abbrechen */
    char text[128];
    XGetErrorText(display, event->error_code, text, sizeof(text));
    fprintf(stderr, "gladio-pc: X-Fehler %s (Anfrage %d, Ressource 0x%lx)\n", text, event->request_code, event->resourceid);
    return 0;
}

static void logmsg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "gladio-pc: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---------------------------------------------------------------- Kontext-Tabellen */

static void* idTableGet(IdEntry* table, int id) {
    for (int i = 0; i < MAX_CONTEXTS; i++) if (table[i].ptr && table[i].id == id) return table[i].ptr;
    return NULL;
}

static bool idTablePut(IdEntry* table, int id, void* ptr) {
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (!table[i].ptr) {
            table[i].id = id;
            table[i].ptr = ptr;
            return true;
        }
    }
    return false;
}

static void idTableRemove(IdEntry* table, int id) {
    for (int i = 0; i < MAX_CONTEXTS; i++) if (table[i].ptr && table[i].id == id) table[i].ptr = NULL;
}

/* ---------------------------------------------------------------- JNI-Attrappe */

enum { M_GET_WINDOW_SIZE = 1, M_CLEAR_WINDOW_CONTENT, M_UPDATE_WINDOW_CONTENT, M_GET_GLX_CONTEXT_PTR, M_PRESENT_WINDOW_TEXTURE, M_RELEASE_WINDOW_TEXTURE };

static struct JNINativeInterface_ jniTable;
static const struct JNINativeInterface_* jniEnv = &jniTable;
static struct JNIInvokeInterface_ jvmTable;
static const struct JNIInvokeInterface_* jvmIface = &jvmTable;

static jclass stub_GetObjectClass(JNIEnv* env, jobject obj) {
    return (jclass)obj;
}

static jmethodID stub_GetMethodID(JNIEnv* env, jclass cls, const char* name, const char* sig) {
    if (!strcmp(name, "getWindowSize")) return (jmethodID)(intptr_t)M_GET_WINDOW_SIZE;
    if (!strcmp(name, "clearWindowContent")) return (jmethodID)(intptr_t)M_CLEAR_WINDOW_CONTENT;
    if (!strcmp(name, "updateWindowContent")) return (jmethodID)(intptr_t)M_UPDATE_WINDOW_CONTENT;
    if (!strcmp(name, "getGLXContextPtr")) return (jmethodID)(intptr_t)M_GET_GLX_CONTEXT_PTR;
    if (!strcmp(name, "presentWindowTexture")) return (jmethodID)(intptr_t)M_PRESENT_WINDOW_TEXTURE;
    if (!strcmp(name, "releaseWindowTexture")) return (jmethodID)(intptr_t)M_RELEASE_WINDOW_TEXTURE;
    logmsg("JNI: unbekannte Methode %s%s", name, sig);
    return NULL;
}

static bool queryWindowSize(int windowId, short* w, short* h) {
    if (xDisplay) {
        Window root;
        int x, y;
        unsigned int width, height, border, depth;
        if (XGetGeometry(xDisplay, (Drawable)windowId, &root, &x, &y, &width, &height, &border, &depth)) {
            *w = (short)width;
            *h = (short)height;
            return true;
        }
        logmsg("XGetGeometry für Fenster 0x%x fehlgeschlagen", windowId);
        *w = *h = 0;
        return false;
    }
    *w = (short)fallbackWidth;
    *h = (short)fallbackHeight;
    return true;
}

static jobject stub_CallObjectMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    Client* client = (Client*)obj;
    va_list ap;
    va_start(ap, mid);
    if ((intptr_t)mid == M_GET_WINDOW_SIZE) {
        int windowId = va_arg(ap, int);
        queryWindowSize(windowId, &client->windowSize[0], &client->windowSize[1]);
        va_end(ap);
        return (jobject)client->windowSize;
    }
    va_end(ap);
    return NULL;
}

static jshort* stub_GetShortArrayElements(JNIEnv* env, jshortArray array, jboolean* isCopy) {
    if (isCopy) *isCopy = JNI_FALSE;
    return (jshort*)array;
}

static void stub_ReleaseShortArrayElements(JNIEnv* env, jshortArray array, jshort* elems, jint mode) {
}

static void stub_CallVoidMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    /* clearWindowContent/releaseWindowTexture: auf dem Gerät wird die Fenstertextur verworfen, hier nichts zu tun */
}

static bool wantDump(int frame) {
    if (!dumpDir) return false;
    if (!strcmp(dumpFrames, "all")) return true;
    const char* p = dumpFrames;
    while (*p) {
        int n = (int)strtol(p, (char**)&p, 10);
        if (n == frame) return true;
        while (*p == ',' || *p == ' ') p++;
    }
    return false;
}

/* texture > 0: Bild aus dieser Textur lesen (ES-2.0-Pfad: übergebene Fenstertextur), sonst aus dem gebundenen FBO */
static void dumpFrame(Client* client, int w, int h, bool flipY, GLuint texture) {
    unsigned char* px = malloc((size_t)w * h * 4);
    GLint savedFbo = 0;
    GLuint fbo = 0;
    if (texture) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFbo);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    }
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) logmsg("glReadPixels: Fehler 0x%x", err);
    if (texture) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedFbo);
        glDeleteFramebuffers(1, &fbo);
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/frame-%d.ppm", dumpDir, client->frames);
    FILE* f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++) {
            int row = flipY ? h - 1 - y : y;
            for (int x = 0; x < w; x++) fwrite(px + ((size_t)row * w + x) * 4, 1, 3, f);
        }
        fclose(f);
        logmsg("Bild %d nach %s geschrieben", client->frames, path);
    }
    else logmsg("%s: %s", path, strerror(errno));
    free(px);
}

static jboolean stub_CallBooleanMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    Client* client = (Client*)obj;
    va_list ap;
    va_start(ap, mid);
    jboolean result = JNI_TRUE;
    if ((intptr_t)mid == M_UPDATE_WINDOW_CONTENT || (intptr_t)mid == M_PRESENT_WINDOW_TEXTURE) {
        bool present = (intptr_t)mid == M_PRESENT_WINDOW_TEXTURE;
        int drawableId = va_arg(ap, int);
        short width = (short)va_arg(ap, int);
        short height = (short)va_arg(ap, int);
        bool flipY = true;
        GLuint texture = 0;
        if (present) texture = (GLuint)va_arg(ap, int);
        else flipY = va_arg(ap, int) != 0;

        short w, h;
        if (queryWindowSize(drawableId, &w, &h) && (w != width || h != height)) {
            logmsg("Fenster 0x%x ist jetzt %dx%d (Puffer %dx%d), Puffer wird neu angelegt", drawableId, w, h, width, height);
            result = JNI_FALSE;
        }
        else {
            client->frames++;
            if (wantDump(client->frames)) dumpFrame(client, width, height, flipY, texture);

            struct timeval now;
            gettimeofday(&now, NULL);
            double dt = (now.tv_sec - client->lastFpsTime.tv_sec) + (now.tv_usec - client->lastFpsTime.tv_usec) / 1e6;
            if (dt >= 2.0) {
                if (client->lastFpsFrames > 0) {
                    logmsg("%d Bilder, %.1f fps", client->frames, (client->frames - client->lastFpsFrames) / dt);
                }
                client->lastFpsTime = now;
                client->lastFpsFrames = client->frames;
            }
        }
    }
    va_end(ap);
    return result;
}

static jlong stub_CallLongMethod(JNIEnv* env, jobject obj, jmethodID mid, ...) {
    Client* client = (Client*)obj;
    va_list ap;
    va_start(ap, mid);
    jlong result = 0;
    if ((intptr_t)mid == M_GET_GLX_CONTEXT_PTR) {
        int clientFd = va_arg(ap, int);
        int contextId = va_arg(ap, int);
        (void)clientFd;
        pthread_mutex_lock(&clientMutex);
        result = (jlong)(intptr_t)idTableGet(client->glxContexts, contextId);
        pthread_mutex_unlock(&clientMutex);
    }
    va_end(ap);
    return result;
}

static jint stub_GetJavaVM(JNIEnv* env, JavaVM** vm) {
    *vm = (JavaVM*)&jvmIface;
    return 0;
}

static jobject stub_NewGlobalRef(JNIEnv* env, jobject obj) {
    return obj;
}

static void stub_DeleteGlobalRef(JNIEnv* env, jobject obj) {
}

static jint stub_AttachCurrentThread(JavaVM* vm, void** penv, void* args) {
    *penv = (void*)&jniEnv;
    return 0;
}

static jint stub_DetachCurrentThread(JavaVM* vm) {
    return 0;
}

static void initJNIStub() {
    jniTable.GetObjectClass = stub_GetObjectClass;
    jniTable.GetMethodID = stub_GetMethodID;
    jniTable.CallObjectMethod = stub_CallObjectMethod;
    jniTable.GetShortArrayElements = stub_GetShortArrayElements;
    jniTable.ReleaseShortArrayElements = stub_ReleaseShortArrayElements;
    jniTable.CallVoidMethod = stub_CallVoidMethod;
    jniTable.CallBooleanMethod = stub_CallBooleanMethod;
    jniTable.CallLongMethod = stub_CallLongMethod;
    jniTable.GetJavaVM = stub_GetJavaVM;
    jniTable.NewGlobalRef = stub_NewGlobalRef;
    jniTable.DeleteGlobalRef = stub_DeleteGlobalRef;
    jvmTable.AttachCurrentThread = stub_AttachCurrentThread;
    jvmTable.DetachCurrentThread = stub_DetachCurrentThread;
}

/* ---------------------------------------------------------------- X11/GLX-Protokoll */

typedef struct {
    char data[4096];
    int size;
} Reply;

static void replyByte(Reply* r, uint8_t v) { r->data[r->size++] = (char)v; }
static void replyShort(Reply* r, uint16_t v) { memcpy(r->data + r->size, &v, 2); r->size += 2; }
static void replyInt(Reply* r, int32_t v) { memcpy(r->data + r->size, &v, 4); r->size += 4; }
static void replyPad(Reply* r, int n) { memset(r->data + r->size, 0, n); r->size += n; }
static void replyString8(Reply* r, const char* s) {
    int len = (int)strlen(s);
    memcpy(r->data + r->size, s, len);
    r->size += len;
    replyPad(r, -len & 3);
}

static void replyHeader(Reply* r, Client* client, bool success, int32_t length) {
    r->size = 0;
    replyByte(r, success ? 1 : 0);
    replyByte(r, 0);
    replyShort(r, client->sequence);
    replyInt(r, length);
}

static bool sendReply(Client* client, Reply* r) {
    return sock_write(client->fd, r->data, r->size) == r->size;
}

static bool readInt(Client* client, int32_t* v) {
    return sock_read(client->fd, (char*)v, 4) == 4;
}

static bool skipBytes(Client* client, int n) {
    char buf[256];
    while (n > 0) {
        int chunk = n > (int)sizeof(buf) ? (int)sizeof(buf) : n;
        if (sock_read(client->fd, buf, chunk) != chunk) return false;
        n -= chunk;
    }
    return true;
}

static bool handleAuth(Client* client) {
    char req[12];
    if (sock_read(client->fd, req, 12) != 12) return false;
    uint16_t major;
    memcpy(&major, req + 2, 2);
    if (major != 11) {
        logmsg("X-Protokollversion %d nicht unterstützt", major);
        return false;
    }
    uint16_t nameLength, dataLength;
    memcpy(&nameLength, req + 6, 2);
    memcpy(&dataLength, req + 8, 2);
    if (!skipBytes(client, (nameLength + 3) & ~3)) return false;
    if (!skipBytes(client, (dataLength + 3) & ~3)) return false;

    /* Der Gladio-Client liest nur den 8-Byte-Kopf und überspringt die Zusatzdaten. */
    char reply[8] = {0};
    reply[0] = 1;
    uint16_t v = 11;
    memcpy(reply + 2, &v, 2);
    v = 0;
    memcpy(reply + 4, &v, 2);
    memcpy(reply + 6, &v, 2);
    return sock_write(client->fd, reply, 8) == 8;
}

static bool glxCreateGLContext(Client* client) {
    int32_t contextId;
    if (!readInt(client, &contextId)) return false;
    GLContext* context = createGLContext((JNIEnv*)&jniEnv, (jobject)client, client->fd);
    if (!context) {
        logmsg("createGLContext fehlgeschlagen");
        return false;
    }
    pthread_mutex_lock(&clientMutex);
    idTablePut(client->glContexts, contextId, context);
    pthread_mutex_unlock(&clientMutex);
    logmsg("GL-Kontext für Thread %d angelegt", contextId);
    return true;
}

static bool glxDestroyGLContext(Client* client) {
    int32_t contextId;
    if (!readInt(client, &contextId)) return false;
    pthread_mutex_lock(&clientMutex);
    GLContext* context = idTableGet(client->glContexts, contextId);
    idTableRemove(client->glContexts, contextId);
    pthread_mutex_unlock(&clientMutex);
    if (context) destroyGLContext((JNIEnv*)&jniEnv, context);

    Reply r;
    replyHeader(&r, client, true, 0);
    replyPad(&r, 24);
    return sendReply(client, &r);
}

static bool createGLXContextForClient(Client* client, int contextId, int shareContextId) {
    pthread_mutex_lock(&clientMutex);
    GLXContext* shared = shareContextId > 0 ? idTableGet(client->glxContexts, shareContextId) : NULL;
    pthread_mutex_unlock(&clientMutex);

    GLXContext* context = createGLXContext(contextId, shared);
    if (!context) {
        logmsg("createGLXContext(%d) fehlgeschlagen", contextId);
        return false;
    }
    pthread_mutex_lock(&clientMutex);
    idTablePut(client->glxContexts, contextId, context);
    pthread_mutex_unlock(&clientMutex);
    logmsg("GLX-Kontext %d angelegt%s", contextId, shared ? " (geteilt)" : "");
    return true;
}

static bool glxCreateContext(Client* client) {
    int32_t contextId, shareList;
    char isDirect;
    if (!readInt(client, &contextId)) return false;
    if (!skipBytes(client, 8)) return false;
    if (!readInt(client, &shareList)) return false;
    if (sock_read(client->fd, &isDirect, 1) != 1) return false;

    bool ok = contextId != 0 && createGLXContextForClient(client, contextId, shareList);
    Reply r;
    replyHeader(&r, client, ok, 0);
    replyPad(&r, 24);
    return sendReply(client, &r);
}

static bool glxDestroyContext(Client* client) {
    int32_t contextId;
    if (!readInt(client, &contextId)) return false;
    pthread_mutex_lock(&clientMutex);
    GLXContext* context = idTableGet(client->glxContexts, contextId);
    idTableRemove(client->glxContexts, contextId);
    pthread_mutex_unlock(&clientMutex);
    if (context) destroyGLXContext(context);
    return true;
}

static bool glxQueryVersion(Client* client) {
    if (!skipBytes(client, 8)) return false;
    Reply r;
    replyHeader(&r, client, true, 0);
    replyInt(&r, GLX_MAJOR_VERSION);
    replyInt(&r, GLX_MINOR_VERSION);
    replyPad(&r, 16);
    return sendReply(client, &r);
}

static bool glxSendString(Client* client, const char* s) {
    int length = (int)strlen(s);
    Reply r;
    replyHeader(&r, client, true, (length + (-length & 3)) / 4);
    replyInt(&r, 0);
    replyInt(&r, length);
    replyPad(&r, 16);
    replyString8(&r, s);
    return sendReply(client, &r);
}

static bool glxQueryExtensionsString(Client* client) {
    if (!skipBytes(client, 4)) return false;
    return glxSendString(client, glxExtensions);
}

static bool glxQueryServerString(Client* client) {
    int32_t name;
    if (!skipBytes(client, 4)) return false;
    if (!readInt(client, &name)) return false;
    const char* s = "";
    char version[16];
    switch (name) {
        case GLX_VENDOR: s = "Winlator"; break;
        case GLX_VERSION:
            snprintf(version, sizeof(version), "%d.%d", GLX_MAJOR_VERSION, GLX_MINOR_VERSION);
            s = version;
            break;
        case GLX_EXTENSIONS: s = glxExtensions; break;
    }
    return glxSendString(client, s);
}

static bool glxGetFBConfigs(Client* client) {
    if (!skipBytes(client, 4)) return false;
    static const int32_t properties[] = {
        GLX_FBCONFIG_ID, DEFAULT_FBCONFIG_ID,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        GLX_STENCIL_SIZE, 8,
        GLX_BUFFER_SIZE, 32,
        GLX_DOUBLEBUFFER, 1,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT
    };
    const int numFBConfigs = 1;
    const int numProperties = sizeof(properties) / sizeof(properties[0]) / 2;
    Reply r;
    replyHeader(&r, client, true, 2 * numFBConfigs * numProperties);
    replyInt(&r, numFBConfigs);
    replyInt(&r, numProperties);
    replyPad(&r, 16);
    for (int i = 0; i < numProperties * 2; i++) replyInt(&r, properties[i]);
    return sendReply(client, &r);
}

static bool glxCreateContextAttribsARB(Client* client) {
    int32_t contextId, fbConfigId, shareContext, numAttribs;
    if (!readInt(client, &contextId)) return false;
    if (!readInt(client, &fbConfigId)) return false;
    if (!skipBytes(client, 4)) return false;
    if (!readInt(client, &shareContext)) return false;
    if (!skipBytes(client, 4)) return false;
    if (!readInt(client, &numAttribs)) return false;

    int glMajor = 3, glMinor = 3;
    for (int i = 0; i < numAttribs; i++) {
        int32_t name, value;
        if (!readInt(client, &name) || !readInt(client, &value)) return false;
        if (name == GLX_CONTEXT_MAJOR_VERSION_ARB) glMajor = value;
        else if (name == GLX_CONTEXT_MINOR_VERSION_ARB) glMinor = value;
    }

    bool success = contextId != 0 && fbConfigId == DEFAULT_FBCONFIG_ID && glMajor <= 3 && glMinor <= 3;
    if (!success) logmsg("CreateContextAttribsARB: GL %d.%d / FBConfig %d abgelehnt", glMajor, glMinor, fbConfigId);
    if (success) success = createGLXContextForClient(client, contextId, shareContext);

    Reply r;
    replyHeader(&r, client, success, 0);
    replyPad(&r, 24);
    return sendReply(client, &r);
}

static void* clientThread(void* param) {
    Client* client = param;
    if (!handleAuth(client)) {
        logmsg("Verbindungsaufnahme fehlgeschlagen");
        goto done;
    }
    logmsg("Client verbunden (fd %d)", client->fd);

    for (;;) {
        char header[4];
        int n = sock_read(client->fd, header, 4);
        if (n != 4) break;
        client->sequence++;
        int8_t opcode = (int8_t)header[0];
        uint8_t minor = (uint8_t)header[1];
        uint16_t length;
        memcpy(&length, header + 2, 2);

        bool ok;
        if (opcode != (int8_t)GLX_EXT_MAJOR_OPCODE) {
            logmsg("Unerwartete X-Anfrage Opcode %d (Länge %d), wird übersprungen", opcode, length);
            ok = skipBytes(client, length * 4 - 4);
        }
        else {
            switch (minor) {
                case GLX_OPCODE_CREATE_GL_CONTEXT: ok = glxCreateGLContext(client); break;
                case GLX_OPCODE_DESTROY_GL_CONTEXT: ok = glxDestroyGLContext(client); break;
                case GLX_OPCODE_CREATE_CONTEXT: ok = glxCreateContext(client); break;
                case GLX_OPCODE_DESTROY_CONTEXT: ok = glxDestroyContext(client); break;
                case GLX_OPCODE_QUERY_VERSION: ok = glxQueryVersion(client); break;
                case GLX_OPCODE_QUERY_EXTENSIONS_STRING: ok = glxQueryExtensionsString(client); break;
                case GLX_OPCODE_QUERY_SERVER_STRING: ok = glxQueryServerString(client); break;
                case GLX_OPCODE_GET_FB_CONFIGS: ok = glxGetFBConfigs(client); break;
                case GLX_OPCODE_CREATE_CONTEXT_ATTRIBS_ARB: ok = glxCreateContextAttribsARB(client); break;
                default:
                    logmsg("Unbekannte GLX-Anfrage %d", minor);
                    ok = skipBytes(client, length * 4 - 4);
                    break;
            }
        }
        if (!ok) break;
    }

done:
    logmsg("Client getrennt (fd %d, %d Bilder)", client->fd, client->frames);
    /* wie onDestroyClientListener: erst die Thread-Kontexte, dann die GLX-Kontexte */
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (client->glContexts[i].ptr) {
            destroyGLContext((JNIEnv*)&jniEnv, client->glContexts[i].ptr);
            client->glContexts[i].ptr = NULL;
        }
    }
    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (client->glxContexts[i].ptr) {
            destroyGLXContext(client->glxContexts[i].ptr);
            client->glxContexts[i].ptr = NULL;
        }
    }
    close(client->fd);
    free(client);
    return NULL;
}

/* ---------------------------------------------------------------- App-Kontext (wie GLSurfaceView) */

static bool createAppContext(int esVersion) {
    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY) {
        logmsg("eglGetDisplay fehlgeschlagen (EGL_PLATFORM=surfaceless gesetzt?)");
        return false;
    }
    EGLint major, minor;
    if (!eglInitialize(eglDisplay, &major, &minor)) {
        logmsg("eglInitialize fehlgeschlagen: 0x%x", eglGetError());
        return false;
    }
    logmsg("EGL %d.%d, %s", major, minor, eglQueryString(eglDisplay, EGL_VENDOR));

    const EGLint confAttribs[] = {
        EGL_RENDERABLE_TYPE, esVersion >= 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, esVersion, EGL_NONE };
    EGLConfig config;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(eglDisplay, confAttribs, &config, 1, &numConfigs) || numConfigs != 1) {
        logmsg("eglChooseConfig fehlgeschlagen: 0x%x", eglGetError());
        return false;
    }
    EGLContext ctx = eglCreateContext(eglDisplay, config, EGL_NO_CONTEXT, ctxAttribs);
    if (ctx == EGL_NO_CONTEXT) {
        logmsg("eglCreateContext fehlgeschlagen: 0x%x", eglGetError());
        return false;
    }
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    logmsg("App-Kontext: %s / %s / GLSL %s", glGetString(GL_VERSION), glGetString(GL_RENDERER), glGetString(GL_SHADING_LANGUAGE_VERSION));
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    globalEGLContext = ctx;
    return true;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    const char* sockPath = getenv("GLADIO_SOCKET");
    if (!sockPath) sockPath = "/tmp/gladio-pc.sock";
    int esVersion = getenv("GLADIO_PC_ES") ? atoi(getenv("GLADIO_PC_ES")) : 3;
    dumpDir = getenv("GLADIO_PC_DUMP");
    if (dumpDir && !dumpDir[0]) dumpDir = NULL;
    if (getenv("GLADIO_PC_DUMP_FRAMES")) dumpFrames = getenv("GLADIO_PC_DUMP_FRAMES");
    if (getenv("GLADIO_PC_SIZE")) sscanf(getenv("GLADIO_PC_SIZE"), "%dx%d", &fallbackWidth, &fallbackHeight);
    if (dumpDir) mkdir(dumpDir, 0755);

    if (!getenv("EGL_PLATFORM")) setenv("EGL_PLATFORM", "surfaceless", 0);

    XInitThreads();
    XSetErrorHandler(xErrorHandler);
    xDisplay = XOpenDisplay(NULL);
    if (xDisplay) logmsg("Fenstergrößen kommen vom X-Server %s", XDisplayString(xDisplay));
    else logmsg("kein $DISPLAY, Fenstergröße fest %dx%d", fallbackWidth, fallbackHeight);

    initJNIStub();
    if (!createAppContext(esVersion)) return 1;

    int listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sockPath, sizeof(addr.sun_path) - 1);
    unlink(sockPath);
    if (bind(listenFd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(listenFd, 8) < 0) {
        logmsg("%s: %s", sockPath, strerror(errno));
        return 1;
    }
    logmsg("warte auf Gladio-Clients an %s (GLADIO_SOCKET)", sockPath);

    for (;;) {
        int fd = accept(listenFd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        Client* client = calloc(1, sizeof(Client));
        client->fd = fd;
        gettimeofday(&client->lastFpsTime, NULL);
        pthread_t thread;
        pthread_create(&thread, NULL, clientThread, client);
        pthread_detach(thread);
    }
    return 0;
}
