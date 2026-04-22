/*
* Copyright (c) 2020 - 2024 Renesas Electronics Corporation and/or its affiliates
*
* SPDX-License-Identifier: BSD-3-Clause
*/

#include "main_task.h"
#include "FreeRTOS.h"
#include "task.h"
/* Main Task entry function */
/* pvParameters contains TaskHandle_t */
#include "openamp/open_amp.h"
#include "openamp/virtio.h"
#include "platform_info.h"
#include "rsc_table.h"
#include "bsp_cfg.h"
#include "posix_shim.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>  // For strlen
#include "px4_startup_shim.h"
#include "hal_data.h"

/* Forward declaration — defined in board_pwm_out.cpp (C++ file).
 * Declared here to avoid including the C++ board_pwm_out.h in a C TU. */
extern void board_emergency_disarm_all_motors(void);
#include "SEGGER_RTT.h"


#include "openamp_rpc_client.h"
#include "rpc/openamp_rpc.h"
#include <unistd.h>

#ifndef APP_PRINT
#define APP_PRINT(fmt, ...) SEGGER_RTT_printf(0, fmt, ##__VA_ARGS__)
#endif

// Include rpmsg_internal.h for explicit namespace announcement
#include "../rzv/linaro/open-amp/lib/rpmsg/rpmsg_internal.h"
#define SSL_CS_LOW(pin)    R_IOPORT_PinWrite(&g_ioport_ctrl, pin, BSP_IO_LEVEL_LOW)
#define SSL_CS_HIGH(pin)   R_IOPORT_PinWrite(&g_ioport_ctrl, pin, BSP_IO_LEVEL_HIGH)

// Global rpmsg endpoints for PX4 use
struct rpmsg_endpoint rpmsg_ept;        // Endpoint 0: RPC only
struct rpmsg_endpoint rpmsg_ept_uxrce;  // Endpoint 1: UXRCE-DDS only
struct rpmsg_endpoint rpmsg_ept_logger; // Endpoint 2: Logger stream (fire-and-forget writes, no RPC round-trip)

// Global rpmsg device for PX4 use
struct rpmsg_device* rpdev = NULL;
struct rpmsg_device* rpdev_uxrce = NULL;

/* File-scope state for OpenAMP transport — shared between
 * initialize_openamp_transport() and cleanup_openamp_transport().
 * Promoted from static-local so cleanup can reset the initialized flag. */
static bool  g_openamp_initialized  = false;
static bool  g_system_initialized   = false;  /* metal_init done; skip on reconnect */
static void *g_platform_rpc         = NULL;

// Global event flags for service unbind - needed by PX4 openamp transport
volatile int evt_svc_unbind = 0;        // For RPC endpoint
volatile int evt_svc_unbind_uxrce = 0;  // For UXRCE-DDS endpoint
volatile int evt_svc_unbind_logger = 0; // For logger stream endpoint

#if defined(__PX4_FREERTOS)
extern const char *px4_work_queue_current_item_name(void *task_handle);
#endif

#define RECONNECT_FLG    (1)           /* 1:reconnect after exit, 0:disconnect after exit */
#define RECONNECT_DLY    (10000u)
extern int init_system(void);

// Forward declaration for the PX4 task starter function
static void px4_task_thread(void *context);
static void px4_dump_task_snapshot(TaskHandle_t overflow_task);

// Global OpenAMP transport mutex to serialize RPC and UXRCE-DDS operations
// This prevents both endpoints from contending for the shared virtio device mutex
#include <metal/mutex.h>
static metal_mutex_t g_openamp_transport_lock = METAL_MUTEX_INIT(g_openamp_transport_lock);

void px4_openamp_transport_lock(void)
{
	/* Avoid priority inversion: metal mutex is a spinlock on FreeRTOS. */
	while (!metal_mutex_try_acquire(&g_openamp_transport_lock)) {
		vTaskDelay(pdMS_TO_TICKS(1));
	}
}

void px4_openamp_transport_unlock(void)
{
	metal_mutex_release(&g_openamp_transport_lock);
}

