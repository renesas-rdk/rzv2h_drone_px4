/*
 * Amazon FreeRTOS POSIX V1.1.0
 * Copyright (C) 2018 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
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
 * @file FreeRTOS_POSIX_pthread_cond.c
 * @brief Implementation of condition variable functions in pthread.h
 */

/* C standard library includes. */
#include <limits.h>
#include <stdint.h>

/* FreeRTOS+POSIX includes. */
#include "FreeRTOS_POSIX.h"
#include "FreeRTOS_POSIX/errno.h"
#include "FreeRTOS_POSIX/pthread.h"
#include "FreeRTOS_POSIX/utils.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "atomic.h"
#if defined(__PX4_FREERTOS)
#if defined(__arm__) || defined(__ARM_ARCH) || defined(__aarch64__)
/* Helper to detect ISR context on ARM cores. */
static BaseType_t prvInISRContext(void)
{
    uint32_t cpsr = 0;

    __asm volatile ("MRS %0, CPSR" : "=r" (cpsr));

    /* User mode (0x10) and System mode (0x1F) indicate thread context. */
    uint32_t mode = cpsr & 0x1FU;

    switch( mode )
    {
        case 0x11U: /* FIQ mode */
        case 0x12U: /* IRQ mode */
        case 0x17U: /* Abort mode */
        case 0x1BU: /* Undefined mode */
            return pdTRUE;

        default:
            /* Treat User (0x10), System (0x1F), Supervisor (0x13) etc. as thread context. */
            return pdFALSE;
    }
}
#else
static BaseType_t prvInISRContext(void)
{
    return pdFALSE;
}
#endif

static BaseType_t prvCondGive(pthread_cond_internal_t * pxCond)
{
    BaseType_t xYieldRequired = pdFALSE;

    if( prvInISRContext() == pdTRUE )
    {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;

        ( void ) xSemaphoreGiveFromISR( ( SemaphoreHandle_t ) &pxCond->xCondWaitSemaphore,
                                        &xHigherPriorityTaskWoken );

        if( xHigherPriorityTaskWoken != pdFALSE )
        {
            xYieldRequired = pdTRUE;
        }
    }
    else
    {
        ( void ) xSemaphoreGive( ( SemaphoreHandle_t ) &pxCond->xCondWaitSemaphore );
    }

    return xYieldRequired;
}
#endif

/**
 * @brief Initialize a PTHREAD_COND_INITIALIZER cond.
 *
 * PTHREAD_COND_INITIALIZER sets a flag for a cond to be initialized later.
 * This function performs the initialization.
 * @param[in] pxCond The cond to initialize.
 *
 * @return nothing
 */
static void prvInitializeStaticCond( pthread_cond_internal_t * pxCond );

/*-----------------------------------------------------------*/

static void prvInitializeStaticCond( pthread_cond_internal_t * pxCond )
{
    /* Check if the condition variable needs to be initialized. */
    if( pxCond->xIsInitialized == pdFALSE )
    {
        /* Cond initialization must be in a critical section to prevent two threads
         * from initializing it at the same time. */
#if defined(__PX4_FREERTOS)
        UBaseType_t uxSavedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
#else
        taskENTER_CRITICAL();
#endif

        /* Check again that the cond is still uninitialized, i.e. it wasn't
         * initialized while this function was waiting to enter the critical
         * section. */
        if( pxCond->xIsInitialized == pdFALSE )
        {
            /* Set the members of the cond. The semaphore create calls will never fail
             * when their arguments aren't NULL. */
            pxCond->xIsInitialized = pdTRUE;
            ( void ) xSemaphoreCreateCountingStatic( INT_MAX, 0U, &pxCond->xCondWaitSemaphore );
            pxCond->iWaitingThreads = 0;
        }

        /* Exit the critical section. */
#if defined(__PX4_FREERTOS)
        taskEXIT_CRITICAL_FROM_ISR( uxSavedInterruptStatus );
#else
        taskEXIT_CRITICAL();
#endif
    }
}

/**
 * @brief Check "atomically" if iLocalWaitingThreads == pxCond->iWaitingThreads and decrement.
 */
