/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file remote_storage_rpc.c
 * @brief Remote storage RPC implementation for Renesas RZ/V2H
 */

#include "openamp_rpc_client.h"
#include "openamp_rpc.h"
#include "platform/rzv_sdram_layout.h"
#include "remote_storage_rpc_async.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <time.h>

// Logger stream channel: fire-and-forget RPMsg writes bypassing RPC round-trip overhead
#include "openamp/rpmsg.h"
#include "FreeRTOS.h"
#include "task.h"

// rpmsg_ept_logger is created by main_task_entry.c (optional - dest_addr=RPMSG_ADDR_ANY if CA55 not connected)
extern struct rpmsg_endpoint rpmsg_ept_logger;

// Transport lock shared with UXRCE-DDS (protects virtio device access)
extern void px4_openamp_transport_lock(void);
extern void px4_openamp_transport_unlock(void);

// Message types for logger stream protocol
#define LOG_STREAM_MSG_WRITE  0x01u  // Write raw data to CA55 fd
#define LOG_STREAM_MSG_FLUSH  0x02u  // Flush & release fd from CA55 logger buffer

// Max data bytes per logger stream message: RPMsg 512B - rpmsg_hdr(16) - log_stream_hdr(8) = 488B
#define LOG_STREAM_MAX_CHUNK  488u

// Logger stream message header (8 bytes, same layout on CR8 and CA55)
struct log_stream_hdr {
    uint8_t  msg_type;   // LOG_STREAM_MSG_WRITE or LOG_STREAM_MSG_FLUSH
    uint8_t  reserved;
    uint16_t data_len;   // bytes of payload following this header
    int32_t  ca55_fd;    // CA55 file descriptor returned by RPC FS_OPEN
};

// Logger fd table: tracks which CA55 fds use the stream channel (typically 1 active logger fd)
#define LOG_STREAM_MAX_FDS  4
static int s_logger_fds[LOG_STREAM_MAX_FDS];
static int s_logger_fd_count = 0;

static void logger_fd_table_init(void)
{
    for (int i = 0; i < LOG_STREAM_MAX_FDS; i++) {
        s_logger_fds[i] = -1;
    }
}

// Call once at startup - FreeRTOS static init is fine since we run before PX4 logger
__attribute__((constructor)) static void logger_fd_table_ctor(void)
{
    logger_fd_table_init();
}

static bool is_logger_fd(int fd)
{
    for (int i = 0; i < LOG_STREAM_MAX_FDS; i++) {
        if (s_logger_fds[i] == fd) { return true; }
    }
    return false;
}

static bool logger_open_intends_write(int flags)
{
    const int access_mode = flags & O_ACCMODE;

    if (access_mode == O_WRONLY || access_mode == O_RDWR) {
        return true;
    }

    return (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0;
}

static void logger_fd_register(int fd)
{
    if (is_logger_fd(fd)) {
        return;
    }

    for (int i = 0; i < LOG_STREAM_MAX_FDS; i++) {
        if (s_logger_fds[i] == -1) {
            s_logger_fds[i] = fd;
            s_logger_fd_count++;
            return;
        }
    }
    // Table full - fall back to RPC for this fd
    fprintf(stderr, "[RPC][LOGGER] register skipped fd=%d table_full=%d\n", fd, LOG_STREAM_MAX_FDS);
}

static void logger_fd_unregister(int fd)
{
    for (int i = 0; i < LOG_STREAM_MAX_FDS; i++) {
        if (s_logger_fds[i] == fd) {
            s_logger_fds[i] = -1;
            if (s_logger_fd_count > 0) { s_logger_fd_count--; }
            return;
        }
    }
}

// Check if logger stream endpoint is connected (CA55 bound its side)
static inline bool logger_stream_ready(void)
{
    return (rpmsg_ept_logger.rdev != NULL) &&
           (rpmsg_ept_logger.dest_addr != RPMSG_ADDR_ANY);
}

static inline bool logger_fd_use_stream(int fd)
{
    return is_logger_fd(fd) && logger_stream_ready();
}

#define LOG_STREAM_MSG_KICK   0x03u

#define LOG_SHM_MAGIC 0x4C4F4753U // "LOGS"

struct logger_shm_ctrl {
    uint32_t magic;
    uint32_t buffer_size;
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t dropped_bytes;
    int32_t  active_fd;
    uint32_t reserved[2];
    uint8_t  data[RZV_SDRAM_LOGGER_BUF_SIZE];
};

struct logger_shm_region {
    struct logger_shm_ctrl streams[RZV_SDRAM_LOGGER_STREAM_COUNT];
};

_Static_assert(sizeof(struct logger_shm_ctrl) == RZV_SDRAM_LOGGER_STREAM_BYTES,
               "logger_shm_ctrl size must match shared SDRAM stream size");
_Static_assert(sizeof(struct logger_shm_region) == RZV_SDRAM_LOGGER_SHM_BYTES,
               "logger_shm_region size must match shared SDRAM reservation");

static volatile struct logger_shm_region *g_log_shm = (volatile struct logger_shm_region *)RZV_SDRAM_LOGGER_SHM_ADDR;

static inline void dcache_clean_range(uintptr_t start, uint32_t len) {
    uintptr_t addr = start & ~(32U - 1U);
    uintptr_t end = start + len;
    while (addr < end) {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(addr) : "memory");
        addr += 32U;
    }
    __asm__ volatile("dsb" : : : "memory");
}

