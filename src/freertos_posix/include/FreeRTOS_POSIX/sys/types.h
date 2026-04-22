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
 * @file sys/types.h
 * @brief Data types.
 *
 * http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/sys_types.h.html
 */

#ifndef _FREERTOS_POSIX_TYPES_H_
#define _FREERTOS_POSIX_TYPES_H_

/* C standard library includes. */
#include <stdint.h>

/* FreeRTOS types include */
#include "FreeRTOS_POSIX_types.h"

/*
 * The FreeRTOS+POSIX headers assume they are the primary provider of various
 * POSIX typedefs. When building PX4 against newlib we need the standard C
 * library definitions (such as dev_t) to remain available.  If newlib has not
 * provided them yet, add lightweight fallbacks that match its layout.
 */
#ifndef _DEV_T_DECLARED
    typedef short dev_t;
#define _DEV_T_DECLARED
#endif

#ifndef _INO_T_DECLARED
    typedef unsigned long ino_t;
#define _INO_T_DECLARED
#endif

#ifndef _NLINK_T_DECLARED
    typedef unsigned short nlink_t;
#define _NLINK_T_DECLARED
#endif

#ifndef _UID_T_DECLARED
    typedef unsigned int uid_t;
#define _UID_T_DECLARED
#endif

#ifndef _GID_T_DECLARED
    typedef unsigned int gid_t;
#define _GID_T_DECLARED
#endif

#ifndef _MODE_T_DECLARED
    typedef unsigned int mode_t;
#define _MODE_T_DECLARED
#endif

#ifndef _CLOCK_T_DECLARED
    typedef unsigned long clock_t;
#define _CLOCK_T_DECLARED
#endif

#ifndef _CLOCKID_T_DECLARED
    typedef int clockid_t;
#define _CLOCKID_T_DECLARED
#endif

#ifndef _TIME_T_DECLARED
    typedef long time_t;
#define _TIME_T_DECLARED
#endif

#ifndef _PID_T_DECLARED
    typedef int pid_t;
#define _PID_T_DECLARED
#endif

#ifndef _TIMER_T_DECLARED
    typedef void * timer_t;
#define _TIMER_T_DECLARED
#endif

#ifndef _USECONDS_T_DECLARED
    typedef unsigned long useconds_t;
#define _USECONDS_T_DECLARED
#endif

#ifndef _SUSECONDS_T_DECLARED
    typedef long suseconds_t;
#define _SUSECONDS_T_DECLARED
#endif

#ifndef _SSIZE_T_DECLARED
    typedef long ssize_t;
#define _SSIZE_T_DECLARED
#endif

#ifndef _BLKCNT_T_DECLARED
    typedef long blkcnt_t;
#define _BLKCNT_T_DECLARED
#endif

#ifndef _BLKSIZE_T_DECLARED
    typedef long blksize_t;
#define _BLKSIZE_T_DECLARED
#endif

/**
 * @brief Used for system times in clock ticks or CLOCKS_PER_SEC.
 *
 * Enabled/disabled by posixconfigENABLE_CLOCK_T.
 */
#if !defined( posixconfigENABLE_CLOCK_T ) || ( posixconfigENABLE_CLOCK_T == 1 )
    #ifndef _CLOCK_T_DECLARED
        typedef uint32_t             clock_t;
        #define _CLOCK_T_DECLARED
    #endif
#endif

/**
 * @brief Used for clock ID type in the clock and timer functions.
 *
 * Enabled/disabled by posixconfigENABLE_CLOCKID_T.
 */
#if !defined( posixconfigENABLE_CLOCKID_T ) || ( posixconfigENABLE_CLOCKID_T == 1 )
    #ifndef _CLOCKID_T_DECLARED
        typedef int                  clockid_t;
        #define _CLOCKID_T_DECLARED
    #endif
#endif

/**
 * @brief Used for some file attributes.
 *
 * Enabled/disabled by posixconfigENABLE_MODE_T.
 */
#if !defined( posixconfigENABLE_MODE_T ) || ( posixconfigENABLE_MODE_T == 1 )
    #ifndef _MODE_T_DECLARED
        typedef int                  mode_t;
        #define _MODE_T_DECLARED
    #endif
#endif

/**
 * @brief Used for process IDs and process group IDs.
 *
 * Enabled/disabled by posixconfigENABLE_PID_T.
 */
#if !defined( posixconfigENABLE_PID_T ) || ( posixconfigENABLE_PID_T == 1 )
    #ifndef _PID_T_DECLARED
        typedef int                  pid_t;
        #define _PID_T_DECLARED
    #endif
#endif

/**
 * @brief Used to identify a thread attribute object.
 *
 * Enabled/disabled by posixconfigENABLE_PTHREAD_ATTR_T.
 */
#if !defined( posixconfigENABLE_PTHREAD_ATTR_T ) || ( posixconfigENABLE_PTHREAD_ATTR_T == 1 )
    typedef PthreadAttrType_t        pthread_attr_t;
