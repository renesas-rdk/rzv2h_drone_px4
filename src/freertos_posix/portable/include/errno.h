#pragma once

#include_next <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmacro-redefined"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbuiltin-macro-redefined"
#endif

#include "FreeRTOS_POSIX/errno.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef __cplusplus
}
#endif

#if defined(ENOTEMPTY)
#if ENOTEMPTY != 39
#undef ENOTEMPTY
#define ENOTEMPTY 39
#endif
#elif !defined(ENOTEMPTY)
#define ENOTEMPTY 39
#endif

#if (configUSE_POSIX_ERRNO == 1)
#ifdef errno
#undef errno
#endif
#define errno FreeRTOS_errno
#endif

/* portable errno.h wrapper */
