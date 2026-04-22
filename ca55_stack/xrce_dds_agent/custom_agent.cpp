// Copyright 2021-present Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Modifications Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
// SPDX-License-Identifier: Apache-2.0

// Include necessary headers
#include <cstdio>
#include <uxr/agent/transport/custom/CustomAgent.hpp>
#include <uxr/agent/transport/endpoint/CustomEndPoint.hpp>
#include <memory>
#include <queue>
#include <vector>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <cstdlib>
#include <utility>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <atomic>
#include "px4_openamp_rpc_server.h"
#include "platform/rzv_sdram_layout.h"
#include "../../src/rpc/openamp_rpc.h"

extern "C" {
    #include "openamp/open_amp.h"
    #include "metal/alloc.h"
    #include "metal/utilities.h"
    #include "platform_info.h"
    #include "rsc_table.h"
    #include "helper.h"
    #include <arpa/inet.h>
    #include <sys/socket.h>
}
#define SHUTDOWN_MSG    (0xEF56A55A)

#ifndef ARRAY_SIZE
    #define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))
#endif

#ifdef ENABLE_AI_CAMERA
#include "ai/ai_thread.h"
#include "ai/obstacle_processor.h"
static pthread_t          g_ai_thread;
volatile bool             ai_thread_running = true;  // extern'd by ai_thread.cpp
static AI::AiThreadConfig g_ai_cfg          = AI::AiThreadDefaultConfig();

// Called by ai_thread.cpp each frame — forward avoidance packet to CR8 via RPMsg.
// Full RPMsg forwarding requires access to the rp_ept endpoint; for now we store
// the latest packet so it can be sent on the next DDS cycle if needed.
static AI::ObstacleAvoidPacket g_last_obstacle_pkt{};
void ai_update_obstacle_packet(const AI::ObstacleAvoidPacket& pkt) {
    g_last_obstacle_pkt = pkt;
}
#endif // ENABLE_AI_CAMERA

/* Internal functions */
static void rpmsg_service_bind(struct rpmsg_device *rdev, const char *name, uint32_t dest);  // RPC bind
static void rpmsg_service_unbind(struct rpmsg_endpoint *ept);  // RPC unbind
static int rpmsg_service_cb(struct rpmsg_endpoint *rp_ept, void *data, size_t len, uint32_t src, void *priv);  // RPC callback
static void rpmsg_uxrce_service_bind(struct rpmsg_device *rdev, const char *name, uint32_t dest);  // UXRCE-DDS bind
static void rpmsg_uxrce_service_unbind(struct rpmsg_endpoint *ept);  // UXRCE-DDS unbind
static int rpmsg_uxrce_service_cb(struct rpmsg_endpoint *rp_ept, void *data, size_t len, uint32_t src, void *priv);  // UXRCE-DDS callback
static void rpmsg_logger_service_bind(struct rpmsg_device *rdev, const char *name, uint32_t dest);
static void rpmsg_logger_service_unbind(struct rpmsg_endpoint *ept);
static int rpmsg_logger_cb(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv);
static void rpmsg_mavlink_bind(struct rpmsg_device *rdev, const char *name, uint32_t dest);
static int rpmsg_mavlink_cb(struct rpmsg_endpoint *rp_ept, void *data, size_t len, uint32_t src, void *priv);
static void rpc_ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t dest);
static void uxrce_ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t dest);
static void register_handler(int signum, void(* handler)(int));
static void stop_handler(int signum);
static void init_cond(void);
static void set_tid(int target);
static void clear_tid(void);
static bool is_rpc_message(const void *data, size_t len);
static int rpmsg_send_retry(struct rpmsg_endpoint *ept, const void *data, size_t len, const char *tag);

// Logger stream protocol constants (must match CR8 remote_storage_rpc.c)
#define LOG_STREAM_MSG_WRITE  0x01u
#define LOG_STREAM_MSG_FLUSH  0x02u
#define LOG_STREAM_MSG_KICK   0x03u

struct log_stream_hdr {
    uint8_t  msg_type;
    uint8_t  reserved;
    uint16_t data_len;
    int32_t  ca55_fd;
};

#include <sys/mman.h>
#include <fcntl.h>

#define LOG_SHM_MAGIC 0x4C4F4753U

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

static_assert(sizeof(logger_shm_ctrl) == RZV_SDRAM_LOGGER_STREAM_BYTES,
              "logger_shm_ctrl size must match shared SDRAM stream size");
static_assert(sizeof(logger_shm_region) == RZV_SDRAM_LOGGER_SHM_BYTES,
              "logger_shm_region size must match shared SDRAM reservation");

static volatile struct logger_shm_region *g_log_shm = nullptr;

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_log_flush_thread;
static volatile bool g_log_flush_running = false;
static pthread_cond_t g_log_flush_cond = PTHREAD_COND_INITIALIZER;
static std::map<int, bool> g_log_write_error_reported;

#define LOG_FLUSH_INTERVAL_MS  100u
#define LOG_MAX_WRITE_CHUNK    (128u * 1024u)

static void clear_logger_error_state(int fd)
{
    g_log_write_error_reported.erase(fd);
}

// Global variables
static struct rpmsg_endpoint rp_ept = { 0 };        // RPC endpoint (service-0)
static struct rpmsg_endpoint rp_ept_uxrce = { 0 };  // UXRCE-DDS endpoint (service-1)
static struct rpmsg_endpoint rp_ept_logger = { 0 }; // Logger stream endpoint (service-2)
static const char *svc_name = NULL;                 // RPC service name
static const char *uxrce_svc_name = NULL;           // UXRCE-DDS service name
static const char *logger_svc_name = NULL;          // Logger stream service name
static struct rpmsg_device *rpdev_rpc = NULL;
static struct rpmsg_device *rpdev_uxrce = NULL;
static bool service_bound = false;        // Flag for RPC endpoint
static bool uxrce_service_bound = false;  // Flag for UXRCE-DDS endpoint
static bool logger_service_bound = false; // Flag for logger stream endpoint
int force_stop = 0;
pthread_mutex_t mutex, rsc_mutex;
pthread_cond_t cond[MBX_CH_NUM];
pthread_t tid_comm_rpc;
pthread_t tid_comm_uxrce;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static const size_t MAX_QUEUE_SIZE = 500;  // Increased from 100
static const size_t QUEUE_WARNING_THRESHOLD = 400;  // 80% full
std::queue<std::vector<uint8_t>> message_queue; // Simplify the queue
static uint32_t queue_drops = 0;
static size_t queue_peak_size = 0;
static std::atomic<uint32_t> g_rpmsg_no_buf_count{0};
static std::atomic<uint32_t> g_rpmsg_retry_events{0};
static std::atomic<uint32_t> g_rpmsg_send_fail_count{0};
static std::atomic<bool> g_transport_stats_running{false};
static pthread_t g_transport_stats_thread{};
static size_t g_next_queue_warn_threshold = QUEUE_WARNING_THRESHOLD;

// Global OpenAMP transport mutex to serialize RPC and UXRCE-DDS operations
// Prevents deadlock from shared virtio device mutex contention
pthread_mutex_t openamp_transport_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_key_t thkey;
bool valid_thread[MBX_CH_NUM] = {false};
static bool uxrce_thread_started = false;

// MAVLink forwarding support
static struct rpmsg_endpoint rp_ept_mavlink = {0};
static bool enable_mavlink_forward = false;
static bool mavlink_endpoint_bound = false;
static int mavlink_udp_sock = -1;
static pthread_t mavlink_udp_thread;
static bool mavlink_udp_thread_running = false;
static const char *mavlink_svc_name = "rpmsg-mavlink-channel";
// CA55 uses addr 0x2, CR8 uses addr 0x1 (advertised via name service)
#define APP_EPT_ADDR_MAVLINK (0x2)

