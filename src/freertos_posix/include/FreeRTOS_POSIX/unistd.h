/*
 * Amazon FreeRTOS POSIX V1.1.0
 * Copyright (C) 2019 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

/**
 * @file unistd.h
 * @brief Standard symbolic constants and types
 *
 * http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/unistd.h.html
 */

#ifndef _FREERTOS_POSIX_UNISTD_H_
#define _FREERTOS_POSIX_UNISTD_H_

#include "FreeRTOS_POSIX/sys/types.h"

/**
 * @brief Suspend execution for an interval of time.
 *
 * http://pubs.opengroup.org/onlinepubs/9699919799/functions/sleep.html
 *
 * @param[in] seconds The number of seconds to suspend execution.
 *
 * @retval 0 - Upon successful completion.
 *
 * @note Return value of a positive number is not yet supported.
 */
unsigned sleep( unsigned seconds );

/**
 * @brief Suspend execution for microsecond intervals.
 *
 * This is a useful, non-POSIX function.
 * @param[in] usec The number of microseconds to suspend execution.
 *
 * @retval 0 - Upon successful completion.
 */
int usleep( useconds_t usec );

/**
 * @brief Make a symbolic link (stub for PX4 compatibility).
 *
 * Note: FreeRTOS+POSIX does not support symbolic links.
 * This function is a stub that always returns -1 (not supported).
 *
 * @param[in] path1 The target path.
 * @param[in] path2 The link path.
 *
 * @retval -1 - Always fails (not implemented).
 */
static inline int symlink( const char *path1 __attribute__((unused)), const char *path2 __attribute__((unused)) )
{
    return -1; /* Not supported in FreeRTOS */
}

/**
 * @brief Get current working directory (stub for PX4 compatibility).
 *
 * Note: FreeRTOS does not have a concept of current working directory.
 * This function returns a fixed root path "/".
 *
 * @param[out] buf Buffer to store the path.
 * @param[in] size Size of the buffer.
 *
 * @retval buf - Success (returns buf pointer).
 * @retval NULL - Failure (buffer too small or invalid).
 */
static inline char *getcwd( char *buf, size_t size )
{
    if (buf == NULL || size < 2) {
        return NULL;
    }
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}

/**
 * @brief Change current working directory (stub for PX4 compatibility).
 *
 * Note: FreeRTOS does not have a concept of current working directory.
 * This function always succeeds but does nothing.
 *
 * @param[in] path Path to change to (ignored).
 *
 * @retval 0 - Always succeeds.
 */
static inline int chdir( const char *path __attribute__((unused)) )
{
    return 0; /* Success (no-op) */
}

/**
 * @brief Get the process ID of the calling process (stub for PX4 compatibility).
 *
 * FreeRTOS has no concept of processes. Returns a fixed value via the newlib
 * _getpid() syscall stub (see syscalls.c). Used by px4 logger watchdog on POSIX
 * builds; the watchdog is guarded by __PX4_NUTTX so the return value is unused.
 *
 * @retval pid_t - Fixed constant (1) representing this firmware image.
 */
static inline pid_t getpid( void )
{
    return (pid_t)1; /* FreeRTOS: no process concept; watchdog_initialize guarded by __PX4_NUTTX */
}

#endif /* ifndef _FREERTOS_POSIX_UNISTD_H_ */
