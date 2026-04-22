/**
 * image_streamer.cpp — UDP JPEG stream with bbox overlay
 */
#include "image_streamer.h"
#include "draw_utils.h"

#include <turbojpeg.h>

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#define STREAM_LOG(fmt, ...) fprintf(stderr, "[STREAM] " fmt "\n", ##__VA_ARGS__)
#define STREAM_ERR(fmt, ...) fprintf(stderr, "[STREAM ERR] " fmt "\n", ##__VA_ARGS__)

static constexpr int MAX_CHUNK_PAYLOAD = 1400;

struct __attribute__((packed)) ChunkHeader {
    uint32_t frame_id;
    uint16_t chunk_index;
    uint16_t total_chunks;
    uint16_t chunk_size;
};
static constexpr int HEADER_SIZE = sizeof(ChunkHeader); // 10 bytes

namespace AI {

UdpImageStreamer::UdpImageStreamer()
    : sock_fd_(-1)
    , frame_id_(0)
    , frame_counter_(0)
    , draw_buf_(nullptr)
    , draw_buf_size_(0)
    , tj_handle_(nullptr)
    , ip_learned_(false)
    , hb_running_(false)
    , hb_sock_(-1)
{
    learned_ip_[0] = '\0';
}

UdpImageStreamer::~UdpImageStreamer()
{
    Shutdown();
}

// Thread trampoline
static void* hb_thread_trampoline(void* arg)
{
    static_cast<UdpImageStreamer*>(arg)->heartbeat_thread_func();
    return nullptr;
}

bool UdpImageStreamer::Init(const StreamConfig& cfg, int frame_w, int frame_h)
{
    cfg_ = cfg;

    // Static dest IP provided — skip heartbeat listener
    if (!cfg_.dest_ip.empty()) {
        strncpy(learned_ip_, cfg_.dest_ip.c_str(), sizeof(learned_ip_) - 1);
        learned_ip_[sizeof(learned_ip_) - 1] = '\0';
        ip_learned_ = true;
    }

    sock_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0) {
        STREAM_ERR("Failed to create UDP socket");
        return false;
    }

