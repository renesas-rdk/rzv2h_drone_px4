/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

#include "hal_data.h"
#include "pd_axi_on.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "common_utils.h"
#include "task.h"

#if defined(__PX4_FREERTOS)
// FreeRTOS assert handler for debugging
void vAssertCalled(unsigned long ulLine, const char * const pcFileName)
{
    extern uint32_t FreeRTOS_GetActiveIRQ(void);

    const uint32_t active_irq = FreeRTOS_GetActiveIRQ();
    const TaskHandle_t current = xTaskGetCurrentTaskHandle();
    const char *task_name = pcTaskGetName(current);

    APP_PRINT("[ASSERT] %s:%lu IRQ=%lu configMAX_SYSCALL=%u task=%s\n",
              (pcFileName != NULL) ? pcFileName : "?",
              ulLine,
              active_irq,
              (unsigned int)configMAX_SYSCALL_INTERRUPT_PRIORITY,
              (task_name != NULL) ? task_name : "<none>");

    // Print error information via RTT or disable interrupts and halt
    __asm volatile ("cpsid i"); // Disable interrupts
    while(1) {
        // Infinite loop for debugging - you can set breakpoint here
        __asm volatile ("nop");
    }
}
#endif

FSP_CPP_HEADER
void R_BSP_WarmStart(bsp_warm_start_event_t event);

FSP_CPP_FOOTER

/*******************************************************************************************************************//**
 * This function is called at various points during the startup process.  This implementation uses the event that is
 * called right before main() to set up the pins.
 *
 * @param[in]  event    Where at in the start up process the code is currently at
 **********************************************************************************************************************/
void R_BSP_WarmStart (bsp_warm_start_event_t event)
{
    if (BSP_WARM_START_RESET == event)
    {
    }

    if (BSP_WARM_START_POST_C == event)
    {
        /* C runtime environment and system clocks are setup. */

        /* Configure pins. */
        R_IOPORT_Open(&IOPORT_CFG_CTRL, &IOPORT_CFG_NAME);

        /* Allow access to IP beyond AXI */
        pd_all_on_postproc_axi();
    }
}
