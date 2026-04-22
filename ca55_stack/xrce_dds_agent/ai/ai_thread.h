/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file ai_thread.h
 * @brief AI Camera thread entry point for custom_agent
 *
 * Architecture: double-buffer async — capture+encode runs at camera FPS,
 * DRP-AI inference runs independently in a worker thread.
 */
#ifndef AI_AI_THREAD_H
#define AI_AI_THREAD_H

#include <cstdlib>
#include <cstdint>

namespace AI {

struct AiThreadConfig {
    const char* camera_device;   // "/dev/video0"
    int         camera_width;    // 640
    int         camera_height;   // 480
    int         camera_fps;      // 30
    const char* model_dir;       // "/home/root/ai_models/yolov8n"
    int         cpu_core;        // CPU core affinity for capture/encode thread
    uint16_t    udp_port;        // obstacle packet UDP port (unused — kept for ABI compat)
    const char* qgc_dest_ip;     // QGC host IP(s), comma-separated ("" = disabled)
    int         qgc_bitrate;     // encoder target bitrate in bits/s
    bool        qgc_h265;        // true = H.265/HEVC, false = H.264 (default)
    int         stream_fps_div;  // stream every Nth frame (1=full fps, 2=half fps)
};

// All fields can be overridden via environment variables (set in
// /etc/default/custom_xrce_agent loaded by the systemd service):
//   AI_CAMERA      — V4L2 device path         (default /dev/video0)
//   AI_MODEL       — YOLOv8n model directory   (default /home/root/ai_models/yolov8n)
//   CAM_WIDTH      — capture width in pixels   (default 640)
//   CAM_HEIGHT     — capture height in pixels  (default 480)
//   CAM_FPS        — capture framerate         (default 30)
//   QGC_IP         — destination IP(s)         (default "")
//   QGC_BITRATE    — encoder bitrate bits/s    (default 4000000)
//   QGC_CODEC      — "h265" enables HEVC       (default h264)
//   STREAM_FPS_DIV — stream every Nth frame    (default 1 = full fps, 2 = half fps)
static inline AiThreadConfig AiThreadDefaultConfig()
{
    auto getenv_int = [](const char* name, int def) -> int {
        const char* v = std::getenv(name);
        return (v && *v) ? std::atoi(v) : def;
    };
    auto getenv_str = [](const char* name, const char* def) -> const char* {
        const char* v = std::getenv(name);
        return (v && *v) ? v : def;
    };

    AiThreadConfig cfg;
    cfg.camera_device = getenv_str("AI_CAMERA",   "/dev/video0");
    cfg.camera_width  = getenv_int("CAM_WIDTH",   640);
    cfg.camera_height = getenv_int("CAM_HEIGHT",  480);
    cfg.camera_fps    = getenv_int("CAM_FPS",     30);
    cfg.model_dir     = getenv_str("AI_MODEL",    "/home/root/ai_models/yolov8n");
    cfg.cpu_core      = 2;
    cfg.udp_port      = 55000;
    cfg.qgc_dest_ip   = getenv_str("QGC_IP",      "");
    cfg.qgc_bitrate   = getenv_int("QGC_BITRATE",    2000000);
    const char* codec = getenv_str("QGC_CODEC",      "h264");
    cfg.qgc_h265      = (codec[0] == 'h' && codec[1] == '2' && codec[2] == '6' && codec[3] == '5');
    cfg.stream_fps_div = getenv_int("STREAM_FPS_DIV", 1);
    return cfg;
}

/** Thread entry point (matches pthread_create signature). */
void* ai_camera_thread_func(void* arg);

} // namespace AI

#endif // AI_AI_THREAD_H
