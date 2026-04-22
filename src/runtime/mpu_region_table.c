/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "bsp_api.h"

/***********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Global Variables
 **********************************************************************************************************************/

#if BSP_SUPPORT_CORE_CR8
__WEAK mpu_region_table_cfg_t const g_mpu_region_table_array[BSP_FEATURE_MPU_REGION_NUM] =
{
 #if (0 == BSP_CR8_CORE_NUM)

    /* MPU definition table for CR8 core0 */
    /* region 0 (ITCM) */
    {0,                                                    /* region number */
     0x00000000,                                           /* base address */
     BSP_MPU_SIZE_128KB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE},

    /* region 1 (DTCM) */
    {1,                                                    /* region number */
     0x00020000,                                           /* base address */
     BSP_MPU_SIZE_128KB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE |
     BSP_MPU_ATTR_XN},

    /* region 2 for core0 (RCPU-SRAM Cacheable) */
    {2,                                                    /* region number */
     0x08180000,                                           /* base address */
     BSP_MPU_SIZE_128KB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_ENABLE},

    /* region 3 fore core0 (RCPU-SRAM Non-cacheable) */
    {3,                                                    /* region number */
     0x081A0000,                                           /* base address */
     BSP_MPU_SIZE_128KB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE},

    /* region 4 (Peripheral) */
    {4,                                                    /* region number */
     0x10000000,                                           /* base address */
     BSP_MPU_SIZE_256MB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_DEVICE_SHAREABLE |
     BSP_MPU_ATTR_XN},

    /* region5 (xSPI) */
    {5,                                                    /* region number */
     0x20000000,                                           /* base address */
     BSP_MPU_SIZE_256MB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READONLY_USER_READONLY |   /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_ENABLE},

    /* region 6 (DDR Cacheable) */
    {6,                                                    /* region number */
     0x40800000,                                           /* base address */
     BSP_MPU_SIZE_8MB,                                     /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_ENABLE},

    /* region 7 (DDR Non-cacheable) */
    {7,                                                    /* region number */
     0x41800000,                                           /* base address */
     BSP_MPU_SIZE_8MB,                                     /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE},

    /* region 8 (Open AMP RSCTBL + MHU SHMEM) */
    {8,                                                    /* region number */
     0x42F00000,                                           /* base address */
     BSP_MPU_SIZE_8KB,                                     /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE |
     BSP_MPU_ATTR_S},

    /* region 9 (Open AMP VRING) */
    {9,                                                    /* region number */
     0x43000000,                                           /* base address */
     BSP_MPU_SIZE_16MB,                                    /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE |
     BSP_MPU_ATTR_S},

    /* region 10 (DDR Cacheable extension) */
    {10,                                                   /* region number */
     0x41000000,                                           /* base address */
     BSP_MPU_SIZE_8MB,                                     /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_ENABLE},

    /* region 11 (No use) */
    {11,                               /* region number */
     0x00000000,                       /* base address */
     0x00000000,                       /* size */
     0x00000000,                       /* sub region enable */
     0x00000000                        /* attribute */
    },

    /* region 12 (No use) */
    {12,                               /* region number */
     0x00000000,                       /* base address */
     0x00000000,                       /* size */
     0x00000000,                       /* sub region enable */
     0x00000000                        /* attribute */
    },

    /* region 13 (No use) */
    {13,                               /* region number */
     0x00000000,                       /* base address */
     0x00000000,                       /* size */
     0x00000000,                       /* sub region enable */
     0x00000000                        /* attribute */
    },

    /* region 14 (No use) */
    {14,                               /* region number */
     0x00000000,                       /* base address */
     0x00000000,                       /* size */
     0x00000000,                       /* sub region enable */
     0x00000000                        /* attribute */
    },

    /* region 15 (No use) */
    {15,                               /* region number */
     0x00000000,                       /* base address */
     0x00000000,                       /* size */
     0x00000000,                       /* sub region enable */
     0x00000000                        /* attribute */
    }
 #else

    /* MPU definition table for CR8 core1 */
    /* region 0 (ITCM) */
    {0,                                                    /* region number */
     0x00000000,                                           /* base address */
     BSP_MPU_SIZE_128KB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE},

    /* region 1 (DTCM) */
    {1,                                                    /* region number */
     0x00020000,                                           /* base address */
     BSP_MPU_SIZE_128KB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE |
     BSP_MPU_ATTR_XN},

    /* region 2 for core1 (RCPU-SRAM Cacheable) */
    {2,                                                    /* region number */
     0x081C0000,                                           /* base address */
     BSP_MPU_SIZE_128KB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_ENABLE},

    /* region 3 fore core1 (RCPU-SRAM Non-cacheable) */
    {3,                                                    /* region number */
     0x081E0000,                                           /* base address */
     BSP_MPU_SIZE_128KB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE},

    /* region 4 (Peripheral) */
    {4,                                                    /* region number */
     0x10000000,                                           /* base address */
     BSP_MPU_SIZE_256MB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_DEVICE_SHAREABLE |
     BSP_MPU_ATTR_XN},

    /* region 5 (xSPI) */
    {5,                                                    /* region number */
     0x20000000,                                           /* base address */
     BSP_MPU_SIZE_256MB,                                   /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READONLY_USER_READONLY |   /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_ENABLE},

    /* region 6 (DDR Cacheable) */
    {6,                                                    /* region number */
     0x41800000,                                           /* base address */
     BSP_MPU_SIZE_8MB,                                     /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_ENABLE},

    /* region 7 (DDR Non-cacheable) */
    {7,                                                    /* region number */
     0x42800000,                                           /* base address */
     BSP_MPU_SIZE_8MB,                                     /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE},

    /* region 8 (Open AMP RSCTBL + MHU SHMEM) */
    {8,                                                    /* region number */
     0x42F00000,                                           /* base address */
     BSP_MPU_SIZE_8KB,                                     /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE |
     BSP_MPU_ATTR_S},

    /* region 9 (Open AMP VRING) */
    {9,                                                    /* region number */
     0x43000000,                                           /* base address */
     BSP_MPU_SIZE_16MB,                                    /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_DISABLE |
     BSP_MPU_ATTR_S},

    /* region 10 (DDR Cacheable extension) */
    {10,                                                   /* region number */
     0x42000000,                                           /* base address */
     BSP_MPU_SIZE_8MB,                                     /* size */
     BSP_MPU_SUB_REGION_ALL_ENABLE,                        /* sub region enable */
     BSP_MPU_ATTR_AP_PRIVILEGED_READWRITE_USER_READWRITE | /* attribute */
     BSP_MPU_ATTR_TEX_C_B_NORMAL_OUTER_DISABLE_INNER_ENABLE},

    /* region 11 (No use) */
    {11,                               /* region number */
     0x00000000,                       /* base address */
     0x00000000,                       /* size */
     0x00000000,                       /* sub region enable */
     0x00000000                        /* attribute */
    },

    /* region 12 (No use) */
    {12,                               /* region number */
     0x00000000,                       /* base address */
     0x00000000,                       /* size */
     0x00000000,                       /* sub region enable */
     0x00000000                        /* attribute */
    },

    /* region 13 (No use) */
    {13,                               /* region number */
     0x00000000,                       /* base address */
     0x00000000,                       /* size */
     0x00000000,                       /* sub region enable */
     0x00000000                        /* attribute */
    },

    /* region 14 (No use) */
    {14,                               /* region number */
     0x00000000,                       /* base address */
     0x00000000,                       /* size */
     0x00000000,                       /* sub region enable */
     0x00000000                        /* attribute */
    },

    /* region 15 (No use) */
    {15,                               /* region number */
     0x00000000,                       /* base address */
     0x00000000,                       /* size */
     0x00000000,                       /* sub region enable */
     0x00000000                        /* attribute */
    }
 #endif
};
#endif
