/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "openamp_rpc_client.h"

#include "openamp_rpc.h"

#include "openamp/open_amp.h"
#include "openamp/rpmsg.h"
#include "openamp/remoteproc.h"
#include "openamp/remoteproc_virtio.h"
#include <metal/utilities.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern struct rpmsg_endpoint rpmsg_ept;

// Global OpenAMP transport lock to serialize RPC and UXRCE-DDS operations
extern void px4_openamp_transport_lock(void);
extern void px4_openamp_transport_unlock(void);

unsigned int is_rpmsg_ept_ready(struct rpmsg_endpoint *ept);

#define MAX_PENDING_RPC 32

struct rpc_pending {
    int in_use;
    int completed;
    int32_t request_id;
    int32_t return_value;
    int32_t errno_value;
    void *response_buffer;
    size_t response_capacity;
    size_t response_length;
    size_t *response_length_out;
    px4_openamp_rpc_response_cb_t callback;
    void *user_data;
    pthread_cond_t cond;
};

static pthread_mutex_t g_rpc_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rpc_pending g_rpc_pending[MAX_PENDING_RPC];
static int g_rpc_initialized = 0;
static int32_t g_rpc_next_request_id = 1;
static volatile int g_rpc_channel_ready = 0;
static volatile uint32_t g_rpc_no_buf_count = 0;

/* Poll rpmsg/remoteproc notifications when IRQ kicks may be missing. */
static void poll_remoteproc_notifications(void)
{
    struct rpmsg_device *rdev = rpmsg_ept.rdev;

    if (!rdev) {
        return;
    }

    /* rpmsg_device is the first member of rpmsg_virtio_device */
    struct rpmsg_virtio_device *rvdev = metal_container_of(rdev, struct rpmsg_virtio_device, rdev);
    if (!rvdev || rvdev->vdev == NULL) {
        return;
    }

    /* virtio_device is embedded in remoteproc_virtio */
    struct remoteproc_virtio *rpvdev = metal_container_of(rvdev->vdev, struct remoteproc_virtio, vdev);
    if (!rpvdev || rpvdev->priv == NULL) {
        return;
    }

    remoteproc_get_notification((struct remoteproc *)rpvdev->priv, RSC_NOTIFY_ID_ANY);
}

static void px4_openamp_rpc_init_locked(void)
{
    if (g_rpc_initialized) {
        return;
    }

    for (size_t i = 0; i < MAX_PENDING_RPC; ++i) {
        pthread_cond_init(&g_rpc_pending[i].cond, NULL);
    }

    g_rpc_initialized = 1;
}

static void release_slot_locked(struct rpc_pending *slot)
{
    slot->in_use = 0;
    slot->completed = 0;
    slot->request_id = 0;
    slot->return_value = 0;
    slot->errno_value = 0;
    slot->response_buffer = NULL;
    slot->response_capacity = 0;
    slot->response_length = 0;
    slot->response_length_out = NULL;
    slot->callback = NULL;
    slot->user_data = NULL;
}

static struct rpc_pending *alloc_pending(void *response_buffer,
                                         size_t response_capacity,
                                         size_t *response_length_out,
                                         px4_openamp_rpc_response_cb_t callback,
                                         void *user_data,
                                         int32_t *out_request_id)
{
    struct rpc_pending *slot = NULL;

    pthread_mutex_lock(&g_rpc_lock);
    px4_openamp_rpc_init_locked();

    for (size_t i = 0; i < MAX_PENDING_RPC; ++i) {
        if (!g_rpc_pending[i].in_use) {
            slot = &g_rpc_pending[i];
            slot->in_use = 1;
            slot->completed = 0;
            slot->response_buffer = response_buffer;
            slot->response_capacity = response_capacity;
            slot->response_length = 0;
            slot->response_length_out = response_length_out;
            slot->return_value = 0;
            slot->errno_value = 0;
            slot->callback = callback;
            slot->user_data = user_data;

            int32_t request_id = __atomic_fetch_add(&g_rpc_next_request_id, 1, __ATOMIC_RELAXED);

            if (request_id == INT32_MIN) {
                request_id = __atomic_fetch_add(&g_rpc_next_request_id, 1, __ATOMIC_RELAXED);
            }

            slot->request_id = request_id;

            if (out_request_id) {
                *out_request_id = request_id;
            }

            break;
        }
    }

    pthread_mutex_unlock(&g_rpc_lock);
    return slot;
}