#endif

/**
 * @brief Used to identify a barrier.
 *
 * Enabled/disabled by posixconfigENABLE_PTHREAD_BARRIER_T.
 */
#if !defined( posixconfigENABLE_PTHREAD_BARRIER_T ) || ( posixconfigENABLE_PTHREAD_BARRIER_T == 1 )
    typedef PthreadBarrierType_t     pthread_barrier_t;
#endif

/**
 * @brief Used to define a barrier attributes object.
 */
typedef void                         * pthread_barrierattr_t;

/**
 * @brief Used for condition variables.
 *
 * Enabled/disabled by posixconfigENABLE_PTHREAD_COND_T.
 */
#if !defined( posixconfigENABLE_PTHREAD_COND_T ) || ( posixconfigENABLE_PTHREAD_COND_T == 1 )
    typedef  PthreadCondType_t       pthread_cond_t;
#endif

/**
 * @brief Used to identify a condition attribute object.
 *
 * Enabled/disabled by posixconfigENABLE_PTHREAD_CONDATTR_T.
 */
#if !defined( posixconfigENABLE_PTHREAD_CONDATTR_T ) || ( posixconfigENABLE_PTHREAD_CONDATTR_T == 1 )
    typedef void                     * pthread_condattr_t;
#endif

/**
 * @brief Used for mutexes.
 *
 * Enabled/disabled by posixconfigENABLE_PTHREAD_MUTEX_T.
 */
#if !defined( posixconfigENABLE_PTHREAD_MUTEX_T ) || ( posixconfigENABLE_PTHREAD_MUTEX_T == 1 )
    typedef PthreadMutexType_t       pthread_mutex_t;
#endif

/**
 * @brief Used to identify a mutex attribute object.
 *
 * Enabled/disabled by posixconfigENABLE_PTHREAD_MUTEXATTR_T.
 */
#if !defined( posixconfigENABLE_PTHREAD_MUTEXATTR_T ) || ( posixconfigENABLE_PTHREAD_MUTEXATTR_T == 1 )
    typedef PthreadMutexAttrType_t   pthread_mutexattr_t;
#endif

/**
 * @brief Used to identify a thread.
 *
 * Enabled/disabled by posixconfigENABLE_PTHREAD_T.
 */
#if !defined( posixconfigENABLE_PTHREAD_T ) || ( posixconfigENABLE_PTHREAD_T == 1 )
    typedef void                     * pthread_t;
#endif

/**
 * @brief Thread-specific data key (stub for PX4 compatibility).
 *
 * Note: FreeRTOS+POSIX does not support thread-specific data functions.
 * This type is defined for compilation compatibility only.
 */
typedef unsigned int pthread_key_t;

/**
 * @brief Used for a count of bytes or an error indication.
 *
 * Enabled/disabled by posixconfigENABLE_SSIZE_T.
 */
#if !defined( posixconfigENABLE_SSIZE_T ) || ( posixconfigENABLE_SSIZE_T == 1 )
    #ifndef _SSIZE_T_DECLARED
        typedef int                  ssize_t;
        #define _SSIZE_T_DECLARED
    #endif
#endif

/**
 * @brief Used for time in seconds.
 *
 * Enabled/disabled by posixconfigENABLE_TIME_T.
 */
#if !defined( posixconfigENABLE_TIME_T ) || ( posixconfigENABLE_TIME_T == 1 )
    #ifndef _TIME_T_DECLARED
        typedef int64_t              time_t;
        #define _TIME_T_DECLARED
    #endif
#endif

/**
 * @brief Used for timer ID returned by timer_create().
 *
 * Enabled/disabled by posixconfigENABLE_TIMER_T.
 */
#if !defined( posixconfigENABLE_TIMER_T ) || ( posixconfigENABLE_TIMER_T == 1 )
    #ifndef _TIMER_T_DECLARED
        typedef void                 * timer_t;
        #define _TIMER_T_DECLARED
    #endif
#endif

/**
 * @brief Used for time in microseconds.
 *
 * Enabled/disabled by posixconfigENABLE_USECONDS_T.
 */
#if !defined( posixconfigENABLE_USECONDS_T ) || ( posixconfigENABLE_USECONDS_T == 1 )
    #ifndef _USECONDS_T_DECLARED
        typedef unsigned long        useconds_t;
        #define _USECONDS_T_DECLARED
    #endif
#endif

/**
 * @brief Used for file sizes.
 *
 * Enabled/disabled by posixconfigENABLE_OFF_T.
 */
#if !defined( posixconfigENABLE_OFF_T ) || ( posixconfigENABLE_OFF_T == 1 )
    #ifndef _OFF_T_DECLARED
        typedef long int             off_t;
        #define _OFF_T_DECLARED
    #endif
#endif

#endif /* ifndef _FREERTOS_POSIX_TYPES_H_ */