/** for information */
pid_t g_tid_cm33 = 0;
pid_t g_tid_cr8_0 = 0;
pid_t g_tid_cr8_1 = 0;

// Initialize condition variables and global state at runtime
extern "C" void init_global_vars(void) {
    /* Use CLOCK_MONOTONIC for all cond vars: timedwait intervals must be
     * immune to wall-clock jumps (NTP sync, RPC CLOCK_SETTIME from GPS/QGC).
     * A CLOCK_REALTIME cond var returns ETIMEDOUT immediately when the clock
     * jumps forward while a thread is blocked, breaking the XRCE-DDS bridge. */
    pthread_condattr_t cond_attr;
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);

    for (int i = 0; i < MBX_CH_NUM; i++) {
        pthread_cond_init(&cond[i], &cond_attr);
        valid_thread[i] = false;
    }

    // IMPORTANT: g_log_flush_cond MUST be initialized with same clock as its timedwait
    pthread_cond_init(&g_log_flush_cond, &cond_attr);
    pthread_condattr_destroy(&cond_attr);

    pthread_key_create(&thkey, free); /* free TLS data on thread exit (matches original init_cond) */
    pthread_mutex_init(&mutex, NULL);
    pthread_mutex_init(&rsc_mutex, NULL);

    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd >= 0) {
        g_log_shm = (volatile struct logger_shm_region *)mmap(NULL, sizeof(struct logger_shm_region), PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, RZV_SDRAM_LOGGER_SHM_ADDR);
        close(mem_fd);
        LPRINTF("Shared SDRAM Logger mapped at %p", g_log_shm);
    } else {
        LPERROR("Failed to open /dev/mem for Shared SDRAM Logger! (err=%d)", errno);
    }
}

// Cleanup global resources  
extern "C" void cleanup_global_vars(void) {
    for (int i = 0; i < MBX_CH_NUM; i++) {
        pthread_cond_destroy(&cond[i]);
    }
    pthread_key_delete(thkey);
    pthread_mutex_destroy(&mutex);
    pthread_mutex_destroy(&rsc_mutex);
    pthread_mutex_destroy(&queue_mutex);
}

struct comm_arg ids[] = {
    {NULL, 0, UIO_RECEIVER1},
    {NULL, 1, UIO_RECEIVER1},
    {NULL, 0, UIO_RECEIVER2},
    {NULL, 1, UIO_RECEIVER2},
    {NULL, 0, UIO_RECEIVER3},
    {NULL, 1, UIO_RECEIVER3},
};

/* External functions */
extern int init_system(void);
extern void cleanup_system(void);

int comm_core_rpc = 2;   // Default: channel 0 on receiver2
int comm_core_uxrce = 5; // Default: channel 1 on receiver3

static void log_transport_stats(const char *reason)
{
    size_t queue_current = 0;
    size_t queue_peak = 0;
    uint32_t drops = 0;

    pthread_mutex_lock(&queue_mutex);
    queue_current = message_queue.size();
    queue_peak = queue_peak_size;
    drops = queue_drops;
    pthread_mutex_unlock(&queue_mutex);

    LPRINTF("Transport stats[%s]: rpmsg_no_buf=%u retry_events=%u send_fail=%u queue_current=%zu queue_peak=%zu queue_drops=%u",
            reason ? reason : "periodic",
            g_rpmsg_no_buf_count.load(),
            g_rpmsg_retry_events.load(),
            g_rpmsg_send_fail_count.load(),
            queue_current,
            queue_peak,
            drops);
}

static void *transport_stats_thread_fn(void *)
{
    uint32_t last_no_buf = 0;
    uint32_t last_retry = 0;
    uint32_t last_fail = 0;
    uint32_t last_drops = 0;
    size_t last_peak = 0;

    while (g_transport_stats_running.load() && !force_stop) {
        sleep(1);

        size_t queue_current = 0;
        size_t queue_peak = 0;
        uint32_t drops = 0;

        pthread_mutex_lock(&queue_mutex);
        queue_current = message_queue.size();
        queue_peak = queue_peak_size;
        drops = queue_drops;
        pthread_mutex_unlock(&queue_mutex);

        const uint32_t no_buf = g_rpmsg_no_buf_count.load();
        const uint32_t retry = g_rpmsg_retry_events.load();
        const uint32_t fail = g_rpmsg_send_fail_count.load();

        const bool changed = (no_buf != last_no_buf) || (retry != last_retry) ||
                             (fail != last_fail) || (drops != last_drops) ||
                             (queue_peak != last_peak);

        if (changed || queue_current >= QUEUE_WARNING_THRESHOLD) {
            LPRINTF("Transport stats[periodic]: rpmsg_no_buf=%u retry_events=%u send_fail=%u queue_current=%zu queue_peak=%zu queue_drops=%u",
                    no_buf, retry, fail, queue_current, queue_peak, drops);
            last_no_buf = no_buf;
            last_retry = retry;
            last_fail = fail;
            last_drops = drops;
            last_peak = queue_peak;
        }
    }

    return nullptr;
}

static int rpmsg_send_retry(struct rpmsg_endpoint *ept, const void *data, size_t len, const char *tag)
{
    if (!ept || !data || len == 0) {
        return -EINVAL;
    }

    if (ept->dest_addr == RPMSG_ADDR_ANY || ept->rdev == NULL) {
        return -ENOTCONN;
    }

    int ret = -EAGAIN;
    const int max_tries = 3;
    for (int attempt = 0; attempt < max_tries; ++attempt) {
        pthread_mutex_lock(&openamp_transport_mutex);
        ret = rpmsg_trysend(ept, data, len);
        pthread_mutex_unlock(&openamp_transport_mutex);

        if (ret >= 0 || ret != RPMSG_ERR_NO_BUFF) {
            if (ret < 0) {
                g_rpmsg_send_fail_count.fetch_add(1);
            }
            return ret;
        }

        g_rpmsg_no_buf_count.fetch_add(1);
        g_rpmsg_retry_events.fetch_add(1);

        if (attempt == 0 && tag) {
            LPRINTF("%s: RPMSG no buffer, retrying", tag);
        }
        usleep(1000U * (unsigned)(attempt + 1));
    }

    if (ret < 0) {
        g_rpmsg_send_fail_count.fetch_add(1);
        log_transport_stats(tag ? tag : "rpmsg_send_retry_fail");
    }

    return ret;
}

static void rpmsg_service_bind(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
    int ret;

    if (name == nullptr || svc_name == nullptr) {
        LPERROR("Error: name or svc_name is null");
        return;
    }

    LPRINTF("Service bind callback: name='%s', dest=0x%x", name, dest);

    if (strcmp(name, svc_name)) {
        LPERROR("Unexpected name service %s, expected %s.", name, svc_name);
        return;
    }

    // Check if endpoint already exists
    if (service_bound) {
        LPRINTF("RPC endpoint already bound, updating dest_addr from 0x%x to 0x%x", rp_ept.dest_addr, dest);
        rp_ept.dest_addr = dest;
        return;
    }

    LPRINTF("Creating RPMsg endpoint for service '%s'", name);

    // Now create the endpoint since the service has been advertised - use RPMSG_ADDR_ANY for auto-assigned address
    pthread_mutex_lock(&rsc_mutex);
    ret = rpmsg_create_ept(&rp_ept, rdev, svc_name,
                           RPMSG_ADDR_ANY, dest,
                           rpmsg_service_cb,
                           rpmsg_service_unbind);
    pthread_mutex_unlock(&rsc_mutex);

    if (ret) {
        LPERROR("Failed to create RPMsg endpoint in bind callback: %d", ret);
        return;
    }

    // Mark service as bound
    service_bound = true;

    LPRINTF("RPMsg endpoint created successfully. rp_ept:%p (addr=0x%x)", &rp_ept, rp_ept.addr);

    // Send an initial handshake message to CR8 to establish bidirectional communication
    LPRINTF("Sending initial handshake to CR8 to establish connection...");
    const char* handshake_msg = "HANDSHAKE_FROM_CA55";
    int send_ret = rpmsg_send_retry(&rp_ept, handshake_msg, strlen(handshake_msg) + 1, "RPC handshake");
    if (send_ret < 0) {
        LPRINTF("Failed to send initial handshake: %d", send_ret);
    } else {
        LPRINTF("Initial handshake sent successfully");
    }

    return;
}

