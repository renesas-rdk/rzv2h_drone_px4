#ifndef AI_IMAGE_STREAMER_H
#define AI_IMAGE_STREAMER_H

/**
 * UDP image streamer: draws bboxes on BGR frames, JPEG-encodes via libjpeg-turbo,
 * sends fragmented UDP packets.
 *
 * Protocol (10-byte header + up to 1400 bytes payload):
 *   uint32_t frame_id
 *   uint16_t chunk_index
 *   uint16_t total_chunks
 *   uint16_t chunk_size
 *
 * IP auto-learning: a background thread listens on heartbeat_port (default 50001).
 * When stream_viewer.py sends a heartbeat, the board learns the PC IP and switches
 * from broadcast to unicast — reduces packet loss significantly.
 */

#include "detection_types.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace AI {

struct StreamConfig {
    std::string dest_ip;       // static dest IP ("" = auto-learn via heartbeat / broadcast fallback)
    uint16_t    dest_port;     // UDP stream destination port (default: 50002)
    uint16_t    heartbeat_port;// UDP port to listen for PC heartbeat (default: 50001)
    int         jpeg_quality;  // JPEG quality 0-100 (default: 50)
    int         skip_frames;   // stream every Nth frame (default: 2)
};

inline StreamConfig StreamDefaultConfig()
{
    StreamConfig c;
    c.dest_ip        = "";
    c.dest_port      = 50002;
    c.heartbeat_port = 50001;
    c.jpeg_quality   = 50;
    c.skip_frames    = 2;
    return c;
}

class UdpImageStreamer {
public:
    UdpImageStreamer();
    ~UdpImageStreamer();

    bool Init(const StreamConfig& cfg, int frame_w, int frame_h);
    void Shutdown();

    /**
     * Draw bboxes on a copy of bgr_buf, JPEG-encode, fragment, send via UDP.
     * Returns true if frame was sent, false if skipped or error.
     */
    bool SendFrame(const uint8_t* bgr_buf, int width, int height,
                   const std::vector<Detection>& detections,
                   int model_size, float inference_ms);

    friend void* hb_thread_trampoline(void*);

private:
    void heartbeat_thread_func();

    int          sock_fd_;
    StreamConfig cfg_;
    uint32_t     frame_id_;
    uint32_t     frame_counter_;

    uint8_t*     draw_buf_;
    size_t       draw_buf_size_;
    void*        tj_handle_;

    // Auto-learned PC IP from heartbeat packets
    char         learned_ip_[64];
    std::atomic<bool> ip_learned_;

    // Heartbeat listener thread
    pthread_t    hb_thread_;
    std::atomic<bool> hb_running_;
    int          hb_sock_;
};

} // namespace AI

#endif // AI_IMAGE_STREAMER_H
