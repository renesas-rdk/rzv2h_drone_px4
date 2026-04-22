/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "px4_openamp_rpc_server.h"
#include "param_converter.hpp"

#include "../../src/rpc/openamp_rpc.h"

#include <atomic>

#include <openamp/rpmsg.h>
#include <openamp/rpmsg_virtio.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <metal/io.h>
#include <metal/utilities.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <queue>
#include <string>
#include <utility>
#include <vector>

// External OpenAMP transport mutex (defined in custom_agent.cpp)
extern pthread_mutex_t openamp_transport_mutex;

namespace
{

using byte = std::uint8_t;

bool handle_message_buffer(struct rpmsg_endpoint *ept, const std::vector<byte> &message);
bool send_error(struct rpmsg_endpoint *ept,
                const px4_rpc_header &request,
                int32_t errno_value);

static int rpmsg_send_response_retry(struct rpmsg_endpoint *ept,
                                     const void *data,
                                     std::size_t len,
                                     uint32_t *no_buf_retries);

// Track file descriptors that need transparent BSON conversion
static std::map<int, std::vector<uint8_t>> g_param_fd_bson_cache;
static std::map<int, std::vector<uint8_t>> g_param_fd_write_buffer;
static std::map<int, std::string> g_param_fd_path;

// Directory handle table for OPENDIR/READDIR/CLOSEDIR RPC
struct DirEntryCache {
    std::vector<struct dirent> entries;
    size_t index = 0;
};
static constexpr int kMaxDirHandles = 16;
static DirEntryCache *s_dir_handles[kMaxDirHandles] = {};
static pthread_mutex_t s_dir_mutex = PTHREAD_MUTEX_INITIALIZER;

static int dir_handle_alloc(const std::vector<struct dirent> &entries)
{
    pthread_mutex_lock(&s_dir_mutex);
    for (int i = 0; i < kMaxDirHandles; ++i) {
        if (s_dir_handles[i] == nullptr) {
            s_dir_handles[i] = new (std::nothrow) DirEntryCache{entries, 0};
            if (!s_dir_handles[i]) {
                pthread_mutex_unlock(&s_dir_mutex);
                return -1;
            }
            pthread_mutex_unlock(&s_dir_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&s_dir_mutex);
    return -1;
}

static DirEntryCache *dir_handle_get(int handle)
{
    if (handle < 0 || handle >= kMaxDirHandles) { return nullptr; }
    pthread_mutex_lock(&s_dir_mutex);
    DirEntryCache *cache = s_dir_handles[handle];
    pthread_mutex_unlock(&s_dir_mutex);
    return cache;
}

static void dir_handle_free(int handle)
{
    if (handle < 0 || handle >= kMaxDirHandles) { return; }
    pthread_mutex_lock(&s_dir_mutex);
    DirEntryCache *cache = s_dir_handles[handle];
    s_dir_handles[handle] = nullptr;
    pthread_mutex_unlock(&s_dir_mutex);
    if (cache) {
        delete cache;
    }
}

static std::atomic<bool> s_system_reboot_scheduled{false};
static std::atomic<uint32_t> s_rpc_resp_no_buf_count{0};
static std::atomic<uint32_t> s_rpc_resp_send_fail_count{0};
static std::atomic<uint64_t> s_rpc_resp_peak_send_us{0};
static std::atomic<uint64_t> s_rpc_req_peak_total_us{0};

static uint64_t monotonic_now_us()
{
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

static void update_peak(std::atomic<uint64_t> &peak, uint64_t value)
{
    uint64_t current = peak.load();

    while (value > current && !peak.compare_exchange_weak(current, value)) {
    }
}

static void *system_reboot_thread_entry(void *)
{
    // Leave enough time for COMMAND_ACK to traverse CR8 -> SiK -> QGC before Linux drops the link.
    // SiK links on this platform can show ~1-2 s ping latency, so keep a wider margin.
    // usleep(5000000);
    ::sync();

    int ret = ::system("systemctl reboot >/dev/null 2>&1");

    if (ret < 0) {
        const int fallback_ret = ::system("/sbin/reboot >/dev/null 2>&1");
        (void)fallback_ret;
    }

    return nullptr;
}

static void schedule_system_reboot()
{
    bool expected = false;

    if (!s_system_reboot_scheduled.compare_exchange_strong(expected, true)) {
        return;
    }

    pthread_t thread{};

    if (pthread_create(&thread, nullptr, system_reboot_thread_entry, nullptr) == 0) {
        pthread_detach(thread);

    } else {
        s_system_reboot_scheduled.store(false);
    }
}

// Limit concurrent param file handles to prevent unbounded memory growth
static constexpr size_t MAX_PARAM_FDS = 8;
// Use global transport mutex instead of local mutex to coordinate with UXRCE-DDS
// static pthread_mutex_t g_rpmsg_send_mutex = PTHREAD_MUTEX_INITIALIZER;

// Virtual paths as seen by CR8 (upstream /fs/microsd/ convention)
static constexpr const char *kParamsBinaryPath    = "/fs/microsd/params";
static constexpr const char *kParamsBinaryPathEtc = "/fs/microsd/etc/params";
static constexpr const char *kParamsStagingPath   = "/tmp/params.txt";

// Physical paths on CA55 filesystem (/drone-data/cr8_data/ on dedicated data partition)
static constexpr const char *kPhysParamsBinaryPath = "/drone-data/cr8_data/params";

// Maximum data bytes per FS_READ RPC response to fit within one RPMsg message.
// RPMsg buffer = 512 B; rpmsg_hdr = 16 B; px4_rpc_header = 24 B → max data = 472 B.
static constexpr size_t kMaxRpcReadPayload = 472U;

/**
 * Translate a virtual CR8 path to the physical path on the CA55 filesystem.
 * CR8 uses the upstream PX4 convention (/fs/microsd/...).
 * On CA55, all CR8 data lives under /drone-data/cr8_data/ (dedicated data partition).
 */
static std::string translate_rpc_path(const std::string &path)
{
    static constexpr const char kVirtualPrefix[] = "/fs/microsd/";
    if (path.compare(0, sizeof(kVirtualPrefix) - 1, kVirtualPrefix) == 0) {
        return std::string("/drone-data/cr8_data/") + path.substr(sizeof(kVirtualPrefix) - 1);
    }
    return path;
}

/**
 * Translate CR8 open(2) flags to Linux open(2) flags.
 *
 * Two flag conventions arrive from CR8:
 *
 * A) FreeRTOS-POSIX (used by PX4 px4_open() calls):
 *   O_RDONLY = 0x2000   O_WRONLY = 0x8000   O_RDWR  = 0xA000
 *   O_CREAT  = 0x0002   O_TRUNC  = 0x0040   O_APPEND = 0x0100
 *
 * B) Newlib arm-none-eabi (used by fopen/fdopen from C library):
 *   O_RDONLY = 0x0000   O_WRONLY = 0x0001   O_RDWR  = 0x0002
 *   O_CREAT  = 0x0200   O_TRUNC  = 0x0400   O_APPEND = 0x0008
 *   O_EXCL   = 0x0800
 *
 * Linux (CA55 target):
 *   O_RDONLY = 0x0000   O_WRONLY = 0x0001   O_RDWR  = 0x0002
 *   O_CREAT  = 0x0040   O_TRUNC  = 0x0200   O_APPEND = 0x0400
 *
 * Detect convention by checking FreeRTOS access-mode bits (0xE000):
 *   if any of bits 13-15 are set → FreeRTOS convention (A)
 *   otherwise                   → Newlib convention (B)
 *
 * Without translation, fopen("w") sends 0x601 (newlib O_WRONLY|O_CREAT|O_TRUNC)
 * which maps to O_RDONLY on Linux → ::open() on a non-existent file returns
 * ENOENT → $log$.txt can never be created → Log Download list is always empty.
 */
static int translate_rtos_to_linux_flags(int rtos_flags)
{
    int linux_flags;

    if (rtos_flags & 0xE000) {
        // Convention A: FreeRTOS-POSIX access mode bits in the high nibble
        if ((rtos_flags & 0xA000) == 0xA000) {
            linux_flags = O_RDWR;           // FreeRTOS O_RDWR
        } else if (rtos_flags & 0x8000) {
            linux_flags = O_WRONLY;         // FreeRTOS O_WRONLY
        } else {
            linux_flags = O_RDONLY;         // FreeRTOS O_RDONLY
        }
        if (rtos_flags & 0x0002) linux_flags |= O_CREAT;   // FreeRTOS O_CREAT → Linux O_CREAT
        if (rtos_flags & 0x0040) linux_flags |= O_TRUNC;   // FreeRTOS O_TRUNC → Linux O_TRUNC
        if (rtos_flags & 0x0100) linux_flags |= O_APPEND;  // FreeRTOS O_APPEND → Linux O_APPEND
    } else {
        // Convention B: Newlib arm-none-eabi — access mode same as Linux (0/1/2)
        linux_flags = rtos_flags & O_ACCMODE;               // O_RDONLY/O_WRONLY/O_RDWR identical
        if (rtos_flags & 0x0200) linux_flags |= O_CREAT;   // Newlib O_CREAT(0x200) → Linux O_CREAT
        if (rtos_flags & 0x0400) linux_flags |= O_TRUNC;   // Newlib O_TRUNC(0x400) → Linux O_TRUNC
        if (rtos_flags & 0x0008) linux_flags |= O_APPEND;  // Newlib O_APPEND(0x008) → Linux O_APPEND
        if (rtos_flags & 0x0800) linux_flags |= O_EXCL;    // Newlib O_EXCL(0x800)  → Linux O_EXCL
    }

    return linux_flags;
}

static bool is_expected_open_probe_miss(const char *path, int rtos_flags, int err)
{
    if (path == nullptr || err != ENOENT) {
        return false;
    }

    const int linux_flags = translate_rtos_to_linux_flags(rtos_flags);
    const bool read_only_no_create = ((linux_flags & O_ACCMODE) == O_RDONLY) && ((linux_flags & O_CREAT) == 0);

    if (!read_only_no_create) {
        return false;
    }

    if (std::strncmp(path, "/fs/microsd/log/", 16) == 0) {
        return true;
    }

    return (std::strcmp(path, "/fs/microsd/etc/logging/logger_topics.txt") == 0);
}

static bool should_skip_remote_logger_topics(const char *path, int rtos_flags)
{
    if (path == nullptr || std::strcmp(path, "/fs/microsd/etc/logging/logger_topics.txt") != 0) {
        return false;
    }

    const int linux_flags = translate_rtos_to_linux_flags(rtos_flags);
    return ((linux_flags & O_ACCMODE) == O_RDONLY) && ((linux_flags & O_CREAT) == 0);
}

static bool is_primary_params_path(const char *path)
{
    if (path == nullptr) {
        return false;
    }

    return (std::strcmp(path, kParamsBinaryPath) == 0) ||
           (std::strcmp(path, kParamsBinaryPathEtc) == 0);
}

bool ensure_parent_dirs(const std::string &path)
{
    std::size_t slash_pos = path.find_last_of('/');

    if (slash_pos == std::string::npos || slash_pos == 0) {
        return true;
    }

    std::string dir = path.substr(0, slash_pos);
    std::size_t pos = 0;

    while (true) {
        pos = dir.find('/', pos + 1);
        std::string sub = dir.substr(0, pos);

        if (sub.empty() || sub == "/") {
            if (pos == std::string::npos) {
                break;
            }
            continue;
        }

        struct stat st {};

        if (::stat(sub.c_str(), &st) != 0) {
            if (errno != ENOENT) {
                return false;
            }

            if (::mkdir(sub.c_str(), 0777) != 0 && errno != EEXIST) {
                return false;
            }

        } else if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return false;
        }

        if (pos == std::string::npos) {
            break;
        }
    }

    return true;
}

bool atomic_overwrite_file(const std::string &target, const std::string &data)
{
    if (!ensure_parent_dirs(target)) {
        return false;
    }

    std::string tmp_path = target + ".tmp";
    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0) {
        std::fprintf(stderr, "[RPC][PARAM] open %s failed: %d\n", tmp_path.c_str(), errno);
        return false;
    }

    bool ok = true;

    if (!data.empty()) {
        ssize_t wret = ::write(fd, data.data(), data.size());

        if (wret < 0 || static_cast<size_t>(wret) != data.size()) {
            std::fprintf(stderr, "[RPC][PARAM] write %s failed: %d\n", tmp_path.c_str(), errno);
            ok = false;
        }
    }

    if (::fsync(fd) != 0) {
        std::fprintf(stderr, "[RPC][PARAM] fsync %s failed: %d\n", tmp_path.c_str(), errno);
        ok = false;
    }

    ::close(fd);

    if (ok) {
        if (::rename(tmp_path.c_str(), target.c_str()) != 0) {
            int rename_err = errno;
            std::fprintf(stderr, "[RPC][PARAM] rename %s -> %s failed: %d\n",
                         tmp_path.c_str(), target.c_str(), rename_err);

            // Retry once after re-ensuring parents in case the path disappeared
            if (rename_err == ENOENT) {
                ensure_parent_dirs(target);

                if (::rename(tmp_path.c_str(), target.c_str()) == 0) {
                    return true;
                }

                rename_err = errno;
            }

            // Last-resort fallback: write directly to the target (non-atomic)
            int fd_direct = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);

            if (fd_direct >= 0) {
                ssize_t wret = ::write(fd_direct, data.data(), data.size());

                if (wret < 0 || static_cast<size_t>(wret) != data.size()) {
                    std::fprintf(stderr, "[RPC][PARAM] direct write %s failed: %d\n",
                                 target.c_str(), errno);
                } else {
                    ::fsync(fd_direct);
                    ok = true;
                }

                ::close(fd_direct);
            } else {
                std::fprintf(stderr, "[RPC][PARAM] direct open %s failed after rename err=%d: %d\n",
                             target.c_str(), rename_err, errno);
            }
        }
    } else {
        ::unlink(tmp_path.c_str());
    }

    return ok;
}

void hexdump_bytes(const char *tag, const byte *data, std::size_t len, std::size_t max_len = 64)
{
    if (!data || len == 0) {
        std::fprintf(stderr, "[RPC][%s] empty payload\n", tag ? tag : "dump");
        return;
    }

    std::size_t dump_len = std::min<std::size_t>(len, max_len);
    std::fprintf(stderr, "[RPC][%s] %zu bytes (show %zu):", tag ? tag : "dump", len, dump_len);

    for (std::size_t i = 0; i < dump_len; ++i) {
        std::fprintf(stderr, " %02x", data[i]);
    }

    if (len > dump_len) {
        std::fprintf(stderr, " ...");
    }

    std::fprintf(stderr, "\n");
}

bool copy_rpmsg_buffer(struct rpmsg_endpoint *ept,
                       const void *data,
                       std::size_t len,
                       std::vector<byte> &out)
{
    out.assign(len, 0);

    if (!data || len == 0) {
        return true;
    }

    struct rpmsg_device *rdev = ept ? ept->rdev : nullptr;
    auto *rvdev = rdev
                  ? reinterpret_cast<struct rpmsg_virtio_device *>(
                        metal_container_of(rdev, struct rpmsg_virtio_device, rdev))
                  : nullptr;
    struct metal_io_region *io = rvdev ? rvdev->shbuf_io : nullptr;

    if (!io) {
        std::fprintf(stderr, "[RPC] no shbuf_io for rpmsg device; aborting copy\n");
        return false;
    }

    unsigned long offset = metal_io_virt_to_offset(io, const_cast<void *>(data));

    if (offset == METAL_BAD_OFFSET) {
        std::fprintf(stderr,
                     "[RPC] metal_io_virt_to_offset failed (data=%p len=%zu)\n",
                     data, len);
        return false;
    }

    int copied = metal_io_block_read(io, offset, out.data(), static_cast<int>(len));

    if (copied != static_cast<int>(len)) {
        std::fprintf(stderr,
                     "[RPC] metal_io_block_read failed (off=0x%lx len=%zu ret=%d)\n",
                     offset, len, copied);
        return false;
    }

    return true;
}

class AsyncRPCWorker
{
public:
    static constexpr size_t NUM_WORKERS = 4;
    static constexpr size_t MAX_QUEUE_SIZE = 32;