static void rpmsg_service_unbind(struct rpmsg_endpoint *ept)
{
    (void)ept;
    LPRINTF("RPC endpoint is destroyed.");

    // Mark service as unbound
    service_bound = false;

    /* service 0 - RPC */
    pthread_mutex_lock(&openamp_transport_mutex);
    rpmsg_destroy_ept(&rp_ept);
    memset(&rp_ept, 0x0, sizeof(struct rpmsg_endpoint));
    pthread_mutex_unlock(&openamp_transport_mutex);
    return ;
}

// UXRCE-DDS service bind callback
static void rpmsg_uxrce_service_bind(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
    int ret;

    if (name == nullptr || uxrce_svc_name == nullptr) {
        LPERROR("Error: name or uxrce_svc_name is null");
        return;
    }

    LPRINTF("UXRCE-DDS service bind callback: name='%s', dest=0x%x", name, dest);

    if (strcmp(name, uxrce_svc_name)) {
        LPERROR("Unexpected name service %s, expected %s.", name, uxrce_svc_name);
        return;
    }

    // Check if endpoint already exists
    if (uxrce_service_bound) {
        LPRINTF("UXRCE-DDS endpoint already bound, updating dest_addr from 0x%x to 0x%x", rp_ept_uxrce.dest_addr, dest);
        rp_ept_uxrce.dest_addr = dest;
        return;
    }

    LPRINTF("Creating RPMsg endpoint for UXRCE-DDS service '%s'", name);

    // Create the UXRCE-DDS endpoint - use RPMSG_ADDR_ANY for auto-assigned address
    pthread_mutex_lock(&rsc_mutex);
    ret = rpmsg_create_ept(&rp_ept_uxrce, rdev, uxrce_svc_name,
                           RPMSG_ADDR_ANY, dest,
                           rpmsg_uxrce_service_cb,
                           rpmsg_uxrce_service_unbind);
    pthread_mutex_unlock(&rsc_mutex);

    if (ret) {
        LPERROR("Failed to create UXRCE-DDS RPMsg endpoint in bind callback: %d", ret);
        return;
    }

    // Mark service as bound
    uxrce_service_bound = true;

    LPRINTF("UXRCE-DDS RPMsg endpoint created successfully. rp_ept_uxrce:%p (addr=0x%x)", &rp_ept_uxrce, rp_ept_uxrce.addr);

    // Send an initial handshake message to CR8
    LPRINTF("Sending initial UXRCE-DDS handshake to CR8...");
    const char* handshake_msg = "UXRCE_HANDSHAKE_FROM_CA55";
    int send_ret = rpmsg_send_retry(&rp_ept_uxrce, handshake_msg, strlen(handshake_msg) + 1, "UXRCE handshake");
    if (send_ret < 0) {
        LPRINTF("Failed to send UXRCE-DDS handshake: %d", send_ret);
    } else {
        LPRINTF("UXRCE-DDS handshake sent successfully");
    }

    return;
}

static void rpmsg_uxrce_service_unbind(struct rpmsg_endpoint *ept)
{
    (void)ept;
    LPRINTF("UXRCE-DDS endpoint is destroyed.");

    // Mark service as unbound
    uxrce_service_bound = false;

    /* service 1 - UXRCE-DDS */
    pthread_mutex_lock(&openamp_transport_mutex);
    rpmsg_destroy_ept(&rp_ept_uxrce);
    memset(&rp_ept_uxrce, 0x0, sizeof(struct rpmsg_endpoint));
    pthread_mutex_unlock(&openamp_transport_mutex);
    return ;
}

// ---------- Logger stream channel implementation ----------

static void process_shm_stream_sync(int target_fd, bool close_stream) {
    if (!g_log_shm) return;
    
    for (int i = 0; i < static_cast<int>(RZV_SDRAM_LOGGER_STREAM_COUNT); i++) {
        volatile struct logger_shm_ctrl *ctrl = &g_log_shm->streams[i];
        if (ctrl->magic != LOG_SHM_MAGIC) continue;
        
        int fd = ctrl->active_fd;
        if (target_fd >= 0 && fd != target_fd) continue;
        if (fd < 0) continue;

        uint32_t head = ctrl->head;
        uint32_t tail = ctrl->tail;
        uint32_t size = ctrl->buffer_size;

        while (tail != head) {
            uint32_t chunk_size = (head > tail) ? (head - tail) : (size - tail);
            if (chunk_size > LOG_MAX_WRITE_CHUNK) chunk_size = LOG_MAX_WRITE_CHUNK;

            ssize_t written = ::write(fd, (const void *)&ctrl->data[tail], chunk_size);
            if (written <= 0) {
                if (!g_log_write_error_reported[fd]) {
                    LPERROR("Logger SHM write failed fd=%d err=%d", fd, errno);
                    g_log_write_error_reported[fd] = true;
                }
                break;
            }

            if (g_log_write_error_reported[fd]) {
                g_log_write_error_reported.erase(fd);
            }

            tail = (tail + written) % size;
            ctrl->tail = tail; // Push update to CR8
        }

        if (close_stream && target_fd == fd) {
            ctrl->active_fd = -1; // Notify CR8 that stream is totally free
            clear_logger_error_state(fd);
        }
    }
}

static void *logger_flush_thread_fn(void *)
{
    LPRINTF("Logger flush thread (Shared SDRAM mode) started (interval=%ums)", LOG_FLUSH_INTERVAL_MS);

    while (g_log_flush_running) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts); /* must match g_log_flush_cond clock attr */
        ts.tv_nsec += (long)(LOG_FLUSH_INTERVAL_MS) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

        pthread_mutex_lock(&g_log_mutex);
        int ret = pthread_cond_timedwait(&g_log_flush_cond, &g_log_mutex, &ts);
        
        if (ret != 0 && ret != ETIMEDOUT) {
            // If the cond var failed (e.g. EINVAL due to bad init), avoid spinning.
            pthread_mutex_unlock(&g_log_mutex);
            LPERROR("Logger flush cond wait failed: %d. Sleeping to avoid spin.", ret);
            usleep(LOG_FLUSH_INTERVAL_MS * 1000u);
            continue;
        }

        if (g_log_flush_running) {
            process_shm_stream_sync(-1, false);
        }
        pthread_mutex_unlock(&g_log_mutex);
    }

    // Final flush on shutdown
    pthread_mutex_lock(&g_log_mutex);
    process_shm_stream_sync(-1, false);
    pthread_mutex_unlock(&g_log_mutex);

    LPRINTF("Logger flush thread stopped");
    return nullptr;
}

