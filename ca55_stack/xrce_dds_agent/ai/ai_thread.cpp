/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file ai_thread.cpp
 * @brief AI Camera thread: capture → detect → stream QGC
 *
 * Double-buffer async pipeline:
 *   Capture/encode thread  — runs at camera FPS (never blocked by inference)
 *   Inference worker thread — DRP-AI YOLOv8n, runs independently
 *
 * Frame flow:
 *   camera (YUYV) → YuyvToBgr → bgr_buf
 *       ├─ copy to stage_buf → inference worker (swap under lock, ~30ms async)
 *       └─ GstH264Streamer (latest detections from previous cycle)
 */
#include "ai_thread.h"
#include "camera_capture.h"
#include "yolo_detector.h"
#include "obstacle_processor.h"
#include "gst_h264_streamer.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>

// Detection UDP broadcast on port 50010 for ROS2 bridge node
// Protocol: magic(4B)=0x44455401 + frame_w(4B) + frame_h(4B) + num(1B)
//           + per-det: x1,y1,x2,y2,conf (4B float each) + class_id (4B int)
static constexpr uint32_t DET_MAGIC    = 0x44455401;
static constexpr uint16_t DET_UDP_PORT = 50010;

static int det_sock_init()
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    int bcast = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    return s;
}

static void det_send(int sock, int frame_w, int frame_h,
                     const std::vector<AI::Detection>& dets)
{
    constexpr int MAX_DETS = 50;
    int num = std::min((int)dets.size(), MAX_DETS);
    const int pkt_size = 4 + 4 + 4 + 1 + num * (5 * 4 + 4);
    uint8_t buf[4 + 4 + 4 + 1 + MAX_DETS * (5 * 4 + 4)];
    uint8_t* p = buf;

    memcpy(p, &DET_MAGIC, 4); p += 4;
    memcpy(p, &frame_w,   4); p += 4;
    memcpy(p, &frame_h,   4); p += 4;
    *p++ = (uint8_t)num;
    for (int i = 0; i < num; i++) {
        const auto& d = dets[i];
        memcpy(p, &d.bbox.x1,    4); p += 4;
        memcpy(p, &d.bbox.y1,    4); p += 4;
        memcpy(p, &d.bbox.x2,    4); p += 4;
        memcpy(p, &d.bbox.y2,    4); p += 4;
        memcpy(p, &d.confidence, 4); p += 4;
        memcpy(p, &d.class_id,   4); p += 4;
    }
    struct sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(DET_UDP_PORT);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(sock, buf, pkt_size, 0, (struct sockaddr*)&dst, sizeof(dst));
}

extern int  force_stop;
extern void ai_update_obstacle_packet(const AI::ObstacleAvoidPacket& pkt);

#ifdef ENABLE_AI_CAMERA
extern volatile bool ai_thread_running;
#endif

#define AI_LOG(fmt, ...) fprintf(stderr, "[AI] "    fmt "\n", ##__VA_ARGS__)
#define AI_ERR(fmt, ...) fprintf(stderr, "[AI ERR] " fmt "\n", ##__VA_ARGS__)

namespace AI {

/* ── Background DRP-AI model loader ────────────────────────────────── */
// Loads the YoloDetector model off the main thread so the camera+GStreamer
// pipeline can start streaming immediately without waiting for DRP-AI init.

struct ModelLoadCtx {
    YoloDetector*      det;
    const char*        model_dir;
    std::atomic<int>   state;  // 0=loading  1=ready  2=failed
};

static void* model_load_worker(void* arg)
{
    auto* ctx = static_cast<ModelLoadCtx*>(arg);
    ctx->state.store(ctx->det->Init(ctx->model_dir) ? 1 : 2,
                     std::memory_order_release);
    return nullptr;
}

/* ── Async inference state (double-buffer ping-pong) ───────────────── */

struct InferState {
    YoloDetector* detector    = nullptr;
    int           w = 0, h   = 0;
    size_t        bgr_bytes   = 0;