    bool init()
    {
        if (_initialized.load()) {
            return true;
        }

        if (pthread_mutex_init(&_mutex, nullptr) != 0) {
            return false;
        }

        if (pthread_cond_init(&_cond, nullptr) != 0) {
            pthread_mutex_destroy(&_mutex);
            return false;
        }

        _shutdown.store(false);
        _workers_started = 0;

        for (size_t i = 0; i < NUM_WORKERS; ++i) {
            if (pthread_create(&_threads[i], nullptr, &AsyncRPCWorker::worker_entry, this) != 0) {
                _shutdown.store(true);
                pthread_cond_broadcast(&_cond);
                break;
            }

            _workers_started++;
        }

        if (_workers_started != NUM_WORKERS) {
            shutdown();
            return false;
        }

        _initialized.store(true);
        return true;
    }

    void shutdown()
    {
        if (!_initialized.load() && _workers_started == 0) {
            return;
        }

        _shutdown.store(true);
        pthread_mutex_lock(&_mutex);
        pthread_cond_broadcast(&_cond);
        pthread_mutex_unlock(&_mutex);

        for (size_t i = 0; i < _workers_started; ++i) {
            pthread_join(_threads[i], nullptr);
        }

        _workers_started = 0;

        pthread_mutex_destroy(&_mutex);
        pthread_cond_destroy(&_cond);

        while (!_queue.empty()) {
            _queue.pop();
        }

        _initialized.store(false);
    }