// Logger stream callback: handles kicks and flush triggers.
static int rpmsg_logger_cb(struct rpmsg_endpoint *ept, void *data, size_t len,
                            uint32_t src, void *priv)
{
    (void)ept; (void)src; (void)priv;

    if (!data || len < sizeof(struct log_stream_hdr)) {
        return RPMSG_SUCCESS;
    }

    struct log_stream_hdr hdr;
    memcpy(&hdr, data, sizeof(hdr));

    if (hdr.msg_type == LOG_STREAM_MSG_KICK) {
        // Just wake up the flush thread to consume SHM data
        pthread_cond_signal(&g_log_flush_cond);
        
    } else if (hdr.msg_type == LOG_STREAM_MSG_FLUSH) {
        // Synchronous flush+remove: called before CR8 sends RPC FS_CLOSE for this fd
        pthread_mutex_lock(&g_log_mutex);
        process_shm_stream_sync(hdr.ca55_fd, true /* close and release */);
        pthread_mutex_unlock(&g_log_mutex);
    }

    return RPMSG_SUCCESS;
}

static void rpmsg_logger_service_bind(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
    LPRINTF("Logger stream service bind: name='%s', dest=0x%x", name, dest);

    if (logger_service_bound) {
        rp_ept_logger.dest_addr = dest;
        return;
    }

    pthread_mutex_lock(&rsc_mutex);
    int ret = rpmsg_create_ept(&rp_ept_logger, rdev, logger_svc_name,
                               RPMSG_ADDR_ANY, dest,
                               rpmsg_logger_cb,
                               rpmsg_logger_service_unbind);
    pthread_mutex_unlock(&rsc_mutex);

    if (ret) {
        LPERROR("Failed to create logger stream endpoint: %d", ret);
        return;
    }

    logger_service_bound = true;

    // Start flush thread on first bind
    if (!g_log_flush_running) {
        g_log_flush_running = true;
        pthread_create(&g_log_flush_thread, nullptr, logger_flush_thread_fn, nullptr);
    }

    LPRINTF("Logger stream endpoint created (addr=0x%x) - SDLOG_PROFILE=17 now safe", rp_ept_logger.addr);
}

static void rpmsg_logger_service_unbind(struct rpmsg_endpoint *ept)
{
    (void)ept;
    LPRINTF("Logger stream endpoint unbound");
    logger_service_bound = false;

    // Stop flush thread
    if (g_log_flush_running) {
        g_log_flush_running = false;
        pthread_cond_signal(&g_log_flush_cond);
        pthread_join(g_log_flush_thread, nullptr);
    }

    pthread_mutex_lock(&openamp_transport_mutex);
    rpmsg_destroy_ept(&rp_ept_logger);
    memset(&rp_ept_logger, 0, sizeof(rp_ept_logger));
    pthread_mutex_unlock(&openamp_transport_mutex);
}

// ---------- End logger stream channel ----------

static bool is_rpc_message(const void *data, size_t len)
{
    if (!data || len < sizeof(px4_rpc_header)) {
        return false;
    }

    px4_rpc_header header{};
    memcpy(&header, data, sizeof(header));
    return header.magic == PX4_RPC_MAGIC;
}

// RPC service callback - handles ONLY RPC messages on rpmsg-service-0
static int rpmsg_service_cb(struct rpmsg_endpoint *rp_ept, void *data, size_t len, uint32_t src, void *priv)
{
    (void)priv;

    if (!data || len == 0) {
        LPRINTF("RPC: Received empty message, ignoring.");
        return RPMSG_SUCCESS;
    }

    // Ensure responses go back to the sender we just heard from
    if (src != RPMSG_ADDR_ANY && src != rp_ept->addr) {
        rp_ept->dest_addr = src;
    }

    // This endpoint handles ONLY RPC messages
    if (is_rpc_message(data, len)) {
        if (!px4_openamp_rpc_server_enqueue(rp_ept, data, len)) {
            LPERROR("RPC message enqueue failed");
        }
    } else {
        LPERROR("Non-RPC message received on RPC endpoint (len=%zu), dropping", len);
    }

    return RPMSG_SUCCESS;
}

// UXRCE-DDS service callback - handles ONLY UXRCE-DDS messages on rpmsg-service-1
static int rpmsg_uxrce_service_cb(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv)
{
    (void)priv;

    if (!data || len == 0) {
        LPRINTF("UXRCE-DDS: Received empty message, ignoring.");
        return RPMSG_SUCCESS;
    }

    // Ensure responses go back to the sender we just heard from
    if (src != RPMSG_ADDR_ANY && src != ept->addr) {
        ept->dest_addr = src;
    }

    // Queue all messages for processing by the UXRCE-DDS agent
    const uint8_t *data_ptr = static_cast<const uint8_t *>(data);
    std::vector<uint8_t> message(len);
    copy_data(message.data(), data_ptr, len);

    pthread_mutex_lock(&queue_mutex);

    // Track queue usage and detect pressure
    size_t current_size = message_queue.size();
    if (current_size > queue_peak_size) {
        queue_peak_size = current_size;
    }

    // Avoid queue overgrowth (limit to 500 queued messages)
    if (current_size >= MAX_QUEUE_SIZE) {
        queue_drops++;
        message_queue.pop(); // Remove oldest message
        LPRINTF("UXRCE queue drop: current=%zu peak=%zu drops=%u", current_size, queue_peak_size, queue_drops);
    }

    message_queue.push(std::move(message));

    const size_t queued_after_push = message_queue.size();
    if (queued_after_push >= g_next_queue_warn_threshold) {
        LPRINTF("UXRCE queue pressure: current=%zu peak=%zu drops=%u", queued_after_push, queue_peak_size, queue_drops);
        g_next_queue_warn_threshold = queued_after_push + 50;
    } else if (queued_after_push < (QUEUE_WARNING_THRESHOLD / 2)) {
        g_next_queue_warn_threshold = QUEUE_WARNING_THRESHOLD;
    }

    // Signal the waiting UXRCE receive thread
    pthread_cond_signal(&cond[2]);
    pthread_mutex_unlock(&queue_mutex);

    return RPMSG_SUCCESS;
}

// MAVLink RPMsg callback (receives data from CR8 MAVLink channel)
static int rpmsg_mavlink_cb(struct rpmsg_endpoint *ept, void *data,
                             size_t len, uint32_t src, void *priv)
{
    (void)priv;

    LPRINTF("MAVLink RX: len=%zu, src=0x%x, my_addr=0x%x, dest=0x%x",
            len, src, ept ? ept->addr : 0xFFFF, ept ? ept->dest_addr : 0xFFFF);

    /* NOTE: dest_addr already set correctly by rpmsg_mavlink_bind()
     *
     * ISSUE: RPMsg bug reports wrong src address (reports dest endpoint's addr as src)
     * When CR8 sends from 0x1, CA55 receives with src=0x2 (our own addr) instead of src=0x1
     *
     * SAFE: dest_addr is correctly set to 0x1 by bind callback when endpoint was created
     * DO NOT modify ept->dest_addr here - causes bus error!
     */

    if (len == 0) {
        LPRINTF("MAVLink RX: Ignoring zero-length message");
        return RPMSG_SUCCESS;
    }

    LPRINTF("MAVLink RX: After len check, len=%zu, data=%p", len, data);

    // If this looks like a handshake, log it (SKIP for now to avoid crash)
    #if 0
    if (len < 100 && data) {
        char preview[64];
        size_t preview_len = len < 63 ? len : 63;
        memcpy(preview, data, preview_len);
        preview[preview_len] = '\0';
        LPRINTF("MAVLink RX: Message preview: '%s'", preview);
    }
    #endif

    LPRINTF("MAVLink RX: Before UDP forward, sock=%d", mavlink_udp_sock);

    // Forward to UDP immediately if socket is ready
    if (mavlink_udp_sock >= 0) {
        LPRINTF("MAVLink RX: Creating dest_addr struct");
        struct sockaddr_in dest_addr;
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(14550);
        dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");  // Broadcast

        LPRINTF("MAVLink RX: Calling sendto, len=%zu", len);
        ssize_t sent = sendto(mavlink_udp_sock, data, len, 0,
                             (struct sockaddr*)&dest_addr, sizeof(dest_addr));

        if (sent < 0) {
            LPERROR("MAVLink UDP send failed: %s", strerror(errno));
        } else {
            LPRINTF("MAVLink: Forwarded %zu bytes to UDP:14550", len);
        }
    }

    LPRINTF("MAVLink RX: Callback complete, returning SUCCESS");
    return RPMSG_SUCCESS;
}