    // Two BGR buffers swapped atomically under lock:
    //   infer_buf — worker reads exclusively after swap
    //   stage_buf — main writes next frame; becomes infer_buf on next swap
    uint8_t*      infer_buf   = nullptr;
    uint8_t*      stage_buf   = nullptr;

    // Latest results written by worker, read by main
    std::vector<Detection> dets;
    float         inf_ms      = 0.0f;
    int           model_size  = 0;

    pthread_mutex_t mtx     = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  cond    = PTHREAD_COND_INITIALIZER;
    bool            pending = false;
    bool            stop    = false;
};

static void* inference_worker(void* arg)
{
    InferState* is = static_cast<InferState*>(arg);

    while (true) {
        pthread_mutex_lock(&is->mtx);
        while (!is->pending && !is->stop)
            pthread_cond_wait(&is->cond, &is->mtx);
        if (is->stop) {
            pthread_mutex_unlock(&is->mtx);
            break;
        }
        // Swap: worker takes infer_buf (latest frame), releases stage_buf back to main
        std::swap(is->infer_buf, is->stage_buf);
        is->pending = false;
        pthread_mutex_unlock(&is->mtx);

        // DRP-AI inference (~30ms) — no lock held
        auto  dets = is->detector->RunOnYUYV(is->infer_buf, is->w, is->h);
        float ms   = is->detector->GetLastInferenceMs();

        pthread_mutex_lock(&is->mtx);
        is->dets       = std::move(dets);
        is->inf_ms     = ms;
        is->model_size = is->detector->GetInputSize();
        pthread_mutex_unlock(&is->mtx);
    }
    return nullptr;
}

/* ── Thread entry point ─────────────────────────────────────────────── */

void* ai_camera_thread_func(void* arg)
{
    const AiThreadConfig* cfg =
        arg ? static_cast<const AiThreadConfig*>(arg) : nullptr;
    AiThreadConfig defaults = AiThreadDefaultConfig();
    if (!cfg) cfg = &defaults;

    AI_LOG("AI Camera Thread starting (core=%d)", cfg->cpu_core);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cfg->cpu_core, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
        AI_LOG("Warning: could not pin to core %d", cfg->cpu_core);

    // ── Stage 1: Camera (fast) ────────────────────────────────────────
    CameraCapture camera;
    if (!camera.Init(cfg->camera_device, cfg->camera_width, cfg->camera_height,
                     cfg->camera_fps)) {
        AI_ERR("Failed to open camera %s", cfg->camera_device);
        return nullptr;
    }
    const int    cam_w    = camera.GetWidth();
    const int    cam_h    = camera.GetHeight();
    const size_t bgr_size = (size_t)cam_w * cam_h * 3;

    uint8_t* bgr_buf = new (std::nothrow) uint8_t[bgr_size];
    if (!bgr_buf) {
        AI_ERR("Failed to alloc frame buffer");
        camera.Release();
        return nullptr;
    }

    // ── Stage 2: QGC H.264 streamer (fast — starts before model load) ─
    // Starting GStreamer pipeline here so video appears on QGC immediately.
    // Detections will be empty until the DRP-AI model finishes loading.
    GstH264Streamer* qgc_streamer = nullptr;
    if (cfg->qgc_dest_ip && cfg->qgc_dest_ip[0] != '\0') {
        qgc_streamer = new (std::nothrow) GstH264Streamer();
        if (qgc_streamer) {
            GstH264Streamer::Config qcfg;
            qcfg.dest_ip     = cfg->qgc_dest_ip;
            qcfg.dest_port   = 5600;
            qcfg.bitrate     = cfg->qgc_bitrate;
            qcfg.fps         = cfg->camera_fps;
            qcfg.skip_frames = cfg->stream_fps_div;
            qcfg.use_h265    = cfg->qgc_h265;
            if (!qgc_streamer->Init(qcfg, cam_w, cam_h)) {
                AI_ERR("Failed to init QGC H.264 streamer");
                delete qgc_streamer;
                qgc_streamer = nullptr;
            }
        }
    }

    // ── Stage 3: Detection UDP socket ─────────────────────────────────
    int det_sock = det_sock_init();
    if (det_sock < 0) AI_ERR("Failed to create detection UDP socket");
    else AI_LOG("Detection UDP broadcaster on port %d", DET_UDP_PORT);

    // ── Stage 4: DRP-AI model load in background ──────────────────────
    // Model loading takes 5–15 s on first boot (DRP-AI TVM JIT compile).
    // We spawn a loader thread so the video stream above is already live
    // while the model is being initialised.
    ModelLoadCtx mctx;
    mctx.det       = new (std::nothrow) YoloDetector();
    mctx.model_dir = cfg->model_dir;
    mctx.state.store(0, std::memory_order_relaxed);
    pthread_t load_tid = {};
    if (!mctx.det) {
        AI_ERR("Failed to alloc YoloDetector");
        mctx.state.store(2, std::memory_order_relaxed);
    } else {
        pthread_create(&load_tid, nullptr, model_load_worker, &mctx);
    }

    // ── Async inference state (allocated upfront; worker starts later) ─
    InferState is;
    is.w         = cam_w;
    is.h         = cam_h;
    is.bgr_bytes = bgr_size;
    is.infer_buf = new (std::nothrow) uint8_t[bgr_size];
    is.stage_buf = new (std::nothrow) uint8_t[bgr_size];
    if (!is.infer_buf || !is.stage_buf) {
        AI_ERR("Failed to alloc inference double-buffers");
        // Non-fatal — inference disabled, streaming continues
        delete[] is.infer_buf; is.infer_buf = nullptr;
        delete[] is.stage_buf; is.stage_buf = nullptr;
    }

    pthread_attr_t infer_attr;
    pthread_attr_init(&infer_attr);
    pthread_attr_setschedpolicy(&infer_attr, SCHED_FIFO);
    struct sched_param infer_sched{};
    infer_sched.sched_priority = 40;
    pthread_attr_setschedparam(&infer_attr, &infer_sched);
    pthread_attr_setinheritsched(&infer_attr, PTHREAD_EXPLICIT_SCHED);
    // Pin inference to the next core after the capture thread so SCHED_FIFO prio-40
    // preemption stays isolated to its own core and doesn't starve the camera thread.
    // With capture on core 2 and inference on core 3, both can run in parallel
    // and cores 0-1 remain fully free for ROS2 / SLAM.
    const int infer_core = (cfg->cpu_core + 1) % 4;
    cpu_set_t infer_cpuset;
    CPU_ZERO(&infer_cpuset);
    CPU_SET(infer_core, &infer_cpuset);
    pthread_attr_setaffinity_np(&infer_attr, sizeof(cpu_set_t), &infer_cpuset);

    pthread_t      infer_tid     = {};
    bool           infer_started = false;

    ObstacleConfig obs_cfg;
    obs_cfg.roi_left_ratio      = 0.0f;
    obs_cfg.roi_right_ratio     = 1.0f;
    obs_cfg.steer_gain          = 1.2f;
    obs_cfg.slow_distance       = 3.0f;
    obs_cfg.slow_speed          = -0.3f;
    obs_cfg.person_only         = true;
    obs_cfg.stairs_class_ids[0] = 73;
    obs_cfg.stairs_class_ids[1] = 39;

    AI_LOG("Streaming started: %dx%d@%dfps — DRP-AI model loading in background",
           cam_w, cam_h, cfg->camera_fps);

    uint32_t frame_count = 0;

    // ── Main loop: capture + stream @ camera FPS ──────────────────────
    while (!force_stop
#ifdef ENABLE_AI_CAMERA
           && ai_thread_running
#endif
    ) {
        // Check if DRP-AI model just finished loading (one-time transition)
        if (!infer_started) {
            int st = mctx.state.load(std::memory_order_acquire);
            if (st == 1 && is.infer_buf && is.stage_buf) {
                is.detector = mctx.det;
                if (pthread_create(&infer_tid, &infer_attr, inference_worker, &is) != 0)
                    pthread_create(&infer_tid, nullptr, inference_worker, &is);
                infer_started = true;
                AI_LOG("DRP-AI model ready — inference active");
            } else if (st == 2) {
                AI_ERR("DRP-AI model failed to load — streaming without AI");
                infer_started = true;  // stop polling
                if (load_tid) pthread_join(load_tid, nullptr);
            }
        }

        // GrabFrame handles MJPEG (libjpeg lenient decode) and YUYV transparently.
        if (!camera.GrabFrame(bgr_buf)) {
            usleep(1000);
            continue;
        }

        // Submit to inference worker (non-blocking, skip if busy or not ready)
        if (is.detector && is.infer_buf) {
            pthread_mutex_lock(&is.mtx);
            if (!is.pending) {
                memcpy(is.stage_buf, bgr_buf, bgr_size);
                is.pending = true;
                pthread_cond_signal(&is.cond);
            }
            pthread_mutex_unlock(&is.mtx);
        }

        // Snapshot latest detections (empty until model loads)
        std::vector<Detection> cur_dets;
        float cur_inf_ms     = 0.0f;
        int   cur_model_size = 0;
        if (is.detector) {
            pthread_mutex_lock(&is.mtx);
            cur_dets       = is.dets;
            cur_inf_ms     = is.inf_ms;
            cur_model_size = is.model_size;
            pthread_mutex_unlock(&is.mtx);
        }

        // Obstacle avoidance → CR8 via RPMsg
        ObstacleAvoidPacket pkt = Process_Obstacles(
            cur_dets, cur_model_size, cur_model_size, obs_cfg);
        ai_update_obstacle_packet(pkt);

        // Detections → ROS2 bridge node via UDP broadcast
        if (det_sock >= 0)
            det_send(det_sock, cam_w, cam_h, cur_dets);

        // QGC H.264 stream (bbox overlay rendered inside SendFrame)
        if (qgc_streamer)
            qgc_streamer->SendFrame(bgr_buf, cam_w, cam_h, cur_dets,
                                    cur_model_size, cur_inf_ms);

        frame_count++;

        // Periodic person-detected log (~5s interval at 30fps)
        if (frame_count % 150 == 0) {
            int persons = 0;
            for (const auto& d : cur_dets)
                if (d.class_id == 0 && d.confidence >= obs_cfg.conf_threshold) persons++;
            if (persons > 0)
                AI_LOG("PERSON x%d | steer=%.2f rad/s | speed=%.2f m/s | dist=%.2fm",
                       persons, pkt.angular_z, pkt.linear_x, pkt.nearest_distance);
        }
    }

    // ── Shutdown ──────────────────────────────────────────────────────
    if (infer_started && is.detector) {
        pthread_mutex_lock(&is.mtx);
        is.stop = true;
        pthread_cond_signal(&is.cond);
        pthread_mutex_unlock(&is.mtx);
        pthread_join(infer_tid, nullptr);
    } else if (load_tid) {
        // Model was still loading — wait for loader thread to exit cleanly
        pthread_join(load_tid, nullptr);
    }

    pthread_attr_destroy(&infer_attr);

    if (det_sock >= 0) close(det_sock);

    if (qgc_streamer) {
        qgc_streamer->Shutdown();
        delete qgc_streamer;
    }

    delete[] is.infer_buf;
    delete[] is.stage_buf;
    delete[] bgr_buf;
    delete mctx.det;
    camera.Release();
    AI_LOG("AI Camera Thread exiting (frames=%u)", frame_count);
    return nullptr;
}

} // namespace AI
