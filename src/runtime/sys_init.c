/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

#include "metal/io.h"
#include "metal/device.h"
#include "metal/sys.h"
#include "FreeRTOS.h"
#include "rsc_table.h"
#include "platform_info.h"
#include "bsp_api.h"

extern void init_resource_table(void);

const metal_phys_addr_t metal_phys[] =
{
    MBX_REG_PA,
    CFG_RSCTBL_MEM_PA,
    CFG_VRING0_BASE0_PA,
    CFG_VRING0_BASE1_PA,
    CFG_VRING_SHM_BASE0_PA,
    CFG_VRING_SHM_BASE1_PA,
};

struct metal_device metal_dev_table[] =
{
    /* Mailbox device */
    {
        MBX_DEV_NAME,
        NULL,
        1,
        {
            {
                (void *) MBX_REG_VA,
                &metal_phys[0],
                MBX_MAP_SIZE,
                (sizeof(metal_phys_addr_t) << 3),
                (metal_phys_addr_t) (-1),
                0,
                {NULL}
            }
        },
        {NULL},
        1,
        (void *) MBX_INT_NUM,
    },

    /* Resource table */
    {
        CFG_RSCTBL_DEV_NAME,
        NULL,
        1,
        {
            {
                (void *) (CFG_RSCTBL_MEM_VA),
                &metal_phys[1],
                CFG_RSCTBL_MAP_SIZE,
                (sizeof(metal_phys_addr_t) << 3),
                (unsigned long) (-1),
                0,
                {NULL},
            }
        },
        {NULL},
        0,
        NULL,
    },

    /* RPMSG (vring) */
    {                                  /* vring #0 CTL */
        CFG_VRING_CTL_NAME0,
        NULL,
        1,
        {
            {
                (void *) (CFG_VRING0_BASE0_VA),
                &metal_phys[2],
                CFG_VRING_SIZE0,
                (sizeof(metal_phys_addr_t) << 3),
                (unsigned long) (-1),
                0,
                {NULL},
            }
        },
        {NULL},
        0,
        NULL,
    },
    {                                  /* vring #1 CTL */
        CFG_VRING_CTL_NAME1,
        NULL,
        1,
        {
            {
                (void *) (CFG_VRING0_BASE1_VA),
                &metal_phys[3],
                CFG_VRING_SIZE1,
                (sizeof(metal_phys_addr_t) << 3),
                (unsigned long) (-1),
                0,
                {NULL},
            }
        },
        {NULL},
        0,
        NULL,
    },
    {                                  /* vring #0 SHM */
        CFG_VRING_SHM_NAME0,
        NULL,
        1,
        {
            {
                (void *) (CFG_VRING_SHM_BASE0_VA),
                &metal_phys[4],
                CFG_VRING_SHM_SIZE0,
                (sizeof(metal_phys_addr_t) << 3),
                (unsigned long) (-1),
                0,
                {NULL},
            }
        },
        {NULL},
        0,
        NULL,
    },
    {                                  /* vring #1 SHM */
        CFG_VRING_SHM_NAME1,
        NULL,
        1,
        {
            {
                (void *) (CFG_VRING_SHM_BASE1_VA),
                &metal_phys[5],
                CFG_VRING_SHM_SIZE1,
                (sizeof(metal_phys_addr_t) << 3),
                (unsigned long) (-1),
                0,
                {NULL},
            }
        },
        {NULL},
        0,
        NULL,
    },
};

int init_system (void)
{
    unsigned int             i;
    int                      ret;
    struct metal_init_params metal_param = METAL_INIT_DEFAULTS;
    struct metal_device    * dev;

    /* init_resource_table() is done once, early in openamp_background_init_task(). */
    // init_resource_table();

    ret = metal_init(&metal_param);
    if (ret)
    {
        return ret;
    }

    /* register device */
    for (i = 0U; i < (sizeof(metal_dev_table) / sizeof(struct metal_device)); i++)
    {
        dev = &metal_dev_table[i];
        ret = metal_register_generic_device(dev);
        if (ret)
        {
            break;
        }
    }

    return ret;
}

void cleanup_system (void)
{
    metal_finish();
}
