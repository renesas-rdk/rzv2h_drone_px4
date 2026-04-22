/*
 * Copyright (c) 2014, Mentor Graphics Corporation
 * All rights reserved.
 * Copyright (c) 2017 Xilinx, Inc.
 * Copyright (c) 2020, eForce Co., Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**************************************************************************
 * FILE NAME
 *
 *       platform_info.c
 *
 * DESCRIPTION
 *
 *       This file define platform specific data and implements APIs to set
 *       platform specific information for OpenAMP.
 *
 * @par  History
 *       - rev 1.0 (2020.10.21) Imada
 *         Initial version.
 *
 **************************************************************************/

#include <metal/atomic.h>
#include <metal/assert.h>
#include <metal/device.h>
#include <metal/irq.h>
#include <metal/utilities.h>
#include <metal/sleep.h>
#include <openamp/rpmsg_virtio.h>
#include "platform_info.h"
#include "rsc_table.h"
#include "helper.h"
#ifdef __linux__
#include <sched.h>
#include <stddef.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#else /* uC3 */
#include <metal/sys.h>
#endif

#ifdef __linux__
#define _rproc_wait()  (sched_yield())
#endif

#define min(a,b) \
    ({ __typeof__ (a) _a = (a); \
        __typeof__ (b) _b = (b); \
      (_a > _b) ? _b : _a; })

/* Variables */
/* IPI dedicated task */
#ifndef __linux__ /* uC3 */
static ID ipi_tsk_id[CFG_RPMSG_SVCNO] = {0};
#endif

/** flag SIGINT or SIGTERM have been received */
extern int force_stop;

/** to stop execution until received an interrupt.  */
extern pthread_mutex_t mutex;
extern pthread_cond_t cond[MBX_CH_NUM];
extern pthread_key_t thkey;
extern int tindex;

struct mbx_channel chn_info[MBX_CH_NUM] ={
    {0, 0, 0                 }, //UIO MBX
	{5, 8 ,INT_MHU_RSP_CH8_NS}, //CM33
	{3, 6 ,INT_MHU_RSP_CH6_NS}, //CR8 core0
    {4, 7 ,INT_MHU_RSP_CH7_NS}, //CR8 core1
};

/** This strucure array is initilized in rz_proc_notify() */
struct mhu_send_type send_type_info[MBX_CH_NUM] ={
    {MHU_SEND_TYPE_MSG},
};

/* IPI(MBX) information */
struct ipi_info ipi[UIO_MAX] = {
{
    MBX_DEV_NAME, // name
    DEV_BUS_NAME, // bus_name
    NULL, // dev
    NULL, // io
    0, // irq_info
    0, // registered
    {0, 0}, // mbx_chn
    0, // chn_mask (not used on this SoC)
#ifdef __linux__
    ATOMIC_FLAG_INIT, // sync
    0, // notify_id
#else /* uC3 */
    {E_ID, E_ID}, // ipi_sem_id
#endif
},
{
    "receiver@10480100", // name
    DEV_BUS_NAME, // bus_name
    NULL, // dev
    NULL, // io
    0, // irq_info
    0, // registered
    {0, 0}, // mbx_chn
    0, // chn_mask (not used on this SoC)
#ifdef __linux__
    ATOMIC_FLAG_INIT, // sync
    0, // notify_id
#else /* uC3 */
    {E_ID, E_ID}, // ipi_sem_id
#endif
},
{
    "receiver@104800C0", // name1
    DEV_BUS_NAME, // bus_name
    NULL, // dev
    NULL, // io
    0, // irq_info
    0, // registered
    {0, 0}, // mbx_chn
    0, // chn_mask (not used on this SoC)
#ifdef __linux__
    ATOMIC_FLAG_INIT, // sync
    0, // notify_id
#else /* uC3 */
    {E_ID, E_ID}, // ipi_sem_id
#endif
},
{
    "receiver@104800E0", // name1
    DEV_BUS_NAME, // bus_name
    NULL, // dev
    NULL, // io
    0, // irq_info
    0, // registered
    {0, 0}, // mbx_chn
    0, // chn_mask (not used on this SoC)
#ifdef __linux__
    ATOMIC_FLAG_INIT, // sync
    0, // notify_id
#else /* uC3 */
    {E_ID, E_ID}, // ipi_sem_id
#endif
},
};

