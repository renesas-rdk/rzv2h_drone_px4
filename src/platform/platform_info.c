/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

#include <metal/atomic.h>
#include <metal/assert.h>
#include <metal/device.h>
#include <metal/irq.h>
#include <metal/utilities.h>
#include <openamp/rpmsg_virtio.h>
#include <openamp/remoteproc.h>
#include "platform_info.h"
#include "rsc_table.h"
#ifdef __linux__
 #include <sched.h>
 #include <stddef.h>
#else                                  /* FreeRTOS */
 #include <metal/sys.h>
 #include <sys/errno.h>
#endif

#ifdef __linux__
 #define _rproc_wait()    (sched_yield())
#endif

/* Variables */
/* IPI dedicated task */
#ifndef __linux__                      /* FreeRTOS */
static TaskHandle_t ipi_tsk_id[CFG_RPMSG_SVCNO] = {0, 0};
#endif

/* IPI(MBX) information */
struct ipi_info ipi =
{
    MBX_DEV_NAME,                      // name
    DEV_BUS_NAME,                      // bus_name
    NULL,                              // dev
    NULL,                              // io
    MBX_INT_NUM,                       // irq_info
    0,                                 // registered
    {0,    0         },                // mbx_chn (not used on this SoC)
    0,                                 // chn_mask (not used on this SoC)
#ifdef __linux__
    ATOMIC_FLAG_INIT,                  // sync
    0,                                 // notify_id
#else /* FreeRTOS */
    {NULL, NULL      },                // ipi_sem_id
#endif
};

/* vring information */
struct vring_info vrinfo[CFG_RPMSG_SVCNO] =
{
    {                                  // vinfo[0]
        {                              // rsc
            CFG_RSCTBL_DEV_NAME,       // name
            DEV_BUS_NAME,              // bus_name
            NULL,                      // dev
            NULL,                      // io
            {0},                       // mem
        },
        {                              // ctl
            CFG_VRING_CTL_NAME0,       // name
            DEV_BUS_NAME,              // bus_name
            NULL,                      // dev
            NULL,                      // io
            {0},                       // mem
        },
        {                              // shm
            CFG_VRING_SHM_NAME0,       // name
            DEV_BUS_NAME,              // bus_name
            NULL,                      // dev
            NULL,                      // io
            {0},                       // mem
        },
    },
    {                                  // vinfo[1]
        {                              // rsc
            CFG_RSCTBL_DEV_NAME,       // name
            DEV_BUS_NAME,              // bus_name
            NULL,                      // dev
            NULL,                      // io
            {0},                       // mem
        },
        {                              // ctl
            CFG_VRING_CTL_NAME1,       // name
            DEV_BUS_NAME,              // bus_name
            NULL,                      // dev
            NULL,                      // io
            {0},                       // mem
        },
        {                              // shm
            CFG_VRING_SHM_NAME1,       // name
            DEV_BUS_NAME,              // bus_name
            NULL,                      // dev
            NULL,                      // io
            {0},                       // mem
        },
    },
};

/* processor operations at RZ/V2. It defines
 * notification operation and remote processor managementi operations. */
extern struct remoteproc_ops rzv2_proc_ops;

/* RPMsg virtio shared buffer pool (one per resource table) */
static struct rpmsg_virtio_shm_pool shpool[CFG_RPMSG_SVCNO];

#ifndef __linux__                      /* FreeRTOS */
static void start_ipi_task(void * platform);

#endif

#ifdef __linux__
static inline void virtio_clear_status (void * rsc_table) {
    struct remote_resource_table * rt = (struct remote_resource_table *) rsc_table;

    rt->rpmsg_vdev.status = 0x0;
}

#endif

static struct remoteproc * platform_create_proc (int proc_index, int rsc_index)
{
    void                   * rsc_table;
    unsigned int             rsc_size;
    int                      ret;
    struct remoteproc      * rproc_inst;
    struct remoteproc_priv * rproc_priv;
#ifdef __linux__
    metal_phys_addr_t pa;
#endif

    /* Allocate and initialize remoteproc_priv instance */
    rproc_priv = metal_allocate_memory(sizeof(struct remoteproc_priv));
    if (!rproc_priv)
    {
        return NULL;
    }

    memset(rproc_priv, 0, sizeof(*rproc_priv));
    rproc_priv->notify_id = (unsigned int) proc_index;
    rproc_priv->rsc_index = (unsigned int)rsc_index;
    rproc_priv->vr_info   = &vrinfo[rsc_index];