// PX4 OpenAMP transport callback functions
/* Local implementations for CR8; C linkage to match declarations used by rpmsg_create_ept */
// Import UXRCE-DDS message forward function
extern bool px4_uxrce_forward_message(const void *data, size_t len);

// RPC endpoint callback - handles ONLY RPC messages on rpmsg-service-0
int px4_rpmsg_recv_callback(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv)
{
    (void)priv;

    if (!ept || !data || len == 0) {
        return RPMSG_SUCCESS;
    }

    /* Track peer address so responses route correctly */
    if (ept->dest_addr == RPMSG_ADDR_ANY || ept->dest_addr != src) {
        APP_PRINT("RPC callback: updating dest_addr from 0x%x to 0x%x (src=0x%x, len=%zu)\n",
                 ept->dest_addr, src, src, len);
        ept->dest_addr = src;
    }

    /* Handle RPC messages only - no multiplexing */
    px4_openamp_rpc_handle_message(data, len);

    return RPMSG_SUCCESS;
}

void px4_rpmsg_service_unbind(struct rpmsg_endpoint *ept)
{
    (void)ept;
    evt_svc_unbind = 1;
}

// UXRCE-DDS endpoint callback - handles ONLY UXRCE-DDS messages on rpmsg-service-1
int px4_uxrce_rpmsg_recv_callback(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv)
{
    (void)priv;

    if (!ept || !data || len == 0) {
        return RPMSG_SUCCESS;
    }

    /* Track peer address so responses route correctly */
    if (ept->dest_addr == RPMSG_ADDR_ANY || ept->dest_addr != src) {
        ept->dest_addr = src;
    }

    /* Forward all messages to UXRCE-DDS transport */
    px4_uxrce_forward_message(data, len);

    return RPMSG_SUCCESS;
}

void px4_uxrce_rpmsg_service_unbind(struct rpmsg_endpoint *ept)
{
    (void)ept;
    evt_svc_unbind_uxrce = 1;
}

// Logger stream endpoint callback - CA55 does not send back on this channel; callback is a no-op
int px4_logger_rpmsg_recv_callback(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv)
{
    (void)ept; (void)data; (void)len; (void)src; (void)priv;
    return RPMSG_SUCCESS;
}

void px4_logger_rpmsg_service_unbind(struct rpmsg_endpoint *ept)
{
    (void)ept;
    evt_svc_unbind_logger = 1;
}

// FIX: Add __attribute__ to suppress warnings
void vApplicationMallocFailedHook(void) __attribute__((weak));
void vApplicationMallocFailedHook(void)
{
    px4_dump_task_snapshot(NULL);

    /* Emergency-disarm motors BEFORE disabling interrupts — PWM timers are
     * hardware-driven and keep spinning at the last duty cycle if not reset. */
    board_emergency_disarm_all_motors();
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
        APP_PRINT("Malloc failed\n");
    }
}
#if configUSE_TICK_HOOK == 1
/**
 * @brief Application tick hook called on each tick
 * This function is called from the tick interrupt.
 * It must be very fast and not call any blocking FreeRTOS API functions.
 */
void vApplicationTickHook(void)
{
    /* Tick hook is enabled and functional.
     * This can be used for LED blinking, watchdog feeding, 
     * or other periodic tasks that need to run every tick */
    
    // Note: Keep this function extremely lightweight as it runs 
    // in interrupt context on every system tick
}
#endif /* configUSE_TICK_HOOK */

/* Stack overflow diagnostics -------------------------------------------------*/

#ifndef PX4_MAX_STACK_SNAPSHOT_TASKS
#define PX4_MAX_STACK_SNAPSHOT_TASKS 48U
#endif

static const char *px4_resolve_task_name(TaskHandle_t task, const char *fallback);
static void px4_dump_task_snapshot(TaskHandle_t overflow_task);