/* vring information */
struct vring_info vrinfo[CFG_RPMSG_SVCNO] = {
    {
        { // rsc
            CFG_RSCTBL_DEV_NAME, // name
            DEV_BUS_NAME, // bus_name
            NULL, // dev
            NULL, // io
            NULL, // mem
        },
        { // ctl
            CFG_VRING_CTL_NAME0, // name
            DEV_BUS_NAME, // bus_name
            NULL, // dev
            NULL, // io
            NULL, // mem
        },
        { // shm
            CFG_VRING_SHM_NAME0, // name
            DEV_BUS_NAME, // bus_name
            NULL, // dev
            NULL, // io
            NULL, // mem
        },
    },
    {
        { // rsc
            CFG_RSCTBL_DEV_NAME, // name
            DEV_BUS_NAME, // bus_name
            NULL, // dev
            NULL, // io
            NULL, // mem
        },
        { // ctl
            CFG_VRING_CTL_NAME1, // name
            DEV_BUS_NAME, // bus_name
            NULL, // dev
            NULL, // io
            NULL, // mem
        },
        { // shm
            CFG_VRING_SHM_NAME1, // name
            DEV_BUS_NAME, // bus_name
            NULL, // dev
            NULL, // io
            NULL, // mem
        },
    }
};

/* shared memory information */
struct shm_info shm = {
    SHM_DEV_NAME, // name
    DEV_BUS_NAME, // bus_name
    NULL, // dev
    NULL, // io
    NULL, // mem
};


/* processor operations at RZ/G2. It defines
 * notification operation and remote processor managementi operations. */
extern struct remoteproc_ops rz_proc_ops;

/* RPMsg virtio shared buffer pool */
static __thread struct rpmsg_virtio_shm_pool shpool;

/** Reusing shared resources */
static struct remote_resource_table *g_rsc_table = NULL;

#ifndef __linux__ /* uC3 */
static void start_ipi_task(void *platform);
#endif

#ifdef __linux__
static inline void virtio_clear_status(void *rsc_table) {
    struct remote_resource_table *rt = (struct remote_resource_table *)rsc_table;

    rt->rpmsg_vdev.status = 0x0;

    return ;
}
#endif

static struct remoteproc *
platform_create_proc(int proc_index, int rsc_index, int mbx_index)
{
    void *rsc_table;
    unsigned int rsc_size;
    int ret = 0;
    struct remoteproc *rproc_inst;
    struct remoteproc_priv *rproc_priv;
#ifdef __linux__
    metal_phys_addr_t pa;
    static struct metal_io_region *g_rsc_io = NULL;
#endif

    /* Allocate and initialize remoteproc_priv instance */
    rproc_priv = metal_allocate_memory(sizeof(struct remoteproc_priv));
    if (!rproc_priv) {
        return NULL;
    }
    memset(rproc_priv, 0, sizeof(*rproc_priv));
    rproc_priv->notify_id = (unsigned int)proc_index;
    rproc_priv->mbx_chn_id = mbx_index;

    /* Allocate remoteproc instance */
    rproc_inst = metal_allocate_memory(sizeof(struct remoteproc));
    if (!rproc_inst) {
        goto err1;
    }
    memset(rproc_inst, 0, sizeof(*rproc_inst));
    /* remoteproc initialization */
    if (!remoteproc_init(rproc_inst, &rz_proc_ops, rproc_priv)) {
        goto err2;
    }

    /*
     * Mmap shared memories
     * Or shall we constraint that they will be set as carved out
     * in the resource table?
     */
    if (g_rsc_table) {
        rproc_inst->rsc_table = rsc_table = &g_rsc_table[rsc_index];
        rproc_inst->rsc_len = rsc_size = sizeof(struct remote_resource_table);
        rproc_inst->rsc_io = g_rsc_io;

        goto skip;
    }
#ifdef __linux__
    rsc_size = sizeof(struct remote_resource_table);
    pa = CFG_RSCTBL_MEM_PA;
    rsc_table = remoteproc_mmap(rproc_inst, &pa,
                NULL, rsc_size * MBX_CH_NUM,
                0, NULL);
    if (!rsc_table) {
        LPRINTF("Failed to map the resource table.");
        goto err2;
    }
#else /* uC3 */
    rsc_table = get_resource_table(rsc_index, &rsc_size);
    rproc_inst->rsc_io = rproc_priv->vr_info[VRING_RSC].io;
#endif

    /* Parse resource table to remoteproc */
    ret = remoteproc_set_rsc_table(rproc_inst, rsc_table, rsc_size);
    if (ret) {
        /* If the resource table is not yet initialized by the remote, retry */
        unsigned int retries = 0;
        const unsigned int max_retries = 500;
        while (ret && retries++ < max_retries) {
            metal_sleep_usec(1000); /* wait 1ms */
            ret = remoteproc_set_rsc_table(rproc_inst, rsc_table, rsc_size);
        }
    }
    if (ret) {
        LPRINTF("Failed to intialize remoteproc");
        goto err2;
    }
    g_rsc_table = rsc_table;
    g_rsc_io = rproc_inst->rsc_io;
skip:
    LPRINTF("Initialize remoteproc successfully.");

    return rproc_inst;
err2:
    (void)remoteproc_remove(rproc_inst);
    metal_free_memory(rproc_inst);
err1:
    metal_free_memory(rproc_priv);
    return NULL;
}

