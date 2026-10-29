#include "gl_context.h"
#include "gl_diag.h"

JNIEXPORT jlong JNICALL
Java_com_winlator_xserver_extensions_GLXExtension_createGLContext(JNIEnv *env, jobject obj,
                                                                  jint clientFd) {
    GLContext* context = createGLContext(env, obj, clientFd);
    return (jlong)context;
}

JNIEXPORT void JNICALL
Java_com_winlator_xserver_extensions_GLXExtension_destroyGLContext(JNIEnv *env, jobject obj,
                                                                   jlong contextPtr) {
    destroyGLContext(env, (GLContext*)contextPtr);
}

JNIEXPORT jlong JNICALL
Java_com_winlator_xserver_extensions_GLXExtension_createGLXContext(JNIEnv *env, jobject obj,
                                                                   jint contextId, jlong sharedContextPtr) {
    GLXContext* context = createGLXContext(contextId, (GLXContext*)sharedContextPtr);
    return (jlong)context;
}

JNIEXPORT void JNICALL
Java_com_winlator_xserver_extensions_GLXExtension_destroyGLXContext(JNIEnv *env, jobject obj,
                                                                    jlong contextPtr) {
    destroyGLXContext((GLXContext*)contextPtr);
}
/* Ordner für Diagnosedateien (Documents/Winlator): Quelltexte nicht übersetzter Shader usw. */
JNIEXPORT void JNICALL
Java_com_winlator_xserver_extensions_GLXExtension_setDiagnosticsDir(JNIEnv *env, jclass cls, jstring dir) {
    const char* path = dir ? (*env)->GetStringUTFChars(env, dir, NULL) : NULL;
    GladioDiag_setDir(path);
    if (path) (*env)->ReleaseStringUTFChars(env, dir, path);
}

/* App-Menü „Bild aufzeichnen (Gladio)“: das nächste vollständige Bild aufzeichnen */
JNIEXPORT void JNICALL
Java_com_winlator_xserver_extensions_GLXExtension_requestFrameCapture(JNIEnv *env, jclass cls, jstring dir) {
    const char* path = dir ? (*env)->GetStringUTFChars(env, dir, NULL) : NULL;
    GladioDiag_requestCapture(path);
    if (path) (*env)->ReleaseStringUTFChars(env, dir, path);
}
