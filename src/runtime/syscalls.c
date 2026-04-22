/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file syscalls.c
 * @brief System call stubs for embedded systems
 * 
 * This file provides minimal implementations of system calls required by newlib
 * for embedded systems that don't have a full operating system.
 */

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <reent.h>
#ifndef errno
extern int errno;
#endif
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <stddef.h>

#if defined(__PX4_FREERTOS)
#include "FreeRTOS.h"
#endif

#if defined(__PX4_FREERTOS)
void mavlink_shell_stdout_hook(const char *buffer, size_t len) __attribute__((weak));
bool mavlink_shell_stdout_active(void) __attribute__((weak));
#endif

// UART bridge functions provided by the FreeRTOS/FSP layer
int uart_open_channel(uint8_t channel, uint32_t baudrate, bool mode_8N1);
int uart_set_baudrate_channel(uint8_t channel, uint32_t baudrate);
int uart_close_channel(uint8_t channel);
void uart_set_rc_serial_mode(bool use_8n1);
int uart_set_serial_mode_channel(uint8_t channel, bool mode_8n1);

#include "rzv_fsp/uart_channel_map.h"
#include "rzv_fsp/uart_transport.h"

#include "SEGGER_RTT.h"

#define UART_FD_BASE            200
#define UART_MAX_CHANNELS       4
#define UART_SUPPORTED_CHANNELS 4

/*
 * Forward declarations for RPC-based file I/O (px4_open routes to CA55 via OpenAMP).
 * These back ::open / ::read / ::close / ::lseek / stat() so that upstream PX4 code
 * that uses the standard C library (e.g. mavlink_ftp.cpp) can access files on CA55
 * without needing #ifdef __PX4_FREERTOS guards everywhere.
 */
#include <fcntl.h>
#include <stdio.h>   /* SEEK_END, SEEK_SET, SEEK_CUR */
#include "services/embedded_metadata.h"
extern int     px4_open(const char *path, int flags, ...);
extern int     px4_close(int fd);
extern ssize_t px4_read(int fd, void *buf, size_t count);
extern ssize_t px4_write(int fd, const void *buf, size_t count);
extern off_t   px4_lseek(int fd, off_t offset, int whence);
extern int     rzv_remote_fs_rename_impl(const char *old_path, const char *new_path);
extern int     rzv_remote_fs_stat_impl(const char *path, int32_t *out_mode, int64_t *out_size, int64_t *out_mtime);

/* Remote fds from px4_rzv_remote_open() are in the range [4000, 4064).
 * kRemoteFdBase=4000 is defined in remote_storage.cpp — mirror that value here.
 * UART fds start at UART_FD_BASE (200), so [3, UART_FD_BASE) covers any small
 * fds from px4_open (legacy path), and [4000, ...) covers remote CA55 fds. */
#define REMOTE_FD_BASE 4000
static inline bool is_px4_file_fd(int fd)
{
    return ((fd >= 3) && (fd < UART_FD_BASE)) || (fd >= REMOTE_FD_BASE);
}

typedef struct {
    int fd;
    uint16_t refcount;
    bool mode_8n1;
    uint32_t baudrate;
} uart_channel_state_t;

static uart_channel_state_t s_uart_channels[UART_MAX_CHANNELS] = { { -1, 0, true, 0 }, { -1, 0, true, 0 }, { -1, 0, true, 0 }, { -1, 0, true, 0 } };

static inline bool uart_fd_is_valid(int fd)
{
    int channel = fd - UART_FD_BASE;

    if (channel < 0 || channel >= UART_MAX_CHANNELS) {
        return false;
    }

    return (s_uart_channels[channel].refcount > 0);
}

static inline int uart_fd_to_channel(int fd)
{
    return fd - UART_FD_BASE;
}

static inline bool uart_channel_supported(uint8_t channel)
{
    return channel < UART_SUPPORTED_CHANNELS;
}