struct  rpmsg_device *
platform_create_rpmsg_vdev(struct remoteproc *rproc, unsigned int vdev_index,
               unsigned int role,
               void (*rst_cb)(struct virtio_device *vdev),
               rpmsg_ns_bind_cb ns_bind_cb)
{
    struct remoteproc_priv *prproc;
    struct rpmsg_virtio_device *rpmsg_vdev;
    struct virtio_device *vdev;
    struct metal_io_region *shbuf_io;
    metal_phys_addr_t pa;
#ifdef __linux__
    void *shbuf;
    size_t len;
#endif
    int ret;

    prproc = rproc->priv;

    rpmsg_vdev = metal_allocate_memory(sizeof(*rpmsg_vdev));
    if (!rpmsg_vdev)
        return NULL;
    memset(rpmsg_vdev, 0, sizeof(*rpmsg_vdev));

    LPRINTF("creating remoteproc virtio");
    vdev = remoteproc_create_virtio(rproc, (int)vdev_index, role, rst_cb);
    if (!vdev) {
        LPRINTF("failed remoteproc_create_virtio");
        goto err;
    }

    pa = metal_io_phys(prproc->vr_info[VRING_SHM].io, 0x0U);
    shbuf_io = remoteproc_get_io_with_pa(rproc, pa);
    if (!shbuf_io) {
        LPRINTF("failed remoteproc_get_io_with_pa");
        goto err;
    }

    /* Only RPMsg virtio master needs to initialize the shared buffers pool */
#ifdef __linux__
    LPRINTF("initializing rpmsg shared buffer pool");
    shbuf = metal_io_phys_to_virt(shbuf_io, pa);
    len = metal_io_region_size(prproc->vr_info[VRING_SHM].io);
    rpmsg_virtio_init_shm_pool(&shpool, shbuf, len);
#endif

    LPRINTF("initializing rpmsg vdev");
    /* RPMsg virtio slave can set shared buffers pool argument to NULL */
    ret =  rpmsg_init_vdev(rpmsg_vdev, vdev, ns_bind_cb,
                   shbuf_io,
                   &shpool);
    if (ret) {
        LPRINTF("failed rpmsg_init_vdev");
        goto err;
    }

#ifndef __linux__ /* uC3 */
    start_ipi_task(rproc);
#endif

    return rpmsg_virtio_get_rpmsg_device(rpmsg_vdev);
err:
#ifdef __linux__
    virtio_clear_status(rproc->rsc_table);
#endif
    remoteproc_remove_virtio(rproc, vdev);
    metal_free_memory(rpmsg_vdev);
    return NULL;
}

/**
 * @fn thread_specific_ipi
 * @brief Get the UIO in use by a thread
 * @return ipi_info
 */
static struct ipi_info *thread_specific_ipi(void)
{
    int *idx = pthread_getspecific(thkey);
    struct ipi_info *pipi;

    if ((*idx) < UIO_MAX) {
        pipi = &ipi[*idx];
    } else {
        pipi = NULL;
    }
    return pipi;
}

int platform_poll(struct remoteproc *rproc)
{
#ifdef __linux__
    unsigned int flags;
    int *idx = pthread_getspecific(thkey);
    struct ipi_info *pipi;

    pipi = thread_specific_ipi();
    if (!pipi) goto error_return;

    while(!force_stop) {
        flags = metal_irq_save_disable();
        if (!(atomic_flag_test_and_set(&pipi->sync))) {
            metal_irq_restore_enable(flags);
            __atomic_thread_fence(__ATOMIC_SEQ_CST); /* Memory barrier for cache coherency */
            remoteproc_get_notification(rproc, RSC_NOTIFY_ID_ANY);
            __atomic_thread_fence(__ATOMIC_SEQ_CST); /* Memory barrier after processing */
            break;
        }
        metal_irq_restore_enable(flags);

        /* Use timed wait instead of indefinite wait to enable periodic polling.
         * Use CLOCK_MONOTONIC: cond[*idx] is initialised with MONOTONIC in
         * init_global_vars(), so the absolute timeout must use the same clock.
         * CLOCK_REALTIME would cause immediate ETIMEDOUT on any wall-clock jump. */
        pthread_mutex_lock(&mutex);
        struct timespec timeout;
        clock_gettime(CLOCK_MONOTONIC, &timeout);

        /* Add 1ms timeout - poll every 1ms even if no IRQ */
        timeout.tv_nsec += 1000000; /* 1ms = 1,000,000 ns */
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec += 1;
            timeout.tv_nsec -= 1000000000;
        }

        int wait_ret = pthread_cond_timedwait(&cond[*idx], &mutex, &timeout);
        pthread_mutex_unlock(&mutex);

        /* On timeout, actively poll virtqueue to check for pending messages */
        if (wait_ret == ETIMEDOUT) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST); /* Memory barrier before checking virtqueue */
            /* Poll notification even on timeout to catch messages without IRQ */
            remoteproc_get_notification(rproc, RSC_NOTIFY_ID_ANY);
            __atomic_thread_fence(__ATOMIC_SEQ_CST); /* Memory barrier after processing */
        }
    }
