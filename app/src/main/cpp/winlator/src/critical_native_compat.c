/*
 * @CriticalNative wirkt erst ab Android 8.0 (API 26). Ältere ART-Versionen ignorieren die
 * Annotation und rufen solche Methoden wie gewöhnliche JNI-Methoden auf, also mit JNIEnv* und
 * jclass vor den eigentlichen Argumenten. Die Implementierungen (Java_..._readByte usw.) erwarten
 * aber nur die Argumente. Auf Android 7 werden deshalb beim Laden der Bibliothek Wrapper mit der
 * normalen JNI-Signatur registriert; ab Android 8 bleibt alles wie im Original.
 */
#include <jni.h>
#include <stdlib.h>
#include <sys/system_properties.h>

#define XIN(name) Java_com_winlator_xconnector_XInputStream_##name
#define XOUT(name) Java_com_winlator_xconnector_XOutputStream_##name

extern jbyte XIN(readByte)(jlong nativePtr);
extern jshort XIN(readShort)(jlong nativePtr);
extern jint XIN(readInt)(jlong nativePtr);
extern jlong XIN(readLong)(jlong nativePtr);
extern void XIN(skip)(jlong nativePtr, jint length);
extern jint XIN(available)(jlong nativePtr);
extern jint XIN(readMoreData)(jlong nativePtr, jboolean canReceiveAncillaryMessages);
extern jint XIN(getActivePosition)(jlong nativePtr);
extern void XIN(setActivePosition)(jlong nativePtr, jint activePosition);
extern jint XIN(getAncillaryFd)(jlong nativePtr);
extern void XIN(destroy)(jlong nativePtr);

extern void XOUT(setAncillaryFd)(jlong nativePtr, jint ancillaryFd);
extern void XOUT(writeByte)(jlong nativePtr, jbyte value);
extern void XOUT(writeShort)(jlong nativePtr, jshort value);
extern void XOUT(writeInt)(jlong nativePtr, jint value);
extern void XOUT(writeLong)(jlong nativePtr, jlong value);
extern void XOUT(writePad)(jlong nativePtr, jint length);
extern jboolean XOUT(sendData)(jlong nativePtr);
extern void XOUT(destroy)(jlong nativePtr);
extern jint XOUT(length)(jlong nativePtr);

extern void Java_com_winlator_xconnector_XConnectorEpoll_closeFd(jint fd);
extern jint Java_com_winlator_core_GPUHelper_vkGetApiVersion(void);

static jbyte in_readByte(JNIEnv* env, jclass cls, jlong p) { return XIN(readByte)(p); }
static jshort in_readShort(JNIEnv* env, jclass cls, jlong p) { return XIN(readShort)(p); }
static jint in_readInt(JNIEnv* env, jclass cls, jlong p) { return XIN(readInt)(p); }
static jlong in_readLong(JNIEnv* env, jclass cls, jlong p) { return XIN(readLong)(p); }
static void in_skip(JNIEnv* env, jclass cls, jlong p, jint length) { XIN(skip)(p, length); }
static jint in_available(JNIEnv* env, jclass cls, jlong p) { return XIN(available)(p); }
static jint in_readMoreData(JNIEnv* env, jclass cls, jlong p, jboolean ancillary) { return XIN(readMoreData)(p, ancillary); }
static jint in_getActivePosition(JNIEnv* env, jclass cls, jlong p) { return XIN(getActivePosition)(p); }
static void in_setActivePosition(JNIEnv* env, jclass cls, jlong p, jint position) { XIN(setActivePosition)(p, position); }
static jint in_getAncillaryFd(JNIEnv* env, jclass cls, jlong p) { return XIN(getAncillaryFd)(p); }
static void in_destroy(JNIEnv* env, jclass cls, jlong p) { XIN(destroy)(p); }