    bool enqueue(struct rpmsg_endpoint *ept,
                 const void *data,
                 std::size_t len,
                 const px4_rpc_header &header)
    {
        if (!_initialized.load()) {
            send_error(ept, header, EBUSY);
            return false;
        }

        WorkItem item;
        item.ept = ept;

        if (!copy_rpmsg_buffer(ept, data, len, item.message)) {
            send_error(ept, header, EIO);
            return false;
        }

        pthread_mutex_lock(&_mutex);

        if (_queue.size() >= MAX_QUEUE_SIZE) {
            pthread_mutex_unlock(&_mutex);
            send_error(ept, header, EBUSY);
            return false;
        }

        _queue.push(std::move(item));
        pthread_cond_signal(&_cond);
        pthread_mutex_unlock(&_mutex);
        return true;
    }

private:
    struct WorkItem {
        struct rpmsg_endpoint *ept{};
        std::vector<byte> message;
    };

    pthread_t _threads[NUM_WORKERS]{};
    std::queue<WorkItem> _queue;
    pthread_mutex_t _mutex{};
    pthread_cond_t _cond{};
    std::atomic<bool> _shutdown{false};
    std::atomic<bool> _initialized{false};
    size_t _workers_started{0};

    static void *worker_entry(void *arg)
    {
        auto *self = static_cast<AsyncRPCWorker *>(arg);

        if (!self) {
            return nullptr;
        }

        self->run();
        return nullptr;
    }

    void run()
    {
        while (true) {
            WorkItem item;

            pthread_mutex_lock(&_mutex);

            while (_queue.empty() && !_shutdown.load()) {
                pthread_cond_wait(&_cond, &_mutex);
            }

            if (_shutdown.load()) {
                pthread_mutex_unlock(&_mutex);
                break;
            }

            item = std::move(_queue.front());
            _queue.pop();
            pthread_mutex_unlock(&_mutex);

            handle_message_buffer(item.ept, item.message);
        }
    }
};

static AsyncRPCWorker g_async_worker;

static int rpmsg_send_response_retry(struct rpmsg_endpoint *ept,
                                     const void *data,
                                     std::size_t len,
                                     uint32_t *no_buf_retries)
{
    if (!ept || !data || len == 0) {
        return -EINVAL;
    }

    if (no_buf_retries) {
        *no_buf_retries = 0;
    }

    if (ept->dest_addr == RPMSG_ADDR_ANY || ept->rdev == nullptr) {
        return -ENOTCONN;
    }

    int ret = -EAGAIN;
    constexpr int kMaxTries = 3;

    for (int attempt = 0; attempt < kMaxTries; ++attempt) {
        /* Keep the shared transport mutex held only around a non-blocking trysend.
         * Holding it across rpmsg_send() can stall UXRCE/RPC traffic long enough
         * to recreate the CR8-side starvation that drops QGC heartbeats. */
        pthread_mutex_lock(&openamp_transport_mutex);
        ret = rpmsg_trysend(ept, data, len);
        pthread_mutex_unlock(&openamp_transport_mutex);

        if (ret >= 0 || ret != RPMSG_ERR_NO_BUFF) {
            return ret;
        }

        if (no_buf_retries) {
            ++(*no_buf_retries);
        }

        usleep(1000U * static_cast<useconds_t>(attempt + 1));
    }

    return ret;
}

bool send_response(struct rpmsg_endpoint *ept,
                   const px4_rpc_header &request,
                   int32_t return_value,
                   int32_t errno_value,
                   const void *payload,
                   std::size_t payload_len)
{
    px4_rpc_header header{};
    header.magic = PX4_RPC_MAGIC;
    header.category = request.category;
    header.opcode = request.opcode;
    header.request_id = request.request_id;
    header.return_value = return_value;
    header.errno_value = errno_value;
    header.payload_size = static_cast<std::uint32_t>(payload_len);

    std::vector<byte> buffer(sizeof(header) + payload_len);
    std::memcpy(buffer.data(), &header, sizeof(header));

    if (payload_len > 0 && payload) {
        std::memcpy(buffer.data() + sizeof(header), payload, payload_len);
    }

    const uint64_t send_start_us = monotonic_now_us();
    uint32_t no_buf_retries = 0;
    int ret = rpmsg_send_response_retry(ept, buffer.data(), buffer.size(), &no_buf_retries);
    const uint64_t send_us = monotonic_now_us() - send_start_us;

    if (no_buf_retries > 0) {
        s_rpc_resp_no_buf_count.fetch_add(no_buf_retries);
    }

    if (ret < 0) {
        s_rpc_resp_send_fail_count.fetch_add(1);
    }

    update_peak(s_rpc_resp_peak_send_us, send_us);

    if (no_buf_retries > 0 || ret < 0 || send_us >= 5000U) {
        std::fprintf(stderr,
                     "[RPC][RESP] req_id=%ld cat=%u op=%u payload=%zu send_us=%llu no_buf_retries=%u ret=%d total_no_buf=%u send_fail=%u peak_send_us=%llu\n",
                     static_cast<long>(request.request_id),
                     static_cast<unsigned>(request.category),
                     static_cast<unsigned>(request.opcode),
                     payload_len,
                     static_cast<unsigned long long>(send_us),
                     no_buf_retries,
                     ret,
                     s_rpc_resp_no_buf_count.load(),
                     s_rpc_resp_send_fail_count.load(),
                     static_cast<unsigned long long>(s_rpc_resp_peak_send_us.load()));
    }

    return ret >= 0;
}

bool send_error(struct rpmsg_endpoint *ept,
                const px4_rpc_header &request,
                int32_t errno_value)
{
    return send_response(ept, request, -1, errno_value, nullptr, 0);
}

bool ensure_payload(const px4_rpc_header &request,
                    std::size_t total_len,
                    std::size_t payload_len)
{
    return total_len >= sizeof(px4_rpc_header) &&
           payload_len >= request.payload_size;
}

bool handle_socket_rpc(struct rpmsg_endpoint *ept,
                       const px4_rpc_header &request,
                       const byte *payload,
                       std::size_t payload_len)
{
    switch (request.opcode) {
    case PX4_RPC_SOCKET: {
            struct __attribute__((packed)) Request {
                int32_t domain;
                int32_t type;
                int32_t protocol;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            errno = 0;
            int fd = ::socket(req->domain, req->type, req->protocol);
            int err = (fd < 0) ? errno : 0;
            return send_response(ept, request, fd, err, nullptr, 0);
        }

    case PX4_RPC_BIND:
    case PX4_RPC_CONNECT: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                int32_t addrlen;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const byte *addr_ptr = payload + sizeof(Request);
            std::size_t addr_available = payload_len - sizeof(Request);

            if (req->addrlen < 0 || static_cast<std::size_t>(req->addrlen) > addr_available) {
                return send_error(ept, request, EINVAL);
            }

            const struct sockaddr *addr = nullptr;

            if (req->addrlen > 0) {
                addr = reinterpret_cast<const struct sockaddr *>(addr_ptr);
            }

            int ret = (request.opcode == PX4_RPC_BIND)
                      ? ::bind(req->fd, addr, static_cast<socklen_t>(req->addrlen))
                      : ::connect(req->fd, addr, static_cast<socklen_t>(req->addrlen));
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_LISTEN: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                int32_t backlog;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            int ret = ::listen(req->fd, req->backlog);
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_ACCEPT: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                int32_t addrlen;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            socklen_t max_len = 0;

            if (req->addrlen > 0) {
                max_len = static_cast<socklen_t>(req->addrlen);
            }

            std::vector<byte> addr_buffer(max_len ? max_len : sizeof(struct sockaddr_storage));
            socklen_t actual_len = max_len;
            struct sockaddr *addr_ptr = max_len > 0
                                         ? reinterpret_cast<struct sockaddr *>(addr_buffer.data())
                                         : nullptr;

            errno = 0;
            int new_fd = ::accept(req->fd, addr_ptr, max_len > 0 ? &actual_len : nullptr);
            int err = (new_fd < 0) ? errno : 0;

            int32_t reported_len = (new_fd >= 0 && max_len > 0)
                                   ? static_cast<int32_t>(actual_len)
                                   : 0;
            std::vector<byte> response_payload(sizeof(int32_t));
            std::memcpy(response_payload.data(), &reported_len, sizeof(int32_t));

            if (reported_len > 0 && new_fd >= 0) {
                response_payload.insert(response_payload.end(),
                                        addr_buffer.begin(),
                                        addr_buffer.begin() + reported_len);
            }

            return send_response(ept, request, new_fd, err,
                                 response_payload.data(), response_payload.size());
        }

    case PX4_RPC_SETSOCKOPT: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                int32_t level;
                int32_t optname;
                int32_t optlen;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const byte *opt_ptr = payload + sizeof(Request);
            std::size_t opt_available = payload_len - sizeof(Request);

            if (req->optlen < 0 || static_cast<std::size_t>(req->optlen) > opt_available) {
                return send_error(ept, request, EINVAL);
            }

            int ret = ::setsockopt(req->fd, req->level, req->optname,
                                   req->optlen > 0 ? opt_ptr : nullptr,
                                   static_cast<socklen_t>(req->optlen));
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_SHUTDOWN: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                int32_t how;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            int ret = ::shutdown(req->fd, req->how);
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_SEND: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                int32_t flags;
                std::uint32_t len;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const byte *data_ptr = payload + sizeof(Request);
            std::size_t available = payload_len - sizeof(Request);

            if (req->len > available) {
                return send_error(ept, request, EINVAL);
            }

            ssize_t ret = ::send(req->fd, data_ptr, req->len, req->flags);
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, static_cast<int32_t>(ret), err, nullptr, 0);
        }