static void px4_dump_task_snapshot(TaskHandle_t overflow_task)
{
#if (configUSE_TRACE_FACILITY == 1)
	static TaskStatus_t task_status[PX4_MAX_STACK_SNAPSHOT_TASKS];
	UBaseType_t total_runtime = 0;
	UBaseType_t task_count = uxTaskGetSystemState(task_status, PX4_MAX_STACK_SNAPSHOT_TASKS, &total_runtime);

	if (task_count == 0) {
		APP_PRINT("[STACK] unable to capture task snapshot\n");
		return;
	}

	APP_PRINT("[STACK] task snapshot (%lu tasks%s, total runtime=%lu)\n",
	          (unsigned long)task_count,
	          (task_count == PX4_MAX_STACK_SNAPSHOT_TASKS) ? ", truncated" : "",
	          (unsigned long)total_runtime);

	for (UBaseType_t i = 0; i < task_count; i++) {
		const TaskStatus_t *ts = &task_status[i];
		const char *name = px4_resolve_task_name(ts->xHandle, ts->pcTaskName);
		const bool is_overflow = (overflow_task != NULL) && (ts->xHandle == overflow_task);
		const unsigned long free_words = (unsigned long)ts->usStackHighWaterMark;
		const unsigned long free_bytes = free_words * sizeof(StackType_t);

		APP_PRINT("%c %s: handle=%p prio=%lu base=%lu state=%lu stack_free=%lu bytes runtime=%lu\n",
		          is_overflow ? '*' : ' ',
		          name,
		          (void *)ts->xHandle,
		          (unsigned long)ts->uxCurrentPriority,
		          (unsigned long)ts->uxBasePriority,
		          (unsigned long)ts->eCurrentState,
		          free_bytes,
		          (unsigned long)ts->ulRunTimeCounter);
	}
#else
	(void)overflow_task;
#endif
}

static inline void px4_log_heap_status(const char *label)
{
	HeapStats_t stats;
	memset(&stats, 0, sizeof(stats));
	vPortGetHeapStats(&stats);

	APP_PRINT("[HEAP] %s: free=%lu min-ever=%lu largest=%lu free-blocks=%lu\n",
	          label ? label : "<unnamed>",
	          (unsigned long)xPortGetFreeHeapSize(),
	          (unsigned long)xPortGetMinimumEverFreeHeapSize(),
	          (unsigned long)stats.xSizeOfLargestFreeBlockInBytes,
	          (unsigned long)stats.xNumberOfFreeBlocks);
}

static const char *px4_resolve_task_name(TaskHandle_t task, const char *fallback)
{
	const char *result = fallback;

#if (defined(INCLUDE_pcTaskGetName) && (INCLUDE_pcTaskGetName == 1))
	if (task != NULL) {
		const char *runtime_name = pcTaskGetName(task);

		if ((runtime_name != NULL) && (runtime_name[0] != '\0')) {
			result = runtime_name;
		}
	}
#else
	(void)task;
#endif

	return result;
}

// FIX: Add __attribute__ to mark unused parameter
void vApplicationStackOverflowHook(TaskHandle_t xTask __attribute__((unused)), char *pcTaskName) __attribute__((weak));
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	const char *task_name = px4_resolve_task_name(xTask,
	                                             (pcTaskName != NULL && pcTaskName[0] != '\0') ?
	                                             pcTaskName : "<unknown>");
	UBaseType_t watermark_words = (xTask != NULL) ? uxTaskGetStackHighWaterMark(xTask) : 0;
	const unsigned long watermark_bytes = (unsigned long)(watermark_words * sizeof(StackType_t));

	APP_PRINT("[THRONE][STACK] overflow detected in task '%s' (handle=%p, free=%lu bytes)\n",
	          task_name,
	          (void *)xTask,
	          watermark_bytes);

#if defined(__PX4_FREERTOS)
	const char *wq_item = px4_work_queue_current_item_name((void *)xTask);

	if ((wq_item != NULL) && (wq_item[0] != '\0')) {
		APP_PRINT("[THRONE][STACK] active work item: %s\n", wq_item);
	}