// MAVLink RPMsg bind callback
static void rpmsg_mavlink_bind(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
    if (strcmp(name, mavlink_svc_name) != 0) {
        return;
    }

    LPRINTF("MAVLink channel bind: name='%s', dest=0x%x", name, dest);

    int ret;
    pthread_mutex_lock(&rsc_mutex);
    ret = rpmsg_create_ept(&rp_ept_mavlink, rdev, mavlink_svc_name,
                           APP_EPT_ADDR_MAVLINK,  // Fixed addr 0x2 for CA55
                           dest,
                           rpmsg_mavlink_cb,
                           NULL);  // No unbind callback for now
    pthread_mutex_unlock(&rsc_mutex);

    if (ret) {
        LPERROR("Failed to create MAVLink endpoint: %d", ret);
        return;
    }

    mavlink_endpoint_bound = true;
    LPRINTF("MAVLink endpoint created successfully (local_addr=0x%x, dest_addr=0x%x)",
            rp_ept_mavlink.addr, rp_ept_mavlink.dest_addr);

    // Send handshake to CR8
    const char* handshake_msg = "HANDSHAKE_MAVLink_CA55";
    size_t msg_len = strlen(handshake_msg) + 1;

    LPRINTF("MAVLink: Preparing to send handshake...");
    LPRINTF("  From: local_addr=0x%x", rp_ept_mavlink.addr);
    LPRINTF("  To: dest_addr=0x%x", rp_ept_mavlink.dest_addr);
    LPRINTF("  Message: '%s' (len=%zu)", handshake_msg, msg_len);

    int send_ret = rpmsg_send_retry(&rp_ept_mavlink, handshake_msg, msg_len, "MAVLink handshake");
    if (send_ret < 0) {
        LPRINTF("Failed to send MAVLink handshake: %d", send_ret);
    } else {
        LPRINTF("MAVLink handshake sent successfully (rpmsg_send returned %d)", send_ret);
    }
}

// Name service bind callback for RPC/MAVLink vdev
static void rpc_ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
    LPRINTF("RPC NS bind callback: name='%s', dest=0x%x", name, dest);

    if (svc_name && strcmp(name, svc_name) == 0) {
        rpmsg_service_bind(rdev, name, dest);
        return;
    }

    if (enable_mavlink_forward && strcmp(name, mavlink_svc_name) == 0) {
        rpmsg_mavlink_bind(rdev, name, dest);
        return;
    }

    // Handle UXRCE service on the same vdev to avoid dual-vdev mismatches
    if (uxrce_svc_name && strcmp(name, uxrce_svc_name) == 0) {
        LPRINTF("RPC NS: binding UXRCE service on shared vdev");
        rpmsg_uxrce_service_bind(rdev, name, dest);
        return;
    }

    // Handle logger stream service on the same vdev
    if (logger_svc_name && strcmp(name, logger_svc_name) == 0) {
        LPRINTF("RPC NS: binding logger stream service on shared vdev");
        rpmsg_logger_service_bind(rdev, name, dest);
        return;
    }

    LPRINTF("RPC NS bind: ignoring service '%s'", name);
}

// Name service bind callback for UXRCE vdev
__attribute__((unused)) static void uxrce_ns_bind_cb(struct rpmsg_device *rdev, const char *name, uint32_t dest)
{
    LPRINTF("UXRCE NS bind callback: name='%s', dest=0x%x", name, dest);

    if (uxrce_svc_name && strcmp(name, uxrce_svc_name) == 0) {
        rpmsg_uxrce_service_bind(rdev, name, dest);
        return;
    }

    LPRINTF("UXRCE NS bind: ignoring service '%s'", name);
}

// MAVLink UDP receive thread - forwards packets from QGC to CR8
void *mavlink_udp_receive_thread(void *arg)
{
    (void)arg;
    uint8_t buffer[2048];
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);

    LPRINTF("MAVLink UDP receive thread started");

    while (mavlink_udp_thread_running && mavlink_udp_sock >= 0) {
        // Receive from UDP (blocking with timeout)
        ssize_t received = recvfrom(mavlink_udp_sock, buffer, sizeof(buffer), 0,
                                   (struct sockaddr*)&src_addr, &src_len);

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;  // Timeout, try again
            }
            LPERROR("MAVLink UDP recvfrom failed: %s", strerror(errno));
            break;
        }

        if (received == 0) {
            continue;  // No data
        }

        // Log reception
        char src_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src_addr.sin_addr, src_ip, sizeof(src_ip));

        // Filter loopback packets (from localhost or RDK's own IP)
        // Check if source is localhost (127.0.0.1) or same as bind address (192.168.1.150)
        if (ntohl(src_addr.sin_addr.s_addr) == INADDR_LOOPBACK ||
            (ntohl(src_addr.sin_addr.s_addr) & 0xFFFFFF00) == 0xC0A80100) {  // 192.168.1.x
            // Additional check: ignore if source IP ends with .150 (our IP)
            if ((ntohl(src_addr.sin_addr.s_addr) & 0xFF) == 150) {
                // Loopback from RDK itself, ignore
                continue;
            }
        }

        LPRINTF("MAVLink UDP RX: %zd bytes from %s:%d",
                received, src_ip, ntohs(src_addr.sin_port));

        // Forward to CR8 via RPMsg if endpoint is bound
        if (mavlink_endpoint_bound && rp_ept_mavlink.dest_addr != RPMSG_ADDR_ANY) {
            int ret = rpmsg_send_retry(&rp_ept_mavlink, buffer, received, "MAVLink UDP->CR8");
            if (ret < 0) {
                LPERROR("Failed to forward UDP→CR8: %d", ret);
            } else {
                LPRINTF("MAVLink: Forwarded %zd bytes UDP→CR8", received);
            }
        } else {
            LPRINTF("MAVLink endpoint not ready, dropping packet");
        }
    }

    LPRINTF("MAVLink UDP receive thread exiting");
    return NULL;
}

// Polling function for RPC/MAVLink
static void *thread_comm_rpc(void *arg)
{
    static int sighandled = 0;
    int ret;

    if (!arg) {
        LPERROR("RPC polling thread failed to start.\n");
        return NULL;
    }

    struct comm_arg *p = (struct comm_arg *)arg;
    if (!p->platform) {
        LPERROR("Invalid RPC platform pointer. Exiting polling thread.\n");
        return NULL;
    }

    set_tid(p->target);

    pthread_mutex_lock(&rsc_mutex);
    rpdev_rpc = platform_create_rpmsg_vdev(p->platform, 0,
                                          VIRTIO_DEV_MASTER,
                                          NULL,
                                          rpc_ns_bind_cb);
    pthread_mutex_unlock(&rsc_mutex);
    if (!rpdev_rpc) {
        LPERROR("Failed to create RPC rpmsg virtio device.");
        return NULL;
    }
    rpdev_uxrce = rpdev_rpc;

    LPRINTF("RPC: Waiting for service '%s' to be advertised...", svc_name);

    // Allocate thread-specific value storage
    int *thread_val = (int*)malloc(sizeof(int));
    if (!thread_val) {
        LPERROR("Failed to allocate memory for RPC thread-specific value");
        return NULL;
    }
    *thread_val = p->target;
    valid_thread[*thread_val] = true;
    pthread_setspecific(thkey, thread_val);

    if (!sighandled) {
        sighandled = 1;
        register_handler(SIGINT, stop_handler);
        register_handler(SIGTERM, stop_handler);
    }

    // Poll until we get a service bind (endpoint creation) or forced to stop
    while (!force_stop && !service_bound) {
        ret = platform_poll(p->platform);
        if (ret < 0) {
            LPERROR("RPC polling failed (%d) while waiting for service bind.\n", ret);
            break;
        }
        usleep(10000); // 10ms polling interval while waiting for bind
    }

    if (service_bound && is_rpmsg_ept_ready(&rp_ept)) {
        LPRINTF("RPC endpoint is ready! Starting main polling loop...");
    } else {
        LPERROR("Failed to establish RPC endpoint. service_bound=%s", service_bound ? "true" : "false");
        return NULL;
    }

    // Main polling loop
    while (!force_stop) {
        ret = platform_poll(p->platform);
        if (ret < 0) {
            LPERROR("RPC polling failed (%d), exiting polling thread.\n", ret);
            break;
        }
        usleep(1000); // 1ms polling interval for active communication
    }
    return NULL;
}