    case PX4_RPC_RECV: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                int32_t flags;
                std::uint32_t len;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            std::vector<byte> buffer(req->len);

            ssize_t ret = 0;
            int err = 0;

            if (req->len > 0) {
                ret = ::recv(req->fd, buffer.data(), req->len, req->flags);
                err = (ret < 0) ? errno : 0;
            } else {
                ret = 0;
            }

            std::size_t payload_size = (ret > 0) ? static_cast<std::size_t>(ret) : 0;
            return send_response(ept, request,
                                 static_cast<int32_t>(ret), err,
                                 buffer.data(), payload_size);
        }

    case PX4_RPC_GETSOCKNAME:
    case PX4_RPC_GETPEERNAME: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                int32_t addrlen;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            socklen_t capacity = req->addrlen > 0 ? static_cast<socklen_t>(req->addrlen) : 0;
            std::vector<byte> addr_buffer(capacity ? capacity : sizeof(struct sockaddr_storage));
            socklen_t actual_len = capacity;
            struct sockaddr *addr_ptr = capacity > 0
                                         ? reinterpret_cast<struct sockaddr *>(addr_buffer.data())
                                         : nullptr;

            int ret = (request.opcode == PX4_RPC_GETSOCKNAME)
                      ? ::getsockname(req->fd, addr_ptr, capacity > 0 ? &actual_len : nullptr)
                      : ::getpeername(req->fd, addr_ptr, capacity > 0 ? &actual_len : nullptr);
            int err = (ret < 0) ? errno : 0;

            int32_t reported_len = (ret == 0 && capacity > 0) ? static_cast<int32_t>(actual_len) : 0;
            std::vector<byte> response_payload(sizeof(int32_t));
            std::memcpy(response_payload.data(), &reported_len, sizeof(int32_t));

            if (reported_len > 0 && ret == 0) {
                response_payload.insert(response_payload.end(),
                                        addr_buffer.begin(),
                                        addr_buffer.begin() + reported_len);
            }

            return send_response(ept, request, ret, err,
                                 response_payload.data(), response_payload.size());
        }

    case PX4_RPC_SOCKET_IOCTL: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                std::uint32_t reserved0;
                std::uint64_t request_code;
                std::uint32_t write_len;
                std::uint32_t read_len;
                std::uint64_t scalar_value;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const byte *data_ptr = payload + sizeof(Request);
            std::size_t data_available = payload_len - sizeof(Request);

            if (req->write_len > data_available) {
                return send_error(ept, request, EINVAL);
            }

            std::size_t buffer_len = std::max<std::size_t>(req->write_len, req->read_len);
            std::vector<byte> io_buffer(buffer_len);
            void *arg_ptr = reinterpret_cast<void *>(req->scalar_value);

            if (req->write_len > 0 || req->read_len > 0) {
                if (req->write_len > 0) {
                    std::memcpy(io_buffer.data(), data_ptr, req->write_len);
                } else {
                    std::fill(io_buffer.begin(), io_buffer.end(), 0);
                }

                arg_ptr = io_buffer.data();
            }

            int ret = ::ioctl(req->fd, static_cast<unsigned long>(req->request_code), arg_ptr);
            int err = (ret < 0) ? errno : 0;

            std::size_t response_size = (ret == 0 && req->read_len > 0)
                                        ? static_cast<std::size_t>(req->read_len)
                                        : 0;

            return send_response(ept, request, ret, err,
                                 io_buffer.data(), response_size);
        }

    case PX4_RPC_CLOSE_SOCKET: {
            struct __attribute__((packed)) Request {
                int32_t fd;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            int ret = ::close(req->fd);
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    default:
        return send_error(ept, request, ENOTSUP);
    }
}