static int uart_path_to_channel(const char *name, bool *mode_8n1, uint32_t *default_baud)
{
	if (!name) {
		return -1;
	}

	const uart_channel_info_t *info = uart_channel_get_info_by_device(name);

	if (info == NULL) {
		return -1;
	}

	if (!uart_channel_supported(info->channel)) {
		return -1;
	}

	if (mode_8n1 != NULL) {
		*mode_8n1 = info->mode_8n1;
	}

	if (default_baud != NULL) {
		*default_baud = info->default_baud;
	}

	return (int)info->channel;
}

#include "FreeRTOS.h"
#include "task.h"

void vPortFree(void * pv);
void *pvPortMalloc(size_t xWantedSize);
void *pvPortRealloc(void *pv, size_t xWantedSize);
void *pvPortCalloc(size_t num, size_t xWantedSize);

/* Assumes 8bit bytes! */
#define heapBITS_PER_BYTE         ( ( size_t ) 8 )

/* Define the linked list structure.  This is used to link free blocks in order
 * of their memory address. */
typedef struct A_BLOCK_LINK
{
    struct A_BLOCK_LINK * pxNextFreeBlock; /*<< The next free block in the list. */
    size_t xBlockSize;                     /*<< The size of the free block. */
} BlockLink_t;

static const size_t xHeapStructSize = ( sizeof( BlockLink_t ) + ( ( size_t ) ( portBYTE_ALIGNMENT - 1 ) ) ) & ~( ( size_t ) portBYTE_ALIGNMENT_MASK );
static size_t  xBlockAllocatedBit = ( ( size_t ) 1 ) << ( ( sizeof( size_t ) * heapBITS_PER_BYTE ) - 1 );

static bool prvHeapBlockIsValid(const BlockLink_t *pxLink, size_t *usable_size_out)
{
	if (pxLink == NULL) {
		return false;
	}

	const size_t raw_size = pxLink->xBlockSize;

	if ((raw_size & xBlockAllocatedBit) == 0U) {
		return false;
	}

	const size_t block_size = raw_size & ~xBlockAllocatedBit;

	if (block_size <= xHeapStructSize) {
		return false;
	}

#if defined(configTOTAL_HEAP_SIZE)
	if (block_size > configTOTAL_HEAP_SIZE) {
		return false;
	}
#endif

	if (usable_size_out != NULL) {
		*usable_size_out = block_size - xHeapStructSize;
	}

	return true;
}

