/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include <cstdint>

#define PX4_RPC_MAGIC 0x52504330u // "RPC0" — must match CR8 firmware openamp_rpc.h

// RPC Categories
enum
{
    PX4_RPC_CATEGORY_SOCKET = 1,
    PX4_RPC_CATEGORY_FS     = 2,
};

// Socket operation opcodes (category = PX4_RPC_CATEGORY_SOCKET)
enum
{
    PX4_RPC_SOCKET       = 1,
    PX4_RPC_BIND         = 2,
    PX4_RPC_CONNECT      = 3,
    PX4_RPC_LISTEN       = 4,
    PX4_RPC_ACCEPT       = 5,
    PX4_RPC_SETSOCKOPT   = 6,
    PX4_RPC_SHUTDOWN     = 7,
    PX4_RPC_SEND         = 8,
    PX4_RPC_RECV         = 9,
    PX4_RPC_GETSOCKNAME  = 10,
    PX4_RPC_GETPEERNAME  = 11,
    PX4_RPC_CLOSE_SOCKET = 12,
    PX4_RPC_SOCKET_IOCTL = 13,
};

// File System operation opcodes (category = PX4_RPC_CATEGORY_FS)
// Values must stay in sync with CR8 src/rpc/openamp_rpc.h px4_rpc_fs_op
enum
{
    PX4_RPC_FS_OPEN            = 1,
    PX4_RPC_FS_READ            = 2,
    PX4_RPC_FS_WRITE           = 3,
    PX4_RPC_FS_CLOSE           = 4,
    PX4_RPC_FS_LSEEK           = 5,
    PX4_RPC_FS_FSYNC           = 6,
    PX4_RPC_FS_UNLINK          = 7,
    PX4_RPC_FS_ACCESS          = 8,
    PX4_RPC_FS_IOCTL           = 9,
    PX4_RPC_FS_MKDIR           = 10,
    PX4_RPC_FS_RMDIR           = 11,
    PX4_RPC_FS_TRUNCATE        = 12,
    PX4_RPC_FS_SESSION_RESET   = 13,
    PX4_RPC_FS_OPENDIR         = 14,
    PX4_RPC_FS_READDIR         = 15,
    PX4_RPC_FS_CLOSEDIR        = 16,
    PX4_RPC_FS_RENAME          = 17,
    PX4_RPC_FS_STAT            = 18,
    PX4_RPC_CA55_CLOCK_SETTIME = 19,
    PX4_RPC_CA55_SYSTEM_REBOOT = 20,
    PX4_RPC_FS_SESSION_OPEN    = 21,
    PX4_RPC_FS_SESSION_CLOSE   = 22,
};

// Wire format — 24 bytes, must match CR8 src/rpc/openamp_rpc.h px4_rpc_header
struct __attribute__((packed)) px4_rpc_header
{
    uint32_t magic;
    uint16_t category;
    uint16_t opcode;
    int32_t  request_id;
    int32_t  return_value;
    int32_t  errno_value;
    uint32_t payload_size;
};