static inline void dcache_invalidate_range(uintptr_t start, uint32_t len) {
    uintptr_t addr = start & ~(32U - 1U);
    uintptr_t end = start + len;
    while (addr < end) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" : : "r"(addr) : "memory");
        addr += 32U;
    }
    __asm__ volatile("dsb" : : : "memory");
}

// Assign an SHM stream out of the pool for the given fd
static volatile struct logger_shm_ctrl *get_shm_stream(int fd) {
    for (int i = 0; i < (int)RZV_SDRAM_LOGGER_STREAM_COUNT; i++) {
        if (g_log_shm->streams[i].magic == LOG_SHM_MAGIC && g_log_shm->streams[i].active_fd == fd) {
            return &g_log_shm->streams[i];
        }
    }
    // Allocate new
    for (int i = 0; i < (int)RZV_SDRAM_LOGGER_STREAM_COUNT; i++) {
        if (g_log_shm->streams[i].magic != LOG_SHM_MAGIC || g_log_shm->streams[i].active_fd < 0) {
            volatile struct logger_shm_ctrl *ctrl = &g_log_shm->streams[i];
            ctrl->buffer_size = RZV_SDRAM_LOGGER_BUF_SIZE;
            ctrl->head = 0;
            ctrl->tail = 0;
            ctrl->dropped_bytes = 0;
            ctrl->active_fd = fd;
            ctrl->magic = LOG_SHM_MAGIC;
            dcache_clean_range((uintptr_t)ctrl, 32);
            return ctrl;
        }
    }
    return NULL;
}

static ssize_t rzv_logger_stream_write(int ca55_fd, const void *buffer, size_t buflen)
{
    volatile struct logger_shm_ctrl *ctrl = get_shm_stream(ca55_fd);
    if (!ctrl) return 0; // No streams available

    dcache_invalidate_range((uintptr_t)ctrl, 32);

    uint32_t head = ctrl->head;
    uint32_t tail = ctrl->tail;
    uint32_t size = ctrl->buffer_size;

    uint32_t free_space = (tail > head) ? (tail - head - 1) : (size - head + tail - 1);
    if (free_space < buflen) {
        ctrl->dropped_bytes += buflen;
        dcache_clean_range((uintptr_t)ctrl, 32);
        return buflen; // Silently drop (ULog recovery handles it)
    }

    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t until_end = size - head;

    if (buflen <= until_end) {
        memcpy((void *)&ctrl->data[head], src, buflen);
        dcache_clean_range((uintptr_t)&ctrl->data[head], buflen);
    } else {
        memcpy((void *)&ctrl->data[head], src, until_end);
        dcache_clean_range((uintptr_t)&ctrl->data[head], until_end);
        memcpy((void *)&ctrl->data[0], src + until_end, buflen - until_end);
        dcache_clean_range((uintptr_t)&ctrl->data[0], buflen - until_end);
    }

    ctrl->head = (head + buflen) % size;
    dcache_clean_range((uintptr_t)ctrl, 32);

    static uint32_t last_kick_head[RZV_SDRAM_LOGGER_STREAM_COUNT] = {0};
    int idx = ctrl - &g_log_shm->streams[0];
    uint32_t diff = (ctrl->head >= last_kick_head[idx]) ? (ctrl->head - last_kick_head[idx]) : (size - last_kick_head[idx] + ctrl->head);
    
    if (diff >= 16384) {
        struct log_stream_hdr hdr = {
            .msg_type = LOG_STREAM_MSG_KICK,
            .reserved = 0,
            .data_len = 0,
            .ca55_fd  = ca55_fd,
        };
        px4_openamp_transport_lock();
        rpmsg_trysend(&rpmsg_ept_logger, &hdr, sizeof(hdr));
        px4_openamp_transport_unlock();
        last_kick_head[idx] = ctrl->head;
    }

    return buflen;
}

static void rzv_logger_stream_flush(int ca55_fd)
{
    struct log_stream_hdr hdr = {
        .msg_type = LOG_STREAM_MSG_FLUSH,
        .reserved = 0,
        .data_len = 0,
        .ca55_fd  = ca55_fd,
    };
    px4_openamp_transport_lock();
    rpmsg_trysend(&rpmsg_ept_logger, &hdr, sizeof(hdr));
    px4_openamp_transport_unlock();
    
    // CA55 will synchronously flush its portion and THEN set active_fd = -1
    vTaskDelay(pdMS_TO_TICKS(5));
}

#define FS_RPC_TIMEOUT_MS 2000U /* Bound RPCs to avoid indefinite stalls */
#define FS_RPC_RETRY_DELAY_US 200000U
#define FS_RPC_MAX_ATTEMPTS 3
#define FS_RPC_SLOW_THRESHOLD_US 150000U /* Log warning if RPC takes >150ms (eMMC normal: 10-60ms) */
#define FS_RPC_SLOW_RATE_LIMIT_US 2000000ULL /* Suppress repeated SLOW logs within 2s */