#endif

	px4_dump_task_snapshot(xTask);

	/* Emergency-disarm motors BEFORE disabling interrupts — GPT timers keep
	 * the last duty cycle indefinitely if PX4 stops updating them. */
	board_emergency_disarm_all_motors();

	taskDISABLE_INTERRUPTS();

	for (;;) {
		__asm volatile("NOP");
	}
}

void px4_dump_all_task_watermarks(void)
{
	px4_dump_task_snapshot(NULL);
}


#if PX4_ENABLE_STACK_MONITOR
static void px4_stack_monitor_task(void *parameter)
{
	(void)parameter;

	APP_PRINT("[STACK_MON] task handle=%p\n", (void *)xTaskGetCurrentTaskHandle());
	const TickType_t delay_ticks = pdMS_TO_TICKS(10000);

	for (;;) {
		px4_dump_all_task_watermarks();
		vTaskDelay(delay_ticks);
	}
}
#endif

static bool initialize_openamp_transport(void)
{
    /* Use file-scope globals so cleanup_openamp_transport() can reset them. */
    if (g_openamp_initialized) {
        return true;
    }

    APP_PRINT("Starting OpenAMP initialization...\n");

    taskENTER_CRITICAL();

    if (!g_system_initialized) {
        if (init_system() != 0) {
            taskEXIT_CRITICAL();
            APP_PRINT("Failed to initialize OpenAMP system\n");
            return false;
        }
        g_system_initialized = true;
    }

    if (g_platform_rpc == NULL) {
        if (platform_init(0, 0, &g_platform_rpc) != 0) {
            taskEXIT_CRITICAL();
            APP_PRINT("Failed to initialize OpenAMP platform\n");
            return false;
        }
    }

    taskEXIT_CRITICAL();

    rpdev = platform_create_rpmsg_vdev(g_platform_rpc, 0x0U, VIRTIO_DEV_SLAVE, NULL, NULL);

    if (!rpdev) {
        APP_PRINT("Failed to create RPMsg virtual device\n");
        return false;
    }

    rpdev_uxrce = rpdev;

    // Create RPC endpoint (service-0)
    int ret = rpmsg_create_ept(&rpmsg_ept,
                               rpdev,
                               CFG_RPMSG_SVC_NAME0,
                               APP_EPT_ADDR,
                               RPMSG_ADDR_ANY,
                               px4_rpmsg_recv_callback,
                               px4_rpmsg_service_unbind);

    if (ret != 0) {
        APP_PRINT("Failed to create RPC endpoint: %d\n", ret);
        platform_release_rpmsg_vdev(g_platform_rpc, rpdev);
        rpdev = NULL;
        memset(&rpmsg_ept, 0, sizeof(rpmsg_ept));
        return false;
    }

    APP_PRINT("RPC endpoint created: %s (addr=0x%x)\n", CFG_RPMSG_SVC_NAME0, rpmsg_ept.addr);

    // Create UXRCE-DDS endpoint (service-1) with different local address
    int ret_uxrce = rpmsg_create_ept(&rpmsg_ept_uxrce,
                                     rpdev,
                                     CFG_RPMSG_SVC_NAME1,
                                     RPMSG_ADDR_ANY,  // Let RPMsg auto-assign unique address
                                     RPMSG_ADDR_ANY,
                                     px4_uxrce_rpmsg_recv_callback,
                                     px4_uxrce_rpmsg_service_unbind);

    if (ret_uxrce != 0) {
        APP_PRINT("Failed to create UXRCE-DDS endpoint: %d\n", ret_uxrce);
        rpmsg_destroy_ept(&rpmsg_ept);
        memset(&rpmsg_ept, 0, sizeof(rpmsg_ept));
        platform_release_rpmsg_vdev(g_platform_rpc, rpdev);
        rpdev = NULL;
        rpdev_uxrce = NULL;
        return false;
    }

    APP_PRINT("UXRCE-DDS endpoint created: %s (addr=0x%x)\n", CFG_RPMSG_SVC_NAME1, rpmsg_ept_uxrce.addr);

    // Create Logger stream endpoint (service-2) - optional, falls back to RPC writes if CA55 not ready
    memset(&rpmsg_ept_logger, 0, sizeof(rpmsg_ept_logger));
    int ret_logger = rpmsg_create_ept(&rpmsg_ept_logger,
                                      rpdev,
                                      CFG_RPMSG_SVC_NAME2,
                                      RPMSG_ADDR_ANY,
                                      RPMSG_ADDR_ANY,
                                      px4_logger_rpmsg_recv_callback,
                                      px4_logger_rpmsg_service_unbind);
    if (ret_logger != 0) {
        APP_PRINT("Logger stream endpoint creation failed (%d) - will use RPC fallback\n", ret_logger);
        memset(&rpmsg_ept_logger, 0, sizeof(rpmsg_ept_logger));
    } else {
        APP_PRINT("Logger stream endpoint created: %s (addr=0x%x)\n", CFG_RPMSG_SVC_NAME2, rpmsg_ept_logger.addr);
    }

    // Do initial platform poll to let OpenAMP infrastructure initialize
    platform_poll((struct remoteproc *)g_platform_rpc);
    vTaskDelay(pdMS_TO_TICKS(50));

    const TickType_t timeout_ticks = pdMS_TO_TICKS(15000);
    const TickType_t poll_interval = pdMS_TO_TICKS(20);
    TickType_t start = xTaskGetTickCount();
    TickType_t last_log = start;
    TickType_t last_ns = start - pdMS_TO_TICKS(600);  // Trigger immediate first announcement

    // Wait for BOTH endpoints to be ready
    bool rpc_ready = false;
    bool uxrce_ready = false;
    bool rpc_ns_sent = false;
    bool uxrce_ns_sent = false;
    bool logger_ns_sent = false;

    while (!rpc_ready || !uxrce_ready) {
        TickType_t now = xTaskGetTickCount();

        if ((now - start) > timeout_ticks) {
            APP_PRINT("OpenAMP endpoints not ready (timeout) - RPC:%s UXRCE:%s\n",
                     rpc_ready ? "ready" : "NOT_READY",
                     uxrce_ready ? "ready" : "NOT_READY");
            rpmsg_destroy_ept(&rpmsg_ept);
            rpmsg_destroy_ept(&rpmsg_ept_uxrce);
            memset(&rpmsg_ept, 0, sizeof(rpmsg_ept));
            memset(&rpmsg_ept_uxrce, 0, sizeof(rpmsg_ept_uxrce));
            platform_release_rpmsg_vdev(g_platform_rpc, rpdev);
            rpdev = NULL;
            rpdev_uxrce = NULL;
            return false;
        }

        // Check endpoint status - require valid destination address
        // Note: dest_addr=0x0 is VALID for RPC endpoint, so we don't check != 0 for it
        bool prev_rpc_ready = rpc_ready;
        bool prev_uxrce_ready = uxrce_ready;

        rpc_ready = (rpmsg_ept.rdev != NULL &&
                     rpmsg_ept.dest_addr != RPMSG_ADDR_ANY &&
                     is_rpmsg_ept_ready(&rpmsg_ept) != 0);

        uxrce_ready = (rpmsg_ept_uxrce.rdev != NULL &&
                       rpmsg_ept_uxrce.dest_addr != RPMSG_ADDR_ANY &&
                       is_rpmsg_ept_ready(&rpmsg_ept_uxrce) != 0);

        // Log when endpoints become ready
        if (!prev_rpc_ready && rpc_ready) {
            APP_PRINT("RPC endpoint ready (dest=0x%x)\n", rpmsg_ept.dest_addr);
        }
        if (!prev_uxrce_ready && uxrce_ready) {
            APP_PRINT("UXRCE-DDS endpoint ready (dest=0x%x)\n", rpmsg_ept_uxrce.dest_addr);
        }

        if ((now - last_log) >= pdMS_TO_TICKS(1000)) {
            APP_PRINT("Waiting for endpoints - RPC:%s(0x%x) UXRCE:%s(0x%x)\n",
                     rpc_ready ? "ready" : "waiting", rpmsg_ept.dest_addr,
                     uxrce_ready ? "ready" : "waiting", rpmsg_ept_uxrce.dest_addr);
            last_log = now;
        }

        /* Announce/re-announce namespaces */
        if ((now - last_ns) >= pdMS_TO_TICKS(500)) {
            if (!rpc_ready) {
                int ns_ret = rpmsg_send_ns_message(&rpmsg_ept, RPMSG_NS_CREATE);
                if (!rpc_ns_sent) {
                    if (ns_ret == 0) {
                        APP_PRINT("RPC namespace announced\n");
                        rpc_ns_sent = true;
                    } else {
                        APP_PRINT("RPC namespace announcement failed: %d\n", ns_ret);
                    }
                }
            }

            if (!uxrce_ready) {
                int ns_ret_uxrce = rpmsg_send_ns_message(&rpmsg_ept_uxrce, RPMSG_NS_CREATE);
                if (!uxrce_ns_sent) {
                    if (ns_ret_uxrce == 0) {
                        APP_PRINT("UXRCE-DDS namespace announced\n");
                        uxrce_ns_sent = true;
                    } else {
                        APP_PRINT("UXRCE-DDS namespace announcement failed: %d\n", ns_ret_uxrce);
                    }
                }
            }

            // Announce logger stream endpoint (optional - don't block on it)
            if (!logger_ns_sent && rpmsg_ept_logger.rdev != NULL) {
                int ns_ret_logger = rpmsg_send_ns_message(&rpmsg_ept_logger, RPMSG_NS_CREATE);
                if (ns_ret_logger == 0) {
                    APP_PRINT("Logger stream namespace announced\n");
                    logger_ns_sent = true;
                }
            }

            last_ns = now;
        }

        /* Drive the virtio/rpmsg state machine while waiting for master */
        platform_poll((struct remoteproc *)g_platform_rpc);
        vTaskDelay(poll_interval);
    }

    APP_PRINT("Both OpenAMP endpoints ready - RPC:0x%x UXRCE:0x%x\n",
             rpmsg_ept.dest_addr, rpmsg_ept_uxrce.dest_addr);

    // Final logger NS announcement after main endpoints are ready (CA55 may now handle it)
    if (!logger_ns_sent && rpmsg_ept_logger.rdev != NULL) {
        rpmsg_send_ns_message(&rpmsg_ept_logger, RPMSG_NS_CREATE);
        APP_PRINT("Logger stream namespace announced (post-ready)\n");
    }
    g_openamp_initialized = true;
    return true;
}