#ifdef CONFIG_SYSCALL_DEBUG_VERBOSE
#define SYSCALL_DBG(fmt, ...) \
	SEGGER_RTT_printf(0, "[SYSCALL] " fmt "\r\n", ##__VA_ARGS__)
#else
#define SYSCALL_DBG(fmt, ...) do { (void)sizeof(fmt); } while (0)
#endif

void *pvPortRealloc(void *pointer, size_t xWantedSize)
{
	void *result = NULL;

	vTaskSuspendAll();

	if (xWantedSize == 0) {
		vPortFree(pointer);
		(void)xTaskResumeAll();
		return NULL;
	}

	result = pvPortMalloc(xWantedSize);

	if ((result != NULL) && (pointer != NULL)) {
		size_t copy_bytes = 0U;
		bool can_copy = false;
		const uintptr_t pointer_value = (uintptr_t)pointer;

		if (pointer_value >= xHeapStructSize) {
			BlockLink_t *pxLink = (BlockLink_t *)(pointer_value - xHeapStructSize);
			can_copy = prvHeapBlockIsValid(pxLink, &copy_bytes);
		}

		if (can_copy && (copy_bytes > 0U)) {
			if (xWantedSize < copy_bytes) {
				copy_bytes = xWantedSize;
			}

			memcpy(result, pointer, copy_bytes);
			vPortFree(pointer);
		}
	}

	(void)xTaskResumeAll();
	return result;
}
/* 
void *pvPortCalloc( size_t num, size_t xWantedSize )
{
    vTaskSuspendAll();

    void *pointer = pvPortMalloc(num * xWantedSize);

    if (pointer)
    {
        // Zero the memory
        memset(pointer, 0, num * xWantedSize);
    }

  (void) xTaskResumeAll();
  return pointer;
}
 */
void *malloc(size_t xSize) {
  return pvPortMalloc(xSize);
}

void free(void *pv) {
  vPortFree(pv);
}

void *calloc(size_t xNum, size_t xSize)
{
	size_t total = 0U;

	if ((xNum != 0U) && (xSize > (SIZE_MAX / xNum))) {
		errno = ENOMEM;
		return NULL;
	}

	total = xNum * xSize;

	if (total == 0U) {
		/* C standard allows returning NULL for zero-sized allocation. */
		return NULL;
	}

	void *pv = pvPortMalloc(total);

	if (pv != NULL) {
		memset(pv, 0, total);
		return pv;
	}

	errno = ENOMEM;
	return NULL;
}

void *realloc(void *pv, size_t xSize)
{
	void *result = pvPortRealloc(pv, xSize);

	if ((result == NULL) && (xSize != 0U)) {
		errno = ENOMEM;
	}

	return result;
}

void *_malloc_r(struct _reent *r, size_t size)
{
    (void)r;
    return pvPortMalloc(size);
}

void _free_r(struct _reent *r, void *ptr)
{
    (void)r;
    vPortFree(ptr);
}

void *_calloc_r(struct _reent *r, size_t num, size_t size)
{
    errno = 0;
    void *ptr = calloc(num, size);

    if ((ptr == NULL) && (r != NULL)) {
        r->_errno = errno;
    }

    return ptr;
}

void *_realloc_r(struct _reent *r, void *ptr, size_t size)
{
    errno = 0;
    void *result = realloc(ptr, size);

    if ((result == NULL) && (size != 0U) && (r != NULL)) {
        r->_errno = errno;
    }

    return result;
}
// Minimal implementations of system calls required by newlib

int _close(int file) {
    if (is_px4_file_fd(file)) {
        return px4_close(file);
    }

    if (!uart_fd_is_valid(file)) {
        errno = EBADF;
        return -1;
    }

    int channel = uart_fd_to_channel(file);
    uart_channel_state_t *state = &s_uart_channels[channel];

    if (state->refcount > 0U) {
        state->refcount--;

        if (state->refcount == 0U) {
            (void)uart_close_channel((uint8_t)channel);
            state->fd = -1;
            state->baudrate = 0U;
            state->mode_8n1 = true;
        }
    }

    return 0;
}

int _fstat(int file, struct stat *st) {
    if (!st) {
        errno = EINVAL;
        return -1;
    }

    memset(st, 0, sizeof(*st));

    /* Embedded metadata files (fd 5000-5015): return actual ROM file size.
     * Without this, logger's write_info_multiple(fd) can read wrong chunk
     * lengths and emit "read failed (403 88)" on arm. */
    if (rzv_embedded_metadata_is_fd(file)) {
        off_t sz = rzv_embedded_metadata_size(file);
        if (sz >= 0) {
            st->st_mode = S_IFREG | 0444;
            st->st_size = sz;
            return 0;
        }
        errno = EBADF;
        return -1;
    }

    /* Generic file-descriptor path (remote/local VFS): try deriving size by
     * preserving current offset, seeking to end, then restoring offset. */
    if (is_px4_file_fd(file)) {
        off_t cur = px4_lseek(file, 0, SEEK_CUR);

        if (cur >= 0) {
            off_t end = px4_lseek(file, 0, SEEK_END);

            if (end >= 0) {
                (void)px4_lseek(file, cur, SEEK_SET);
                st->st_mode = S_IFREG | 0666;
                st->st_size = end;
                return 0;
            }

            (void)px4_lseek(file, cur, SEEK_SET);
        }
    }

    /* Default: character device (UART, console, etc.) */
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file) {
    (void)file;
    return 1;
}

int _lseek(int file, int ptr, int dir) {
    if (is_px4_file_fd(file)) {
        return (int)px4_lseek(file, (off_t)ptr, dir);
    }
    return 0;
}

int _open(const char *name, int flags, int mode) {
    (void)flags;
    (void)mode;

    SYSCALL_DBG("_open() path='%s' flags=0x%X mode=0x%X",
                name ? name : "(null)", flags, mode);

    bool mode_8n1 = true;
    uint32_t default_baud = 0;
    int channel = uart_path_to_channel(name, &mode_8n1, &default_baud);

    if (channel < 0) {
        /* Not a UART device path — try the RPC-based remote filesystem on CA55 */
        int fd = px4_open(name, flags, (mode_t)mode);
        if (fd < 0) {
            SYSCALL_DBG("_open(): px4_open('%s') failed (errno=%d)", name ? name : "(null)", errno);
        }
        return fd;
    }

    if (!uart_channel_supported((uint8_t)channel)) {
        SYSCALL_DBG("_open(): channel %d unsupported", channel);
        errno = ENODEV;
        return -1;
    }

    SYSCALL_DBG("_open(): ch=%d baud=%lu 8N1=%d",
                channel, (unsigned long)default_baud, mode_8n1 ? 1 : 0);

    uart_channel_state_t *state = &s_uart_channels[channel];

    if (state->refcount > 0U) {
        SYSCALL_DBG("_open(): channel %d already open (fd=%d, ref=%u)",
                    channel, UART_FD_BASE + channel, (unsigned)state->refcount);

        if ((channel == 0) && (mode_8n1 != state->mode_8n1)) {
            uart_set_rc_serial_mode(mode_8n1);
        }

        if (mode_8n1 != state->mode_8n1) {
            (void)uart_set_serial_mode_channel((uint8_t)channel, mode_8n1);
            state->mode_8n1 = mode_8n1;
        }

        if ((default_baud != 0U) && (default_baud != state->baudrate)) {
            if (uart_set_baudrate_channel((uint8_t)channel, default_baud) == 0) {
                state->baudrate = default_baud;
            }
        }

        state->refcount++;
        return state->fd;
    }

    if (channel == 0) {
        uart_set_rc_serial_mode(mode_8n1);
        SYSCALL_DBG("_open(): configured RC serial mode for channel 0");
    }

    if (uart_open_channel((uint8_t)channel, default_baud, mode_8n1) != 0) {
        SYSCALL_DBG("_open(): uart_open_channel failed for channel %d", channel);
        errno = EIO;
        return -1;
    }

    state->fd = UART_FD_BASE + channel;
    state->refcount = 1;
    state->mode_8n1 = mode_8n1;
    state->baudrate = default_baud;

    SYSCALL_DBG("_open(): returning fd %d", state->fd);
    return state->fd;
}

int _access(const char *name, int mode)
{
	(void)mode;

	if (name == NULL) {
		errno = EINVAL;
		return -1;
	}

	bool mode_8n1 = true;
	uint32_t baud = 0;

	if (uart_path_to_channel(name, &mode_8n1, &baud) >= 0) {
		return 0;
	}

	errno = ENOENT;
	return -1;
}


int _read(int file, char *ptr, int len) {
    if (is_px4_file_fd(file)) {
        if (!ptr || len <= 0) { errno = EINVAL; return -1; }
        /*
         * RPC-backed reads (px4_read → CA55 via OpenAMP) may return fewer
         * bytes than requested per call because RPMsg frames have a finite
         * payload size (~496 bytes per packet).  Callers such as the PX4
         * logger embed large files (e.g. all_events.json.xz ~15 KB) using
         * write_info_multiple(fd) which treats any ret != read_length as a
         * hard error ("read failed (%i %i)").
         *
         * Wrap px4_read in a retry loop so short reads are transparent to
         * the caller.  We stop when:
         *   - all bytes have been received (ret_total == len), or
         *   - px4_read returns 0 (EOF), or
         *   - px4_read returns -1 (error).
         */
        int ret_total = 0;
        while (ret_total < len) {
            ssize_t ret = px4_read(file, ptr + ret_total, (size_t)(len - ret_total));
            if (ret < 0) {
                /* Real error — propagate it.  errno already set by px4_read. */
                return (ret_total > 0) ? ret_total : -1;
            }
            if (ret == 0) {
                /* EOF */
                break;
            }
            ret_total += (int)ret;
        }
        return ret_total;
    }

    if (!uart_fd_is_valid(file) || !ptr || len <= 0) {
        errno = EBADF;
        return -1;
    }

    int channel = uart_fd_to_channel(file);
    if (!uart_channel_supported((uint8_t)channel)) {
        errno = EBADF;
        return -1;
    }

    /* Channel-aware policy in transport layer:
     * - RC (ch0): low-latency ring-buffer fast path
     * - Others: backend read path (arms/restarts RX in task context)
     */
    int ret = rzv_uart_transport_read_with_policy((uint8_t)channel, (uint8_t *)ptr, len);

#if defined(__PX4_FREERTOS)
    if (channel == 0) {
        static unsigned warn_counter = 0U;

        if (ret <= 0) {
            if (warn_counter < 20U) {
                uint32_t rx = 0, overflow = 0, isr_writes = 0, app_reads = 0;
                uint32_t widx = 0, ridx = 0;
                rzv_uart_transport_get_extended_stats(0U, &rx, &overflow, &isr_writes, &app_reads);
                if (rzv_uart_transport_get_indices(0U, &widx, &ridx) != 0) {
                    widx = 0;
                    ridx = 0;
                }

                SEGGER_RTT_printf(0, "[syscalls] _read ch0 len=%d ret=%d errno=%d rx=%u overflow=%u isr=%u app=%u widx=%u ridx=%u\r\n",
                                  len,
                                  ret,
                                  errno,
                                  (unsigned)rx,
                                  (unsigned)overflow,
                                  (unsigned)isr_writes,
                                  (unsigned)app_reads,
                                  (unsigned)widx,
                                  (unsigned)ridx);
                warn_counter++;
            }

        } else {
            warn_counter = 0U;
        }
    }
#endif

    if (ret < 0) {
        errno = EIO;
        return -1;
    }

    return ret;
}

int _write(int file, char *ptr, int len) {
    if (!ptr || len < 0) {
        errno = EBADF;
        return -1;
    }

    if ((file == 1) || (file == 2)) {
        int written = (len > 0) ? (int)SEGGER_RTT_Write(0, ptr, (unsigned)len) : 0;

#if defined(__PX4_FREERTOS)
        bool shell_forwarded = false;

        if ((len > 0) && (mavlink_shell_stdout_hook != NULL)) {
            bool shell_active = false;

            if (mavlink_shell_stdout_active != NULL) {
                shell_active = mavlink_shell_stdout_active();
            }

            if (shell_active) {
                mavlink_shell_stdout_hook(ptr, (size_t)len);
                shell_forwarded = true;
            }
        }

        if (shell_forwarded) {
            return len;
        }
#endif

        if (written < 0) {
            errno = EIO;
            return -1;
        }

        return written;
    }

    if (is_px4_file_fd(file)) {
        if (!ptr || len <= 0) { errno = EINVAL; return -1; }
        return (int)px4_write(file, ptr, (size_t)len);
    }

    if (!uart_fd_is_valid(file)) {
        errno = EBADF;
    	return -1;
    }

    int channel = uart_fd_to_channel(file);
    int ret = rzv_uart_transport_write_channel((uint8_t)channel, (const uint8_t *)ptr, (unsigned)len);

    if (ret < 0) {
        errno = EIO;
        return -1;
    }

    return ret;
}

void _exit(int status) {
    (void)status;
    while(1) {
        // Infinite loop - no way to exit in embedded system
    }
}

// Additional stubs that might be needed
// Note: _sbrk is already implemented in BSP layer (bsp_sbrk.c)

int _kill(int pid, int sig) {
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

int _getpid(void) {
    return 1;
}

/* Provide minimal implementations for functions Newlib may reference in
 * the C library so the linker doesn't pull in stubs that warn at link time.
 * These implementations intentionally return sensible errors for an
 * embedded read-only / limited environment. */

int _link(const char *oldpath, const char *newpath)
{
    (void)oldpath;
    (void)newpath;
    errno = ENOSYS; /* Function not implemented */
    return -1;
}

int _link_r(struct _reent *r, const char *oldpath, const char *newpath)
{
    if (r) r->_errno = ENOSYS;
    (void)oldpath;
    (void)newpath;
    return -1;
}

int _stat(const char *file, struct stat *st)
{
    if (!file || !st) {
        errno = EINVAL;
        return -1;
    }

    /* Treat known serial devices as character devices */
    if (strncmp(file, "/dev/ttyS", 9) == 0) {
        st->st_mode = S_IFCHR;
        return 0;
    }

    /* For remote filesystem paths, query via RPC to get the actual file info.
     * Uses PX4_RPC_FS_STAT to obtain mode, size, and mtime from CA55.
     * mtime is needed by mavlink_log_handler to show correct date in QGC Log Download. */
    if (strncmp(file, "/fs/", 4) == 0) {
        int32_t rpc_mode = 0;
        int64_t rpc_size = 0;
        int64_t rpc_mtime = 0;
        int ret = rzv_remote_fs_stat_impl(file, &rpc_mode, &rpc_size, &rpc_mtime);
        if (ret < 0) {
            errno = ENOENT;
            return -1;
        }
        st->st_mode  = (mode_t)rpc_mode;
        st->st_size  = (off_t)rpc_size;
        st->st_mtime = (time_t)rpc_mtime;
        return 0;
    }

    /* Default to regular file with zero size for unknown paths. */
    st->st_mode = S_IFREG;
    st->st_size = 0;
    return 0;
}

int _stat_r(struct _reent *r, const char *file, struct stat *st)
{
    if (r) r->_errno = 0;
    int ret = _stat(file, st);
    if (ret < 0 && r) r->_errno = errno;
    return ret;
}

int _unlink(const char *name)
{
    /* Most embedded filesystems are read-only; indicate operation
     * is not permitted. If the target is a known UART path, return ENOENT.
     */
    if (!name) {
        errno = EINVAL;
        return -1;
    }

    if (strncmp(name, "/dev/", 5) == 0) {
        errno = ENOENT;
        return -1;
    }

    errno = EROFS; /* Read-only filesystem */
    return -1;
}

int _unlink_r(struct _reent *r, const char *name)
{
    if (r) r->_errno = 0;
    int ret = _unlink(name);
    if (ret < 0 && r) r->_errno = errno;
    return ret;
}

/* Rename: route /fs/ paths through RPC to CA55.
 * Used by mavlink_log_handler to atomically commit $log$.txt → logdata.txt. */
int _rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) {
        errno = EINVAL;
        return -1;
    }

    if (strncmp(old_path, "/fs/", 4) == 0 || strncmp(new_path, "/fs/", 4) == 0) {
        int ret = rzv_remote_fs_rename_impl(old_path, new_path);
        if (ret < 0) {
            errno = -ret;
            return -1;
        }
        return 0;
    }

    errno = EROFS;
    return -1;
}

