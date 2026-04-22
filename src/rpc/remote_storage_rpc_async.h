/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rzv_remote_fs_async_cb_t)(int32_t request_id,
					int32_t return_value,
					int32_t errno_value,
					void *user_data);

int rzv_remote_fs_open_async(const char *path,
			     int flags,
			     mode_t mode,
			     rzv_remote_fs_async_cb_t callback,
			     void *user_data);

int rzv_remote_fs_write_async(int fd,
			      const void *buffer,
			      size_t buflen,
			      rzv_remote_fs_async_cb_t callback,
			      void *user_data);

int rzv_remote_fs_close_async(int fd,
			      rzv_remote_fs_async_cb_t callback,
			      void *user_data);

#ifdef __cplusplus
}
#endif