static int fs_rpc(uint16_t opcode,
                  const void *payload,
                  size_t payload_len,
                  void *response_buffer,
                  size_t response_capacity,
                  size_t *response_len,
                  int32_t *ret_val_out,
                  int32_t *err_val_out)
{
    int32_t ret_val = -1;
    int32_t err_val = 0;

    /* Return immediately if CA55 endpoint not ready.
     * Callers (io_worker param save, logger) already have their own retry loops.
     * The old 30s blocking wait caused logger::run() mkdir to stall the logger task
     * for 30s at startup before CA55 was available. */
    if (!px4_openamp_rpc_endpoint_ready()) {
        return -ENOTCONN;
    }

    const int max_attempts = FS_RPC_MAX_ATTEMPTS;
    int attempt = 0;
    int call_ret = -1;
    int saved_errno = 0;

    /* Start timing measurement */
    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    for (attempt = 0; attempt < max_attempts; ++attempt) {
        call_ret = px4_openamp_rpc_call_timeout(PX4_RPC_CATEGORY_FS,
                                                opcode,
                                                payload,
                                                payload_len,
                                                response_buffer,
                                                response_capacity,
                                                response_len,
                                                &ret_val,
                                                &err_val,
                                                FS_RPC_TIMEOUT_MS);
        if (call_ret == 0) {
            break;
        }

        saved_errno = errno;

        /* Retry on transient timeout/busy conditions */
        if (saved_errno == ETIMEDOUT || saved_errno == EAGAIN || saved_errno == EBUSY) {
            if (!px4_openamp_rpc_endpoint_ready()) {
                return -ENOTCONN;
            }

            usleep(FS_RPC_RETRY_DELAY_US);
            continue;
        }

        /* Non-retryable failure */
        return (saved_errno != 0) ? -saved_errno : -EIO;
    }

    /* End timing measurement and log slow operations */
    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    long sec_diff  = end_ts.tv_sec  - start_ts.tv_sec;
    long nsec_diff = end_ts.tv_nsec - start_ts.tv_nsec;
    if (nsec_diff < 0) { sec_diff -= 1; nsec_diff += 1000000000L; }
    uint64_t elapsed_us = (uint64_t)sec_diff * 1000000ULL + (uint64_t)nsec_diff / 1000ULL;

    if (elapsed_us > FS_RPC_SLOW_THRESHOLD_US) {
        static uint64_t last_slow_log_us = 0;
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        uint64_t now_us = (uint64_t)now_ts.tv_sec * 1000000ULL + (uint64_t)now_ts.tv_nsec / 1000ULL;
        if ((now_us - last_slow_log_us) >= FS_RPC_SLOW_RATE_LIMIT_US) {
            fprintf(stderr, "[FS_RPC] SLOW op=%u elapsed=%llu us attempts=%d ret=%d\n",
                    opcode, (unsigned long long)elapsed_us, attempt + 1, call_ret);
            last_slow_log_us = now_us;
        }
    }

    if (call_ret < 0) {
        return (saved_errno != 0) ? -saved_errno : (errno != 0 ? -errno : -EIO);
    }

    if (ret_val_out) {
        *ret_val_out = ret_val;
    }

    if (err_val_out) {
        *err_val_out = err_val;
    }

    return 0;
}

static int translate_rpc_error(int32_t err_val)
{
    return (err_val == 0) ? 0 : -err_val;
}

int rzv_remote_fs_open_impl(const char *path, int flags, mode_t mode)
{
    if (!path) {
        return -EINVAL;
    }

    size_t path_len = strlen(path) + 1U;

    if (path_len > UINT32_MAX) {
        return -ENAMETOOLONG;
    }

    struct fs_open_request {
        int32_t flags;
        int32_t mode;
        uint32_t path_len;
    } req = {
        .flags = flags,
        .mode = mode,
        .path_len = (uint32_t)path_len,
    };

    size_t payload_len = sizeof(req) + path_len;
    uint8_t stack_buf[256];
    uint8_t *payload = stack_buf;

    if (payload_len > sizeof(stack_buf)) {
        payload = (uint8_t *)malloc(payload_len);

        if (!payload) {
            return -ENOMEM;
        }
    }

    memcpy(payload, &req, sizeof(req));
    memcpy(payload + sizeof(req), path, path_len);

    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_OPEN,
                         payload,
                         payload_len,
                         NULL,
                         0,
                         NULL,
                         &ret_val,
                         &err_val);

    if (payload != stack_buf) {
        free(payload);
    }

    if (rpc_ret < 0) {
        return (errno != 0) ? -errno : -EIO;
    }

    if (err_val != 0) {
        return -err_val;
    }

    // Track logger fds even if the stream endpoint is not bound yet.
    // This lets a log opened before CA55/logger-service readiness switch from
    // RPC fallback to the stream channel later in the same arm session.
    if (ret_val >= 0 && strstr(path, ".ulg") != NULL && logger_open_intends_write(flags)) {
        logger_fd_register(ret_val);
    }

    return ret_val;
}

ssize_t rzv_remote_fs_read_impl(int fd, void *buffer, size_t buflen)
{
    if (buflen > UINT32_MAX) {
        return -EINVAL;
    }

    struct fs_read_request {
        int32_t fd;
        uint32_t len;
    } req = {
        .fd = fd,
        .len = (uint32_t)buflen,
    };

    size_t response_len = 0;
    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_READ,
                         &req,
                         sizeof(req),
                         buffer,
                         buflen,
                         &response_len,
                         &ret_val,
                         &err_val);

    if (rpc_ret < 0) {
        return (errno != 0) ? -errno : -EIO;
    }

    if (err_val != 0) {
        return -err_val;
    }

    if (response_len > buflen) {
        return -EIO;
    }

    return ret_val;
}

/* Maximum data bytes per FS_WRITE RPC call.
 * RPMsg buffer is 512 bytes; rpmsg_hdr=16, px4_rpc_header=24, fs_write_req=8 → 464 bytes free.
 * Use 448 (multiple of 64) to leave headroom for alignment/virtio overhead. */
#define FS_WRITE_MAX_CHUNK 448u

