/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef AI_GST_H264_STREAMER_H
#define AI_GST_H264_STREAMER_H

/**
 * GStreamer H.264 hardware-encoded stream to QGC (UDP RTP, port 5600).
 * Uses Renesas OMX H.264 encoder (OMX.RENESAS.VIDEO.ENCODER.H264).
 *
 * Pipeline:
 *   appsrc (BGR) → videoconvert → I420 → omxh264enc → rtph264pay → udpsink
 *
 * QGC setup: Application Settings → Video → Source: UDP H.264, Port: 5600
 *
 * Same SendFrame() interface as UdpImageStreamer — drop-in replacement.
 */

#include "obstacle_processor.h"
#include <cstdint>
#include <string>
#include <vector>

// Forward-declare GStreamer types to keep GStreamer headers out of this header
typedef struct _GstElement GstElement;

namespace AI {

class GstH264Streamer {
public:
    struct Config {
        std::string dest_ip;              // QGC host IP(s), comma-separated (required)
        uint16_t    dest_port  = 5600;    // QGC UDP video port
        int         bitrate    = 4000000; // bits/s target bitrate (4 Mbps default)
        int         fps        = 30;
        int         skip_frames = 1;      // stream every Nth frame (1 = no skip)
        bool        use_h265   = false;   // true = omxh265enc/rtph265pay (HEVC, ~50% less BW)
    };

    GstH264Streamer();
    ~GstH264Streamer();

    bool Init(const Config& cfg, int frame_w, int frame_h);
    void Shutdown();

    /**
     * Draw bboxes on a copy of bgr_buf, H.264-encode, send via RTP/UDP.
     * Returns true if frame was pushed to encoder, false if skipped or error.
     */
    bool SendFrame(const uint8_t* bgr_buf, int width, int height,
                   const std::vector<Detection>& dets,
                   int model_size, float inference_ms);

private:
    GstElement* pipeline_     = nullptr;
    GstElement* appsrc_       = nullptr;

    Config      cfg_;
    int         frame_w_      = 0;
    int         frame_h_      = 0;
    uint64_t    frame_id_     = 0;
    uint32_t    frame_counter_= 0;

    uint8_t*    draw_buf_     = nullptr;  // scratch buffer for bbox drawing
};

} // namespace AI

#endif // AI_GST_H264_STREAMER_H