/* Tear down the OpenAMP transport so openamp_background_init_task can
 * re-initialize it when the CA55 agent reconnects (DRIVER_OK re-asserted).
 * Called only from the watchdog task after DRIVER_OK goes low. */
static void cleanup_openamp_transport(void)
{
    APP_PRINT("OpenAMP cleanup: tearing down endpoints and vdev for reconnect\n");

    /* Destroy endpoints in reverse creation order (logger → uxrce → rpc) */
    if (rpmsg_ept_logger.rdev != NULL) {
        rpmsg_destroy_ept(&rpmsg_ept_logger);
        memset(&rpmsg_ept_logger, 0, sizeof(rpmsg_ept_logger));
    }
    if (rpmsg_ept_uxrce.rdev != NULL) {
        rpmsg_destroy_ept(&rpmsg_ept_uxrce);
        memset(&rpmsg_ept_uxrce, 0, sizeof(rpmsg_ept_uxrce));
    }
    if (rpmsg_ept.rdev != NULL) {
        rpmsg_destroy_ept(&rpmsg_ept);
        memset(&rpmsg_ept, 0, sizeof(rpmsg_ept));
    }

    if (rpdev != NULL && g_platform_rpc != NULL) {
        platform_release_rpmsg_vdev(g_platform_rpc, rpdev);
        rpdev = NULL;
        rpdev_uxrce = NULL;
    }
    if (g_platform_rpc != NULL) {
        platform_cleanup(g_platform_rpc);
        g_platform_rpc = NULL;
    }

    /* Reset service unbind flags */
    evt_svc_unbind        = 0;
    evt_svc_unbind_uxrce  = 0;
    evt_svc_unbind_logger = 0;

    /* Allow initialize_openamp_transport() to run the full init path again */
    g_openamp_initialized = false;

    APP_PRINT("OpenAMP cleanup done — ready for CA55 reconnect\n");
}

