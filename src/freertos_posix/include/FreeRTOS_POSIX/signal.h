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
 * @file signal.h
 * @brief Signals.
 *
 * Signals are currently not implemented in FreeRTOS+POSIX. This header only
 * defines the signal data structures used elsewhere.
 *
 * http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/signal.h.html
 */


#ifndef _FREERTOS_POSIX_SIGNAL_H_
#define _FREERTOS_POSIX_SIGNAL_H_

/* Define sigset_t, sigaction FIRST to avoid circular dependency issues */
typedef unsigned long sigset_t;

typedef void (*_sig_func_ptr)(int);

struct sigaction {
    _sig_func_ptr sa_handler;  /* Signal handler function */
    sigset_t      sa_mask;      /* Signals to block during handler */
    int           sa_flags;     /* Special flags */
};

/* Signal handler constants */
#define SIG_DFL ((_sig_func_ptr)0)   /* Default action */
#define SIG_IGN ((_sig_func_ptr)1)   /* Ignore signal */
#define SIG_ERR ((_sig_func_ptr)-1)  /* Error return */

/* Signal numbers - minimal set */
#define SIGINT  2   /* Interrupt */
#define SIGPIPE 13  /* Broken pipe */
#define SIGTERM 15  /* Termination */

#ifdef __cplusplus
extern "C" {
#endif

/* Stub: sigaction() always succeeds but does nothing */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
static inline int sigaction(int sig __attribute__((unused)),
                           const struct sigaction *act __attribute__((unused)),
                           struct sigaction *oldact __attribute__((unused)))
{
    return 0; /* Success (no-op) */
}
#pragma GCC diagnostic pop

#ifdef __cplusplus
}
#endif

/**
 * @name Values of sigev_notify.
 */
/**@{ */
#define SIGEV_NONE      0 /**< No asynchronous notification is delivered when the event of interest occurs. */
#define SIGEV_SIGNAL    1 /**< A queued signal, with an application-defined value, is generated when the event of interest occurs. Not supported. */
#define SIGEV_THREAD    2 /**< A notification function is called to perform notification. */
/**@} */

/**
 * @brief Signal value.
 */
union sigval
{
    int sival_int;    /**< Integer signal value. */
    void * sival_ptr; /**< Pointer signal value. */
};

/**
 * @brief Signal event structure.
 */
struct sigevent
{
    int sigev_notify;                                 /**< Notification type. A value of SIGEV_SIGNAL is not supported. */
    int sigev_signo;                                  /**< Signal number. This member is ignored. */
    union sigval sigev_value;                         /**< Signal value. Only the sival_ptr member is used. */
    void ( * sigev_notify_function )( union sigval ); /**< Notification function. */
    pthread_attr_t * sigev_notify_attributes;         /**< Notification attributes. */
};

#endif /* ifndef _FREERTOS_POSIX_SIGNAL_H_ */