ssize_t rzv_remote_fs_write_impl(int fd, const void *buffer, size_t buflen)
{
    if ((buflen > 0 && buffer == NULL) || buflen > UINT32_MAX) {
        return -EINVAL;
    }

    // Fast path: logger fds bypass RPC round-trip via dedicated stream channel
    if (logger_fd_use_stream(fd)) {
        return rzv_logger_stream_write(fd, buffer, buflen);
    }

    struct fs_write_request {
        int32_t fd;
        uint32_t len;
    } req;

    /* Chunk the write to fit within the RPMsg buffer (RPMSG_BUFFER_SIZE = 512 bytes).
     * A single RPC message = rpmsg_hdr(16) + px4_rpc_header(24) + fs_write_req(8) + data
     * → max data per call = 512 - 16 - 24 - 8 = 464 bytes → use FS_WRITE_MAX_CHUNK = 448. */
    const uint8_t *src = (const uint8_t *)buffer;
    size_t remaining = buflen;
    ssize_t total_written = 0;

    /* Stack buffer fits one max-chunk payload (req header + data). */
    uint8_t chunk_buf[sizeof(req) + FS_WRITE_MAX_CHUNK];

    while (remaining > 0 || buflen == 0) {
        size_t chunk = (remaining < FS_WRITE_MAX_CHUNK) ? remaining : FS_WRITE_MAX_CHUNK;

        req.fd  = fd;
        req.len = (uint32_t)chunk;

        memcpy(chunk_buf, &req, sizeof(req));
        if (chunk > 0 && src) {
            memcpy(chunk_buf + sizeof(req), src, chunk);
        }

        size_t payload_len = sizeof(req) + chunk;

        int32_t ret_val = -1;
        int32_t err_val = 0;

        int rpc_ret = fs_rpc(PX4_RPC_FS_WRITE,
                             chunk_buf,
                             payload_len,
                             NULL,
                             0,
                             NULL,
                             &ret_val,
                             &err_val);

        if (rpc_ret < 0) {
            return (total_written > 0) ? total_written : ((errno != 0) ? -errno : -EIO);
        }

        if (err_val != 0) {
            return (total_written > 0) ? total_written : -err_val;
        }

        if (ret_val < 0) {
            return (total_written > 0) ? total_written : (ssize_t)ret_val;
        }

        total_written += (ssize_t)ret_val;
        src       += (size_t)ret_val;
        remaining -= (size_t)ret_val;

        /* Handle zero-length write or short write — stop. */
        if (buflen == 0 || (size_t)ret_val < chunk) {
            break;
        }
    }

    return total_written;
}

int rzv_remote_fs_close_impl(int fd)
{
    // For logger fds: flush remaining buffered data on CA55 before closing via RPC
    const bool tracked_logger_fd = is_logger_fd(fd);

    if (tracked_logger_fd && logger_stream_ready()) {
        rzv_logger_stream_flush(fd);
    }

    if (tracked_logger_fd) {
        logger_fd_unregister(fd);
    }

    struct fs_close_request {
        int32_t fd;
    } req = {
        .fd = fd,
    };

    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_CLOSE,
                         &req,
                         sizeof(req),
                         NULL,
                         0,
                         NULL,
                         &ret_val,
                         &err_val);

    if (rpc_ret < 0) {
        return (errno != 0) ? -errno : -EIO;
    }

    return translate_rpc_error(err_val);
}

off_t rzv_remote_fs_lseek_impl(int fd, off_t offset, int whence)
{
    struct fs_lseek_request {
        int32_t fd;
        int32_t whence;
        int64_t offset;
    } req = {
        .fd = fd,
        .whence = whence,
        .offset = offset,
    };

    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_LSEEK,
                         &req,
                         sizeof(req),
                         NULL,
                         0,
                         NULL,
                         &ret_val,
                         &err_val);

    if (rpc_ret < 0) {
        return (errno != 0) ? -errno : -EIO;
    }

    if (err_val != 0) {
        return -err_val;
    }

    return (off_t)ret_val;
}

int rzv_remote_fs_fsync_impl(int fd)
{
    /* Logger .ulg fds use the RPMsg stream channel for all writes.
     * The CA55 flush thread (100 ms interval) handles eMMC sync — no need
     * to issue a blocking RPC fsync that times out when CA55 is busy under
     * motor load.  Returning 0 here silences the [RPC][WAIT] fsync timeouts
     * seen during flight (op=6 in running.log). */
    if (logger_fd_use_stream(fd)) {
        return 0;
    }

    struct fs_fsync_request {
        int32_t fd;
    } req = {
        .fd = fd,
    };

    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_FSYNC,
                         &req,
                         sizeof(req),
                         NULL,
                         0,
                         NULL,
                         &ret_val,
                         &err_val);

    if (rpc_ret < 0) {
        return (errno != 0) ? -errno : -EIO;
    }

    return translate_rpc_error(err_val);
}

int rzv_remote_fs_unlink_impl(const char *path)
{
    if (!path) {
        return -EINVAL;
    }

    size_t path_len = strlen(path) + 1U;

    if (path_len > UINT32_MAX) {
        return -ENAMETOOLONG;
    }

    struct fs_unlink_request {
        uint32_t path_len;
    } req = {
        .path_len = (uint32_t)path_len,
    };

    size_t payload_len = sizeof(req) + path_len;
    uint8_t stack_buf[256];
    uint8_t *payload = stack_buf;

    if (payload_len > sizeof(stack_buf)) {
        payload = (uint8_t *)malloc(payload_len);

        if (!payload) {
            return -ENOMEM;
        }
    }

    memcpy(payload, &req, sizeof(req));
    memcpy(payload + sizeof(req), path, path_len);

    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_UNLINK,
                         payload,
                         payload_len,
                         NULL,
                         0,
                         NULL,
                         &ret_val,
                         &err_val);

    if (payload != stack_buf) {
        free(payload);
    }

    if (rpc_ret < 0) {
        return (errno != 0) ? -errno : -EIO;
    }

    return translate_rpc_error(err_val);
}