static void prvTestAndDecrement( pthread_cond_t * pxCond,
                                 unsigned iLocalWaitingThreads )
{
    /* Test local copy of threads waiting is larger than zero. */
    while( iLocalWaitingThreads > 0 )
    {
        /* Test-and-set. Atomically check whether the copy in memory has changed.
         * And, if not decrease the copy of threads waiting in memory. */
        if( ATOMIC_COMPARE_AND_SWAP_SUCCESS == Atomic_CompareAndSwap_u32( ( uint32_t * ) &pxCond->iWaitingThreads, ( uint32_t ) iLocalWaitingThreads - 1, ( uint32_t ) iLocalWaitingThreads ) )
        {
            /* Signal one succeeded. Break. */
            break;
        }

        /* Local copy may be out dated. Reload, and retry. */
        iLocalWaitingThreads = pxCond->iWaitingThreads;
    }
}

/*-----------------------------------------------------------*/

int pthread_cond_broadcast( pthread_cond_t * cond )
{
    unsigned i = 0;
    pthread_cond_internal_t * pxCond = ( pthread_cond_internal_t * ) ( cond );

    /* If the cond is uninitialized, perform initialization. */
    prvInitializeStaticCond( pxCond );

    /* Local copy of number of threads waiting. */
    unsigned iLocalWaitingThreads = pxCond->iWaitingThreads;

    /* Test local copy of threads waiting is larger than zero. */
    while( iLocalWaitingThreads > 0 )
    {
        /* Test-and-set. Atomically check whether the copy in memory has changed.
         * And, if not set the copy of threads waiting in memory to zero. */
        if( ATOMIC_COMPARE_AND_SWAP_SUCCESS == Atomic_CompareAndSwap_u32( ( uint32_t * ) &pxCond->iWaitingThreads, 0, ( uint32_t ) iLocalWaitingThreads ) )
        {
            /* Unblock all. */
            BaseType_t xYieldRequired = pdFALSE;

            for( i = 0; i < iLocalWaitingThreads; i++ )
#if defined(__PX4_FREERTOS)
            {
                if( prvCondGive( pxCond ) != pdFALSE )
                {
                    xYieldRequired = pdTRUE;
                }
            }

            if( xYieldRequired != pdFALSE )
            {
                #if defined( portYIELD_FROM_ISR )
                portYIELD_FROM_ISR( pdTRUE );
                #endif
            }
#else
            {
                ( void ) xSemaphoreGive( ( SemaphoreHandle_t ) &pxCond->xCondWaitSemaphore );
            }
#endif

            break;
        }

        /* Local copy is out dated. Reload, and retry. */
        iLocalWaitingThreads = pxCond->iWaitingThreads;
    }

    return 0;
}

/*-----------------------------------------------------------*/

int pthread_cond_destroy( pthread_cond_t * cond )
{
    pthread_cond_internal_t * pxCond = ( pthread_cond_internal_t * ) ( cond );

#if defined(__PX4_FREERTOS)
    /* Free resources only if the condition variable was ever initialized. */
    if( pxCond->xIsInitialized == pdTRUE )
    {
        vSemaphoreDelete( ( SemaphoreHandle_t ) &pxCond->xCondWaitSemaphore );
        pxCond->xIsInitialized = pdFALSE;
        pxCond->iWaitingThreads = 0;
    }
#else
    /* Free all resources in use by the cond. */
    vSemaphoreDelete( ( SemaphoreHandle_t ) &pxCond->xCondWaitSemaphore );
#endif

    return 0;
}

/*-----------------------------------------------------------*/

int pthread_cond_init( pthread_cond_t * cond,
                       const pthread_condattr_t * attr )
{
    int iStatus = 0;
    pthread_cond_internal_t * pxCond = ( pthread_cond_internal_t * ) cond;

    /* Silence warnings about unused parameters. */
    ( void ) attr;

    if( pxCond == NULL )
    {
        iStatus = ENOMEM;
    }

    if( iStatus == 0 )
    {
        /* Set the members of the cond. The semaphore create calls will never fail
         * when their arguments aren't NULL. */
        pxCond->xIsInitialized = pdTRUE;

        ( void ) xSemaphoreCreateCountingStatic( INT_MAX, 0U, &pxCond->xCondWaitSemaphore );
        pxCond->iWaitingThreads = 0;
    }

    return iStatus;
}

/*-----------------------------------------------------------*/

