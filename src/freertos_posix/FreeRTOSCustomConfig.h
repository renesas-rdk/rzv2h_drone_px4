/*
 * FreeRTOSCustomConfig.h
 * Project-specific FreeRTOS configuration overrides for Renesas RZ/V2H.
 *
 * This file is force-included BEFORE FreeRTOSConfig.h (e2studio-generated via
 * rzv_cfg/aws/FreeRTOSConfig.h). All #define here win over FreeRTOSConfig.h's
 * #ifndef guards, so e2studio can regenerate FreeRTOSConfig.h freely without
 * breaking the project.
 *
 * NOTE: Do NOT add #ifndef guards here — we intentionally override defaults.
 *
 * For e2studio's own build (if used separately), add this to:
 *   Project > Properties > C/C++ Build > Settings > GCC Compiler > Other flags:
 *   -include ${ProjDirPath}/src/freertos_posix/FreeRTOSCustomConfig.h
 *
 * For our CMake/compile.sh build this is injected automatically via:
 *   - CMakeLists.txt: target_compile_options(-include ...)
 *   - px4/boards/renesas/rzv/cmake/init.cmake: add_compile_options(-include ...)
 */
#pragma once

#if defined(__PX4_FREERTOS)
/*
 * FPU context mode: MUST match portASM.asm (rzv/fsp/src/rm_freertos_port/cr/).
 * portASM.asm is compiled with -mfloat-abi=hard (-mfpu=vfpv3-d16), which sets
 * configUSE_TASK_FPU_SUPPORT=2 in the ASM source. port.c uses this value from
 * FreeRTOSConfig.h for pxPortInitialiseStack(). A mismatch (ASM=2, C=1 or 0)
 * causes stack corruption: ASM pushes 33 FPU words but pxPortInitialiseStack
 * only reserves 1 word → crash under -O2.
 *
 * NOTE: FreeRTOS.h sets this to 1 via #ifndef if not previously defined.
 * Because visibility.h (PX4 force-include) pulls in FreeRTOS.h before this
 * file runs, we must #undef first to override that default.
 */
#undef  configUSE_TASK_FPU_SUPPORT
#define configUSE_TASK_FPU_SUPPORT                  2

/*
 * Priority count: priority_map.cpp (px4/boards/renesas/rzv/src/board/) is
 * coded assuming configMAX_PRIORITIES=32. e2studio default (256) wastes memory
 * and breaks the linear priority-to-FreeRTOS mapping.
 */
#undef  configMAX_PRIORITIES
#define configMAX_PRIORITIES                        32

/*
 * Idle hook: disabled — no vApplicationIdleHook() implementation in project.
 * FSP port.c provides a weak stub, but we explicitly disable to avoid any
 * implicit dependency.
 */
#undef  configUSE_IDLE_HOOK
#define configUSE_IDLE_HOOK                         0

/*
 * Stack overflow check level 2: fill stack with 0xa5 pattern at task creation,
 * verify pattern on each context switch. More thorough than level 1 (checks
 * only the last 16 bytes). Required for our embedded debug builds.
 */
#undef  configCHECK_FOR_STACK_OVERFLOW
#define configCHECK_FOR_STACK_OVERFLOW              2

/*
 * Use hardware CLZ (Count Leading Zeros) instruction for O(1) priority lookup.
 * Cortex-R8 supports CLZ → faster scheduler than generic bitmap scan (level 0).
 */
#undef  configUSE_PORT_OPTIMISED_TASK_SELECTION
#define configUSE_PORT_OPTIMISED_TASK_SELECTION     1

/*
 * Enable time-slicing so equal-priority tasks share the CPU each tick.
 * Required for PX4 work queues that run multiple tasks at the same priority.
 */
#undef  configUSE_TIME_SLICING
#define configUSE_TIME_SLICING                      1

/*
 * Enable FreeRTOS per-task errno (FreeRTOS_errno).
 * REQUIRED: FreeRTOS_POSIX/errno.h unconditionally does #undef errno at line 67
 * and only restores it when configUSE_POSIX_ERRNO==1. Without this, C++ stdlib
 * headers that use errno (e.g. string_conversions.h) will fail to compile
 * with "errno was not declared in this scope".
 */
#undef  configUSE_POSIX_ERRNO
#define configUSE_POSIX_ERRNO                       1

/*
 * Enable task application tag — used by the FreeRTOS POSIX shim to store
 * per-task errno thread-local storage. Required by pthread_create/pthread_self
 * which call vTaskSetApplicationTaskTag / xTaskGetApplicationTaskTag.
 */
#undef  configUSE_APPLICATION_TASK_TAG
#define configUSE_APPLICATION_TASK_TAG                  1

/*
 * Enable xQueueGetMutexHolder — exposes xSemaphoreGetMutexHolder() used by
 * FreeRTOS_POSIX_pthread_mutex.c. Without this the macro is not defined and
 * the linker sees an unresolved reference.
 */
#undef  INCLUDE_xQueueGetMutexHolder
#define INCLUDE_xQueueGetMutexHolder                    1

/*
 * Prevent the FreeRTOS+POSIX shim from redefining standard libc types that
 * are already provided by newlib (clock_t, clockid_t, mode_t, pid_t, etc.).
 * Redefining them causes "conflicting types" errors with C++ stdlib headers.
 */
#undef  posixconfigENABLE_CLOCK_T
#define posixconfigENABLE_CLOCK_T                   0
#undef  posixconfigENABLE_CLOCKID_T
#define posixconfigENABLE_CLOCKID_T                 0
#undef  posixconfigENABLE_MODE_T
#define posixconfigENABLE_MODE_T                    0
#undef  posixconfigENABLE_PID_T
#define posixconfigENABLE_PID_T                     0
#undef  posixconfigENABLE_SSIZE_T
#define posixconfigENABLE_SSIZE_T                   0
#undef  posixconfigENABLE_TIME_T
#define posixconfigENABLE_TIME_T                    0
#undef  posixconfigENABLE_TIMER_T
#define posixconfigENABLE_TIMER_T                   0
#undef  posixconfigENABLE_USECONDS_T
#define posixconfigENABLE_USECONDS_T                0

#endif /* __PX4_FREERTOS */
