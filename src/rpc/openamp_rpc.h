/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum px4_rpc_category : uint16_t {
	PX4_RPC_CATEGORY_SOCKET = 1,
	PX4_RPC_CATEGORY_FS     = 2,
};

enum px4_rpc_socket_op : uint16_t {
	PX4_RPC_SOCKET            = 1,
	PX4_RPC_BIND              = 2,
	PX4_RPC_CONNECT           = 3,
	PX4_RPC_LISTEN            = 4,
	PX4_RPC_ACCEPT            = 5,
	PX4_RPC_SETSOCKOPT        = 6,
	PX4_RPC_SHUTDOWN          = 7,
	PX4_RPC_SEND              = 8,
	PX4_RPC_RECV              = 9,
	PX4_RPC_GETSOCKNAME       = 10,
	PX4_RPC_GETPEERNAME       = 11,
	PX4_RPC_CLOSE_SOCKET      = 12,
	PX4_RPC_SOCKET_IOCTL      = 13,
};

enum px4_rpc_fs_op : uint16_t {
	PX4_RPC_FS_OPEN   = 1,
	PX4_RPC_FS_READ   = 2,
	PX4_RPC_FS_WRITE  = 3,
	PX4_RPC_FS_CLOSE  = 4,
	PX4_RPC_FS_LSEEK  = 5,
	PX4_RPC_FS_FSYNC  = 6,
	PX4_RPC_FS_UNLINK = 7,
	PX4_RPC_FS_ACCESS = 8,
	PX4_RPC_FS_IOCTL  = 9,
	PX4_RPC_FS_MKDIR  = 10,
	PX4_RPC_FS_RMDIR  = 11,
	PX4_RPC_FS_TRUNCATE = 12,
	/* Sent by CR8 on reconnect (JLink reset / warm boot without CA55 restart).
	 * CA55 closes all stale file descriptors and clears buffered state. */
	PX4_RPC_FS_SESSION_RESET = 13,

	/* Directory listing — used by mavlink_log_handler and mavlink_ftp */
	PX4_RPC_FS_OPENDIR  = 14,   /* path → dir_handle (int) */
	PX4_RPC_FS_READDIR  = 15,   /* dir_handle → d_name[256] + d_type[1] */
	PX4_RPC_FS_CLOSEDIR = 16,   /* dir_handle → void */

	/* Atomic file rename — used by mavlink_log_handler ($log$.txt → logdata.txt) */
	PX4_RPC_FS_RENAME   = 17,   /* old_path + new_path → 0 or -errno */

	/* File stat — returns mode, size, mtime for date display in QGC log list */
	PX4_RPC_FS_STAT     = 18,   /* path → { int32 mode, int64 size, int64 mtime } */

	/* Clock sync — CR8 forwards QGC SYSTEM_TIME to CA55 so file mtimes are correct */
	PX4_RPC_CA55_CLOCK_SETTIME = 19,  /* int64 unix_sec → 0 or -errno */

	/* System management — CA55 reboots the whole Linux side, which also resets CR8 */
	PX4_RPC_CA55_SYSTEM_REBOOT = 20,  /* no payload → 0 or -errno */

	/* Log session management — CA55 owns session dir creation and logdata.txt rebuild.
	 * SESSION_OPEN: CR8 sends ARM timestamp; CA55 scans log dirs, creates sessXXX/,
	 *               opens the .ulg file, returns fd (used for stream channel routing).
	 * SESSION_CLOSE: CR8 sends fd; CA55 flushes+closes file, rebuilds logdata.txt. */
	PX4_RPC_FS_SESSION_OPEN  = 21,  /* uint64 timestamp_us → fd (≥0) or -errno */
	PX4_RPC_FS_SESSION_CLOSE = 22,  /* int32 fd → 0 or -errno */
};

#define PX4_RPC_MAGIC 0x52504330u /* "RPC0" */

#pragma pack(push, 1)
struct px4_rpc_header {
	uint32_t magic;
	uint16_t category;
	uint16_t opcode;
	int32_t request_id;
	int32_t return_value;
	int32_t errno_value;
	uint32_t payload_size;
};
#pragma pack(pop)

#ifdef __cplusplus
}
#endif
