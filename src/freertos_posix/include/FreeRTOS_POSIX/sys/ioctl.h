#pragma once

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>

#ifndef FIONREAD
#define FIONREAD 0x6678   /* PX4 FreeRTOS stub value */
#endif

/* Linux-compatible alias so mixed headers don't break ioctl switches */
#ifndef FIONREAD_LINUX
#define FIONREAD_LINUX 0x541B
#endif

#ifndef FIONSPACE
#define FIONSPACE 0x6679
#endif

#if defined(__PX4_FREERTOS)
#ifdef __cplusplus
extern "C" {
#endif
int ioctl(int fd, unsigned long request, ...);
#ifdef __cplusplus
}
#endif
#else
static inline int ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    (void)request;

    va_list args;
    va_start(args, request);

    void *out = va_arg(args, void *);

    if (out) {
        *((uint32_t *)out) = 0;
    }

    va_end(args);
    return -1;
}
#endif
