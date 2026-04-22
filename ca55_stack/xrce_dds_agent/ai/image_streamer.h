/**
 * @file image_streamer.h
 * @brief UDP image streamer for visual debugging
 *
 * Draws bounding boxes on camera frames, JPEG-encodes them using
 * libjpeg-turbo, and sends fragmented UDP packets to a PC viewer.
 *
 * UDP Chunk Protocol (10-byte header + up to 1400 bytes payload):
 *   uint32_t frame_id       // monotonic counter
 *   uint16_t chunk_index    // 0-based
 *   uint16_t total_chunks   // total chunks in this frame
 *   uint16_t chunk_size     // payload bytes in this chunk
 */
#ifndef AI_IMAGE_STREAMER_H
#define AI_IMAGE_STREAMER_H

#include "obstacle_processor.h"
#include <cstdint>
#include <vector>

namespace AI {

struct StreamConfig {
    uint16_t    dest_port;     // UDP destination port (default: 50002)
    int         jpeg_quality;  // JPEG quality 0-100 (default: 50)
    int         skip_frames;   // stream every Nth frame (default: 2)
};

static inline StreamConfig StreamDefaultConfig()
{
    StreamConfig c;
    c.dest_port    = 50002;
    c.jpeg_quality = 50;
    c.skip_frames  = 2;
    return c;
}

class UdpImageStreamer {
public:
    UdpImageStreamer();
    ~UdpImageStreamer();

    bool Init(const StreamConfig& cfg, int frame_w, int frame_h);
    void Shutdown();

    /**
     * Draw bboxes on a copy of bgr_buf, JPEG encode, fragment, and send via UDP.
     *
     * @param bgr_buf       Raw BGR888 frame from camera
     * @param width         Frame width
     * @param height        Frame height
     * @param detections    Detection results from YoloDetector
     * @param model_size    Model input size (e.g. 640) for bbox coord mapping
     * @param inference_ms  Inference time for FPS overlay
     * @return true if frame was sent, false if skipped or error
     */
    bool SendFrame(const uint8_t* bgr_buf, int width, int height,
                   const std::vector<Detection>& detections,
                   int model_size, float inference_ms);

private:
    int         sock_fd_;
    StreamConfig cfg_;
    uint32_t    frame_id_;
    uint32_t    frame_counter_;

    // Pre-allocated buffers
    uint8_t*    draw_buf_;
    size_t      draw_buf_size_;

    // turbojpeg compressor handle
    void*       tj_handle_;
};

} // namespace AI

#endif // AI_IMAGE_STREAMER_H
