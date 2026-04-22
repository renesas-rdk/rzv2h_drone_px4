/***********************************************************************************************************************
 * Copyright [2020-2021] Renesas Electronics Corporation and/or its affiliates.  All Rights Reserved.
 *
 * This software and documentation are supplied by Renesas Electronics Corporation and/or its affiliates and may only
 * be used with products of Renesas Electronics Corp. and its affiliates ("Renesas").  No other uses are authorized.
 * Renesas products are sold pursuant to Renesas terms and conditions of sale.  Purchasers are solely responsible for
 * the selection and use of Renesas products and Renesas assumes no liability.  No license, express or implied, to any
 * intellectual property right is granted by Renesas.  This software is protected under all applicable laws, including
 * copyright laws. Renesas reserves the right to change or discontinue this software and/or this documentation.
 * THE SOFTWARE AND DOCUMENTATION IS DELIVERED TO YOU "AS IS," AND RENESAS MAKES NO REPRESENTATIONS OR WARRANTIES, AND
 * TO THE FULLEST EXTENT PERMISSIBLE UNDER APPLICABLE LAW, DISCLAIMS ALL WARRANTIES, WHETHER EXPLICITLY OR IMPLICITLY,
 * INCLUDING WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NONINFRINGEMENT, WITH RESPECT TO THE
 * SOFTWARE OR DOCUMENTATION.  RENESAS SHALL HAVE NO LIABILITY ARISING OUT OF ANY SECURITY VULNERABILITY OR BREACH.
 * TO THE MAXIMUM EXTENT PERMITTED BY LAW, IN NO EVENT WILL RENESAS BE LIABLE TO YOU IN CONNECTION WITH THE SOFTWARE OR
 * DOCUMENTATION (OR ANY PERSON OR ENTITY CLAIMING RIGHTS DERIVED FROM YOU) FOR ANY LOSS, DAMAGES, OR CLAIMS WHATSOEVER,
 * INCLUDING, WITHOUT LIMITATION, ANY DIRECT, CONSEQUENTIAL, SPECIAL, INDIRECT, PUNITIVE, OR INCIDENTAL DAMAGES; ANY
 * LOST PROFITS, OTHER ECONOMIC DAMAGE, PROPERTY DAMAGE, OR PERSONAL INJURY; AND EVEN IF RENESAS HAS BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH LOSS, DAMAGES, CLAIMS OR COSTS.
 **********************************************************************************************************************/
/**
 * @file    OpenAMP_RPMsg_cfg.h
 * @brief   OpenAMP configurations
 * @date    2020.10.21
 * @author  Copyright (c) 2020, eForce Co., Ltd. All rights reserved.
 *
 ****************************************************************************
 * @par     History
 *          - rev 1.0 (2020.10.21) Imada
 *            Initial version for RZ/V2.
 ****************************************************************************
 */

 #ifndef OPENAMP_RPMSG_CFG_H_
 #define OPENAMP_RPMSG_CFG_H_

 // RPMSG config
 #define APP_EPT_ADDR              (0x0U)

 // Memory region reserved between 0x43000000 - 0x437FFFFF for RPMSG
 #define RPMSG_MEM_BASE            (0x43000000U)
 #define RPMSG_MEM_SIZE            (0x00800000U)

 #define VRING_SIZE                (0x100000U)
 #define VRING_SHM_SIZE            (0x300000U)

 #define CFG_RPMSG_SVCNO           (0x2U)

 #if BSP_SUPPORT_CORE_CM33
 #define VA_OFFSET                 (0x40000000U)
 #else
 #define VA_OFFSET                 (0x00000000U)
 #endif
 // RPMSG channel #0
 #define CFG_RPMSG_SVC_NAME0       "rpmsg-service-0"
 #define CFG_VRING0_BASE0_PA       (0x43000000U)
 #define CFG_VRING0_BASE0_VA       (CFG_VRING0_BASE0_PA + VA_OFFSET)
 #define CFG_VRING1_BASE0_PA       (0x43050000U)
 #define CFG_VRING1_BASE0_VA       (CFG_VRING1_BASE0_PA + VA_OFFSET)
 #define CFG_VRING_SIZE0           (VRING_SIZE)
 #define CFG_VRING_ALIGN0          (0x100U)
#define CFG_RPMSG_NUM_BUFS0       (1024U)
 #define CFG_VRING_SHM_BASE0_PA    (0x43200000U)
 #define CFG_VRING_SHM_BASE0_VA    (CFG_VRING_SHM_BASE0_PA + VA_OFFSET)
 #define CFG_VRING_SHM_SIZE0       (VRING_SHM_SIZE)
 #define CFG_VRING_CTL_NAME0       "43000000.vring-ctl0"
 #define CFG_VRING_SHM_NAME0       "43200000.vring-shm0"
 #define VRING_NOTIFYID0           (0U)

 // RPMSG channel #1
 #define CFG_RPMSG_SVC_NAME1       "rpmsg-service-1"

 // Logger stream channel: fire-and-forget, no RPC round-trip, shares the same vring as RPC+UXRCE
 #define CFG_RPMSG_SVC_NAME2       "rpmsg-logger-0"
 #define CFG_VRING0_BASE1_PA       (0x43100000U)
 #define CFG_VRING0_BASE1_VA       (CFG_VRING0_BASE1_PA + VA_OFFSET)
 #define CFG_VRING1_BASE1_PA       (0x43150000U)
 #define CFG_VRING1_BASE1_VA       (CFG_VRING1_BASE1_PA + VA_OFFSET)
 #define CFG_VRING_SIZE1           (VRING_SIZE)
 #define CFG_VRING_ALIGN1          (0x100U)
#define CFG_RPMSG_NUM_BUFS1       (1024U)
 #define CFG_VRING_SHM_BASE1_PA    (0x43500000U)
 #define CFG_VRING_SHM_BASE1_VA    (CFG_VRING_SHM_BASE1_PA + VA_OFFSET)
 #define CFG_VRING_SHM_SIZE1       (VRING_SHM_SIZE)
 #define CFG_VRING_CTL_NAME1       "43100000.vring-ctl1"
 #define CFG_VRING_SHM_NAME1       "43500000.vring-shm1"
 #define VRING_NOTIFYID1           (1U)
 #endif                                 /* OPENAMP_RPMSG_CFG_H_ */
