/*
 * Copyright (c) 2016 Xilinx, Inc. All rights reserved.
 * Copyright (c) 2020, eForce Co., Ltd.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 ****************************************************************************
 * @par     History
 *          - rev 1.0 (2020.10.27) Imada
 *            Initial version for RZ/G2.
 ****************************************************************************
 */

#ifndef PLATFORM_INFO_H_
#define PLATFORM_INFO_H_

#include <openamp/rpmsg.h>
#include <openamp/remoteproc.h>
#include "OpenAMP_RPMsg_cfg.h"
#ifndef __linux__ /* uC3 */
#include "RZG2_UC3.h"
#include "kernel.h"
#endif

/* gettid() was added to glibc 2.30 (unistd.h); provide a fallback for older sysroots. */
#if !defined(__GLIBC__) || (__GLIBC__ < 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 30)
static inline pid_t gettid(void) {
    return syscall(SYS_gettid);
}
#endif
// Macros for printf
#ifdef __linux__

extern pid_t g_tid_cm33;
extern pid_t g_tid_cr8_0;
extern pid_t g_tid_cr8_1;
#define LPRINTF(format, ...) \
do { \
    pid_t tid = gettid(); \
    if (tid == g_tid_cm33) { \
        (void)printf("[CM33] " format "\n", ##__VA_ARGS__); \
    } else if(tid == g_tid_cr8_0) { \
        (void)printf("[CR8_0 ] " format "\n", ##__VA_ARGS__); \
    } else if(tid == g_tid_cr8_1) { \
        (void)printf("[CR8_1 ] " format "\n", ##__VA_ARGS__); \
    } else {\
        (void)printf("[%d] " format "\n", tid, ##__VA_ARGS__); \
    } \
} while(0)

#define LPERROR(format, ...) LPRINTF("ERROR: " format, ##__VA_ARGS__)
#else /* uC3 */
#define LPRINTF(format, ...) {}
#define LPERROR(format, ...) {}
#endif

/** @def for debug, read atomic variable */
#define atomic_load_bool(x) __extension__({ \
    __auto_type __atomic_load_ptr = (x); \
    __typeof__(*__atomic_load_ptr) __atomic_load_tmp; \
    char *p = (char*)&__atomic_load_tmp; \
    __atomic_load(__atomic_load_ptr, &__atomic_load_tmp, 5); \
    int a = *p; \
    a;})

// Page size on Linux (Default: 4KB)
#define PAGE_SIZE (0x01000U) // 4KB page size as the dafault value

// Mailbox config
#define MBX_DEV_NAME    "10480000.mbox-uio"
#define MBX_NO          (0x5)/* Maibox number (0, 1, ..., or 5 this program uses */
#define RSP_NO          (0x8)/* Maibox number (0, 1, ..., or 5 this program uses */
// The number of maximum MHU channels are able to use for messaging or responding
#define MHU_CH_NUM_MAX (12U)

// Mailbox user ID
#if defined(__linux__)
#define MBX_LOCAL    (0x1U)
#define MBX_REMOTE   (0x0U)
#else /* uC3 */
#define MBX_LOCAL    (0x0U)
#define MBX_REMOTE   (0x1U)
#endif

// Definitions representing a MBX IRQ number
#if defined(__linux__)


/** @enum MBX_IRQ_ID - interrupt ID for each MHU_B channel is used */
enum MBX_IRQ_ID
{
// Interrupt ID for the responding channel on CM33
    INT_MHU_MSG_CH36_NS = 376,    //msg_ch36_ns
    INT_MHU_MSG_CH37_NS = 377,    //msg_ch37_ns
    INT_MHU_MSG_CH38_NS = 378,    //msg_ch38_ns
    INT_MHU_MSG_CH39_NS = 379,    //msg_ch39_ns

// Interrupt ID for the responding channel on CR8_0
    INT_MHU_MSG_CH24_NS = 368,    //msg_ch24_ns
    INT_MHU_MSG_CH25_NS = 369,    //msg_ch25_ns
    INT_MHU_MSG_CH26_NS = 370,    //msg_ch26_ns
    INT_MHU_MSG_CH27_NS = 371,    //msg_ch27_ns

// Interrupt ID for the responding channel on CR8_1
    INT_MHU_MSG_CH30_NS = 372,    //msg_ch30_ns
    INT_MHU_MSG_CH31_NS = 373,    //msg_ch31_ns
    INT_MHU_MSG_CH32_NS = 374,    //msg_ch32_ns
    INT_MHU_MSG_CH33_NS = 375,    //msg_ch33_ns

// Interrupt ID for the messaging channel on CM33
    INT_MHU_RSP_CH8_NS = 385,     //rsp_ch8_ns
    INT_MHU_RSP_CH20_NS = 391,    //rsp_ch20_ns
    INT_MHU_RSP_CH31_NS = 397,    //rsp_ch31_ns
    INT_MHU_RSP_CH39_NS = 403,    //rsp_ch39_ns

// Interrupt ID for the messaging channel on CR8_0
    INT_MHU_RSP_CH6_NS = 383,     //rsp_ch6_ns
    INT_MHU_RSP_CH18_NS = 389,    //rsp_ch18_ns
    INT_MHU_RSP_CH27_NS = 395,    //rsp_ch27_ns
    INT_MHU_RSP_CH37_NS = 401,    //rsp_ch37_ns

// Interrupt ID for the messaging channel on CR8_0
    INT_MHU_RSP_CH7_NS = 384,     //rsp_ch7_ns
    INT_MHU_RSP_CH19_NS = 390,    //rsp_ch19_ns
    INT_MHU_RSP_CH30_NS = 396,    //rsp_ch30_ns
    INT_MHU_RSP_CH38_NS = 402,    //rsp_ch38_ns
};
#endif

// MHU register definitions
#define MHU_REG_BASE   (0x10480000U)
#define MHU_MAP_SIZE   (0x7FFU)

// Register definitions mainly used for metal_device_open()
#define MBX_REG_BASE    (MHU_REG_BASE) // map all the mailbox registers due to the 4KB page size on Linux
#define MBX_REG_SIZE    (MHU_MAP_SIZE) // including register regions for both MBX and HWSPL
#define MBX_MAP_SIZE    (PAGE_SIZE) // uio-based registers must be page-aligned (also applied to uC3)

// Core specific settings
#ifdef __linux__ /* Linux (A55) */
#define DEV_BUS_NAME            "platform"

#else /* uC3 (M33) */
#define DEV_BUS_NAME            "generic"

#endif /* end of #ifdef __linux__ */

// The number of maximum mailbox channels on the mailbox cluster
#define MBX_MAX_CHN (42U)
#define MBX_CH_NUM (4U)

// Macros for mailboxes
#define MBX_REG_START   (0x0U) // Registers for mailboxes start at 0x10400000U
#define MBX_REG_END     (0x7FFU)
#define MBX_MSG_INT_STS_REG(y)  (MBX_REG_START + (y)*0x20U)
#define MBX_MSG_INT_SET_REG(y)  (MBX_MSG_INT_STS_REG(y) + 0x4U)
#define MBX_MSG_INT_CLR_REG(y)  (MBX_MSG_INT_STS_REG(y) + 0x8U)
#define MBX_RSP_INT_STS_REG(y) (MBX_REG_START + (y)*0x20U + 0x10U)
#define MBX_RSP_INT_SET_REG(y) (MBX_RSP_INT_STS_REG(y) + 0x4U)
#define MBX_RSP_INT_CLR_REG(y) (MBX_RSP_INT_STS_REG(y) + 0x8U)

// Shared memory config
#define SHM_DEV_NAME    "42f01000.mhu-shm"

// Macros for shared memory allocation
#define MBX_SHMEM_CH_OFFSET(y) (0x08U*(y))
#define MBX_SHMEM_TXD_OFFSET (0x00U)
#define MBX_SHMEM_RXD_OFFSET (0x04U)

// The number of maximum remoteproc vdevs
#define RPVDEV_MAX_NUM (MBX_MAX_CHN)

/** @enum UIO_DEV - uio device index */
enum UIO_DEV {
    UIO_MBX,
    UIO_RECEIVER1,
    UIO_RECEIVER2,
    UIO_RECEIVER3,
    UIO_MAX,
};

enum MHU_SEND_TYPE {
    MHU_SEND_TYPE_MSG = 0,
    MHU_SEND_TYPE_RSP,
};

struct mhu_send_type{
    unsigned int send_type;
};

struct mbx_channel{
    unsigned int msg;
    unsigned int rsp;
    unsigned int irq_info;
};

struct ipi_info {
    const char *name;
    const char *bus_name;
    struct metal_device *dev;
    struct metal_io_region *io;
    uintptr_t irq_info;
    int registered;
    struct mbx_channel mbx_chn;
    unsigned int chn_mask; /**< IPI channel mask */
#ifdef __linux__
    atomic_flag sync;
    uint32_t notify_id;
#else
    ID ipi_sem_id[CFG_RPMSG_SVCNO];
#endif
};

struct shm_info {
    const char *name;
    const char *bus_name;
    struct metal_device *dev; /**< pointer to shared memory device */
    struct metal_io_region *io; /**< pointer to shared memory i/o region */
    struct remoteproc_mem *mem; /**< shared memory */
};

/**
 * @enum VRING_INFO_IDXS
 * @brief memory device index that platform use
 */
enum VRING_INFO_IDXS {
    VRING_RSC,
    VRING_CTL,
    VRING_SHM,
    VRING_MHU,
    VRING_MAX,
};

struct vring_info {
    struct shm_info rsc;
    struct shm_info ctl;
    struct shm_info shm;
};

struct remoteproc_priv {
    unsigned int notify_id;
    unsigned int mbx_chn_id;
    struct shm_info *vr_info;
};

/**
 * @struct comm_arg
 * @remarks
 * arguments to be hand over to the test thread
 * for each test execution.
 */
struct comm_arg {
    struct remoteproc *platform;
    int channel;
    int target;
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
int platform_init(unsigned long proc_id, unsigned long rsc_id, unsigned long mbx_id, struct remoteproc **platform);

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
struct rpmsg_device *
platform_create_rpmsg_vdev(struct remoteproc *platform, unsigned int vdev_index,
               unsigned int role,
               void (*rst_cb)(struct virtio_device *vdev),
               rpmsg_ns_bind_cb ns_bind_cb);

/**
 * platform_poll - platform poll function
 *
 * @platform: pointer to the platform
 *
 * return negative value for errors, otherwise 0.
 */
int platform_poll(struct remoteproc *platform);

/**
 * platform_release_rpmsg_vdev - release rpmsg virtio device
 *
 * @platform: pointer to the platform
 * @rpdev: pointer to the rpmsg device
 */
void platform_release_rpmsg_vdev(struct remoteproc *platform, struct rpmsg_device *rpdev);

/**
 * platform_cleanup - clean up the platform resource
 *
 * @platform: pointer to the platform
 */
void platform_cleanup(struct remoteproc *platform);

/**
 * platform_clear_driver_ok - clear VIRTIO_CONFIG_STATUS_DRIVER_OK in the
 * shared SDRAM resource table.
 *
 * Call this before _exit() so CR8 sees the CA55-side disconnect and
 * re-enters its "waiting for DRIVER_OK" poll loop instead of continuing
 * to fire MHU interrupts into an unresponsive CA55 kernel.
 *
 * Safe to call from a signal handler.
 */
void platform_clear_driver_ok(void);

#endif /* PLATFORM_INFO_H_ */