int rzv_remote_fs_rename_impl(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) {
        return -EINVAL;
    }

    size_t old_len = strlen(old_path) + 1U;
    size_t new_len = strlen(new_path) + 1U;

    struct fs_rename_request {
        uint32_t old_path_len;
        uint32_t new_path_len;
    } req = {
        .old_path_len = (uint32_t)old_len,
        .new_path_len = (uint32_t)new_len,
    };

    size_t payload_len = sizeof(req) + old_len + new_len;
    uint8_t stack_buf[512];
    uint8_t *payload = stack_buf;

    if (payload_len > sizeof(stack_buf)) {
        payload = (uint8_t *)malloc(payload_len);

        if (!payload) {
            return -ENOMEM;
        }
    }

    memcpy(payload, &req, sizeof(req));
    memcpy(payload + sizeof(req), old_path, old_len);
    memcpy(payload + sizeof(req) + old_len, new_path, new_len);

    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_RENAME,
                         payload,
                         payload_len,
                         NULL,
                         0,
                         NULL,
                         &ret_val,
                         &err_val);

    if (payload != stack_buf) {
        free(payload);
    }

    if (rpc_ret < 0) {
        return (errno != 0) ? -errno : -EIO;
    }

    return translate_rpc_error(err_val);
}

/* Compact stat response — mode (int32) + size (int64) + mtime (int64) = 20 bytes */
#pragma pack(push, 1)
struct rzv_rpc_stat_response {
    int32_t  mode;
    int64_t  size;
    int64_t  mtime;
};
#pragma pack(pop)

int rzv_remote_fs_stat_impl(const char *path, int32_t *out_mode, int64_t *out_size, int64_t *out_mtime)
{
    if (!path || !out_mode || !out_size || !out_mtime) {
        return -EINVAL;
    }

    size_t path_len = strlen(path) + 1U;

    struct fs_stat_request {
        uint32_t path_len;
    } req = { .path_len = (uint32_t)path_len };

    size_t payload_len = sizeof(req) + path_len;
    uint8_t stack_buf[256];
    uint8_t *payload = stack_buf;

    if (payload_len > sizeof(stack_buf)) {
        payload = (uint8_t *)malloc(payload_len);
        if (!payload) { return -ENOMEM; }
    }

    memcpy(payload, &req, sizeof(req));
    memcpy(payload + sizeof(req), path, path_len);

    struct rzv_rpc_stat_response resp = {0};
    size_t resp_len = sizeof(resp);
    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_STAT,
                         payload,
                         payload_len,
                         (uint8_t *)&resp,
                         resp_len,
                         &resp_len,
                         &ret_val,
                         &err_val);

    if (payload != stack_buf) { free(payload); }

    if (rpc_ret < 0) { return (errno != 0) ? -errno : -EIO; }
    if (err_val != 0) { return -err_val; }

    *out_mode  = resp.mode;
    *out_size  = resp.size;
    *out_mtime = resp.mtime;
    return 0;
}

int rzv_remote_fs_access_impl(const char *path, int mode)
{
    if (!path) {
        return -EINVAL;
    }

    size_t path_len = strlen(path) + 1U;

    if (path_len > UINT32_MAX) {
        return -ENAMETOOLONG;
    }

    struct fs_access_request {
        int32_t mode;
        uint32_t path_len;
    } req = {
        .mode = mode,
        .path_len = (uint32_t)path_len,
    };

    size_t payload_len = sizeof(req) + path_len;
    uint8_t stack_buf[256];
    uint8_t *payload = stack_buf;

    if (payload_len > sizeof(stack_buf)) {
        payload = (uint8_t *)malloc(payload_len);

        if (!payload) {
            return -ENOMEM;
        }
    }

    memcpy(payload, &req, sizeof(req));
    memcpy(payload + sizeof(req), path, path_len);

    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_ACCESS,
                         payload,
                         payload_len,
                         NULL,
                         0,
                         NULL,
                         &ret_val,
                         &err_val);

    if (payload != stack_buf) {
        free(payload);
    }

    if (rpc_ret < 0) {
        return (errno != 0) ? -errno : -EIO;
    }

    if (err_val != 0) {
        return -err_val;
    }

    return ret_val;
}

int rzv_remote_fs_mkdir_impl(const char *path, mode_t mode)
{
	if (!path) {
		return -EINVAL;
	}

	size_t path_len = strlen(path) + 1U;

	if (path_len > UINT32_MAX) {
		return -ENAMETOOLONG;
	}

	struct fs_mkdir_request {
		uint32_t path_len;
		int32_t mode;
	} req = {
		.path_len = (uint32_t)path_len,
		.mode = (int32_t)mode,
	};

	size_t payload_len = sizeof(req) + path_len;
	uint8_t stack_buf[256];
	uint8_t *payload = stack_buf;

	if (payload_len > sizeof(stack_buf)) {
		payload = (uint8_t *)malloc(payload_len);

		if (!payload) {
			return -ENOMEM;
		}
	}

	memcpy(payload, &req, sizeof(req));
	memcpy(payload + sizeof(req), path, path_len);

	int32_t ret_val = -1;
	int32_t err_val = 0;
	int rpc_ret = fs_rpc(PX4_RPC_FS_MKDIR,
			     payload,
			     payload_len,
			     NULL,
			     0,
			     NULL,
			     &ret_val,
			     &err_val);

	if (payload != stack_buf) {
		free(payload);
	}

	if (rpc_ret < 0) {
		return (errno != 0) ? -errno : -EIO;
	}

	return translate_rpc_error(err_val);
}