bool px4_openamp_rpc_endpoint_ready(void)
{
    if (rpmsg_ept.rdev == NULL) {
        return false;
    }

    if (rpmsg_ept.dest_addr == RPMSG_ADDR_ANY) {
        return false;
    }

    return is_rpmsg_ept_ready(&rpmsg_ept) != 0;
}

int px4_openamp_rpc_call_timeout(uint16_t category,
                                 uint16_t opcode,
                                 const void *request_payload,
                                 size_t request_len,
                                 void *response_buffer,
                                 size_t response_capacity,
                                 size_t *response_len,
                                 int32_t *return_value,
                                 int32_t *errno_value,
                                 uint32_t timeout_ms)
{
    if (!px4_openamp_rpc_endpoint_ready()) {
        errno = ENOTCONN;
        return -1;
    }

    if (request_len > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    struct rpc_pending *slot = NULL;
    int32_t request_id = 0;

    slot = alloc_pending(response_buffer, response_capacity, response_len, NULL, NULL, &request_id);

    if (!slot) {
        errno = EBUSY;
        return -1;
    }

    const size_t total_len = sizeof(struct px4_rpc_header) + request_len;
    uint8_t *buffer = (uint8_t *)malloc(total_len);

    if (!buffer) {
        pthread_mutex_lock(&g_rpc_lock);
        release_slot_locked(slot);
        pthread_mutex_unlock(&g_rpc_lock);
        errno = ENOMEM;
        return -1;
    }

    struct px4_rpc_header header;
    header.magic = PX4_RPC_MAGIC;
    header.category = category;
    header.opcode = opcode;
    header.request_id = request_id;
    header.return_value = 0;
    header.errno_value = 0;
    header.payload_size = (uint32_t)request_len;

    memcpy(buffer, &header, sizeof(header));

    if (request_len > 0 && request_payload) {
        memcpy(buffer + sizeof(header), request_payload, request_len);
    }

    // Acquire transport lock to prevent RPC/UXRCE-DDS contention for shared virtio device
    px4_openamp_transport_lock();
    int send_result = rpmsg_trysend(&rpmsg_ept, buffer, total_len);
    px4_openamp_transport_unlock();

    if (send_result < 0 || rpmsg_ept.dest_addr == RPMSG_ADDR_ANY) {
        fprintf(stderr,
                "[RPC][SEND] request_id=%ld cat=%u op=%u dest=0x%lx len=%zu ret=%d errno=%d\n",
                (long)request_id,
                category,
                opcode,
                (unsigned long)rpmsg_ept.dest_addr,
                total_len,
                send_result,
                errno);
        if (send_result == RPMSG_ERR_NO_BUFF) {
            __atomic_fetch_add(&g_rpc_no_buf_count, 1u, __ATOMIC_RELAXED);
        }
    }

    free(buffer);

    if (send_result < 0) {
        pthread_mutex_lock(&g_rpc_lock);
        release_slot_locked(slot);
        pthread_mutex_unlock(&g_rpc_lock);
        if (send_result == RPMSG_ERR_NO_BUFF) {
            errno = EAGAIN;
        } else {
            errno = EIO;
        }
        return -1;
    }

    pthread_mutex_lock(&g_rpc_lock);

    /* Poll-driven wait: combine timed wait slices with manual remoteproc polling */
    const uint32_t poll_slice_ms = 1; /* poll every 1ms for faster response */
    int wait_result = 0;
    uint32_t poll_count = 0;

    /* Special path: infinite timeout uses blocking wait + polling to avoid timedwait EINVAL */
    if (timeout_ms == PX4_OPENAMP_RPC_TIMEOUT_FOREVER) {
        while (!slot->completed) {
            pthread_mutex_unlock(&g_rpc_lock);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            poll_remoteproc_notifications();
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            pthread_mutex_lock(&g_rpc_lock);

            if (slot->completed) {
                break;
            }

            pthread_cond_wait(&slot->cond, &g_rpc_lock);
        }
    } else {
        struct timespec start_ts;
        clock_gettime(CLOCK_MONOTONIC, &start_ts);

        while (!slot->completed) {
            /* Check total timeout using monotonic clock */
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);

            uint64_t now_ms = (uint64_t)now_ts.tv_sec * 1000ULL + (uint64_t)now_ts.tv_nsec / 1000000ULL;
            uint64_t start_ms = (uint64_t)start_ts.tv_sec * 1000ULL + (uint64_t)start_ts.tv_nsec / 1000000ULL;
            uint64_t elapsed_ms = (now_ms > start_ms) ? (now_ms - start_ms) : 0;

            if (elapsed_ms >= timeout_ms) {
                wait_result = -ETIMEDOUT;
                break;
            }

            /* Poll remoteproc BEFORE waiting on condition variable to catch any pending messages */
            pthread_mutex_unlock(&g_rpc_lock);
            __atomic_thread_fence(__ATOMIC_SEQ_CST); /* Memory barrier to ensure cache coherency */
            poll_remoteproc_notifications();
            __atomic_thread_fence(__ATOMIC_SEQ_CST); /* Memory barrier after polling */
            pthread_mutex_lock(&g_rpc_lock);

            /* Check again after polling - response might have arrived */
            if (slot->completed) {
                break;
            }

            poll_count++;

            /* Compute per-slice deadline using the correct clock for pthread_cond_timedwait */
            struct timespec slice_deadline;
#if defined(__PX4_FREERTOS)
            if (clock_gettime(CLOCK_MONOTONIC, &slice_deadline) != 0) {
#else
            if (clock_gettime(CLOCK_REALTIME, &slice_deadline) != 0) {
#endif
                wait_result = -errno;
                break;
            }

            uint64_t slice_ns = (uint64_t)slice_deadline.tv_nsec + (uint64_t)poll_slice_ms * 1000000ULL;
            slice_deadline.tv_sec += (time_t)(slice_ns / 1000000000ULL);
            slice_deadline.tv_nsec = (long)(slice_ns % 1000000000ULL);

            int cond_ret = pthread_cond_timedwait(&slot->cond, &g_rpc_lock, &slice_deadline);

            if (slot->completed) {
                break;
            }

            if (cond_ret != 0 && cond_ret != ETIMEDOUT) {
                wait_result = -cond_ret;
                break;
            }
        }
    }

    if (wait_result != 0 && !slot->completed) {
        bool timeout = (wait_result == -ETIMEDOUT);
        fprintf(stderr,
                "[RPC][WAIT] req_id=%ld cat=%u op=%u timeout=%s wait_result=%d errno=%d\n",
                (long)slot->request_id,
                category,
                opcode,
                timeout ? "yes" : "no",
                wait_result,
                errno);
        release_slot_locked(slot);
        pthread_mutex_unlock(&g_rpc_lock);
        errno = timeout ? ETIMEDOUT : (wait_result < 0 ? -wait_result : EIO);
        return -1;
    }

    if (return_value) {
        *return_value = slot->return_value;
    }

    if (errno_value) {
        *errno_value = slot->errno_value;
    }

    if (response_len) {
        *response_len = slot->response_length;
    }

    release_slot_locked(slot);
    pthread_mutex_unlock(&g_rpc_lock);

    return 0;
}

static void rpc_noop_callback(int32_t request_id,
                              int32_t return_value,
                              int32_t errno_value,
                              const void *payload,
                              size_t payload_len,
                              void *user_data)
{
    (void)request_id;
    (void)return_value;
    (void)errno_value;
    (void)payload;
    (void)payload_len;
    (void)user_data;
}

int px4_openamp_rpc_call_async(uint16_t category,
                               uint16_t opcode,
                               const void *request_payload,
                               size_t request_len,
                               void *response_buffer,
                               size_t response_capacity,
                               px4_openamp_rpc_response_cb_t callback,
                               void *user_data,
                               int32_t *out_request_id)
{
    if (!px4_openamp_rpc_endpoint_ready()) {
        errno = ENOTCONN;
        return -1;
    }

    if (request_len > UINT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    if (callback == NULL) {
        callback = rpc_noop_callback;
    }

    struct rpc_pending *slot = NULL;
    int32_t request_id = 0;

    slot = alloc_pending(response_buffer, response_capacity, NULL, callback, user_data, &request_id);

    if (!slot) {
        errno = EBUSY;
        return -1;
    }

    const size_t total_len = sizeof(struct px4_rpc_header) + request_len;
    uint8_t *buffer = (uint8_t *)malloc(total_len);

    if (!buffer) {
        pthread_mutex_lock(&g_rpc_lock);
        release_slot_locked(slot);
        pthread_mutex_unlock(&g_rpc_lock);
        errno = ENOMEM;
        return -1;
    }

    struct px4_rpc_header header;
    header.magic = PX4_RPC_MAGIC;
    header.category = category;
    header.opcode = opcode;
    header.request_id = request_id;
    header.return_value = 0;
    header.errno_value = 0;
    header.payload_size = (uint32_t)request_len;

    memcpy(buffer, &header, sizeof(header));

    if (request_len > 0 && request_payload) {
        memcpy(buffer + sizeof(header), request_payload, request_len);
    }

    // Acquire transport lock to prevent RPC/UXRCE-DDS contention for shared virtio device.
    // Use rpmsg_trysend() (non-blocking) instead of rpmsg_send() (blocks up to 3s holding lock).
    // Holding the transport lock for 3 seconds would starve higher-priority tasks (e.g. logger
    // writer) that also need the lock for sync RPC calls.
    px4_openamp_transport_lock();
    int send_result = rpmsg_trysend(&rpmsg_ept, buffer, total_len);
    px4_openamp_transport_unlock();
    if (send_result == RPMSG_ERR_NO_BUFF) {
        /* TX vring buffer temporarily full. Release lock, wait 2ms for virtio to reclaim
         * used buffers, then retry once. This avoids holding the lock across the wait. */
        usleep(2000);
        px4_openamp_transport_lock();
        send_result = rpmsg_trysend(&rpmsg_ept, buffer, total_len);
        px4_openamp_transport_unlock();
    }

    if (send_result < 0 || rpmsg_ept.dest_addr == RPMSG_ADDR_ANY) {
        fprintf(stderr,
                "[RPC][SEND] request_id=%ld cat=%u op=%u dest=0x%lx len=%zu ret=%d errno=%d\n",
                (long)request_id,
                category,
                opcode,
                (unsigned long)rpmsg_ept.dest_addr,
                total_len,
                send_result,
                errno);
    }

    free(buffer);

    if (send_result < 0) {
        pthread_mutex_lock(&g_rpc_lock);
        release_slot_locked(slot);
        pthread_mutex_unlock(&g_rpc_lock);
        errno = EIO;
        return -1;
    }

    if (out_request_id) {
        *out_request_id = request_id;
    }

    return 0;
}

uint32_t px4_openamp_rpc_pending_count(void)
{
    uint32_t count = 0;
    pthread_mutex_lock(&g_rpc_lock);

    for (size_t i = 0; i < MAX_PENDING_RPC; ++i) {
        if (g_rpc_pending[i].in_use) {
            count++;
        }
    }

    pthread_mutex_unlock(&g_rpc_lock);
    return count;
}

uint32_t px4_openamp_rpc_no_buf_count(void)
{
    return __atomic_load_n(&g_rpc_no_buf_count, __ATOMIC_RELAXED);
}

int px4_openamp_rpc_call(uint16_t category,
                         uint16_t opcode,
                         const void *request_payload,
                         size_t request_len,
                         void *response_buffer,
                         size_t response_capacity,
                         size_t *response_len,
                         int32_t *return_value,
                         int32_t *errno_value)
{
    return px4_openamp_rpc_call_timeout(category,
                                        opcode,
                                        request_payload,
                                        request_len,
                                        response_buffer,
                                        response_capacity,
                                        response_len,
                                        return_value,
                                        errno_value,
                                        PX4_OPENAMP_RPC_TIMEOUT_FOREVER);
}

bool px4_openamp_rpc_handle_message(const void *data, size_t len)
{
    if (len < sizeof(struct px4_rpc_header)) {
        return false;
    }

    /* Memory barrier to ensure we read the latest data from shared memory */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    struct px4_rpc_header header;
    memcpy(&header, data, sizeof(header));

    if (header.magic != PX4_RPC_MAGIC) {
        return false;
    }

    g_rpc_channel_ready = 1;

    const size_t available_payload = len - sizeof(struct px4_rpc_header);

    if (available_payload < header.payload_size) {
        return true;
    }

    pthread_mutex_lock(&g_rpc_lock);
    px4_openamp_rpc_init_locked();

    struct rpc_pending *slot = NULL;

    for (size_t i = 0; i < MAX_PENDING_RPC; ++i) {
        if (g_rpc_pending[i].in_use && g_rpc_pending[i].request_id == header.request_id) {
            slot = &g_rpc_pending[i];
            break;
        }
    }

    if (!slot) {
        fprintf(stderr, "[RPC][DISPATCH] no pending slot for req_id=%ld len=%zu cat=%u op=%u ret=%ld errno=%ld payload=%lu\n",
                (long)header.request_id,
                len,
                header.category,
                header.opcode,
                (long)header.return_value,
                (long)header.errno_value,
                (unsigned long)header.payload_size);
        pthread_mutex_unlock(&g_rpc_lock);
        return true;
    }

    const size_t to_copy = (header.payload_size < slot->response_capacity) ? header.payload_size : slot->response_capacity;

    if (to_copy > 0 && slot->response_buffer && header.payload_size > 0) {
        const uint8_t *payload = (const uint8_t *)data + sizeof(struct px4_rpc_header);
        /* Add memory barrier before copying from shared memory */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        memcpy(slot->response_buffer, payload, to_copy);
        /* Add memory barrier after copying from shared memory */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }

    slot->response_length = header.payload_size;

    if (slot->response_length_out) {
        *slot->response_length_out = header.payload_size;
    }

    slot->return_value = header.return_value;
    slot->errno_value = header.errno_value;
    slot->completed = 1;

    if (slot->callback) {
        px4_openamp_rpc_response_cb_t callback = slot->callback;
        void *user_data = slot->user_data;
        const void *payload = slot->response_buffer;
        const size_t payload_len = header.payload_size;
        const int32_t request_id = slot->request_id;
        const int32_t return_value = slot->return_value;
        const int32_t errno_value = slot->errno_value;

        release_slot_locked(slot);
        pthread_mutex_unlock(&g_rpc_lock);

        callback(request_id, return_value, errno_value, payload, payload_len, user_data);
        return true;
    }

    pthread_cond_signal(&slot->cond);
    pthread_mutex_unlock(&g_rpc_lock);

    return true;
}

bool px4_openamp_rpc_channel_ready(void)
{
    return g_rpc_channel_ready != 0;
}

int px4_openamp_rpc_session_reset(void)
{
    if (!px4_openamp_rpc_endpoint_ready()) {
        return -1;
    }

    /* Jump g_rpc_next_request_id far ahead of whatever stale req_ids the previous
     * session left in the RPMsg ring buffer (typically 1–100).  Stale responses that
     * arrive after this point will have req_ids in the old range and will be discarded
     * as "no pending slot" without poisoning any new-session pending slots. */
    const int32_t old_id = __atomic_load_n(&g_rpc_next_request_id, __ATOMIC_RELAXED);
    const int32_t new_start = (old_id > 2000) ? old_id : 10000;
    __atomic_store_n(&g_rpc_next_request_id, new_start, __ATOMIC_RELAXED);

    fprintf(stderr,
            "[RPC][SESSION] *** NEW SESSION: req_id reset %ld -> %ld, sending SESSION_RESET to CA55 ***\n",
            (long)old_id, (long)new_start);

    int32_t ret_val = 0, err_val = 0;
    int rc = px4_openamp_rpc_call_timeout(
        PX4_RPC_CATEGORY_FS,
        PX4_RPC_FS_SESSION_RESET,
        NULL, 0,
        NULL, 0, NULL,
        &ret_val, &err_val,
        5000 /* 5s timeout */);

    if (rc != 0 || ret_val != 0) {
        fprintf(stderr,
                "[RPC][SESSION] SESSION_RESET failed: rc=%d ret=%ld err=%ld\n",
                rc, (long)ret_val, (long)err_val);
        return -1;
    }

    fprintf(stderr, "[RPC][SESSION] SESSION_RESET OK — CA55 stale fds cleared\n");
    return 0;
}
