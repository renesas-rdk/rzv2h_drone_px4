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
 * @file errno.h
 * @brief System error numbers.
 *
 * http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/errno.h.html
 *
 * The values defined in this file may not be compatible with the strerror
 * function provided by this system.
 */

#ifndef _FREERTOS_POSIX_ERRNO_H_
#define _FREERTOS_POSIX_ERRNO_H_

/* Undefine all errnos to avoid redefinition errors with system errnos. */
#undef EPERM
#undef ENOENT
#undef ECHILD
#undef EBADF
#undef EAGAIN
#undef ENOMEM
#undef EEXIST
#undef EBUSY
#undef EINVAL
#undef ENOSPC
#undef EDOM
#undef ERANGE
#undef ENAMETOOLONG
#undef EDEADLK
#undef EOVERFLOW
#undef ENOSYS
#undef EMSGSIZE
#undef ENOTSUP
#undef ETIMEDOUT
#undef ENOTEMPTY
#undef ECONNABORTED
#undef ECONNREFUSED
#undef ECONNRESET
#undef EHOSTUNREACH
#undef EINPROGRESS
#undef EISCONN
#undef ENOTCONN
#undef errno

/**
 * @name Definition of POSIX errnos.
 */
/**@{ */
#define EPERM           1   /**< Operation not permitted. */
#define ENOENT          2   /**< No such file or directory. */
#define ESRCH           3   /**< No such process. */
#define ECHILD          10  /**< No child processes. */
#define ENXIO           6   /**< No such device or address. */
#define E2BIG           7   /**< Argument list too long. */
#define ENOEXEC         8   /**< Exec format error. */
#define EBADF           9   /**< Bad file descriptor. */
#define EAGAIN          11  /**< Resource unavailable, try again. */
#define ENOMEM          12  /**< Not enough space. */
#define EACCES          13  /**< Permission denied. */
#define ENOTBLK         15  /**< Block device required. */
#define EBUSY           16  /**< Device or resource busy. */
#define EEXIST          17  /**< File exists. */
#define EXDEV           18  /**< Improper link. */
#define ENODEV          19  /**< No such device. */
#define ENOTDIR         20  /**< Not a directory. */
#define EISDIR          21  /**< Is a directory. */
#define EMFILE          24  /**< Too many open files. */
#define ENFILE          23  /**< Too many open files in system. */
#define ENOTTY          25  /**< Inappropriate ioctl for device. */
#define ETXTBSY         26  /**< Text file busy. */
#define ENOSPC          28  /**< No space left on device. */
#define EINVAL          22  /**< Invalid argument. */
#define EPIPE           32  /**< Broken pipe. */
#define EDOM            33  /**< Mathematics argument out of domain. */
#define ERANGE          34  /**< Result too large. */
#define EFBIG           27  /**< File too large. */
#define ESPIPE          29  /**< Illegal seek. */
#define EROFS           30  /**< Read-only file system. */
#define EMLINK          31  /**< Too many links. */
#define ENAMETOOLONG    36  /**< File name too long. */
#define EDEADLK         45  /**< Resource deadlock would occur. */
#define ENODATA         61  /**< No data available. */
#define EOVERFLOW       75  /**< Value too large to be stored in data type. */
#define ENOSYS          88  /**< Function not supported. */
#define EMSGSIZE        90  /**< Message too long. */
#define ENOTSUP         95  /**< Operation not supported. */
#define ETIMEDOUT       116 /**< Connection timed out. */
#define ENOTEMPTY       39  /**< Directory not empty. */
#define ECONNABORTED    103 /**< Connection aborted. */
#define ECONNREFUSED    111 /**< Connection refused. */
#define ECONNRESET      104 /**< Connection reset. */
#define EHOSTUNREACH    113 /**< Host is unreachable. */
#define EINPROGRESS     115 /**< Operation in progress. */
#define EISCONN         106 /**< Socket is connected. */
#define ENOTCONN        107 /**< Socket is not connected. */
#define EILSEQ          138 /**< Illegal byte sequence. */
#define EINTR           4   /**< Interrupted function. */
#define EIO             5   /**< I/O error. */
#define EFAULT          14  /**< Bad address. */
/**@} */

/**
 * @name System Variable
 *
 * @brief Define FreeRTOS+POSIX errno, if enabled.
 * Set configUSE_POSIX_ERRNO to enable, and clear to disable. See FreeRTOS.h.
 *
 * @{
 */
#if ( configUSE_POSIX_ERRNO == 1 )
    extern int FreeRTOS_errno;
    #define errno    FreeRTOS_errno
#endif
/**@} */

#endif /* ifndef _FREERTOS_POSIX_ERRNO_H_ */