int rzv_remote_fs_rmdir_impl(const char *path)
{
	if (!path) {
		return -EINVAL;
	}

	size_t path_len = strlen(path) + 1U;

	if (path_len > UINT32_MAX) {
		return -ENAMETOOLONG;
	}

	struct fs_rmdir_request {
		uint32_t path_len;
	} req = {
		.path_len = (uint32_t)path_len,
	};

	size_t payload_len = sizeof(req) + path_len;
	uint8_t stack_buf[256];
	uint8_t *payload = stack_buf;

	if (payload_len > sizeof(stack_buf)) {
		payload = (uint8_t *)malloc(payload_len);

		if (!payload) {
			return -ENOMEM;
		}
	}

	memcpy(payload, &req, sizeof(req));
	memcpy(payload + sizeof(req), path, path_len);

	int32_t ret_val = -1;
	int32_t err_val = 0;
	int rpc_ret = fs_rpc(PX4_RPC_FS_RMDIR,
			     payload,
			     payload_len,
			     NULL,
			     0,
			     NULL,
			     &ret_val,
			     &err_val);

	if (payload != stack_buf) {
		free(payload);
	}

	if (rpc_ret < 0) {
		return (errno != 0) ? -errno : -EIO;
	}

	return translate_rpc_error(err_val);
}

int rzv_remote_fs_truncate_impl(const char *path, off_t length)
{
	if (!path) {
		return -EINVAL;
	}

	size_t path_len = strlen(path) + 1U;

	if (path_len > UINT32_MAX) {
		return -ENAMETOOLONG;
	}

	struct fs_truncate_request {
		uint32_t path_len;
		int64_t length;
	} req = {
		.path_len = (uint32_t)path_len,
		.length = (int64_t)length,
	};

	size_t payload_len = sizeof(req) + path_len;
	uint8_t stack_buf[256];
	uint8_t *payload = stack_buf;

	if (payload_len > sizeof(stack_buf)) {
		payload = (uint8_t *)malloc(payload_len);

		if (!payload) {
			return -ENOMEM;
		}
	}

	memcpy(payload, &req, sizeof(req));
	memcpy(payload + sizeof(req), path, path_len);

	int32_t ret_val = -1;
	int32_t err_val = 0;
	int rpc_ret = fs_rpc(PX4_RPC_FS_TRUNCATE,
			     payload,
			     payload_len,
			     NULL,
			     0,
			     NULL,
			     &ret_val,
			     &err_val);

	if (payload != stack_buf) {
		free(payload);
	}

	if (rpc_ret < 0) {
		return (errno != 0) ? -errno : -EIO;
	}

	return translate_rpc_error(err_val);
}