/* No stub definitions here; real implementations are linked from openamp_transport.cpp */

static void px4_task_thread(void *context)
{
    (void)context;

    if (rzv_px4_bootstrap() != 0) {
        APP_PRINT("PX4 bootstrap failed\n");
        vTaskDelete(NULL);
        return;
    }

    int result = rzv_px4_run_script("/etc/init.d/rc.board_defaults", 0);
    if (result != 0) {
        APP_PRINT("PX4 command failed (%d): sh /etc/init.d/rc.board_defaults\n", result);
    } else {
        APP_PRINT("PX4 command OK: sh /etc/init.d/rc.board_defaults\n");
    }

    // Apply SYS_AUTOCONFIG if the user requested a factory reset via QGC.
    // Must run after the "param load /fs/microsd/params" step so we see the saved value.
    // Note: if the initial param load succeeded (CA55 was already up), this runs inline.
    rzv_px4_apply_autoconfig();

    // Start background task that retries param load once CA55 becomes available.
    // Handles the common case where CR8 boots before CA55/Linux is ready.
    // Starts a background task that retries param load once CA55 becomes available.
    rzv_px4_start_deferred_param_sync();

    APP_PRINT("PX4 bootstrap task complete\n");
    vTaskDelete(NULL);
}

/* Permanent watchdog task: initialises OpenAMP while PX4 runs and
 * re-initialises it when the CA55 agent disconnects and reconnects.
 *
 * Loop:
 *   1. Poll virtio DRIVER_OK (pure memory read, no MHU interrupt fired).
 *   2. Once DRIVER_OK is set, run initialize_openamp_transport().
 *   3. Monitor DRIVER_OK — when it goes low the CA55 agent has exited cleanly.
 *   4. Tear down endpoints/vdev via cleanup_openamp_transport().
 *   5. Go back to step 1 to wait for the next agent instance.
 *
 * Without the DRIVER_OK guard, platform_create_rpmsg_vdev() fires
 * MHU IRQ 75 (RSP1_NS_IRQn) on CA55 during U-Boot / early-boot where
 * no interrupt handler exists → hang/abort. */