// Polling function for UXRCE
static void *thread_comm_uxrce(void *arg)
{
    if (!arg) {
        LPERROR("UXRCE polling thread failed to start.\n");
        return NULL;
    }

    struct comm_arg *p = (struct comm_arg *)arg;
    set_tid(p->target);

    // Wait for RPC vdev to be created; UXRCE endpoint is bound via rpc_ns_bind_cb.
    while (!force_stop && rpdev_rpc == NULL) {
        usleep(10000);
    }

    if (force_stop) {
        return NULL;
    }

    rpdev_uxrce = rpdev_rpc;
    LPRINTF("UXRCE: Waiting for service '%s' to be advertised on shared vdev...", uxrce_svc_name);

    while (!force_stop && !uxrce_service_bound) {
        usleep(10000);
    }

    if (uxrce_service_bound && is_rpmsg_ept_ready(&rp_ept_uxrce)) {
        LPRINTF("UXRCE endpoint is ready on shared vdev.");
    } else if (!force_stop) {
        LPERROR("Failed to establish UXRCE endpoint. service_bound=%s", uxrce_service_bound ? "true" : "false");
    }

    while (!force_stop) {
        usleep(5000);
    }
    return NULL;
}

static void register_handler(int signum, void(* handler)(int)) {
    if (signal(signum, handler) == SIG_ERR) {
        LPERROR("register sig:%d failed.", signum);
    } else {
        LPRINTF("register sig:%d succeeded.", signum);
    }
}

// Signal handler
static void stop_handler(int signum) {
    /* ASYNC-SIGNAL-SAFE.
     * pthread_mutex_lock, pthread_cond_signal, rpmsg_send_retry are NOT
     * async-signal-safe — do not call them from a signal handler (SIGSEGV).
     *
     * Clear DRIVER_OK before exiting so CR8 detects the CA55 disconnect and
     * re-enters its "waiting for DRIVER_OK" poll loop. Without this, CR8
     * continues sending MHU interrupts into an unresponsive CA55 kernel,
     * causing a kernel hang/reboot 3-5 minutes later.
     *
     * Sleep 500ms after clearing DRIVER_OK before exiting: CR8 polls the
     * status byte every 200ms. Without this delay, a rapid "systemctl restart"
     * causes the new agent to re-set DRIVER_OK within CR8's 200ms poll window,
     * so CR8 never detects the disconnect and stays stuck with stale endpoints.
     * 500ms guarantees at least two CR8 poll cycles see the cleared status.
     * nanosleep() is not strictly POSIX async-signal-safe but is safe on Linux
     * in this context (no malloc, no locks, single-threaded signal delivery).
     *
     * Use _exit(0) to terminate immediately without running C++ destructors.
     * OpenAMP/DDS destructors crash with SIGSEGV during shutdown (UIO/vring
     * teardown while CR8 is still running). _exit(0) bypasses them cleanly.
     * systemd sees exit code 0 → Restart=on-failure does NOT restart. */
    force_stop = 1;
    (void)signum;
    platform_clear_driver_ok();
    /* Spin-wait 600ms so CR8's 200ms poll sees DRIVER_OK cleared before the
     * new agent re-sets it.  nanosleep() returns EINTR when interrupted; the
     * loop re-enters until clock_gettime shows ≥600ms have elapsed.
     * Both clock_gettime and nanosleep are safe to call here on Linux. */
    {
        struct timespec start, now, delay;
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (;;) {
            delay.tv_sec = 0; delay.tv_nsec = 10 * 1000 * 1000L;  /* 10ms */
            nanosleep(&delay, NULL);
            clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec  - start.tv_sec)  * 1000L
                    + (now.tv_nsec - start.tv_nsec) / 1000000L;
            if (ms >= 600) break;
        }
    }
    _exit(0);
}

static void init_cond(void)
{
    // Use the comprehensive init function
    init_global_vars();
}

/**
 * @fn set_tid
 * @brief set thread information
 */
static void set_tid(int target)
{
    pid_t tid = syscall(SYS_gettid);
    if (target == UIO_RECEIVER1) {
        g_tid_cm33 = tid;
    } else if (target == UIO_RECEIVER2) {
        g_tid_cr8_0 = tid;
    } else if (target == UIO_RECEIVER3) {
        g_tid_cr8_1 = tid;
    }
}

/**
 * @fn clear_tid
 * @brief clear thread information
 */
static void clear_tid(void) {
    pid_t tid = syscall(SYS_gettid);
    if (tid == g_tid_cm33) {
        g_tid_cm33 = 0;
    } else if (tid == g_tid_cr8_0) {
        g_tid_cr8_0 = 0;
    } else if (tid == g_tid_cr8_1) {
        g_tid_cr8_1 = 0;
    }
}

