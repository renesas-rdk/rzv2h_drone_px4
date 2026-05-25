/* generated configuration header file - do not edit */
#ifndef BSP_PIN_CFG_H_
#define BSP_PIN_CFG_H_
#include "r_ioport.h"
#define GPS_SAFETY_SWITCH (BSP_IO_PORT_05_PIN_02) /* Using for GPS M10 SAFETY SWITCH */
#define GPS_BUZZER (BSP_IO_PORT_09_PIN_07) /* Using for GPS M10 BUZZER */
#define GPS_SAFETY_SWITCH_LED (BSP_IO_PORT_10_PIN_06) /* Using for GPS M10 SAFETY SWITCH LED */
extern const ioport_cfg_t g_bsp_pin_cfg; /* RDK */

void BSP_PinConfigSecurityInit(void);
#endif /* BSP_PIN_CFG_H_ */