int rzv_remote_fs_ioctl_rpc(int fd,
                            unsigned long request,
                            const void *write_data,
                            size_t write_len,
                            uint64_t scalar_value,
                            void *read_buffer,
                            size_t read_capacity,
                            size_t *out_read_len)
{
    if (write_len > 0 && write_data == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (write_len > UINT32_MAX || read_capacity > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    struct fs_ioctl_request {
        int32_t fd;
        uint32_t reserved0;
        uint64_t request;
        uint32_t write_len;
        uint32_t read_len;
        uint64_t scalar_value;
    } __attribute__((packed));

    struct fs_ioctl_request header = {
        .fd = fd,
        .reserved0 = 0,
        .request = request,
        .write_len = (uint32_t)write_len,
        .read_len = (uint32_t)read_capacity,
        .scalar_value = scalar_value,
    };

    size_t payload_len = sizeof(header) + write_len;
    uint8_t stack_buf[256];
    uint8_t *payload = stack_buf;

    if (payload_len > sizeof(stack_buf)) {
        payload = (uint8_t *)malloc(payload_len);

        if (!payload) {
            errno = ENOMEM;
            return -1;
        }
    }

    memcpy(payload, &header, sizeof(header));

    if (write_len > 0 && write_data) {
        memcpy(payload + sizeof(header), write_data, write_len);
    }

    size_t response_len = 0;
    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_IOCTL,
                         payload,
                         payload_len,
                         read_buffer,
                         read_capacity,
                         &response_len,
                         &ret_val,
                         &err_val);

    if (payload != stack_buf) {
        free(payload);
    }

    if (rpc_ret < 0) {
        return -1;
    }

    errno = err_val;

    if (err_val != 0) {
        return -1;
    }

    if (read_buffer == NULL && response_len > 0) {
        errno = EMSGSIZE;
        return -1;
    }

    if (response_len > read_capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    if (out_read_len) {
        *out_read_len = response_len;
    }

    errno = 0;
    return ret_val;
}

struct fs_async_context {
    rzv_remote_fs_async_cb_t callback;
    void *user_data;
};

static void fs_async_dispatch(int32_t request_id,
                              int32_t return_value,
                              int32_t errno_value,
                              const void *payload,
                              size_t payload_len,
                              void *user_data)
{
    (void)payload;
    (void)payload_len;

    struct fs_async_context *ctx = (struct fs_async_context *)user_data;

    if (!ctx) {
        return;
    }

    if (ctx->callback) {
        ctx->callback(request_id, return_value, errno_value, ctx->user_data);
    }

    free(ctx);
}

static int fs_rpc_async(uint16_t opcode,
                        const void *payload,
                        size_t payload_len,
                        rzv_remote_fs_async_cb_t callback,
                        void *user_data)
{
    if (!px4_openamp_rpc_endpoint_ready()) {
        return -ENOTCONN;
    }

    if (payload_len > UINT32_MAX) {
        return -EINVAL;
    }

    struct fs_async_context *ctx = (struct fs_async_context *)malloc(sizeof(*ctx));

    if (!ctx) {
        return -ENOMEM;
    }

    ctx->callback = callback;
    ctx->user_data = user_data;

    int32_t request_id = 0;
    int ret = px4_openamp_rpc_call_async(PX4_RPC_CATEGORY_FS,
                                         opcode,
                                         payload,
                                         payload_len,
                                         NULL,
                                         0,
                                         fs_async_dispatch,
                                         ctx,
                                         &request_id);

    if (ret < 0) {
        free(ctx);
        return (errno != 0) ? -errno : -EIO;
    }

    return request_id;
}

int rzv_remote_fs_open_async(const char *path,
                             int flags,
                             mode_t mode,
                             rzv_remote_fs_async_cb_t callback,
                             void *user_data)
{
    if (!path) {
        return -EINVAL;
    }

    size_t path_len = strlen(path) + 1U;

    if (path_len > UINT32_MAX) {
        return -ENAMETOOLONG;
    }

    struct fs_open_request {
        int32_t flags;
        int32_t mode;
        uint32_t path_len;
    } req = {
        .flags = flags,
        .mode = mode,
        .path_len = (uint32_t)path_len,
    };

    size_t payload_len = sizeof(req) + path_len;
    uint8_t stack_buf[256];
    uint8_t *payload = stack_buf;

    if (payload_len > sizeof(stack_buf)) {
        payload = (uint8_t *)malloc(payload_len);

        if (!payload) {
            return -ENOMEM;
        }
    }

    memcpy(payload, &req, sizeof(req));
    memcpy(payload + sizeof(req), path, path_len);

    int ret = fs_rpc_async(PX4_RPC_FS_OPEN, payload, payload_len, callback, user_data);

    if (payload != stack_buf) {
        free(payload);
    }

    return ret;
}

int rzv_remote_fs_write_async(int fd,
                              const void *buffer,
                              size_t buflen,
                              rzv_remote_fs_async_cb_t callback,
                              void *user_data)
{
    if ((buflen > 0 && buffer == NULL) || buflen > UINT32_MAX) {
        return -EINVAL;
    }

    struct fs_write_request {
        int32_t fd;
        uint32_t len;
    } req = {
        .fd = fd,
        .len = (uint32_t)buflen,
    };

    size_t payload_len = sizeof(req) + buflen;
    uint8_t stack_buf[256];
    uint8_t *payload = stack_buf;

    if (payload_len > sizeof(stack_buf)) {
        payload = (uint8_t *)malloc(payload_len);

        if (!payload) {
            return -ENOMEM;
        }
    }

    memcpy(payload, &req, sizeof(req));

    if (buflen > 0 && buffer) {
        memcpy(payload + sizeof(req), buffer, buflen);
    }

    int ret = fs_rpc_async(PX4_RPC_FS_WRITE, payload, payload_len, callback, user_data);

    if (payload != stack_buf) {
        free(payload);
    }

    return ret;
}

int rzv_remote_fs_close_async(int fd,
                              rzv_remote_fs_async_cb_t callback,
                              void *user_data)
{
    struct fs_close_request {
        int32_t fd;
    } req = {
        .fd = fd,
    };

    return fs_rpc_async(PX4_RPC_FS_CLOSE, &req, sizeof(req), callback, user_data);
}

/* ---------------------------------------------------------------------------
 * Directory listing RPC — used by opendir/readdir/closedir stubs.
 * These run synchronously (blocking) since directory scanning is infrequent
 * (only during MAVLink log-list or FTP directory list requests).
 * --------------------------------------------------------------------------- */

int rzv_remote_opendir(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    size_t path_len = strlen(path) + 1U;

    if (path_len > UINT32_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }

    struct fs_opendir_request {
        uint32_t path_len;
    } req = {
        .path_len = (uint32_t)path_len,
    };

    size_t payload_len = sizeof(req) + path_len;
    uint8_t stack_buf[256];
    uint8_t *payload = stack_buf;

    if (payload_len > sizeof(stack_buf)) {
        payload = (uint8_t *)malloc(payload_len);

        if (!payload) {
            errno = ENOMEM;
            return -1;
        }
    }

    memcpy(payload, &req, sizeof(req));
    memcpy(payload + sizeof(req), path, path_len);

    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_OPENDIR,
                         payload,
                         payload_len,
                         NULL,
                         0,
                         NULL,
                         &ret_val,
                         &err_val);

    if (payload != stack_buf) {
        free(payload);
    }

    if (rpc_ret < 0) {
        errno = (errno != 0) ? errno : EIO;
        return -1;
    }

    if (err_val != 0) {
        errno = err_val;
        return -1;
    }

    return ret_val;  /* CA55 dir handle (non-negative int) */
}

/* Returns 0 if an entry was read, 1 on EOF, -1 on error.
 * d_name and d_type are populated on success (return 0). */
int rzv_remote_readdir(int dir_handle, char *d_name, size_t name_len, unsigned char *d_type)
{
    if (!d_name || name_len == 0 || !d_type) {
        errno = EINVAL;
        return -1;
    }

    struct fs_readdir_request {
        int32_t dir_handle;
    } req = {
        .dir_handle = dir_handle,
    };

    /* Response payload: d_name[256] + d_type[1] = 257 bytes, within RPMsg 496-byte limit */
    struct fs_readdir_response {
        char     d_name[256];
        uint8_t  d_type;
    } resp;

    size_t response_len = 0;
    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_READDIR,
                         &req,
                         sizeof(req),
                         &resp,
                         sizeof(resp),
                         &response_len,
                         &ret_val,
                         &err_val);

    if (rpc_ret < 0) {
        errno = (errno != 0) ? errno : EIO;
        return -1;
    }

    if (err_val != 0) {
        errno = err_val;
        return -1;
    }

    /* ret_val: 0=entry read, 1=EOF, <0=error */
    if (ret_val == 1) {
        return 1;  /* EOF */
    }

    if (ret_val < 0) {
        errno = (err_val != 0) ? err_val : EIO;
        return -1;
    }

    /* Copy entry fields to caller buffers */
    resp.d_name[sizeof(resp.d_name) - 1] = '\0';
    size_t copy_len = strlen(resp.d_name) + 1U;

    if (copy_len > name_len) {
        copy_len = name_len;
    }

    memcpy(d_name, resp.d_name, copy_len);
    d_name[name_len - 1] = '\0';
    *d_type = resp.d_type;
    return 0;
}

