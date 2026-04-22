/*
 * FreeRTOS+POSIX port configuration for Renesas RZ/V
 */

#ifndef _FREERTOS_POSIX_PORTABLE_H_
#define _FREERTOS_POSIX_PORTABLE_H_

/* Use the default portable settings provided by FreeRTOS+POSIX.
 * Override knobs here when the RZ/V integration requires tuning.
 */

#define posixconfigPTHREAD_TASK_NAME   "pthread"

/* Rely on newlib for the standard scalar typedefs so the shim only supplies
 * the FreeRTOS-backed pthread structures.
 */
#define posixconfigENABLE_CLOCK_T      0
#define posixconfigENABLE_CLOCKID_T    0
#define posixconfigENABLE_MODE_T       0
#define posixconfigENABLE_PID_T        0
#define posixconfigENABLE_SSIZE_T      0
#define posixconfigENABLE_TIME_T       0
#define posixconfigENABLE_TIMER_T      0
#define posixconfigENABLE_USECONDS_T   0
#define posixconfigENABLE_OFF_T        0


#endif /* _FREERTOS_POSIX_PORTABLE_H_ */