    /* Allocate remoteproc instance */
    rproc_inst = metal_allocate_memory(sizeof(struct remoteproc));
    if (!rproc_inst)
    {
        goto err1;
    }

    memset(rproc_inst, 0, sizeof(*rproc_inst));

    /* remoteproc initialization */
    if (!remoteproc_init(rproc_inst, &rzv2_proc_ops, rproc_priv))
    {
        goto err2;
    }

    /*
     * Mmap shared memories
     * Or shall we constraint that they will be set as carved out
     * in the resource table?
     */
#ifdef __linux__
    rsc_size  = sizeof(struct remote_resource_table);
    pa        = CFG_RSCTBL_MEM_PA + rsc_index * rsc_size;
    rsc_table = remoteproc_mmap(rproc_inst, &pa, NULL, rsc_size, 0, NULL);
    if (!rsc_table)
    {
        LPRINTF("Failed to map the resource table.\n");
        goto err2;
    }

#else                                  /* FreeRTOS */
    rsc_table          = get_resource_table(rsc_index, &rsc_size);
    rproc_inst->rsc_io = rproc_priv->vr_info->rsc.io;
#endif

    /* Parse resource table to remoteproc */
    ret = remoteproc_set_rsc_table(rproc_inst, rsc_table, rsc_size);
    if (ret)
    {
        LPRINTF("Failed to initialize remoteproc\n");
        goto err2;
    }

    LPRINTF("Initialize remoteproc successfully.\n");

    return rproc_inst;
err2:
    (void) remoteproc_remove(rproc_inst);
    metal_free_memory(rproc_inst);
err1:
    metal_free_memory(rproc_priv);

    return NULL;
}