int rzv_remote_closedir(int dir_handle)
{
    struct fs_closedir_request {
        int32_t dir_handle;
    } req = {
        .dir_handle = dir_handle,
    };

    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_CLOSEDIR,
                         &req,
                         sizeof(req),
                         NULL,
                         0,
                         NULL,
                         &ret_val,
                         &err_val);

    if (rpc_ret < 0) {
        errno = (errno != 0) ? errno : EIO;
        return -1;
    }

    return (err_val != 0) ? -1 : 0;
}

int rzv_remote_clock_settime(int64_t unix_sec)
{
    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_CA55_CLOCK_SETTIME,
                         &unix_sec, sizeof(unix_sec),
                         NULL, 0, NULL,
                         &ret_val, &err_val);

    if (rpc_ret < 0) {
        return -1;
    }

    return ret_val;
}

int rzv_remote_system_reboot(void)
{
    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_CA55_SYSTEM_REBOOT,
                         NULL, 0,
                         NULL, 0, NULL,
                         &ret_val, &err_val);

    if (rpc_ret < 0) {
        return -1;
    }

    return ret_val;
}

/**
 * @brief Open a new log session on CA55.
 *
 * Replaces the N-probe mkdir loop in logger create_log_dir(): CA55 scans log directories,
 * creates the next sessNNN/ subdirectory, and returns the full file path that CR8 should
 * subsequently open via a normal RPC FS_OPEN call.
 *
 * Called once per ARM cycle (from logger::start_log_file). CA55 must be ready before this
 * is called (ca55Check.cpp already ensures this via rzv_ca55_storage_ready()).
 *
 * @param timestamp_us  ARM timestamp in microseconds (used for date-based subdir naming)
 * @param path_out      Output buffer for the full virtual file path
 *                      (e.g. /fs/microsd/log/sess001/19_37_52.ulg)
 * @param path_size     Size of path_out buffer (must be >= 256)
 * @return 0 on success, -errno on failure
 */
int rzv_remote_fs_session_open(uint64_t timestamp_us, char *path_out, size_t path_size)
{
    char resp_buf[256];
    size_t resp_len = 0;
    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_SESSION_OPEN,
                         &timestamp_us, sizeof(timestamp_us),
                         resp_buf, sizeof(resp_buf), &resp_len,
                         &ret_val, &err_val);

    if (rpc_ret < 0) {
        return -EIO;
    }

    if (ret_val < 0) {
        errno = err_val;
        return ret_val;
    }

    /* CA55 returns the log file path as a null-terminated string in the response buffer */
    if (resp_len > 0 && path_out != NULL) {
        size_t copy = (resp_len < path_size) ? resp_len : path_size - 1;
        memcpy(path_out, resp_buf, copy);
        path_out[copy] = '\0';
    }

    return 0;
}

/**
 * @brief Close the current log session and trigger logdata.txt rebuild.
 *
 * CA55 flushes any buffered stream data for the fd, closes the file, and
 * rebuilds /fs/microsd/logdata.txt so QGC can list the new log without a
 * full directory rescan.
 *
 * Called once per ARM cycle (from logger::stop_log_file), before the writer
 * closes the fd. The fd remains valid until _writer.stop_log_file() is called
 * immediately after this function returns.
 *
 * @param fd  CA55 file descriptor (as returned by the FS_OPEN RPC after SESSION_OPEN)
 * @return 0 on success, -errno on failure
 */
int rzv_remote_fs_session_close(int32_t fd)
{
    int32_t ret_val = -1;
    int32_t err_val = 0;

    int rpc_ret = fs_rpc(PX4_RPC_FS_SESSION_CLOSE,
                         &fd, sizeof(fd),
                         NULL, 0, NULL,
                         &ret_val, &err_val);

    if (rpc_ret < 0) {
        return -EIO;
    }

    if (ret_val < 0) {
        errno = err_val;
    }

    return ret_val;
}
