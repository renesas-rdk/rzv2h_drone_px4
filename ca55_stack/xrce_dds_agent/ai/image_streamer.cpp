/**
 * @file image_streamer.cpp
 * @brief UDP image streamer — JPEG encode + fragmented send
 */
#include "image_streamer.h"
#include "draw_utils.h"

#include <turbojpeg.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>

/* ── Externs: auto-learned host IP from custom_agent.cpp ───────── */
extern char            learned_host_ip[64];
extern pthread_mutex_t learned_ip_mutex;
extern volatile bool   host_ip_learned;

#define STREAM_LOG(fmt, ...) fprintf(stderr, "[STREAM] " fmt "\n", ##__VA_ARGS__)
#define STREAM_ERR(fmt, ...) fprintf(stderr, "[STREAM ERROR] " fmt "\n", ##__VA_ARGS__)

/* ── UDP chunk protocol constants ─────────────────────────────── */
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
{
}

UdpImageStreamer::~UdpImageStreamer()
{
    Shutdown();
}

bool UdpImageStreamer::Init(const StreamConfig& cfg, int frame_w, int frame_h)
{
    cfg_ = cfg;

    // Create UDP socket
    sock_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0) {
        STREAM_ERR("Failed to create UDP socket");
        return false;
    }

    // Enable broadcast (fallback when host IP not learned)
    int broadcast = 1;
    setsockopt(sock_fd_, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    // Increase send buffer to reduce drops when sending chunked frames
    int sndbuf = 512 * 1024;
    setsockopt(sock_fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    // Pre-allocate draw buffer (copy of BGR frame for annotation)
    draw_buf_size_ = frame_w * frame_h * 3;
    draw_buf_ = new (std::nothrow) uint8_t[draw_buf_size_];
    if (!draw_buf_) {
        STREAM_ERR("Failed to allocate draw buffer (%zu bytes)", draw_buf_size_);
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }

    // Init turbojpeg compressor
    tj_handle_ = tjInitCompress();
    if (!tj_handle_) {
        STREAM_ERR("Failed to init turbojpeg compressor");
        delete[] draw_buf_;
        draw_buf_ = nullptr;
        close(sock_fd_);
        sock_fd_ = -1;
        return false;
    }

    // STREAM_LOG("Initialized: port=%d, quality=%d, skip=%d, frame=%dx%d",
    //            cfg_.dest_port, cfg_.jpeg_quality, cfg_.skip_frames,
    //            frame_w, frame_h);
    return true;
}

void UdpImageStreamer::Shutdown()
{
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

bool UdpImageStreamer::SendFrame(const uint8_t* bgr_buf, int width, int height,
                                  const std::vector<Detection>& detections,
                                  int model_size, float inference_ms)
{
    if (sock_fd_ < 0 || !tj_handle_) return false;

    // Frame skipping
    frame_counter_++;
    if (cfg_.skip_frames > 1 && (frame_counter_ % cfg_.skip_frames != 0))
        return false;

    // Copy frame to draw buffer (don't corrupt inference buffer)
    size_t frame_size = (size_t)width * height * 3;
    if (frame_size > draw_buf_size_) return false;
    memcpy(draw_buf_, bgr_buf, frame_size);

    // Draw bounding boxes with letterbox coordinate mapping
    draw_detections_mapped(draw_buf_, width, height, detections, model_size);

    // Draw FPS overlay (top-left corner)
    float fps = (inference_ms > 0) ? (1000.0f / inference_ms) : 0;
    char overlay[64];
    snprintf(overlay, sizeof(overlay), "%dFPS %dMS %dDET",
             (int)fps, (int)inference_ms, (int)detections.size());

    // Draw overlay background
    int ow = (int)strlen(overlay) * 6 + 4;
    for (int y = 2; y < 12; ++y)
        for (int x = 2; x < ow + 2 && x < width; ++x)
            draw_pixel(draw_buf_, width, height, x, y, 0, 0, 0);
    draw_text(draw_buf_, width, height, 4, 3, overlay, 0, 255, 0);

    // JPEG encode using turbojpeg
    unsigned char* jpeg_buf = nullptr;
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

    // Resolve destination address
    struct sockaddr_in dest;
    dest.sin_family = AF_INET;
    dest.sin_port = htons(cfg_.dest_port);

    pthread_mutex_lock(&learned_ip_mutex);
    if (host_ip_learned) {
        dest.sin_addr.s_addr = inet_addr(learned_host_ip);
    } else {
        dest.sin_addr.s_addr = inet_addr("192.168.1.255");
    }
    pthread_mutex_unlock(&learned_ip_mutex);

    // Fragment and send
    uint16_t total_chunks = (uint16_t)((jpeg_size + MAX_CHUNK_PAYLOAD - 1) / MAX_CHUNK_PAYLOAD);
    uint8_t packet[HEADER_SIZE + MAX_CHUNK_PAYLOAD];

    for (uint16_t i = 0; i < total_chunks; ++i) {
        size_t offset = (size_t)i * MAX_CHUNK_PAYLOAD;
        uint16_t chunk_sz = (uint16_t)((offset + MAX_CHUNK_PAYLOAD <= jpeg_size)
                                        ? MAX_CHUNK_PAYLOAD
                                        : (jpeg_size - offset));

        ChunkHeader hdr;
        hdr.frame_id     = frame_id_;
        hdr.chunk_index  = i;
        hdr.total_chunks = total_chunks;
        hdr.chunk_size   = chunk_sz;

        memcpy(packet, &hdr, HEADER_SIZE);
        memcpy(packet + HEADER_SIZE, jpeg_buf + offset, chunk_sz);

        sendto(sock_fd_, packet, HEADER_SIZE + chunk_sz, 0,
               (struct sockaddr*)&dest, sizeof(dest));

        // Small delay between chunks to avoid overwhelming receiver's UDP buffer
        if (i < total_chunks - 1 && total_chunks > 4)
            usleep(100);  // 100us between chunks
    }

    // Free turbojpeg-allocated buffer
    tjFree(jpeg_buf);

    frame_id_++;
    return true;
}

} // namespace AI
