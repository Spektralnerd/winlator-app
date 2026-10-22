#include "ahb_compat.h"

#if __ANDROID_API__ < 26

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>

/* Zur Laufzeit aufgelöste AHardwareBuffer-Funktionen (siehe ahb_compat.h) */
static struct {
    int (*allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
    void (*acquire)(AHardwareBuffer*);
    void (*release)(AHardwareBuffer*);
    void (*describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
    int (*lock)(AHardwareBuffer*, uint64_t, int32_t, const ARect*, void**);
    int (*unlock)(AHardwareBuffer*, int32_t*);
    const native_handle_t* (*getNativeHandle)(const AHardwareBuffer*);
} ahb;

static pthread_once_t ahbOnce = PTHREAD_ONCE_INIT;

static void* findSymbol(const char* name) {
    static const char* libs[] = {"libandroid.so", "libnativewindow.so"};
    for (int i = 0; i < 2; i++) {
        void* handle = dlopen(libs[i], RTLD_NOW);
        if (!handle) continue;
        void* symbol = dlsym(handle, name);
        if (symbol) return symbol;
    }
    return NULL;
}

static void loadFunctions(void) {
    ahb.allocate = findSymbol("AHardwareBuffer_allocate");
    ahb.acquire = findSymbol("AHardwareBuffer_acquire");
    ahb.release = findSymbol("AHardwareBuffer_release");
    ahb.describe = findSymbol("AHardwareBuffer_describe");
    ahb.lock = findSymbol("AHardwareBuffer_lock");
    ahb.unlock = findSymbol("AHardwareBuffer_unlock");
    ahb.getNativeHandle = findSymbol("AHardwareBuffer_getNativeHandle");
}

bool WinlatorAHB_isAvailable(void) {
    pthread_once(&ahbOnce, loadFunctions);
    return ahb.allocate && ahb.acquire && ahb.release && ahb.describe && ahb.lock && ahb.unlock;
}

int WinlatorAHB_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer) {
    if (!WinlatorAHB_isAvailable()) {
        if (outBuffer) *outBuffer = NULL;
        return -1;
    }
    return ahb.allocate(desc, outBuffer);
}

void WinlatorAHB_acquire(AHardwareBuffer* buffer) {
    if (WinlatorAHB_isAvailable() && buffer) ahb.acquire(buffer);
}

void WinlatorAHB_release(AHardwareBuffer* buffer) {
    if (WinlatorAHB_isAvailable() && buffer) ahb.release(buffer);
}

void WinlatorAHB_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc) {
    if (WinlatorAHB_isAvailable() && buffer) ahb.describe(buffer, outDesc);
}

int WinlatorAHB_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect, void** outVirtualAddress) {
    if (!WinlatorAHB_isAvailable() || !buffer) {
        if (outVirtualAddress) *outVirtualAddress = NULL;
        return -1;
    }
    return ahb.lock(buffer, usage, fence, rect, outVirtualAddress);
}

int WinlatorAHB_unlock(AHardwareBuffer* buffer, int32_t* fence) {
    if (!WinlatorAHB_isAvailable() || !buffer) return -1;
    return ahb.unlock(buffer, fence);
}

const native_handle_t* WinlatorAHB_getNativeHandle(const AHardwareBuffer* buffer) {
    WinlatorAHB_isAvailable();
    return ahb.getNativeHandle && buffer ? ahb.getNativeHandle(buffer) : NULL;
}

#else

bool WinlatorAHB_isAvailable(void) {
    return true;
}

#endif