static void out_setAncillaryFd(JNIEnv* env, jclass cls, jlong p, jint fd) { XOUT(setAncillaryFd)(p, fd); }
static void out_writeByte(JNIEnv* env, jclass cls, jlong p, jbyte value) { XOUT(writeByte)(p, value); }
static void out_writeShort(JNIEnv* env, jclass cls, jlong p, jshort value) { XOUT(writeShort)(p, value); }
static void out_writeInt(JNIEnv* env, jclass cls, jlong p, jint value) { XOUT(writeInt)(p, value); }
static void out_writeLong(JNIEnv* env, jclass cls, jlong p, jlong value) { XOUT(writeLong)(p, value); }
static void out_writePad(JNIEnv* env, jclass cls, jlong p, jint length) { XOUT(writePad)(p, length); }
static jboolean out_sendData(JNIEnv* env, jclass cls, jlong p) { return XOUT(sendData)(p); }
static void out_destroy(JNIEnv* env, jclass cls, jlong p) { XOUT(destroy)(p); }
static jint out_length(JNIEnv* env, jclass cls, jlong p) { return XOUT(length)(p); }

static void epoll_closeFd(JNIEnv* env, jclass cls, jint fd) { Java_com_winlator_xconnector_XConnectorEpoll_closeFd(fd); }
static jint gpu_vkGetApiVersion(JNIEnv* env, jclass cls) { return Java_com_winlator_core_GPUHelper_vkGetApiVersion(); }

static const JNINativeMethod inputStreamMethods[] = {
    {"readByte", "(J)B", (void*)in_readByte},
    {"readShort", "(J)S", (void*)in_readShort},
    {"readInt", "(J)I", (void*)in_readInt},
    {"readLong", "(J)J", (void*)in_readLong},
    {"skip", "(JI)V", (void*)in_skip},
    {"available", "(J)I", (void*)in_available},
    {"readMoreData", "(JZ)I", (void*)in_readMoreData},
    {"getActivePosition", "(J)I", (void*)in_getActivePosition},
    {"setActivePosition", "(JI)V", (void*)in_setActivePosition},
    {"getAncillaryFd", "(J)I", (void*)in_getAncillaryFd},
    {"destroy", "(J)V", (void*)in_destroy},
};

static const JNINativeMethod outputStreamMethods[] = {
    {"setAncillaryFd", "(JI)V", (void*)out_setAncillaryFd},
    {"writeByte", "(JB)V", (void*)out_writeByte},
    {"writeShort", "(JS)V", (void*)out_writeShort},
    {"writeInt", "(JI)V", (void*)out_writeInt},
    {"writeLong", "(JJ)V", (void*)out_writeLong},
    {"writePad", "(JI)V", (void*)out_writePad},
    {"sendData", "(J)Z", (void*)out_sendData},
    {"destroy", "(J)V", (void*)out_destroy},
    {"length", "(J)I", (void*)out_length},
};

static const JNINativeMethod epollMethods[] = {
    {"closeFd", "(I)V", (void*)epoll_closeFd},
};

static const JNINativeMethod gpuHelperMethods[] = {
    {"vkGetApiVersion", "()I", (void*)gpu_vkGetApiVersion},
};

static int deviceApiLevel(void) {
    char value[PROP_VALUE_MAX] = {0};
    if (__system_property_get("ro.build.version.sdk", value) <= 0) return 0;
    return atoi(value);
}

static void registerMethods(JNIEnv* env, const char* className, const JNINativeMethod* methods, int count) {
    jclass cls = (*env)->FindClass(env, className);
    if (!cls) {
        (*env)->ExceptionClear(env);
        return;
    }
    if ((*env)->RegisterNatives(env, cls, methods, count) != JNI_OK) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
}

#define COUNT(array) (int)(sizeof(array) / sizeof(array[0]))

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    JNIEnv* env;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_VERSION_1_6;

    int apiLevel = deviceApiLevel();
    if (apiLevel > 0 && apiLevel < 26) {
        registerMethods(env, "com/winlator/xconnector/XInputStream", inputStreamMethods, COUNT(inputStreamMethods));
        registerMethods(env, "com/winlator/xconnector/XOutputStream", outputStreamMethods, COUNT(outputStreamMethods));
        registerMethods(env, "com/winlator/xconnector/XConnectorEpoll", epollMethods, COUNT(epollMethods));
        registerMethods(env, "com/winlator/core/GPUHelper", gpuHelperMethods, COUNT(gpuHelperMethods));
    }
    return JNI_VERSION_1_6;
}