#else /* uC3 */
    (void) rproc;
#endif
error_return:
    return 0;
}

int platform_init(unsigned long proc_id, unsigned long rsc_id, unsigned long mbx_id, struct remoteproc **platform)
{
    struct remoteproc *rproc;

    if (!platform) {
        LPRINTF("Failed to initialize platform," "NULL pointer to store platform data.");
        return -EINVAL;
    }

    if ((proc_id >= RPVDEV_MAX_NUM) || (rsc_id >= RPVDEV_MAX_NUM)) {
        LPRINTF("Invalid rproc number specified.");
        return -EINVAL;
    }

    LPRINTF("proc_id:%ld rsc_id:%ld mbx_id:%ld", proc_id, rsc_id, mbx_id);
    rproc = platform_create_proc((int)proc_id, (int)rsc_id,(int)mbx_id);
    if (!rproc) {
        LPRINTF("Failed to create remoteproc device.");
        return -EINVAL;
    }
    *platform = rproc;

    return 0;
}

void platform_release_rpmsg_vdev(struct remoteproc *rproc, struct rpmsg_device *rpdev)
{
    /* Need to free memory regions already allocated but not used anymore? */
    struct rpmsg_virtio_device *rpmsg_vdev;

#ifdef __linux__
    virtio_clear_status(rproc->rsc_table);
#endif

    rpmsg_vdev = metal_container_of(rpdev, struct rpmsg_virtio_device, rdev);
    rpmsg_deinit_vdev(rpmsg_vdev);
    remoteproc_remove_virtio(rproc, rpmsg_vdev->vdev);
    metal_free_memory(rpmsg_vdev);
}

void platform_cleanup(struct remoteproc *rproc)
{
    if (rproc){
        (void)remoteproc_remove(rproc);

        /* Release the private area */
        metal_free_memory(rproc->priv);
        rproc->priv = NULL;
        metal_free_memory(rproc);
    }

    g_rsc_table = NULL;
}

/**
 * platform_clear_driver_ok - clear VIRTIO_CONFIG_STATUS_DRIVER_OK in the
 * shared SDRAM resource table so that CR8 stops sending MHU interrupts and
 * re-enters its "waiting for CA55 ready" poll loop.
 *
 * Safe to call from a signal handler (only performs a volatile store to
 * an already-mmap'd region — no malloc, no locks).
 */
void platform_clear_driver_ok(void)
{
#ifdef __linux__
    if (g_rsc_table) {
        virtio_clear_status(g_rsc_table);
    }
#endif
}

#ifndef __linux__ /* uC3 */
static void IpiTaskId(VP_INT exinf)
{
    struct remoteproc *rproc = exinf;
    struct remoteproc_priv *prproc = rproc->priv;
    int ret;

    while (1) {
        wai_sem(ipi[UIO_MBX].ipi_sem_id[prproc->notify_id]);

        /* Ignore a incoming interrupt if the virtio layer of a target remoteproc is not yet initialized */
        if (metal_list_is_empty(&rproc->vdevs)) {
            continue ;
        }

        ret = remoteproc_get_notification(rproc, RSC_NOTIFY_ID_ANY);
        if (ret) {
            LPRINTF("remoteproc_get_notification() failed with %d", reg);
        }
    }
}

static void start_ipi_task(void *platform)
{
    struct remoteproc *rproc = platform;
    struct remoteproc_priv *prproc = rproc->priv;
    const T_CSEM csem_ipi = { TA_TFIFO, 0, 1 };

    ipi[UIO_MBX].ipi_sem_id[prproc->notify_id] = acre_sem((T_CSEM*)&csem_ipi);

    T_CTSK ctsk_ipitask;
    ctsk_ipitask.tskatr = (TA_HLNG | TA_FPU | TA_ACT);
    ctsk_ipitask.name = "metal_ipi_tsk";
    ctsk_ipitask.itskpri = 2;
    ctsk_ipitask.stksz   = 0x400U;
    ctsk_ipitask.stk     = NULL;
    ctsk_ipitask.task =  (FP)IpiTaskId;
    ctsk_ipitask.exinf = (VP_INT)platform;
    ipi_tsk_id[prproc->notify_id] = acre_tsk((T_CTSK*)&ctsk_ipitask);

    if (ipi_tsk_id[prproc->notify_id] <= 0) {
        LPRINTF("Failed to register an interrupt service routine.");
    }
}
#endif
