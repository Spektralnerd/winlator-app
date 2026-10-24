/* Ersatz für <android/log.h> beim PC-Bau des Gladio-Servers (armv7/tools/gladio-pc). */
#ifndef GLADIO_PC_ANDROID_LOG_H
#define GLADIO_PC_ANDROID_LOG_H
#include <stdio.h>
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#define __android_log_print(prio, tag, ...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr), 0)
#endif