struct  rpmsg_device * platform_create_rpmsg_vdev (void           * platform,
                                                   unsigned int     vdev_index,
                                                   unsigned int     role,
                                                   void (         * rst_cb)(struct virtio_device * vdev),
                                                   rpmsg_ns_bind_cb ns_bind_cb)
{
    struct remoteproc          * rproc = platform;
    struct remoteproc_priv     * prproc;
    struct rpmsg_virtio_device * rpmsg_vdev;
    struct virtio_device       * vdev;
    struct metal_io_region     * shbuf_io;
    metal_phys_addr_t            pa;
#ifdef __linux__
    void * shbuf;
    size_t len;
#endif
    int ret;

    prproc = rproc->priv;

    rpmsg_vdev = metal_allocate_memory(sizeof(*rpmsg_vdev));
    if (!rpmsg_vdev)
    {
        return NULL;
    }

    memset(rpmsg_vdev, 0, sizeof(*rpmsg_vdev));

    LPRINTF("creating remoteproc virtio\n");
    vdev = remoteproc_create_virtio(rproc, (int) vdev_index, role, rst_cb);
    if (!vdev)
    {
        LPRINTF("failed remoteproc_create_virtio\n");
        goto err;
    }

    pa       = metal_io_phys(prproc->vr_info->shm.io, 0x0U);
    shbuf_io = remoteproc_get_io_with_pa(rproc, pa);
    if (!shbuf_io)
    {
        LPRINTF("failed remoteproc_get_io_with_pa\n");
        goto err;
    }

    /* Only RPMsg virtio master needs to initialize the shared buffers pool */
#ifdef __linux__
    LPRINTF("initializing rpmsg shared buffer pool\n");
    shbuf = metal_io_phys_to_virt(shbuf_io, pa);
    len   = metal_io_region_size(prproc->vr_info->shm.io);
    rpmsg_virtio_init_shm_pool(&shpool[prproc->rsc_index], shbuf, len);
#endif

    LPRINTF("initializing rpmsg vdev\n");

    /* RPMsg virtio slave can set shared buffers pool argument to NULL */
    ret = rpmsg_init_vdev(rpmsg_vdev, vdev, ns_bind_cb, shbuf_io,
                          &shpool[prproc->rsc_index]);
    if (ret)
    {
        LPRINTF("failed rpmsg_init_vdev\n");
        goto err;
    }

#ifndef __linux__                      /* FreeRTOS */
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

int platform_poll (void * priv)
{
#ifdef __linux__
    struct remoteproc * rproc = priv;
    unsigned int        flags;

    while (1)
    {
        flags = metal_irq_save_disable();
        if (!(atomic_flag_test_and_set(&ipi.sync)))
        {
            metal_irq_restore_enable(flags);
            remoteproc_get_notification(rproc, RSC_NOTIFY_ID_ANY);
            break;
        }

        metal_irq_restore_enable(flags);
        _rproc_wait();
    }

#else                                  /* FreeRTOS */
    struct remoteproc * rproc = priv;

    if (rproc != NULL)
    {
        /* Drive notifications manually when no IRQ kick arrives */
        remoteproc_get_notification(rproc, RSC_NOTIFY_ID_ANY);
    }
#endif

    return 0;
}

int platform_init (unsigned long proc_id, unsigned long rsc_id, void ** platform)
{
    struct remoteproc * rproc;

    if (!platform)
    {
        LPRINTF("Failed to initialize platform," "NULL pointer to store platform data.\n");

        return -EINVAL;
    }

    if ((proc_id >= RSC_MAX_NUM) || (rsc_id >= RSC_MAX_NUM))
    {
        LPRINTF("Invalid rproc number specified.\n");

        return -EINVAL;
    }

    rproc = platform_create_proc((int) proc_id, (int) rsc_id);
    if (!rproc)
    {
        LPRINTF("Failed to create remoteproc device.\n");

        return -EINVAL;
    }

    *platform = rproc;

    return 0;
}

void platform_release_rpmsg_vdev (void * platform, struct rpmsg_device * rpdev)
{
    /* Need to free memory regions already allocated but not used anymore? */
    struct remoteproc          * rproc = platform;
    struct rpmsg_virtio_device * rpmsg_vdev;
    struct remoteproc_priv     * prproc = rproc->priv;
    SemaphoreHandle_t semid;

#ifdef __linux__
    virtio_clear_status(rproc->rsc_table);
#endif

#ifndef __linux__                      /* FreeRTOS */
    vTaskDelete(ipi_tsk_id[prproc->notify_id]);
    semid = ipi.ipi_sem_id[prproc->notify_id];
    ipi.ipi_sem_id[prproc->notify_id] = 0;
    vSemaphoreDelete(semid);
#endif

    rpmsg_vdev = metal_container_of(rpdev, struct rpmsg_virtio_device, rdev);
    rpmsg_deinit_vdev(rpmsg_vdev);
    remoteproc_remove_virtio(rproc, rpmsg_vdev->vdev);
    metal_free_memory(rpmsg_vdev);
}

void platform_cleanup (void * platform)
{
    struct remoteproc      * rproc = platform;
    struct remoteproc_priv * prproc;
    struct metal_device    * dev;

    if (rproc)
    {
        prproc = rproc->priv;
        if (rproc->priv)
        {
            /* Release allocated resource */
            /* Shared memory devices */
            metal_list_del(&(prproc->vr_info->ctl.mem.node));
            dev = prproc->vr_info->ctl.dev;
            if (dev)
            {
                metal_device_close(dev);
            }

            metal_list_del(&(prproc->vr_info->shm.mem.node));
            dev = prproc->vr_info->shm.dev;
            if (dev)
            {
                metal_device_close(dev);
            }

            /* Resource table device */
            metal_list_del(&(prproc->vr_info->rsc.mem.node));
            dev = prproc->vr_info->rsc.dev;
            if (dev)
            {
                metal_device_close(dev);
            }

            /* Release the private area */
            metal_free_memory(rproc->priv);
        }

        (void) remoteproc_remove(rproc);
        metal_free_memory(rproc);
    }
}

#ifndef __linux__                      /* FreeRTOS */
static void IpiTaskId(void * exinf);

static void IpiTaskId (void * exinf)
{
    struct remoteproc      * rproc  = exinf;
    struct remoteproc_priv * prproc = rproc->priv;
    int ret;

    while (1)
    {
        xSemaphoreTake(ipi.ipi_sem_id[prproc->notify_id], -1);

        /* Ignore a incoming interrupt if the virtio layer of a target remoteproc is not yet initialized */
        if (metal_list_is_empty(&rproc->vdevs))
        {
            continue;
        }

        ret = remoteproc_get_notification(rproc, RSC_NOTIFY_ID_ANY);
        if (ret)
        {
            LPRINTF("remoteproc_get_notification() failed with %d", ret);
        }
    }
}

static void start_ipi_task (void * platform)
{
    struct remoteproc      * rproc  = platform;
    struct remoteproc_priv * prproc = rproc->priv;

    ipi.ipi_sem_id[prproc->notify_id] = xSemaphoreCreateCounting(1, 0);

    BaseType_t xReturned;
    xReturned =
        xTaskCreate(IpiTaskId,
                    "metal_ipi_task",
                    0x400U,
                    platform,
                    (tskIDLE_PRIORITY + 4),
                    &ipi_tsk_id[prproc->notify_id]);

    if (xReturned != pdPASS)
    {
        LPRINTF("Failed to register an interrupt service routine.\n");
    }
}

#endif
