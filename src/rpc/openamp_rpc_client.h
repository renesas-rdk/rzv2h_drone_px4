/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool px4_openamp_rpc_handle_message(const void *data, size_t len);
bool px4_openamp_rpc_channel_ready(void);

#define PX4_OPENAMP_RPC_TIMEOUT_FOREVER UINT32_MAX

typedef void (*px4_openamp_rpc_response_cb_t)(int32_t request_id,
					      int32_t return_value,
					      int32_t errno_value,
					      const void *payload,
					      size_t payload_len,
					      void *user_data);

int px4_openamp_rpc_call(uint16_t category,
                         uint16_t opcode,
                         const void *request_payload,
                         size_t request_len,
                         void *response_buffer,
                         size_t response_capacity,
                         size_t *response_len,
                         int32_t *return_value,
                         int32_t *errno_value);

int px4_openamp_rpc_call_timeout(uint16_t category,
                                 uint16_t opcode,
                                 const void *request_payload,
                                 size_t request_len,
                                 void *response_buffer,
                                 size_t response_capacity,
                                 size_t *response_len,
                                 int32_t *return_value,
                                 int32_t *errno_value,
                                 uint32_t timeout_ms);

int px4_openamp_rpc_call_async(uint16_t category,
			       uint16_t opcode,
			       const void *request_payload,
			       size_t request_len,
			       void *response_buffer,
			       size_t response_capacity,
			       px4_openamp_rpc_response_cb_t callback,
			       void *user_data,
			       int32_t *out_request_id);

bool px4_openamp_rpc_endpoint_ready(void);
uint32_t px4_openamp_rpc_pending_count(void);
uint32_t px4_openamp_rpc_no_buf_count(void);

/* Send SESSION_RESET to CA55 to flush stale file descriptors from the previous
 * CR8 session (JLink warm reset / reboot without CA55 restart).
 * Also bumps g_rpc_next_request_id to avoid stale-response req_id collisions.
 * Call once after RPC endpoint becomes ready, before any file I/O. */
int px4_openamp_rpc_session_reset(void);

#ifdef __cplusplus
}
#endif