#define OPENAMP_INIT_STACK_WORDS (4096U / sizeof(StackType_t))
static void openamp_background_init_task(void *pvParameters)
{
    (void)pvParameters;
    APP_PRINT("OpenAMP: watchdog task started\n");

    init_resource_table();
    __asm volatile ("dsb sy" ::: "memory");

    volatile struct remote_resource_table *rsctbl =
        (volatile struct remote_resource_table *)CFG_RSCTBL_MEM_PA;

    for (;;) {
        /* === Step 1: Wait for CA55/Linux master to assert DRIVER_OK === */
        {
            APP_PRINT("OpenAMP: waiting for CA55/Linux master "
                      "(polling virtio status @ 0x%08X)\n",
                      (unsigned int)CFG_RSCTBL_MEM_PA);
            uint32_t wait_count = 0;
            while (!(rsctbl->rpmsg_vdev.status & VIRTIO_CONFIG_STATUS_DRIVER_OK)) {
                vTaskDelay(pdMS_TO_TICKS(200));
                if ((++wait_count % 25U) == 0U) {   /* log every 5 s */
                    APP_PRINT("OpenAMP: still waiting for Linux master (%u s)...\n",
                              (unsigned int)(wait_count / 5U));
                }
            }
            APP_PRINT("OpenAMP: Linux master ready (status=0x%02x), proceeding\n",
                      (unsigned int)rsctbl->rpmsg_vdev.status);
        }

        /* === Step 2: Initialise transport (retry until endpoints are ready) === */
        for (;;) {
            if (initialize_openamp_transport()) {
                APP_PRINT("OpenAMP: both endpoints ready\n");
                break;
            }
            APP_PRINT("OpenAMP: endpoints not ready yet, retrying in 200 ms...\n");
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        /* === Step 3: Monitor DRIVER_OK while the transport is active ===
         * The CA55 agent calls platform_clear_driver_ok() before _exit(),
         * clearing this bit. When we see it go to 0 the agent has disconnected. */
        while (rsctbl->rpmsg_vdev.status & VIRTIO_CONFIG_STATUS_DRIVER_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        APP_PRINT("OpenAMP: CA55 agent disconnected (DRIVER_OK cleared) — "
                  "cleaning up for reconnect\n");

        init_resource_table();
        __asm volatile ("dsb sy" ::: "memory");

        /* === Step 4: Tear down endpoints / vdev === */
        cleanup_openamp_transport();

        /* Loop back to step 1 and wait for the next agent instance */
    }
    /* Never reached — this task runs forever */
}

void main_task_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);  /* Suppress unused parameter warning */

    // CRITICAL: Disable interrupts temporarily during initialization to prevent
    // FreeRTOS API calls from high-priority ISRs before system is ready
    taskENTER_CRITICAL();

    taskEXIT_CRITICAL();

    /* === CR8 Boot Handshake (Tier 1): Signal U-Boot that basic init is done ===
     * U-Boot polls 0x41500000 instead of using a fixed sleep delay.
     * This flag MUST be set before starting openamp_background_init_task so that
     * U-Boot can safely run `booti` and hand off to the Linux kernel before
     * any MHU interrupt is fired from CR8.
     * Memory barrier (dsb sy) ensures the write is visible to CA55/U-Boot immediately.
     */
    {
        volatile uint32_t * cr8_boot_magic = (volatile uint32_t *)0x41500000U;
        *cr8_boot_magic = 0xDEADBEEFU;
        __asm volatile ("dsb sy" ::: "memory");
        APP_PRINT("CR8 handshake: ready flag set @ 0x41500000 = 0xDEADBEEF\n");
    }

    /* Start OpenAMP init asynchronously so PX4 boots immediately.
     * Endpoints are tracked via rpmsg_ept / rpmsg_ept_uxrce readiness flags.
     * deferred_param_sync_task will load saved params once CA55 RPC is ready. */
    {
        static StaticTask_t  s_openamp_init_task_buf;
        static StackType_t   s_openamp_init_task_stack[OPENAMP_INIT_STACK_WORDS];
        TaskHandle_t h = xTaskCreateStatic(
            openamp_background_init_task,
            "openamp_init",
            OPENAMP_INIT_STACK_WORDS,
            NULL,
            tskIDLE_PRIORITY + 2U,
            s_openamp_init_task_stack,
            &s_openamp_init_task_buf);
        if (h == NULL) {
            APP_PRINT("OpenAMP: WARNING - failed to create background init task\n");
        }
    }

    TaskHandle_t px4_task_handle;

    const uint32_t stack_size = 24576U;

    px4_log_heap_status("before PX4_Starter creation");

    BaseType_t task_created = xTaskCreate(
        px4_task_thread,
        "PX4_Starter",
        stack_size,
        NULL,
        20,
        &px4_task_handle
    );
    
    if (task_created != pdPASS) {
        APP_PRINT("ERROR: Failed to create PX4 starter task\n");
        goto err1;
    } else {
        px4_log_heap_status("after PX4_Starter creation");
    }
#if PX4_ENABLE_STACK_MONITOR
    if (xTaskCreate(px4_stack_monitor_task,
                    "stack_mon",
                    8192,
                    NULL,
                    tskIDLE_PRIORITY + 2,
                    NULL) != pdPASS) {
        APP_PRINT("WARN: Failed to start stack monitor task\n");
    }
#endif
#if PX4_ENABLE_STACK_MONITOR
    px4_dump_all_task_watermarks();
#endif
    // If we get here, initialization completed and tasks are running on their own.
    vTaskDelete(NULL);
err1:
    vTaskDelete(NULL);
}