    int broadcast = 1;
    setsockopt(sock_fd_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    int sndbuf = 512 * 1024;
    setsockopt(sock_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    draw_buf_size_ = (size_t)frame_w * frame_h * 3;
    draw_buf_ = new (std::nothrow) uint8_t[draw_buf_size_];
    if (!draw_buf_) {
        STREAM_ERR("Failed to allocate draw buffer");
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }

    tj_handle_ = tjInitCompress();
    if (!tj_handle_) {
        STREAM_ERR("Failed to init turbojpeg");
        delete[] draw_buf_; draw_buf_ = nullptr;
        close(sock_fd_); sock_fd_ = -1;
        return false;
    }

    // Start heartbeat listener only if no static IP and port is non-zero
    if (!ip_learned_ && cfg_.heartbeat_port > 0) {
        hb_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (hb_sock_ >= 0) {
            struct sockaddr_in hb_addr{};
            hb_addr.sin_family = AF_INET;
            hb_addr.sin_port   = htons(cfg_.heartbeat_port);
            hb_addr.sin_addr.s_addr = INADDR_ANY;
            if (bind(hb_sock_, (struct sockaddr*)&hb_addr, sizeof(hb_addr)) == 0) {
                hb_running_ = true;
                pthread_create(&hb_thread_, nullptr, hb_thread_trampoline, this);
                STREAM_LOG("Heartbeat listener on port %d", cfg_.heartbeat_port);
            } else {
                close(hb_sock_);
                hb_sock_ = -1;
            }
        }
    }

    STREAM_LOG("Init: port=%d quality=%d skip=%d frame=%dx%d",
               cfg_.dest_port, cfg_.jpeg_quality, cfg_.skip_frames, frame_w, frame_h);
    return true;
}

void UdpImageStreamer::Shutdown()
{
    // Stop heartbeat thread
    if (hb_running_) {
        hb_running_ = false;
        if (hb_sock_ >= 0) {
            close(hb_sock_);
            hb_sock_ = -1;
        }
        pthread_join(hb_thread_, nullptr);
    }

    if (tj_handle_) {
        tjDestroy((tjhandle)tj_handle_);
        tj_handle_ = nullptr;
    }
    if (draw_buf_) {
        delete[] draw_buf_;
        draw_buf_ = nullptr;
    }
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
    STREAM_LOG("Shutdown (sent %u frames)", frame_id_);
}

void UdpImageStreamer::heartbeat_thread_func()
{
    struct sockaddr_in src{};
    socklen_t src_len = sizeof(src);
    char buf[64];

    while (hb_running_) {
        ssize_t n = recvfrom(hb_sock_, buf, sizeof(buf), 0,
                             (struct sockaddr*)&src, &src_len);
        if (n > 0 && !ip_learned_) {
            const char* ip = inet_ntoa(src.sin_addr);
            // Ignore packets from loopback or same board (self-send)
            if (strncmp(ip, "127.", 4) != 0) {
                strncpy(learned_ip_, ip, sizeof(learned_ip_) - 1);
                ip_learned_ = true;
                STREAM_LOG("Learned viewer IP: %s", learned_ip_);
            }
        }
    }
}

bool UdpImageStreamer::SendFrame(const uint8_t* bgr_buf, int width, int height,
                                  const std::vector<Detection>& detections,
                                  int model_size, float inference_ms)
{
    if (sock_fd_ < 0 || !tj_handle_) return false;

    frame_counter_++;
    if (cfg_.skip_frames > 1 && (frame_counter_ % cfg_.skip_frames != 0))
        return false;

    size_t frame_size = (size_t)width * height * 3;
    if (frame_size > draw_buf_size_) return false;
    memcpy(draw_buf_, bgr_buf, frame_size);

    draw_detections_mapped(draw_buf_, width, height, detections, model_size);

    // FPS/stats overlay (top-left)
    float fps = (inference_ms > 0) ? (1000.0f / inference_ms) : 0;
    char overlay[64];
    snprintf(overlay, sizeof(overlay), "%dFPS %dMS %dDET",
             (int)fps, (int)inference_ms, (int)detections.size());

    int ow = (int)strlen(overlay) * 6 + 4;
    for (int y = 2; y < 12; ++y)
        for (int x = 2; x < ow + 2 && x < width; ++x)
            draw_pixel(draw_buf_, width, height, x, y, 0, 0, 0);
    draw_text(draw_buf_, width, height, 4, 3, overlay, 0, 255, 0);

    // JPEG encode
    unsigned char* jpeg_buf  = nullptr;
    unsigned long  jpeg_size = 0;

    int tj_ret = tjCompress2(
        (tjhandle)tj_handle_,
        draw_buf_, width, 0, height,
        TJPF_BGR,
        &jpeg_buf, &jpeg_size,
        TJSAMP_420, cfg_.jpeg_quality,
        TJFLAG_FASTDCT
    );

    if (tj_ret != 0 || !jpeg_buf) {
        STREAM_ERR("JPEG encode failed: %s", tjGetErrorStr());
        return false;
    }

    // Resolve destination
    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(cfg_.dest_port);

    if (ip_learned_) {
        dest.sin_addr.s_addr = inet_addr(learned_ip_);
    } else {
        // Broadcast fallback — works on LAN before viewer connects
        dest.sin_addr.s_addr = inet_addr("192.168.1.255");
    }

    // Fragment and send
    uint16_t total_chunks = (uint16_t)((jpeg_size + MAX_CHUNK_PAYLOAD - 1) / MAX_CHUNK_PAYLOAD);
    uint8_t packet[HEADER_SIZE + MAX_CHUNK_PAYLOAD];

    for (uint16_t i = 0; i < total_chunks; ++i) {
        size_t   offset   = (size_t)i * MAX_CHUNK_PAYLOAD;
        uint16_t chunk_sz = (uint16_t)(
            (offset + MAX_CHUNK_PAYLOAD <= jpeg_size)
            ? MAX_CHUNK_PAYLOAD
            : (jpeg_size - offset));

        ChunkHeader hdr;
        hdr.frame_id     = frame_id_;
        hdr.chunk_index  = i;
        hdr.total_chunks = total_chunks;
        hdr.chunk_size   = chunk_sz;

        memcpy(packet,               &hdr, HEADER_SIZE);
        memcpy(packet + HEADER_SIZE, jpeg_buf + offset, chunk_sz);

        sendto(sock_fd_, packet, HEADER_SIZE + chunk_sz, 0,
               (struct sockaddr*)&dest, sizeof(dest));

        if (i < total_chunks - 1 && total_chunks > 4)
            usleep(100);
    }

    tjFree(jpeg_buf);
    frame_id_++;
    return true;
}

} // namespace AI