int _rename_r(struct _reent *r, const char *old_path, const char *new_path)
{
    if (r) r->_errno = 0;
    int ret = _rename(old_path, new_path);
    if (ret < 0 && r) r->_errno = errno;
    return ret;
}

/*
 * UART RC function stubs
 * 
 * These functions were previously provided by uart_rc_switch.c for dynamic
 * protocol switching (SBUS/DSM/CRSF/GHST). Since hardware is fixed to FS-A8S
 * (SBUS only), UART configuration is now fixed in FSP (100kHz, 8E2).
 * 
 * Stubs provided for API compatibility with PX4 RC drivers.
 */

/**
 * @brief Set RC serial mode (8E2 vs 8N1)
 * @note Stub - SBUS format (8E2) is fixed in FSP configuration
 */
void uart_set_rc_serial_mode(bool use_8n1)
{
    if (uart_set_serial_mode_channel(0U, use_8n1) != 0) {
        SYSCALL_DBG("uart_set_rc_serial_mode: failed (mode=%d errno=%d)", use_8n1 ? 1 : 0, errno);
    }
}

/**
 * @brief Reopen RC channel with new baudrate
 * @note Reconfigures logical channel 0 through the UART backend
 * @return 0 on success, -1 with errno set on failure
 */
int uart_reopen_rc_channel(uint32_t baudrate)
{
    const uint32_t target_baud = (baudrate != 0U) ? baudrate : 100000U;

    if (uart_set_baudrate_channel(0U, target_baud) != 0) {
        return -1;
    }

    return 0;
}