int main(int argc, char** argv)
{
    eprosima::uxr::Middleware::Kind mw_kind(eprosima::uxr::Middleware::Kind::FASTDDS);

    // Define the agent initialization function
    eprosima::uxr::CustomAgent::InitFunction init_function = [&]() -> bool
    {
        unsigned long proc_id;
        unsigned long rsc_id;
        unsigned long mbx_id; // Use CR8 core0 ch0
        int i;
        int ret;

        // Initialize the system
        ret = init_system();
        if (ret) {
            return false;
        }
        init_cond();
        if (!px4_openamp_rpc_server_init_async()) {
            LPERROR("Failed to initialize async RPC worker pool");
            return false;
        }
        if (!g_transport_stats_running.exchange(true)) {
            if (pthread_create(&g_transport_stats_thread, NULL, transport_stats_thread_fn, NULL) != 0) {
                g_transport_stats_running.store(false);
                LPERROR("Failed to create transport stats thread");
            }
        }

        // Initialize platform
        for (i = 0; i < ARRAY_SIZE(ids); i++) {
            proc_id = rsc_id = ids[i].channel;
            mbx_id = ids[i].target;
            ret = platform_init(proc_id, rsc_id, mbx_id, &ids[i].platform);
            if (ret) {
                LPERROR("Failed to initialize platform.");
                return false;
            }
        }

        // Set service names for binding
        svc_name = (const char *)CFG_RPMSG_SVC_NAME0;       // RPC service (service-0)
        uxrce_svc_name = (const char *)CFG_RPMSG_SVC_NAME1; // UXRCE-DDS service (service-1)
        logger_svc_name = (const char *)CFG_RPMSG_SVC_NAME2; // Logger stream (service-2)

        // Start the communicate threads
        pthread_create(&tid_comm_rpc, NULL, thread_comm_rpc, &ids[comm_core_rpc]);
        if (pthread_create(&tid_comm_uxrce, NULL, thread_comm_uxrce, &ids[comm_core_uxrce]) == 0) {
            uxrce_thread_started = true;
        } else {
            LPERROR("Failed to create UXRCE thread, continuing with RPC poll only");
            uxrce_thread_started = false;
        }

        // Launch AI camera thread (ENABLE_AI_CAMERA build + --qgc-ip flag)
#ifdef ENABLE_AI_CAMERA
        if (g_ai_cfg.qgc_dest_ip && g_ai_cfg.qgc_dest_ip[0] != '\0') {
            ai_thread_running = true;
            if (pthread_create(&g_ai_thread, nullptr, AI::ai_camera_thread_func,
                               &g_ai_cfg) != 0) {
                LPERROR("Failed to create AI camera thread");
            } else {
                LPRINTF("AI camera thread started — QGC stream → %s:5600 (%d kbps)",
                        g_ai_cfg.qgc_dest_ip, g_ai_cfg.qgc_bitrate / 1000);
            }
        }
#endif

        // Setup MAVLink UDP socket if enabled
        if (enable_mavlink_forward) {
            mavlink_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (mavlink_udp_sock < 0) {
                LPERROR("Failed to create MAVLink UDP socket: %s", strerror(errno));
            } else {
                // Enable broadcast
                int broadcast = 1;
                if (setsockopt(mavlink_udp_sock, SOL_SOCKET, SO_BROADCAST,
                              &broadcast, sizeof(broadcast)) < 0) {
                    LPERROR("Failed to enable broadcast: %s", strerror(errno));
                }

                // Enable address reuse
                int reuse = 1;
                if (setsockopt(mavlink_udp_sock, SOL_SOCKET, SO_REUSEADDR,
                              &reuse, sizeof(reuse)) < 0) {
                    LPERROR("Failed to enable SO_REUSEADDR: %s", strerror(errno));
                }

                // Bind to port 14550 for sending AND receiving
                struct sockaddr_in bind_addr;
                memset(&bind_addr, 0, sizeof(bind_addr));
                bind_addr.sin_family = AF_INET;
                bind_addr.sin_port = htons(14550);
                bind_addr.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces

                if (bind(mavlink_udp_sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
                    LPERROR("Failed to bind MAVLink socket to port 14550: %s", strerror(errno));
                } else {
                    LPRINTF("MAVLink UDP socket bound to port 14550");

                    // Start UDP receive thread
                    mavlink_udp_thread_running = true;
                    if (pthread_create(&mavlink_udp_thread, NULL, mavlink_udp_receive_thread, NULL) != 0) {
                        LPERROR("Failed to create MAVLink UDP receive thread");
                        mavlink_udp_thread_running = false;
                    } else {
                        LPRINTF("MAVLink UDP receive thread started");
                    }
                }
            }
        }

        return true;
    };

    // Define the agent destruction function
    eprosima::uxr::CustomAgent::FiniFunction fini_function = [&]() -> bool
    {
        LPRINTF("Finishing agent...");
        force_stop = 1;
#ifdef ENABLE_AI_CAMERA
        ai_thread_running = false;
        pthread_join(g_ai_thread, nullptr);
#endif
        if (g_transport_stats_running.exchange(false)) {
            pthread_join(g_transport_stats_thread, NULL);
        }
        if (comm_core_rpc >= 0 && comm_core_rpc < (int)ARRAY_SIZE(ids)) {
            valid_thread[ids[comm_core_rpc].target] = false;
        }
        if (comm_core_uxrce >= 0 && comm_core_uxrce < (int)ARRAY_SIZE(ids)) {
            valid_thread[ids[comm_core_uxrce].target] = false;
        }
        pthread_join(tid_comm_rpc, NULL);
        if (uxrce_thread_started) {
            pthread_join(tid_comm_uxrce, NULL);
        }
        px4_openamp_rpc_server_shutdown_async();

        // Destroy the RPMsg endpoints
        pthread_mutex_lock(&openamp_transport_mutex);
        rpmsg_destroy_ept(&rp_ept);
        rpmsg_destroy_ept(&rp_ept_uxrce);
        pthread_mutex_unlock(&openamp_transport_mutex);
        if (mavlink_endpoint_bound) {
            pthread_mutex_lock(&openamp_transport_mutex);
            rpmsg_destroy_ept(&rp_ept_mavlink);
            pthread_mutex_unlock(&openamp_transport_mutex);
            mavlink_endpoint_bound = false;
        }

        // Close MAVLink UDP socket
        if (mavlink_udp_sock >= 0) {
            close(mavlink_udp_sock);
            mavlink_udp_sock = -1;
            LPRINTF("MAVLink UDP socket closed");
        }

        struct rpmsg_device *shared_rpdev = rpdev_rpc;
        if (rpdev_uxrce == shared_rpdev) {
            rpdev_uxrce = NULL;
        }
        if (shared_rpdev) {
            platform_release_rpmsg_vdev(ids[comm_core_rpc].platform, shared_rpdev);
            rpdev_rpc = NULL;
        }
        if (rpdev_uxrce) {
            platform_release_rpmsg_vdev(ids[comm_core_uxrce].platform, rpdev_uxrce);
            rpdev_uxrce = NULL;
        }
        for (int i = 0; i < ARRAY_SIZE(ids); i++) {
            platform_cleanup(ids[i].platform);
            ids[i].platform = NULL;
        }
        cleanup_system();

        return true;
    };

    // Define the agent receive message function
    eprosima::uxr::CustomAgent::RecvMsgFunction recv_msg_function = [&](
        eprosima::uxr::CustomEndPoint* source_endpoint,
        uint8_t* buffer,
        size_t buffer_length,
        int timeout,
        eprosima::uxr::TransportRc& transport_rc) -> ssize_t
    {
        ssize_t bytes_received = -1;
        struct timespec ts;
        int ret;

        // UXR_AGENT_LOG_INFO(
        //     UXR_DECORATE_GREEN("recv_msg_function start"),
        //     "", "");

        pthread_mutex_lock(&queue_mutex);

        // Wait for message or timeout
        while (message_queue.empty() && !force_stop) {
            if (timeout >= 0) {
                clock_gettime(CLOCK_MONOTONIC, &ts); /* must match cond[2] clock attr */
                ts.tv_sec += timeout / 1000;
                ts.tv_nsec += (timeout % 1000) * 1000000;
                if (ts.tv_nsec >= 1000000000) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000;
                }
                ret = pthread_cond_timedwait(&cond[2], &queue_mutex, &ts);
                if (ret == ETIMEDOUT) {
                    break;
                }
            } else {
                pthread_cond_wait(&cond[2], &queue_mutex);
            }
        }

        if (!message_queue.empty()) {
            // UXR_AGENT_LOG_INFO(
            //     UXR_DECORATE_GREEN("Message received"),
            //     "", "");
            std::vector<uint8_t>& message = message_queue.front();
            bytes_received = std::min(message.size(), buffer_length);
            memcpy(buffer, message.data(), bytes_received);
            message_queue.pop();

            transport_rc = eprosima::uxr::TransportRc::ok;
        } else if (force_stop) {
            // UXR_AGENT_LOG_INFO(
            //     UXR_DECORATE_GREEN("Force stop received"),
            //     "", "");
            transport_rc = eprosima::uxr::TransportRc::server_error;
            bytes_received = -1;
        } else {
            // UXR_AGENT_LOG_INFO(
            //     UXR_DECORATE_GREEN("timeout_error"),
            //     "", "");
            transport_rc = eprosima::uxr::TransportRc::timeout_error;
            bytes_received = 0;
        }

        pthread_mutex_unlock(&queue_mutex);
        // UXR_AGENT_LOG_INFO(
        //     UXR_DECORATE_GREEN("recv_msg_function end"),
        //     "", "");
        return bytes_received;
    };

    // Define the agent send message function
    eprosima::uxr::CustomAgent::SendMsgFunction send_msg_function = [&](
        const eprosima::uxr::CustomEndPoint* destination_endpoint,
        const uint8_t* buffer,
        size_t message_length,
        eprosima::uxr::TransportRc& transport_rc) -> ssize_t
    {
        int ret;
        // UXR_AGENT_LOG_INFO(
        //     UXR_DECORATE_GREEN("Sending message..."),
        //     "message_length: %zu",
        //     message_length);

        if (!uxrce_service_bound || rp_ept_uxrce.dest_addr == RPMSG_ADDR_ANY) {
            transport_rc = eprosima::uxr::TransportRc::server_error;
            return -1;
        }

        // Send using dedicated UXRCE-DDS endpoint (service-1)
        ret = rpmsg_send_retry(&rp_ept_uxrce, buffer, message_length, "UXRCE send");
        if (ret >= 0) {
            // UXR_AGENT_LOG_INFO(
            //     UXR_DECORATE_GREEN("Message sent successfully"),
            //     "bytes_sent: %d",
            //     ret);
            transport_rc = eprosima::uxr::TransportRc::ok;
            return message_length;
        } else {
            // UXR_AGENT_LOG_ERROR(
            //     UXR_DECORATE_RED("Failed to send message"),
            //     "rpmsg_send returned: %d",
            //     ret);
            transport_rc = eprosima::uxr::TransportRc::server_error;
            return -1;
        }
    };

    try
    {
        // Create a CustomEndPoint (even if empty)
        eprosima::uxr::CustomEndPoint custom_endpoint;

        // Instantiate the CustomAgent
        eprosima::uxr::CustomAgent custom_agent(
            "OpenAMP",          // Transport name
            &custom_endpoint,   // Custom endpoint
            mw_kind,
            false,              // Framing (set to false if not needed)
            init_function,
            fini_function,
            send_msg_function,
            recv_msg_function);

        int verbosity = 0; // Declare verbosity variable here

        // Check for --mavlink flag and create filtered argv
        // This prevents getopt from seeing the unknown long option
        std::vector<char*> filtered_argv;
        filtered_argv.push_back(argv[0]);  // Program name

        // Static storage for QGC IP string (must outlive AiThreadConfig)
#ifdef ENABLE_AI_CAMERA
        static char s_qgc_ip[64] = {};
#endif

        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--mavlink") == 0) {
                enable_mavlink_forward = true;
                fprintf(stderr, "MAVLink forwarding enabled\n");
#ifdef ENABLE_AI_CAMERA
            } else if (strcmp(argv[i], "--qgc-ip") == 0 && i + 1 < argc) {
                ++i;
                strncpy(s_qgc_ip, argv[i], sizeof(s_qgc_ip) - 1);
                g_ai_cfg.qgc_dest_ip = s_qgc_ip;
                fprintf(stderr, "QGC H.264 stream target: %s:5600\n", s_qgc_ip);
            } else if (strcmp(argv[i], "--qgc-bitrate") == 0 && i + 1 < argc) {
                ++i;
                g_ai_cfg.qgc_bitrate = atoi(argv[i]);
                fprintf(stderr, "QGC H.264 bitrate: %d kbps\n", g_ai_cfg.qgc_bitrate / 1000);
            } else if (strcmp(argv[i], "--ai-model") == 0 && i + 1 < argc) {
                ++i;
                g_ai_cfg.model_dir = argv[i];
            } else if (strcmp(argv[i], "--ai-camera") == 0 && i + 1 < argc) {
                ++i;
                g_ai_cfg.camera_device = argv[i];
#endif
            } else {
                filtered_argv.push_back(argv[i]);
            }
        }

        int filtered_argc = static_cast<int>(filtered_argv.size());

        // Parse command-line arguments with getopt using filtered argv
        int opt;
        optind = 1;  // Reset getopt

        while ((opt = getopt(filtered_argc, filtered_argv.data(), "v:c:u:")) != -1) {
            switch (opt) {
                case 'v':
                    verbosity = atoi(optarg);
                    if (verbosity < 0 || verbosity > 6) {
                        fprintf(stderr, "Invalid verbosity level. Must be between 0 and 6.\n");
                        return 1;
                    }
                    break;
                case 'c':
                    comm_core_rpc = atoi(optarg);
                    if (comm_core_rpc < 0 || comm_core_rpc >= ARRAY_SIZE(ids)) {
                        fprintf(stderr, "Invalid RPC comm_core value. Must be between 0 and %lu.\n", ARRAY_SIZE(ids) - 1);
                        return 1;
                    }
                    break;
                case 'u':
                    comm_core_uxrce = atoi(optarg);
                    if (comm_core_uxrce < 0 || comm_core_uxrce >= ARRAY_SIZE(ids)) {
                        fprintf(stderr, "Invalid UXRCE comm_core value. Must be between 0 and %lu.\n", ARRAY_SIZE(ids) - 1);
                        return 1;
                    }
                    break;
                default:
                    fprintf(stderr,
                        "Usage: %s [-v verbosity] [-c rpc_core] [-u uxrce_core] [--mavlink]"
#ifdef ENABLE_AI_CAMERA
                        " [--qgc-ip <IP>] [--qgc-bitrate <bps>] [--ai-model <dir>] [--ai-camera <dev>]"
#endif
                        "\n", argv[0]);
                    return 1;
            }
        }

        // Set verbosity level
        custom_agent.set_verbose_level(verbosity);

        fprintf(stderr, "rpc_core=%d uxrce_core=%d\n", comm_core_rpc, comm_core_uxrce);

        // Start the agent
        custom_agent.start();

        // Wait for termination signal
        int n_signal = 0;
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        sigwait(&signals, &n_signal);

        // Use _exit(0) instead of custom_agent.stop() + return.
        // custom_agent.stop() triggers OpenAMP/DDS destructors that crash with
        // SIGSEGV during teardown (UIO/vring cleanup while CR8 is still running).
        // _exit(0) bypasses all C++ destructors — CR8 detects the RPMsg disconnect
        // naturally. systemd sees exit code 0 → Restart=on-failure does not restart.
        //
        /* Clear DRIVER_OK first so CR8 re-enters its "waiting for CA55" poll loop
         * instead of continuing to fire MHU interrupts into an unresponsive kernel.
         *
         * Then spin-wait 600ms: CR8 polls DRIVER_OK every 200ms. Without this delay,
         * systemd restarts the new agent within ~100ms and re-sets DRIVER_OK before
         * CR8's 200ms poll fires — CR8 never sees the disconnect and stays stuck with
         * stale endpoints. 600ms guarantees at least two CR8 poll cycles see 0.
         * sigwait() is not a signal handler so nanosleep()/clock_gettime() are safe. */
        platform_clear_driver_ok();
        {
            struct timespec _start, _now, _delay;
            clock_gettime(CLOCK_MONOTONIC, &_start);
            for (;;) {
                _delay.tv_sec = 0; _delay.tv_nsec = 10 * 1000 * 1000L; /* 10ms */
                nanosleep(&_delay, NULL);
                clock_gettime(CLOCK_MONOTONIC, &_now);
                long _ms = (_now.tv_sec  - _start.tv_sec)  * 1000L
                         + (_now.tv_nsec - _start.tv_nsec) / 1000000L;
                if (_ms >= 600) break;
            }
        }
        _exit(0);
    }
    catch (const std::exception& e)
    {
        return 1;
    }
}
