/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

#include "pd_axi_on.h"

/*******************************************************************************************************************//**
 * @addtogroup pd_axi_on
 * @{
 **********************************************************************************************************************/

/* Initialize value of MSTOP Register */
#define CPG_BUS_12_MSTOP_INITVAL    (0x06000000U)

/* Initialize value of RST Registers */
#define CPG_RST_15_INITVAL          (0x40004000U)
#define CPG_RST_16_INITVAL          (0xFFFFFFFFU)
#define CPG_RST_17_INITVAL          (0x03FF03FFU)

/* Mask value of RSTMON Registers */
#define CPG_RSTMON_7_MASKVAL        (0xFFFF0000U)
#define CPG_RSTMON_8_MASKVAL        (0x000003FFU)

/*******************************************************************************************************************//**
 *  @brief
 *  @param[IN]   None
 *  @retval      FSP_SUCCESS
 *  @retval      Any Other Error code apart from FSP_SUCCESS is  Unsuccessful open or start
 **********************************************************************************************************************/
fsp_err_t pd_all_on_postproc_axi (void)
{
    fsp_err_t err = FSP_SUCCESS;

    /* Allow access "from MCPU-Bus to ACPU-Bus" and "from ACPU-Bus to MCPU-Bus" */
    R_CPG->CPG_BUS_12_MSTOP = CPG_BUS_12_MSTOP_INITVAL;

    /* Release reset of AXIs */
    R_CPG->CPG_RST_15 = CPG_RST_15_INITVAL; /* Release reset of RCPU_AXI_RESETN and MCPU_AXI_RESETN */
    R_CPG->CPG_RST_16 = CPG_RST_16_INITVAL; /* Release reset of ACPU_AXI* and VIDEO_AXI* and DRP_AXI* and COM_AXI* and TZC_AXI* */
    R_CPG->CPG_RST_17 = CPG_RST_17_INITVAL; /* Release reset of TZC_AXI* */

    FSP_HARDWARE_REGISTER_WAIT((CPG_RSTMON_7_MASKVAL & R_CPG->CPG_RSTMON_7), 0x00000000);

    FSP_HARDWARE_REGISTER_WAIT((CPG_RSTMON_8_MASKVAL & R_CPG->CPG_RSTMON_8), 0x00000000);

    return err;
}

/*******************************************************************************************************************//**
 * @} (end addtogroup pd_axi_on)
 **********************************************************************************************************************/
