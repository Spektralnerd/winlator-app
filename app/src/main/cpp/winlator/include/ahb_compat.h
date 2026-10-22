#ifndef WINLATOR_AHB_COMPAT_H
#define WINLATOR_AHB_COMPAT_H

#include <stdbool.h>
#include <stdint.h>
#include <android/hardware_buffer.h>

#include "native_handle.h"

/*
 * AHardwareBuffer gibt es erst ab Android 8.0 (API 26). Für ein älteres minSdk (32-Bit-Builds,
 * Android 7) holt libwinlator die Funktionen zur Laufzeit (ahb_compat.c). Auf Geräten ohne
 * AHardwareBuffer schlagen die Aufrufe fehl und WinlatorAHB_isAvailable() liefert false.
 */
extern bool WinlatorAHB_isAvailable(void);

#if __ANDROID_API__ < 26
extern int WinlatorAHB_allocate(const AHardwareBuffer_Desc* desc, AHardwareBuffer** outBuffer);
extern void WinlatorAHB_acquire(AHardwareBuffer* buffer);
extern void WinlatorAHB_release(AHardwareBuffer* buffer);
extern void WinlatorAHB_describe(const AHardwareBuffer* buffer, AHardwareBuffer_Desc* outDesc);
extern int WinlatorAHB_lock(AHardwareBuffer* buffer, uint64_t usage, int32_t fence, const ARect* rect, void** outVirtualAddress);
extern int WinlatorAHB_unlock(AHardwareBuffer* buffer, int32_t* fence);
extern const native_handle_t* WinlatorAHB_getNativeHandle(const AHardwareBuffer* buffer);

#define AHardwareBuffer_allocate WinlatorAHB_allocate
#define AHardwareBuffer_acquire WinlatorAHB_acquire
#define AHardwareBuffer_release WinlatorAHB_release
#define AHardwareBuffer_describe WinlatorAHB_describe
#define AHardwareBuffer_lock WinlatorAHB_lock
#define AHardwareBuffer_unlock WinlatorAHB_unlock
#define AHardwareBuffer_getNativeHandle WinlatorAHB_getNativeHandle
#endif

#endif
