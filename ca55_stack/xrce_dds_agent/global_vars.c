/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Global variables definitions for RZV2H uXRCE-DDS Agent
 * These variables are declared as extern in platform_info.h and other headers
 * but need to be defined somewhere for linking.
 */

#include <pthread.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdlib.h>
#include "platform_info.h"

/* Thread IDs for different cores/processors */
#ifdef __linux__
pid_t g_tid_cm33 = 0;
pid_t g_tid_cr8_0 = 0;  
pid_t g_tid_cr8_1 = 0;
#endif

/* Threading synchronization objects */
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond[MBX_CH_NUM];
pthread_key_t thkey;

/* Thread validity tracking */
bool valid_thread[MBX_CH_NUM] = {false};

/* Force stop flag for signal handling */
int force_stop = 0;

/* Thread index for current thread context */
int tindex = 0;

/* Initialize condition variables at runtime */
void init_global_vars(void) {
    /* Use CLOCK_MONOTONIC so timedwait intervals are immune to wall-clock jumps
     * (NTP sync, RPC CLOCK_SETTIME from GPS/QGC).  A CLOCK_REALTIME cond var
     * returns ETIMEDOUT immediately if time jumps forward while waiting. */
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    for (int i = 0; i < MBX_CH_NUM; i++) {
        pthread_cond_init(&cond[i], &attr);
        valid_thread[i] = false;
    }
    pthread_condattr_destroy(&attr);
    pthread_key_create(&thkey, free);
}

/* Cleanup global resources */
void cleanup_global_vars(void) {
    for (int i = 0; i < MBX_CH_NUM; i++) {
        pthread_cond_destroy(&cond[i]);
    }
    pthread_key_delete(thkey);
    pthread_mutex_destroy(&mutex);
}
