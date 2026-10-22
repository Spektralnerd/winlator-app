#include <android/log.h>
#include <android/native_window.h>

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <jni.h>
#include <string.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "native_handle.h"
#include "ahb_compat.h"
#include "winlator.h"

#define HAL_PIXEL_FORMAT_BGRA_8888 5

#if __ANDROID_API__ >= 26
extern const native_handle_t* _Nullable AHardwareBuffer_getNativeHandle(const AHardwareBuffer* _Nonnull buffer);
#else
/* eglGetNativeClientBufferANDROID kommt erst mit Android 8.0 in die NDK-Stubs von libEGL */
static EGLClientBuffer getNativeClientBufferCompat(const struct AHardwareBuffer* buffer) {
    static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC function = NULL;
    static bool resolved = false;
    if (!resolved) {
        function = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
        resolved = true;
    }
    return function ? function(buffer) : NULL;
}
#define eglGetNativeClientBufferANDROID getNativeClientBufferCompat
#endif

int AHardwareBuffer_getFd(AHardwareBuffer* hardwareBuffer) {
    const native_handle_t* nativeHandle = AHardwareBuffer_getNativeHandle(hardwareBuffer);
    return nativeHandle && nativeHandle->numFds > 0 ? nativeHandle->data[0] : -1;
}

EGLImageKHR createImageKHR(AHardwareBuffer* hardwareBuffer, int textureId) {
    const EGLint attribList[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    if (!hardwareBuffer) return NULL;

    AHardwareBuffer_acquire(hardwareBuffer);

    EGLClientBuffer clientBuffer = eglGetNativeClientBufferANDROID(hardwareBuffer);
    if (!clientBuffer) return NULL;

    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLImageKHR imageKHR = eglCreateImageKHR(eglDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, clientBuffer, attribList);
    if (!imageKHR) return NULL;

    glBindTexture(GL_TEXTURE_2D, textureId);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, imageKHR);
    glBindTexture(GL_TEXTURE_2D, 0);

    return imageKHR;
}

AHardwareBuffer* createHardwareBuffer(int width, int height, bool cpuAccess, bool useHALPixelFormatBGRA8888) {
    AHardwareBuffer_Desc buffDesc = {0};
    buffDesc.width = width;
    buffDesc.height = height;
    buffDesc.layers = 1;
    buffDesc.usage = cpuAccess ? AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN : AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
    buffDesc.format = useHALPixelFormatBGRA8888 ? HAL_PIXEL_FORMAT_BGRA_8888 : AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;

    AHardwareBuffer* hardwareBuffer = NULL;
    AHardwareBuffer_allocate(&buffDesc, &hardwareBuffer);

    return hardwareBuffer;
}

/* AHardwareBuffer (Android 8.0+) und die passende EGL-Erweiterung vorhanden? */
JNIEXPORT jboolean JNICALL
Java_com_winlator_renderer_GPUImage_isSupportedNative(JNIEnv *env, jclass obj) {
    if (!WinlatorAHB_isAvailable()) return JNI_FALSE;
#if __ANDROID_API__ < 26
    if (!eglGetProcAddress("eglGetNativeClientBufferANDROID")) return JNI_FALSE;
#endif
    return JNI_TRUE;
}

JNIEXPORT jlong JNICALL
Java_com_winlator_renderer_GPUImage_createHardwareBuffer(JNIEnv *env, jclass obj, jshort width,
                                                         jshort height, jboolean cpuAccess, jboolean useHALPixelFormatBGRA8888) {
    AHardwareBuffer* hardwareBuffer = createHardwareBuffer(width, height, cpuAccess, useHALPixelFormatBGRA8888);
    if (hardwareBuffer) {
        jclass cls = (*env)->GetObjectClass(env, obj);

        AHardwareBuffer_Desc buffDesc = {0};
        AHardwareBuffer_describe(hardwareBuffer, &buffDesc);

        jmethodID setStride = (*env)->GetMethodID(env, cls, "setStride", "(S)V");
        (*env)->CallVoidMethod(env, obj, setStride, (jshort)buffDesc.stride);

        int fd = AHardwareBuffer_getFd(hardwareBuffer);
        if (fd != -1) {
            jmethodID setNativeHandle = (*env)->GetMethodID(env, cls, "setNativeHandle", "(I)V");
            (*env)->CallVoidMethod(env, obj, setNativeHandle, fd);
        }
    }
    return (jlong)hardwareBuffer;
}

JNIEXPORT jlong JNICALL
Java_com_winlator_renderer_GPUImage_createImageKHR(JNIEnv *env, jclass obj,
                                                   jlong hardwareBufferPtr, jint textureId) {
    return (jlong)createImageKHR((AHardwareBuffer*)hardwareBufferPtr, textureId);
}

JNIEXPORT void JNICALL
Java_com_winlator_renderer_GPUImage_destroyHardwareBuffer(JNIEnv *env, jclass obj,
                                                          jlong hardwareBufferPtr, jboolean locked) {
    AHardwareBuffer* hardwareBuffer = (AHardwareBuffer*)hardwareBufferPtr;
    if (hardwareBuffer) {
        if (locked) {
            AHardwareBuffer_unlock(hardwareBuffer, NULL);
            locked = false;
        }
        AHardwareBuffer_release(hardwareBuffer);
    }
}

JNIEXPORT jobject JNICALL
Java_com_winlator_renderer_GPUImage_lockHardwareBuffer(JNIEnv *env, jclass obj,
                                                       jlong hardwareBufferPtr) {
    AHardwareBuffer* hardwareBuffer = (AHardwareBuffer*)hardwareBufferPtr;
    void *virtualAddr = NULL;
    AHardwareBuffer_lock(hardwareBuffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &virtualAddr);
    if (!virtualAddr) return NULL;

    AHardwareBuffer_Desc buffDesc = {0};
    AHardwareBuffer_describe(hardwareBuffer, &buffDesc);

    jlong size = buffDesc.stride * buffDesc.height * 4;
    return (*env)->NewDirectByteBuffer(env, virtualAddr, size);
}

JNIEXPORT void JNICALL
Java_com_winlator_renderer_GPUImage_destroyImageKHR(JNIEnv *env, jclass obj, jlong imageKHRPtr) {
    EGLImageKHR imageKHR = (EGLImageKHR)imageKHRPtr;
    if (imageKHR) {
        EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        eglDestroyImageKHR(eglDisplay, imageKHR);
    }
}