bool handle_fs_rpc(struct rpmsg_endpoint *ept,
                   const px4_rpc_header &request,
                   const byte *payload,
                   std::size_t payload_len)
{
    /* Copy payload into local buffer to avoid accessing rpmsg shared memory repeatedly */
    std::vector<byte> local_payload(payload_len);
    if (payload_len > 0 && payload) {
        memcpy(local_payload.data(), payload, payload_len);
        payload = local_payload.data();
    }
    switch (request.opcode) {
    case PX4_RPC_FS_OPEN: {
            /* Copy header safely to avoid alignment/device memory issues */
            if (payload_len < sizeof(uint32_t) * 3) {
                return send_error(ept, request, EINVAL);
            }
            uint32_t flags = 0, mode = 0, path_len = 0;
            /* Use memcpy to avoid unaligned access */
            memcpy(&flags, payload + 0, sizeof(uint32_t));
            memcpy(&mode, payload + 4, sizeof(uint32_t));
            memcpy(&path_len, payload + 8, sizeof(uint32_t));

            if (path_len == 0 || path_len > payload_len - (sizeof(uint32_t) * 3) || path_len > 4096) {
                return send_error(ept, request, EINVAL);
            }

            const char *path = reinterpret_cast<const char *>(payload + (sizeof(uint32_t) * 3));
            /* Copy path into a bounded buffer to avoid accessing RPMsg buffer beyond limits */
            constexpr size_t kMaxPath = 4096;
            if (path_len >= kMaxPath) {
                return send_error(ept, request, EINVAL);
            }
            char path_buf[kMaxPath];
            memcpy(path_buf, path, path_len);
            path_buf[path_len - 1] = '\0';

            if (should_skip_remote_logger_topics(path_buf, static_cast<int>(flags))) {
                std::fprintf(stderr,
                             "[RPC][FS_OPEN][PROBE_MISS] path='%s' flags=0x%x err=%d elapsed=0 us (forced fallback)\n",
                             path_buf, (unsigned)flags, ENOENT);
                return send_response(ept, request, -1, ENOENT, nullptr, 0);
            }

            // Check if this is the params file that needs transparent conversion
            bool needs_conversion = px4_param_converter::should_convert_params(path_buf);

            errno = 0;
            int fd = -1;
            struct timespec start_ts;
            clock_gettime(CLOCK_MONOTONIC, &start_ts);

            if (needs_conversion) {
                // For primary params path reads: serve the persistent BSON file directly.
                // MUST NOT use /tmp/params.txt staging (tmpfs is cleared on CA55 reboot).
                // param_load() calls param_reset_all_internal() BEFORE reading — if we
                // serve an empty staging file, ALL in-memory params (including calibration
                // from rc.board_defaults.cmds) are wiped before anything is imported.
                // By opening the physical BSON directly and skipping the bson_cache/write_buffer
                // maps, the READ handler falls through to ::read() → raw BSON served to CR8.
                if (is_primary_params_path(path_buf)) {
                    bool is_write_op = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0;
                    if (!is_write_op) {
                        std::string phys_path = translate_rpc_path(std::string(path_buf));
                        fd = ::open(phys_path.c_str(), O_RDONLY, static_cast<mode_t>(mode));
                        // NOT in g_param_fd_bson_cache → READ uses ::read() → raw BSON
                        // NOT in g_param_fd_write_buffer → CLOSE just calls ::close()
                        std::fprintf(stderr, "[RPC][PARAM] read-only open: '%s' -> '%s' fd=%d\n",
                                     path_buf, phys_path.c_str(), fd);
                        // skip the rest of needs_conversion block — fall through to error check
                        goto param_open_done;
                    }
                }

                {
                // Open .txt file instead for editing
                std::string txt_path;
                if (is_primary_params_path(path_buf)) {
                    // keep temporary writes in RAM
                    txt_path = kParamsStagingPath;
                } else {
                    txt_path = std::string(path_buf) + ".txt";
                }

                if (!ensure_parent_dirs(txt_path)) {
                    int err = (errno != 0) ? errno : EIO;
                    return send_error(ept, request, err);
                }

                fd = ::open(txt_path.c_str(), O_RDWR | O_CREAT, static_cast<mode_t>(mode));

                if (fd >= 0) {
                    // Check if we've exceeded max param file handles
                    if (g_param_fd_bson_cache.size() >= MAX_PARAM_FDS) {
                        std::fprintf(stderr, "[RPC][PARAM] Too many open param files (%zu >= %zu), closing fd=%d\n",
                                    g_param_fd_bson_cache.size(), MAX_PARAM_FDS, fd);
                        ::close(fd);
                        return send_error(ept, request, EMFILE);
                    }

                    g_param_fd_write_buffer[fd] = std::vector<uint8_t>(); // always buffer writes
                    g_param_fd_bson_cache[fd] = std::vector<uint8_t>();   // default empty cache
                    g_param_fd_path[fd] = txt_path;

                    // Read entire text file using low-level I/O (avoid C++ streams)
                    struct stat st;
                    if (::fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < 1024*1024) {
                        std::vector<char> text_buffer(st.st_size + 1);
                        ::lseek(fd, 0, SEEK_SET);
                        ssize_t bytes_read = ::read(fd, text_buffer.data(), st.st_size);

                        if (bytes_read > 0) {
                            text_buffer[bytes_read] = '\0';
                            std::string text_content(text_buffer.data(), bytes_read);

                            std::vector<uint8_t> bson_data;
                            if (px4_param_converter::text_to_bson(text_content, bson_data)) {
                                g_param_fd_bson_cache[fd] = std::move(bson_data);

                                // Reset file position to beginning
                                ::lseek(fd, 0, SEEK_SET);
                            } else {
                                // Failed to convert existing text; clear cache but keep buffering writes
                                g_param_fd_bson_cache[fd].clear();
                            }
                        }
                    }
                }
                } // end write-path scope
            } else {
                // Normal file operation — translate virtual CR8 path to physical CA55 path
                std::string phys_path = translate_rpc_path(path_buf);
                fd = ::open(phys_path.c_str(), translate_rtos_to_linux_flags(static_cast<int>(flags)), static_cast<mode_t>(mode));
            }

            param_open_done:
            int err = (fd < 0) ? errno : 0;

            struct timespec end_ts;
            clock_gettime(CLOCK_MONOTONIC, &end_ts);
            int64_t elapsed_ns = (int64_t)(end_ts.tv_sec - start_ts.tv_sec) * 1000000000LL
                               + (int64_t)(end_ts.tv_nsec - start_ts.tv_nsec);
            uint64_t elapsed_us = (elapsed_ns >= 0) ? (uint64_t)(elapsed_ns / 1000) : 0;

            if (fd < 0) {
                if (is_expected_open_probe_miss(path_buf, static_cast<int>(flags), err)) {
                    std::fprintf(stderr, "[RPC][FS_OPEN][PROBE_MISS] path='%s' flags=0x%x err=%d elapsed=%llu us\n",
                                 path_buf, (unsigned)flags, err, (unsigned long long)elapsed_us);

                } else {
                    std::fprintf(stderr, "[RPC][FS_OPEN] FAILED path='%s' flags=0x%x err=%d elapsed=%llu us\n",
                                 path_buf, (unsigned)flags, err, (unsigned long long)elapsed_us);
                }
            }

            bool sent = send_response(ept, request, fd, err, nullptr, 0);
            return sent;
        }

    case PX4_RPC_FS_READ: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                std::uint32_t len;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            // Cap to RPMsg transport limit: RPMSG_BUFFER_SIZE(512) - rpmsg_hdr(16) - px4_rpc_header(24)
            size_t actual_len = std::min(static_cast<size_t>(req->len), kMaxRpcReadPayload);
            std::vector<byte> buffer(actual_len);
            ssize_t ret = 0;
            int err = 0;

            // Check if this is a params file descriptor
            auto it = g_param_fd_bson_cache.find(req->fd);
            if (it != g_param_fd_bson_cache.end()) {
                // Return BSON data from cache (converted from text)
                const auto &bson_data = it->second;

                // Get current file position
                off_t current_pos = ::lseek(req->fd, 0, SEEK_CUR);
                if (current_pos < 0) {
                    err = errno;
                    ret = -1;
                } else {
                    // Calculate how much to return (capped to RPMsg limit)
                    size_t available = (current_pos < static_cast<off_t>(bson_data.size()))
                                     ? (bson_data.size() - current_pos)
                                     : 0;
                    size_t to_read = std::min(available, actual_len);

                    if (to_read > 0) {
                        std::memcpy(buffer.data(), bson_data.data() + current_pos, to_read);
                        ret = to_read;
                        // Advance file position
                        ::lseek(req->fd, current_pos + to_read, SEEK_SET);
                    } else {
                        ret = 0; // EOF
                    }
                }
            } else {
                // Normal file read (capped to RPMsg limit so response always fits in one message)
                if (actual_len > 0) {
                    ret = ::read(req->fd, buffer.data(), actual_len);
                    err = (ret < 0) ? errno : 0;
                }
            }

            std::size_t response_size = (ret > 0) ? static_cast<std::size_t>(ret) : 0;
            return send_response(ept, request,
                                 static_cast<int32_t>(ret), err,
                                 buffer.data(), response_size);
        }

    case PX4_RPC_FS_WRITE: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                std::uint32_t len;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const byte *data_ptr = payload + sizeof(Request);

            if (req->len > payload_len - sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            ssize_t ret = 0;
            int err = 0;

            // Check if this is a params file descriptor
            auto it = g_param_fd_write_buffer.find(req->fd);
            if (it != g_param_fd_write_buffer.end()) {
                // Buffer BSON data instead of writing to file
                // We'll convert and write on close
                auto &write_buffer = it->second;
                size_t old_size = write_buffer.size();
                write_buffer.insert(write_buffer.end(), data_ptr, data_ptr + req->len);
                ret = req->len; // Report success

                // Log every 1KB or on final chunk
                if ((write_buffer.size() / 1024) > (old_size / 1024) || write_buffer.size() < 512) {
                    std::fprintf(stderr, "[RPC][FS_WRITE] req_id=%ld fd=%d len=%u total_buffered=%zu (param)\n",
                                 (long)request.request_id, req->fd, (unsigned)req->len, write_buffer.size());
                }
            } else {
                // Normal file write
                ret = ::write(req->fd, data_ptr, req->len);
                err = (ret < 0) ? errno : 0;

                if (ret < 0) {
                    std::fprintf(stderr, "[RPC][FS_WRITE] FAILED fd=%d len=%u err=%d\n",
                                 req->fd, (unsigned)req->len, err);
                }
            }

            return send_response(ept, request, static_cast<int32_t>(ret), err, nullptr, 0);
        }

    case PX4_RPC_FS_CLOSE: {
            struct __attribute__((packed)) Request {
                int32_t fd;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            int ret = 0;
            int err = 0;

            struct timespec start_ts;
            clock_gettime(CLOCK_MONOTONIC, &start_ts);

            // Check if this is a params file descriptor
            auto write_it = g_param_fd_write_buffer.find(req->fd);
            bool is_param_fd = (write_it != g_param_fd_write_buffer.end());

            if (write_it != g_param_fd_write_buffer.end()) {
                // Convert buffered BSON data to text and write
                auto &bson_buffer = write_it->second;

                if (!bson_buffer.empty()) {
                    // Validate BSON completeness using the BSON document size header.
                    // The first 4 bytes of a BSON document are a little-endian uint32 declaring
                    // the TOTAL document size. If the buffer is smaller than this declared size,
                    // the async write chain was interrupted (e.g. rpmsg TX buffer unavailable
                    // mid-chain) and we received only PARTIAL data.
                    // CRITICAL: NEVER write a partial BSON — it would be "fixed up" to look
                    // valid with wrong size and only N of 100+ params saved, causing the
                    // remaining params (including PID values from QGC) to revert to defaults.
                    bool skip_disk_write = false;

                    if (bson_buffer.size() >= 4) {
                        uint32_t doc_size = bson_buffer[0] | (bson_buffer[1] << 8) | (bson_buffer[2] << 16) | (bson_buffer[3] << 24);

                        if (doc_size == 0) {
                            // Zero size header: fix up to match what was received (legacy behaviour)
                            doc_size = static_cast<uint32_t>(bson_buffer.size());
                            std::fprintf(stderr, "[RPC][PARAM] fixing zero BSON size header -> %u (fd=%d)\n",
                                         doc_size, req->fd);
                            const_cast<std::vector<uint8_t>&>(bson_buffer)[0] = doc_size & 0xFF;
                            const_cast<std::vector<uint8_t>&>(bson_buffer)[1] = (doc_size >> 8) & 0xFF;
                            const_cast<std::vector<uint8_t>&>(bson_buffer)[2] = (doc_size >> 16) & 0xFF;
                            const_cast<std::vector<uint8_t>&>(bson_buffer)[3] = (doc_size >> 24) & 0xFF;

                        } else if (bson_buffer.size() < static_cast<size_t>(doc_size)) {
                            // Buffer is SMALLER than the declared BSON document size.
                            // This means the async write chain was interrupted and CR8 sent only
                            // a partial payload. Writing this would produce a truncated BSON that
                            // appears valid (after size fixup) but contains only the first N params.
                            // The remaining params (e.g. PID tuning values set via QGC) would
                            // silently revert to factory defaults on next boot.
                            std::fprintf(stderr,
                                "[RPC][PARAM] INCOMPLETE BSON on close: declared=%u bytes, received=%zu bytes (fd=%d) — skip disk write\n",
                                doc_size, bson_buffer.size(), req->fd);
                            skip_disk_write = true;
                        }
                        // If bson_buffer.size() >= doc_size: BSON is complete (OK to write)
                    }

                    // Secondary size guard: full PX4 params BSON is always > 1 KB.
                    // Keeps protection against zero-param edge cases.
                    static constexpr size_t kMinFullParamsBsonSize = 1024U;

                    // --- Diagnostic: show file size before write ---
                    off_t existing_size = -1;
                    {
                        struct stat st_existing;
                        if (::stat(kPhysParamsBinaryPath, &st_existing) == 0) {
                            existing_size = st_existing.st_size;
                        }
                    }
                    std::fprintf(stderr,
                        "[RPC][PARAM] CLOSE fd=%d: buffer=%zu bytes, existing=%s=%lld bytes\n",
                        req->fd, bson_buffer.size(), kPhysParamsBinaryPath, (long long)existing_size);

                    if (!skip_disk_write && bson_buffer.size() < kMinFullParamsBsonSize) {
                        std::fprintf(stderr,
                            "[RPC][PARAM] SKIP: buffer %zu bytes < %zu (too small), fd=%d, existing=%lld bytes\n",
                            bson_buffer.size(), kMinFullParamsBsonSize, req->fd, (long long)existing_size);
                        skip_disk_write = true;
                    }

                    if (!skip_disk_write) {
                        std::string bson_payload(reinterpret_cast<const char *>(bson_buffer.data()),
                                                 bson_buffer.size());
                        if (!atomic_overwrite_file(std::string(kPhysParamsBinaryPath), bson_payload)) {
                            std::fprintf(stderr, "[RPC][PARAM] CRITICAL: write BSON to %s failed\n", kPhysParamsBinaryPath);
                            err = EIO;
                            ret = -1;
                        } else {
                            // Confirm actual size on disk after write
                            off_t written_size = -1;
                            {
                                struct stat st_after;
                                if (::stat(kPhysParamsBinaryPath, &st_after) == 0) {
                                    written_size = st_after.st_size;
                                }
                            }
                            std::fprintf(stderr,
                                "[RPC][PARAM] WRITE OK: %s is now %lld bytes (wrote %zu, fd=%d)\n",
                                kPhysParamsBinaryPath, (long long)written_size, bson_buffer.size(), req->fd);
                        }
                    }

                }

                // Clean up tracking
                g_param_fd_write_buffer.erase(req->fd);
                g_param_fd_bson_cache.erase(req->fd);
                g_param_fd_path.erase(req->fd);
            }

            // Close the actual file descriptor
            if (ret == 0) {
                ret = ::close(req->fd);
                err = (ret < 0) ? errno : 0;
            } else {
                ::close(req->fd); // Still close even if conversion failed
            }

            g_param_fd_path.erase(req->fd);

            struct timespec end_ts;
            clock_gettime(CLOCK_MONOTONIC, &end_ts);
            int64_t elapsed_ns = (int64_t)(end_ts.tv_sec - start_ts.tv_sec) * 1000000000LL
                               + (int64_t)(end_ts.tv_nsec - start_ts.tv_nsec);
            uint64_t elapsed_us = (elapsed_ns >= 0) ? (uint64_t)(elapsed_ns / 1000) : 0;

            if (is_param_fd) {
                std::fprintf(stderr, "[RPC][FS_CLOSE] req_id=%ld fd=%d ret=%d err=%d elapsed=%llu us (param file, conversion done)\n",
                             (long)request.request_id, req->fd, ret, err,
                             (unsigned long long)elapsed_us);
            } else if (elapsed_us > 10000) { // Log if >10ms
                std::fprintf(stderr, "[RPC][FS_CLOSE] req_id=%ld fd=%d ret=%d err=%d elapsed=%llu us (SLOW)\n",
                             (long)request.request_id, req->fd, ret, err,
                             (unsigned long long)elapsed_us);
            }

            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_FS_LSEEK: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                int32_t whence;
                std::int64_t offset;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            off_t ret = ::lseek(req->fd, static_cast<off_t>(req->offset), req->whence);
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, static_cast<int32_t>(ret), err, nullptr, 0);
        }

    case PX4_RPC_FS_FSYNC: {
            struct __attribute__((packed)) Request {
                int32_t fd;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            int ret = ::fsync(req->fd);
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_FS_UNLINK: {
            struct __attribute__((packed)) Request {
                std::uint32_t path_len;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const char *path = reinterpret_cast<const char *>(payload + sizeof(Request));

            if (req->path_len == 0 || req->path_len > payload_len - sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            std::string path_str(path, path + req->path_len - 1);
            int ret = ::unlink(translate_rpc_path(path_str).c_str());
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_FS_ACCESS: {
            struct __attribute__((packed)) Request {
                int32_t mode;
                std::uint32_t path_len;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const char *path = reinterpret_cast<const char *>(payload + sizeof(Request));

            if (req->path_len == 0 || req->path_len > payload_len - sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            std::string path_str(path, path + req->path_len - 1);
            int ret = ::access(translate_rpc_path(path_str).c_str(), req->mode);
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_FS_MKDIR: {
            struct __attribute__((packed)) Request {
                std::uint32_t path_len;
                std::int32_t mode;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const char *path = reinterpret_cast<const char *>(payload + sizeof(Request));

            if (req->path_len == 0 || req->path_len > payload_len - sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            std::string path_str(path, path + req->path_len - 1);
            int ret = ::mkdir(translate_rpc_path(path_str).c_str(), static_cast<mode_t>(req->mode));
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_FS_RMDIR: {
            struct __attribute__((packed)) Request {
                std::uint32_t path_len;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const char *path = reinterpret_cast<const char *>(payload + sizeof(Request));

            if (req->path_len == 0 || req->path_len > payload_len - sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            std::string path_str(path, path + req->path_len - 1);
            int ret = ::rmdir(translate_rpc_path(path_str).c_str());
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_FS_TRUNCATE: {
            struct __attribute__((packed)) Request {
                std::uint32_t path_len;
                std::int64_t length;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const char *path = reinterpret_cast<const char *>(payload + sizeof(Request));

            if (req->path_len == 0 || req->path_len > payload_len - sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            std::string path_str(path, path + req->path_len - 1);
            int ret = ::truncate(translate_rpc_path(path_str).c_str(), static_cast<off_t>(req->length));
            int err = (ret < 0) ? errno : 0;
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_FS_IOCTL: {
            struct __attribute__((packed)) Request {
                int32_t fd;
                std::uint32_t reserved0;
                std::uint64_t request_code;
                std::uint32_t write_len;
                std::uint32_t read_len;
                std::uint64_t scalar_value;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            const byte *data_ptr = payload + sizeof(Request);
            std::size_t data_available = payload_len - sizeof(Request);

            if (req->write_len > data_available) {
                return send_error(ept, request, EINVAL);
            }

            std::size_t buffer_len = std::max<std::size_t>(req->write_len, req->read_len);
            std::vector<byte> io_buffer(buffer_len);
            void *arg_ptr = reinterpret_cast<void *>(req->scalar_value);

            if (req->write_len > 0 || req->read_len > 0) {
                if (req->write_len > 0) {
                    std::memcpy(io_buffer.data(), data_ptr, req->write_len);
                } else {
                    std::fill(io_buffer.begin(), io_buffer.end(), 0);
                }

                arg_ptr = io_buffer.data();
            }

            int ret = ::ioctl(req->fd, static_cast<unsigned long>(req->request_code), arg_ptr);
            int err = (ret < 0) ? errno : 0;
            std::size_t response_size = (ret == 0 && req->read_len > 0)
                                        ? static_cast<std::size_t>(req->read_len)
                                        : 0;

            return send_response(ept, request, ret, err,
                                 io_buffer.data(), response_size);
        }

    case PX4_RPC_FS_SESSION_RESET: {
            // CR8 has restarted (JLink reset / warm reboot without CA55 restart).
            // Only clear parameter staging state from the old session.
            // Do NOT close unrelated file descriptors (for example an active .ulg log fd),
            // because deferred_param_sync can run after the logger has already started.
            int n_param_closed = 0;
            std::vector<int> stale_fds;
            for (auto const &kv : g_param_fd_write_buffer) {
                stale_fds.push_back(kv.first);
            }
            for (int fd : stale_fds) {
                std::fprintf(stderr, "[RPC][SESSION] closing stale param fd=%d (buffered %zu bytes)\n",
                             fd, g_param_fd_write_buffer.count(fd) ? g_param_fd_write_buffer[fd].size() : 0);
                ::close(fd);
                g_param_fd_write_buffer.erase(fd);
                g_param_fd_bson_cache.erase(fd);
                g_param_fd_path.erase(fd);
                ++n_param_closed;
            }

            const int preserved_non_param_fds = 0;

            std::fprintf(stderr,
                         "[RPC][SESSION] *** NEW CR8 SESSION *** closed_param_fds=%d preserved_non_param_fds=%d\n",
                         n_param_closed, preserved_non_param_fds);

            return send_response(ept, request, 0, 0, nullptr, 0);
        }

    case PX4_RPC_FS_OPENDIR: {
            struct __attribute__((packed)) Request {
                std::uint32_t path_len;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);

            if (payload_len < sizeof(Request) + req->path_len || req->path_len == 0) {
                return send_error(ept, request, EINVAL);
            }

            std::string path_str(reinterpret_cast<const char *>(payload) + sizeof(Request),
                                 req->path_len - 1U);

            std::string translated = translate_rpc_path(path_str);
            DIR *dp = ::opendir(translated.c_str());

            if (!dp) {
                int err = errno;
                return send_response(ept, request, -1, err, nullptr, 0);
            }

            std::vector<struct dirent> entries;
            while (struct dirent *ent = ::readdir(dp)) {
                entries.push_back(*ent);
            }
            ::closedir(dp);

            std::sort(entries.begin(), entries.end(), [](const struct dirent &a, const struct dirent &b) {
                return std::strcmp(a.d_name, b.d_name) < 0;
            });

            int handle = dir_handle_alloc(entries);

            if (handle < 0) {
                return send_response(ept, request, -1, EMFILE, nullptr, 0);
            }

            return send_response(ept, request, handle, 0, nullptr, 0);
        }

    case PX4_RPC_FS_READDIR: {
            struct __attribute__((packed)) Request {
                std::int32_t dir_handle;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            DirEntryCache *cache = dir_handle_get(req->dir_handle);

            if (!cache) {
                return send_response(ept, request, -1, EBADF, nullptr, 0);
            }

            if (cache->index >= cache->entries.size()) {
                /* EOF */
                return send_response(ept, request, 1, 0, nullptr, 0);
            }

            struct dirent *ent = &cache->entries[cache->index++];

            /* Serialize: d_name[256] + d_type[1] */
            struct __attribute__((packed)) ReadDirResponse {
                char    d_name[256];
                uint8_t d_type;
            } resp{};

            std::strncpy(resp.d_name, ent->d_name, sizeof(resp.d_name) - 1);
            resp.d_type = static_cast<uint8_t>(ent->d_type);

            return send_response(ept, request, 0, 0,
                                 reinterpret_cast<const uint8_t *>(&resp), sizeof(resp));
        }

    case PX4_RPC_FS_CLOSEDIR: {
            struct __attribute__((packed)) Request {
                std::int32_t dir_handle;
            };

            if (payload_len < sizeof(Request)) {
                return send_error(ept, request, EINVAL);
            }

            const auto *req = reinterpret_cast<const Request *>(payload);
            DirEntryCache *cache = dir_handle_get(req->dir_handle);

            if (!cache) {
                return send_response(ept, request, -1, EBADF, nullptr, 0);
            }

            dir_handle_free(req->dir_handle);
            return send_response(ept, request, 0, 0, nullptr, 0);
        }

    case PX4_RPC_FS_STAT: {
            // Payload: { uint32 path_len, path[] }
            if (payload_len < sizeof(uint32_t)) {
                return send_error(ept, request, EINVAL);
            }

            uint32_t path_len = 0;
            memcpy(&path_len, payload, sizeof(uint32_t));

            if (payload_len < sizeof(uint32_t) + path_len) {
                return send_error(ept, request, EINVAL);
            }

            char path_buf[PATH_MAX];
            size_t copy = std::min((size_t)path_len, (size_t)PATH_MAX - 1);
            memcpy(path_buf, payload + sizeof(uint32_t), copy);
            path_buf[copy] = '\0';

            std::string phys_path = translate_rpc_path(path_buf);

            struct stat st;
            int ret = ::stat(phys_path.c_str(), &st);
            int err = (ret < 0) ? errno : 0;

            if (ret < 0) {
                return send_response(ept, request, -1, err, nullptr, 0);
            }

#pragma pack(push, 1)
            struct StatResponse {
                int32_t  mode;
                int64_t  size;
                int64_t  mtime;
            } resp;
#pragma pack(pop)

            resp.mode  = (int32_t)st.st_mode;
            resp.size  = (int64_t)st.st_size;
            resp.mtime = (int64_t)st.st_mtime;

            return send_response(ept, request, 0, 0,
                                 reinterpret_cast<const uint8_t *>(&resp), sizeof(resp));
        }

    case PX4_RPC_FS_RENAME: {
            // Payload: { uint32 old_path_len, uint32 new_path_len, old_path[], new_path[] }
            if (payload_len < sizeof(uint32_t) * 2) {
                return send_error(ept, request, EINVAL);
            }

            uint32_t old_len = 0, new_len = 0;
            memcpy(&old_len, payload + 0, sizeof(uint32_t));
            memcpy(&new_len, payload + 4, sizeof(uint32_t));

            if (payload_len < sizeof(uint32_t) * 2 + old_len + new_len) {
                return send_error(ept, request, EINVAL);
            }

            char old_buf[PATH_MAX];
            char new_buf[PATH_MAX];
            size_t copy_old = std::min((size_t)old_len, (size_t)PATH_MAX - 1);
            size_t copy_new = std::min((size_t)new_len, (size_t)PATH_MAX - 1);
            memcpy(old_buf, payload + 8, copy_old);
            old_buf[copy_old] = '\0';
            memcpy(new_buf, payload + 8 + old_len, copy_new);
            new_buf[copy_new] = '\0';

            std::string phys_old = translate_rpc_path(old_buf);
            std::string phys_new = translate_rpc_path(new_buf);

            int ret = ::rename(phys_old.c_str(), phys_new.c_str());
            int err = (ret < 0) ? errno : 0;
            std::fprintf(stderr, "[RPC][FS_RENAME] '%s' -> '%s' ret=%d err=%d\n",
                         phys_old.c_str(), phys_new.c_str(), ret, err);
            return send_response(ept, request, ret, err, nullptr, 0);
        }

    case PX4_RPC_CA55_CLOCK_SETTIME: {
        if (payload_len < sizeof(int64_t)) {
            return send_error(ept, request, EINVAL);
        }

        int64_t unix_sec = 0;
        std::memcpy(&unix_sec, payload, sizeof(unix_sec));

        struct timespec ts{};
        ts.tv_sec  = static_cast<time_t>(unix_sec);
        ts.tv_nsec = 0;

        int ret = ::clock_settime(CLOCK_REALTIME, &ts);
        int err = (ret < 0) ? errno : 0;

        if (ret == 0) {
            // Also sync hwclock if available
            const int hwclock_ret = ::system("hwclock -w 2>/dev/null");
            (void)hwclock_ret;
        }

        std::fprintf(stderr, "[RPC][CLOCK_SETTIME] unix_sec=%lld ret=%d err=%d\n",
                     (long long)unix_sec, ret, err);

        return send_response(ept, request, ret, err, nullptr, 0);
    }

    case PX4_RPC_CA55_SYSTEM_REBOOT: {
        std::fprintf(stderr, "[RPC][SYSTEM_REBOOT] scheduling CA55 reboot\n");
        schedule_system_reboot();
        return send_response(ept, request, 0, 0, nullptr, 0);
    }

    case PX4_RPC_FS_SESSION_OPEN: {
        /* CR8 sends ARM timestamp_us; CA55 finds/creates the next sessNNN/ directory
         * and returns the virtual log file path (1 RPC call, replacing the N-probe loop). */

        // uint64_t timestamp_us in payload (currently unused; we use CA55 wall clock)
        static const std::string kVirtLogRoot = "/fs/microsd/log";
        const std::string kPhysLogRoot = translate_rpc_path(kVirtLogRoot);

        // Ensure log root directory exists
        ::mkdir(kPhysLogRoot.c_str(), 0755);

        // Find next sessNNN directory that doesn't exist yet
        uint16_t sess_num = 1;
        char phys_sess_dir[512];

        for (sess_num = 1; sess_num <= 999; sess_num++) {
            ::snprintf(phys_sess_dir, sizeof(phys_sess_dir), "%s/sess%03u",
                       kPhysLogRoot.c_str(), sess_num);

            if (::access(phys_sess_dir, F_OK) != 0) {
                break; // directory doesn't exist — use this slot
            }
        }

        if (sess_num > 999) {
            std::fprintf(stderr, "[RPC][SESSION_OPEN] no free session slots\n");
            return send_error(ept, request, ENOSPC);
        }

        if (::mkdir(phys_sess_dir, 0755) != 0 && errno != EEXIST) {
            int err = errno;
            std::fprintf(stderr, "[RPC][SESSION_OPEN] mkdir '%s' failed: %d\n", phys_sess_dir, err);
            return send_error(ept, request, err);
        }

        // Generate log file name from CA55 wall clock (HH_MM_SS.ulg) or sequential fallback
        char log_file_name[32] = "log001.ulg";
        time_t now = ::time(nullptr);

        if (now > 1000000000L) { // sanity: valid UNIX time (after year 2001)
            struct tm tt{};
            ::gmtime_r(&now, &tt);
            ::snprintf(log_file_name, sizeof(log_file_name), "%02d_%02d_%02d.ulg",
                       tt.tm_hour, tt.tm_min, tt.tm_sec);
        }

        // Build virtual path to return to CR8
        char virt_path[512];
        ::snprintf(virt_path, sizeof(virt_path), "%s/sess%03u/%s",
                   kVirtLogRoot.c_str(), sess_num, log_file_name);

        std::fprintf(stderr, "[RPC][SESSION_OPEN] sess=%u path=%s\n", sess_num, virt_path);

        size_t path_len = ::strlen(virt_path) + 1; // include null terminator
        return send_response(ept, request, 0, 0,
                             reinterpret_cast<const uint8_t *>(virt_path), path_len);
    }

    case PX4_RPC_FS_SESSION_CLOSE: {
        /* CR8 signals end of a log session. CA55 rebuilds logdata.txt from a full
         * local directory scan so QGC's log list is up-to-date after each disarm.
         * The fd payload is informational only; actual fd close goes through FS_CLOSE. */

        static const std::string kVirtLogRoot   = "/fs/microsd/log";
        static const std::string kVirtLogData   = "/fs/microsd/logdata.txt";
        static const std::string kVirtLogDataTmp = "/fs/microsd/$log$.txt";

        const std::string kPhysLogRoot    = translate_rpc_path(kVirtLogRoot);
        const std::string kPhysLogData    = translate_rpc_path(kVirtLogData);
        const std::string kPhysLogDataTmp = translate_rpc_path(kVirtLogDataTmp);

        FILE *fp = ::fopen(kPhysLogDataTmp.c_str(), "w");

        if (!fp) {
            int err = errno;
            std::fprintf(stderr, "[RPC][SESSION_CLOSE] fopen temp failed: %d\n", err);
            return send_error(ept, request, err);
        }

        int num_entries = 0;
        DIR *root_dp = ::opendir(kPhysLogRoot.c_str());

        if (root_dp) {
            // Collect and sort session dirs for deterministic ordering
            std::vector<std::string> sess_dirs;
            struct dirent *sess_ent;

            while ((sess_ent = ::readdir(root_dp)) != nullptr) {
                if (sess_ent->d_type != DT_DIR) { continue; }
                if (sess_ent->d_name[0] == '.') { continue; }
                sess_dirs.push_back(sess_ent->d_name);
            }

            ::closedir(root_dp);
            std::sort(sess_dirs.begin(), sess_dirs.end());

            for (const auto &sess_name : sess_dirs) {
                std::string sess_phys = kPhysLogRoot + "/" + sess_name;
                DIR *sess_dp = ::opendir(sess_phys.c_str());

                if (!sess_dp) { continue; }

                std::vector<std::string> log_files;
                struct dirent *file_ent;

                while ((file_ent = ::readdir(sess_dp)) != nullptr) {
                    if (file_ent->d_type != DT_REG) { continue; }
                    const char *name = file_ent->d_name;
                    size_t nlen = ::strlen(name);
                    if (nlen < 4 || ::strcmp(name + nlen - 4, ".ulg") != 0) { continue; }
                    log_files.push_back(name);
                }

                ::closedir(sess_dp);
                std::sort(log_files.begin(), log_files.end());

                for (const auto &fname : log_files) {
                    std::string file_phys = sess_phys + "/" + fname;
                    struct stat st;

                    if (::stat(file_phys.c_str(), &st) != 0) { continue; }

                    // Write virtual path so CR8 logdata.txt reader uses /fs/microsd/ prefix
                    std::fprintf(fp, "%u %u %s/%s/%s\n",
                                 (unsigned)st.st_mtime, (unsigned)st.st_size,
                                 kVirtLogRoot.c_str(), sess_name.c_str(), fname.c_str());
                    num_entries++;
                }
            }
        }

        ::fclose(fp);

        // Atomic rename
        if (::rename(kPhysLogDataTmp.c_str(), kPhysLogData.c_str()) != 0) {
            int err = errno;
            std::fprintf(stderr, "[RPC][SESSION_CLOSE] rename failed: %d\n", err);
            ::unlink(kPhysLogDataTmp.c_str());
            return send_error(ept, request, err);
        }

        std::fprintf(stderr, "[RPC][SESSION_CLOSE] logdata.txt rebuilt: %d entries\n", num_entries);
        return send_response(ept, request, 0, 0, nullptr, 0);
    }

    default:
        return send_error(ept, request, ENOTSUP);
    }
}

bool handle_message_buffer(struct rpmsg_endpoint *ept, const std::vector<byte> &message)
{
    if (!ept || message.size() < sizeof(px4_rpc_header)) {
        return false;
    }

    const auto *header = reinterpret_cast<const px4_rpc_header *>(message.data());

    if (header->magic != PX4_RPC_MAGIC) {
        fprintf(stderr, "[RPC] bad magic: 0x%x\n", header->magic);
        return false;
    }

    const byte *payload = reinterpret_cast<const byte *>(message.data()) + sizeof(px4_rpc_header);
    std::size_t payload_len = message.size() - sizeof(px4_rpc_header);

    if (!ensure_payload(*header, message.size(), payload_len) || payload_len < header->payload_size) {
        px4_rpc_header err_header = *header;
        send_error(ept, err_header, EINVAL);
        return true;
    }

    payload_len = header->payload_size;
    const uint64_t start_us = monotonic_now_us();
    bool ok = false;

    switch (header->category) {
    case PX4_RPC_CATEGORY_SOCKET:
        ok = handle_socket_rpc(ept, *header, payload, payload_len);
        break;

    case PX4_RPC_CATEGORY_FS:
        ok = handle_fs_rpc(ept, *header, payload, payload_len);
        break;

    default:
        ok = send_error(ept, *header, ENOTSUP);
        break;
    }

    const uint64_t total_us = monotonic_now_us() - start_us;
    update_peak(s_rpc_req_peak_total_us, total_us);

    if (header->category == PX4_RPC_CATEGORY_FS || !ok || total_us >= 50000U) {
        std::fprintf(stderr,
                     "[RPC][TIME] req_id=%ld cat=%u op=%u payload=%zu total_us=%llu ok=%d peak_total_us=%llu\n",
                     static_cast<long>(header->request_id),
                     static_cast<unsigned>(header->category),
                     static_cast<unsigned>(header->opcode),
                     payload_len,
                     static_cast<unsigned long long>(total_us),
                     ok ? 1 : 0,
                     static_cast<unsigned long long>(s_rpc_req_peak_total_us.load()));
    }

    return ok;
}

} // namespace

bool px4_openamp_rpc_server_handle(struct rpmsg_endpoint *ept, const void *data, std::size_t len)
{
    if (!ept || !data || len < sizeof(px4_rpc_header)) {
        return false;
    }

    std::vector<byte> message;
    if (!copy_rpmsg_buffer(ept, data, len, message)) {
        return false;
    }

    return handle_message_buffer(ept, message);
}

static void ensure_cr8_data_dirs(void)
{
    // Pre-create the CR8 data directories at startup so that param writes don't fail
    // because the directory doesn't exist.  Log clearly if /drone-data/ is not mounted.
    const char *dirs[] = {
        "/drone-data/cr8_data",
        "/drone-data/cr8_data/etc",
        nullptr
    };
    for (const char **d = dirs; *d; ++d) {
        struct stat st{};
        if (::stat(*d, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                std::fprintf(stderr, "[RPC] WARNING: %s exists but is not a directory\n", *d);
            }
            continue;
        }
        if (errno != ENOENT) {
            std::fprintf(stderr, "[RPC] WARNING: stat(%s) failed: %d\n", *d, errno);
            continue;
        }
        if (::mkdir(*d, 0777) != 0) {
            std::fprintf(stderr, "[RPC] WARNING: mkdir(%s) failed: %d (%s)\n",
                         *d, errno, errno == EROFS ? "filesystem is read-only!" : strerror(errno));
        } else {
            std::fprintf(stderr, "[RPC] Created directory: %s\n", *d);
        }
    }
}

bool px4_openamp_rpc_server_init_async(void)
{
    ensure_cr8_data_dirs();
    return g_async_worker.init();
}

void px4_openamp_rpc_server_shutdown_async(void)
{
    g_async_worker.shutdown();
}

bool px4_openamp_rpc_server_enqueue(struct rpmsg_endpoint *ept, const void *data, std::size_t len)
{
    if (!ept || !data || len < sizeof(px4_rpc_header)) {
        return false;
    }

    px4_rpc_header header{};
    std::memcpy(&header, data, sizeof(header));

    if (header.magic != PX4_RPC_MAGIC) {
        return false;
    }

    return g_async_worker.enqueue(ept, data, len, header);
}
