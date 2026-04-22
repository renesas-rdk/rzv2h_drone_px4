/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

#include "rsc_table.h"

#ifdef __ICCARM__
 #pragma location=".resource_table"
struct remote_resource_table resources[2];
#else
struct remote_resource_table __resource resources[2];
#endif

struct remote_resource_table resources_init[] =
{
    {
        /* Version */
        1,

        /* NUmber of table entries */
        NUM_TABLE_ENTRIES,

        /* reserved fields */
        {0,                   0},

        /* Offsets of rsc entries */
        {
            offsetof(struct remote_resource_table, rproc_mem),
            offsetof(struct remote_resource_table, rpmsg_vdev)
        },

        {RSC_RPROC_MEM,       RPMSG_MEM_BASE, RPMSG_MEM_BASE, RPMSG_MEM_SIZE, 0},

#ifdef __ICCARM__

        /* Virtio device entry */
        {
            RSC_VDEV, VIRTIO_ID_RPMSG_, VRING_NOTIFYID0, RPMSG_IPU_C0_FEATURES, 0, 0, 0,
            NUM_VRINGS,{0,                   0},
            {
                /* Vring rsc entry - part of vdev rsc entry */
                {CFG_VRING0_BASE0_PA, CFG_VRING_ALIGN0, CFG_RPMSG_NUM_BUFS0, 0, 0},
                {CFG_VRING1_BASE0_PA, CFG_VRING_ALIGN0, CFG_RPMSG_NUM_BUFS0, 0, 0}
            }
        }
#else

        /* Virtio device entry */
        {
            RSC_VDEV, VIRTIO_ID_RPMSG_, VRING_NOTIFYID0, RPMSG_IPU_C0_FEATURES, 0, 0, 0,
            NUM_VRINGS,{0,                   0},
        },

        /* Vring rsc entry - part of vdev rsc entry */
        {CFG_VRING0_BASE0_PA, CFG_VRING_ALIGN0, CFG_RPMSG_NUM_BUFS0, 0, 0},
        {CFG_VRING1_BASE0_PA, CFG_VRING_ALIGN0, CFG_RPMSG_NUM_BUFS0, 0, 0}
#endif
    },
    {
        /* Version */
        1,

        /* NUmber of table entries */
        NUM_TABLE_ENTRIES,

        /* reserved fields */
        {0,                   0},

        /* Offsets of rsc entries */
        {
            offsetof(struct remote_resource_table, rproc_mem),
            offsetof(struct remote_resource_table, rpmsg_vdev)
        },

        {RSC_RPROC_MEM,       RPMSG_MEM_BASE, RPMSG_MEM_BASE, RPMSG_MEM_SIZE, 0},

#ifdef __ICCARM__

        /* Virtio device entry */
        {
            RSC_VDEV, VIRTIO_ID_RPMSG_, VRING_NOTIFYID1, RPMSG_IPU_C0_FEATURES, 0, 0, 0,
            NUM_VRINGS,{0,                   0},
            {
                /* Vring rsc entry - part of vdev rsc entry */
                {CFG_VRING0_BASE1_PA, CFG_VRING_ALIGN1, CFG_RPMSG_NUM_BUFS1, 1, 0},
                {CFG_VRING1_BASE1_PA, CFG_VRING_ALIGN1, CFG_RPMSG_NUM_BUFS1, 1, 0}
            }
        }
#else

        /* Virtio device entry */
        {
            RSC_VDEV, VIRTIO_ID_RPMSG_, VRING_NOTIFYID1, RPMSG_IPU_C0_FEATURES, 0, 0, 0,
            NUM_VRINGS,{0,                   0},
        },

        /* Vring rsc entry - part of vdev rsc entry */
        {CFG_VRING0_BASE1_PA, CFG_VRING_ALIGN1, CFG_RPMSG_NUM_BUFS1, 1, 0},
        {CFG_VRING1_BASE1_PA, CFG_VRING_ALIGN1, CFG_RPMSG_NUM_BUFS1, 1, 0}
#endif
    },
};

void init_resource_table (void)
{
    memcpy(resources, resources_init, sizeof(resources_init));
}

void * get_resource_table (int rsc_id, unsigned int * len)
{
    *len = sizeof(resources[rsc_id]);

    return (void *) &resources[rsc_id];
}
