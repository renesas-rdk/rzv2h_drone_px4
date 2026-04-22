/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

#ifndef PLATFORM_INFO_H_
#define PLATFORM_INFO_H_

#include <openamp/rpmsg.h>
#include <openamp/remoteproc.h>
#include "OpenAMP_RPMsg_cfg.h"
#include "bsp_api.h"
#include "FreeRTOSConfig.h"
#include "task.h"
#include "semphr.h"
#include "r_mhu_api.h"

#define DEV_BUS_NAME     "generic"
#ifdef __linux__
 #define MBX_REG_BASE    0x10480000
#else
#if BSP_SUPPORT_CORE_CM33
 #define MBX_REG_PA      0x10480000
 #define MBX_REG_VA      0x50480000
#else
 #define MBX_REG_PA      0x10480000
 #define MBX_REG_VA      0x10480000
#endif
#endif
#define MBX_MAP_SIZE     0x00000800

#ifdef __linux__
 #define MBX_INT_NUM     75            // RSP1_NS_IRQn
#else
#if BSP_SUPPORT_CORE_CM33
#define MBX_INT_NUM     293            // MSG5_NS_IRQn
#else
#define MBX_INT_NUM     314            // MSG3_NS_IRQn
#endif
#endif

#define RSC_MAX_NUM      2

// Macros for printf
#ifdef __linux__
 #define LPRINTF(format, ...)    (printf(format, ## __VA_ARGS__))
 #define LPERROR(format, ...)    (LPRINTF("ERROR: " format, ## __VA_ARGS__))
#else                                  /* FreeRTOS */

extern int printf_raw(const char * format, ...);

/*
 * If you want to enable OpenAMP debug output,
 * you need to use the SCIF output from the expansion connector.
 * And edit config.h to set DEBUG_SCIF to 1.
 */
 #if 0
  #define LPRINTF(format, ...)    (printf_raw(format, ## __VA_ARGS__))
  #define LPERROR(format, ...)    (LPRINTF("ERROR: " format, ## __VA_ARGS__))
 #else
  #define LPRINTF(format, ...)    {}
  #define LPERROR(format, ...)    {}
 #endif

#endif

// Page size on Linux (Default: 4KB)
#define PAGE_SIZE       (0x01000U)     // 4KB page size as the dafault value

// Mailbox config
#define MBX_DEV_NAME    "0x10480000.mbox-uio"
#define MBX_NO          (0x0U)         /* Maibox number (0, 1, ..., or 7) this program uses */

struct ipi_info
{
    const char             * name;
    const char             * bus_name;
    struct metal_device    * dev;
    struct metal_io_region * io;
    uintptr_t                irq_info;
    int          registered;
    unsigned int mbx_chn[CFG_RPMSG_SVCNO];
    unsigned int chn_mask;             /**< IPI channel mask */
#ifdef __linux__
    atomic_flag sync;
    uint32_t    notify_id;
#else
    SemaphoreHandle_t ipi_sem_id[CFG_RPMSG_SVCNO];
#endif
};

struct shm_info
{
    const char             * name;
    const char             * bus_name;
    struct metal_device    * dev;      /**< pointer to shared memory device */
    struct metal_io_region * io;       /**< pointer to shared memory i/o region */
    struct remoteproc_mem    mem;      /**< shared memory */
};

struct vring_info
{
    struct shm_info rsc;
    struct shm_info ctl;
    struct shm_info shm;
};

struct remoteproc_priv
{
    unsigned int        notify_id;
    unsigned int        mbx_chn_id;
    unsigned int        rsc_index;
    struct vring_info * vr_info;
};

/**
 * platform_init - initialize the platform
 *
 * It will initialize the platform.
 *
 * @proc_id: processor id
 * @rsc_id: resource id
 * @platform: pointer to store the platform data pointer
 *
 * return 0 for success or negative value for failure
 */
int platform_init(unsigned long proc_id, unsigned long rsc_id, void ** platform);

/**
 * platform_create_rpmsg_vdev - create rpmsg vdev
 *
 * It will create rpmsg virtio device, and returns the rpmsg virtio
 * device pointer.
 *
 * @platform: pointer to the private data
 * @vdev_index: index of the virtio device, there can more than one vdev
 *              on the platform.
 * @role: virtio master or virtio slave of the vdev
 * @rst_cb: virtio device reset callback
 * @ns_bind_cb: rpmsg name service bind callback
 *
 * return pointer to the rpmsg virtio device
 */
struct rpmsg_device * platform_create_rpmsg_vdev(void           * platform,
                                                 unsigned int     vdev_index,
                                                 unsigned int     role,
                                                 void (         * rst_cb)(struct virtio_device * vdev),
                                                 rpmsg_ns_bind_cb ns_bind_cb);

/**
 * platform_poll - platform poll function
 *
 * @platform: pointer to the platform
 *
 * return negative value for errors, otherwise 0.
 */
int platform_poll(void * platform);

/**
 * platform_release_rpmsg_vdev - release rpmsg virtio device
 *
 * @platform: pointer to the platform
 * @rpdev: pointer to the rpmsg device
 */
void platform_release_rpmsg_vdev(void * platform, struct rpmsg_device * rpdev);

/**
 * platform_cleanup - clean up the platform resource
 *
 * @platform: pointer to the platform
 */
void platform_cleanup(void * platform);

#endif                                 /* PLATFORM_INFO_H_ */