int pthread_cond_signal( pthread_cond_t * cond )
{
    pthread_cond_internal_t * pxCond = ( pthread_cond_internal_t * ) ( cond );

    /* If the cond is uninitialized, perform initialization. */
    prvInitializeStaticCond( pxCond );

    /* Local copy of number of threads waiting. */
    unsigned iLocalWaitingThreads = pxCond->iWaitingThreads;

    /* Test local copy of threads waiting is larger than zero. */
    while( iLocalWaitingThreads > 0 )
    {
        /* Test-and-set. Atomically check whether the copy in memory has changed.
         * And, if not decrease the copy of threads waiting in memory. */
        if( ATOMIC_COMPARE_AND_SWAP_SUCCESS == Atomic_CompareAndSwap_u32( ( uint32_t * ) &pxCond->iWaitingThreads, ( uint32_t ) iLocalWaitingThreads - 1, ( uint32_t ) iLocalWaitingThreads ) )
        {
            /* Unblock one. */
#if defined(__PX4_FREERTOS)
            BaseType_t xYieldRequired = prvCondGive( pxCond );

            if( xYieldRequired != pdFALSE )
            {
                #if defined( portYIELD_FROM_ISR )
                portYIELD_FROM_ISR( pdTRUE );
                #endif
            }
#else
            ( void ) xSemaphoreGive( ( SemaphoreHandle_t ) &pxCond->xCondWaitSemaphore );
#endif

            /* Signal one succeeded. Break. */
            break;
        }

        /* Local copy may be out dated. Reload, and retry. */
        iLocalWaitingThreads = pxCond->iWaitingThreads;
    }

    return 0;
}

/*-----------------------------------------------------------*/

int pthread_cond_timedwait( pthread_cond_t * cond,
                            pthread_mutex_t * mutex,
                            const struct timespec * abstime )
{
    unsigned iLocalWaitingThreads = 0U;
    int iStatus = 0;
    pthread_cond_internal_t * pxCond = ( pthread_cond_internal_t * ) ( cond );
    TickType_t xDelay = portMAX_DELAY;

    /* If the cond is uninitialized, perform initialization. */
    prvInitializeStaticCond( pxCond );

    /* Convert abstime to a delay in TickType_t if provided. */
    if( abstime != NULL )
    {
        struct timespec xCurrentTime = { 0 };

        /* Get current time */
        if( clock_gettime(
#if defined(__PX4_FREERTOS)
                CLOCK_MONOTONIC,
#else
                CLOCK_REALTIME,
#endif
                &xCurrentTime ) != 0 )
        {
            iStatus = EINVAL;
        }
        else
        {
            iStatus = UTILS_AbsoluteTimespecToDeltaTicks( abstime, &xCurrentTime, &xDelay );
        }
    }

    /* Increase the counter of threads blocking on condition variable, then
     * unlock mutex. */
    if( iStatus == 0 )
    {
        /* Atomically increments thread waiting by 1, and
         * stores number of threads waiting before increment. */
        iLocalWaitingThreads = Atomic_Increment_u32( ( uint32_t * ) &pxCond->iWaitingThreads );

        iStatus = pthread_mutex_unlock( mutex );
    }

    /* Wait on the condition variable. */
    if( iStatus == 0 )
    {
        if( xSemaphoreTake( ( SemaphoreHandle_t ) &pxCond->xCondWaitSemaphore,
                            xDelay ) == pdPASS )
        {
            /* When successful, relock mutex. */
            iStatus = pthread_mutex_lock( mutex );
        }
        else
        {
            /* Timeout. Relock mutex and decrement number of waiting threads. */
            iStatus = ETIMEDOUT;
            ( void ) pthread_mutex_lock( mutex );

            /* Atomically decrements thread waiting by 1.
             * If iLocalWaitingThreads is updated by other thread(s) in between,
             * this implementation guarantees to decrement by 1 based on the
             * value currently in pxCond->iWaitingThreads. */
            prvTestAndDecrement( pxCond, iLocalWaitingThreads + 1 );
        }
    }
#if defined(__PX4_FREERTOS)
    else if( iStatus != 0 && iStatus != ETIMEDOUT )
    {
        /* Only decrement if the counter was incremented earlier. */
#else
    else
    {
#endif
        /* Atomically decrements thread waiting by 1.
         * If iLocalWaitingThreads is updated by other thread(s) in between,
         * this implementation guarantees to decrement by 1 based on the
         * value currently in pxCond->iWaitingThreads. */
        prvTestAndDecrement( pxCond, iLocalWaitingThreads + 1 );
    }

    return iStatus;
}

/*-----------------------------------------------------------*/

int pthread_cond_wait( pthread_cond_t * cond,
                       pthread_mutex_t * mutex )
{
    return pthread_cond_timedwait( cond, mutex, NULL );
}
