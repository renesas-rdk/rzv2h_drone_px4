/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * gst_h264_streamer.cpp — GStreamer H.264 hardware-encoded stream to QGC
 *
 * Uses Renesas OMX encoder (omxh264enc / OMX.RENESAS.VIDEO.ENCODER.H264).
 * Input: BGR888 frames pushed via appsrc.
 * Output: RTP H.264 UDP stream on port 5600 (QGC default).
 *
 * Validated on RZ/V2H: ~1.2–1.5 Mbps actual at 2 Mbps target, 640×480@30fps,
 * ~14% CPU overhead (vs ~80% for software x264 + ~9 Mbps for JPEG/UDP).
 */
#include "gst_h264_streamer.h"
#include "draw_utils.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <sstream>

#define H264_LOG(fmt, ...) fprintf(stderr, "[GST-H264] " fmt "\n", ##__VA_ARGS__)
#define H264_ERR(fmt, ...) fprintf(stderr, "[GST-H264 ERR] " fmt "\n", ##__VA_ARGS__)

namespace AI {

GstH264Streamer::GstH264Streamer() = default;

GstH264Streamer::~GstH264Streamer()
{
    Shutdown();
}

// Build "host1:port,host2:port,..." string for multiudpsink from a comma-separated
// IP list (e.g. "192.168.1.188,192.168.1.126") and a single port.
static std::string build_multiudp_clients(const std::string& ip_csv, uint16_t port)
{
    std::string result;
    std::istringstream ss(ip_csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto a = token.find_first_not_of(" \t");
        auto b = token.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        if (!result.empty()) result += ',';
        result += token.substr(a, b - a + 1) + ':' + std::to_string(port);
    }
    return result;
}

bool GstH264Streamer::Init(const Config& cfg, int frame_w, int frame_h)
{
    if (cfg.dest_ip.empty()) {
        H264_ERR("dest_ip is empty — set QGC host IP");
        return false;
    }

    cfg_     = cfg;
    frame_w_ = frame_w;
    frame_h_ = frame_h;

    draw_buf_ = new (std::nothrow) uint8_t[(size_t)frame_w * frame_h * 3];
    if (!draw_buf_) {
        H264_ERR("Failed to allocate draw buffer (%dx%d BGR)", frame_w, frame_h);
        return false;
    }

    // Safe to call multiple times — GStreamer ignores if already initialised
    gst_init(nullptr, nullptr);

    // Pipeline: appsrc (BGR) → videoconvert → I420 → omxh264enc → RTP → UDP
    // omxh264enc requires I420 or NV12; I420 confirmed working on RZ/V2H.
    // do-timestamp=true: appsrc stamps each buffer with the pipeline running-time at
    //   push time — no manual PTS calculation, no possible clock-offset accumulation.
    // max-size-time=0 max-size-bytes=0: disable time/byte limits on queues; only
    //   the buffer count limit (1 or 2) applies, preventing silent time-based buffering.
    // leaky=2 (downstream): drop oldest frame when queue is full to bound end-to-end latency.
    // periodicity-idr=15: IDR every ~0.5s so QGC syncs quickly on stream join.
    // multiudpsink sends one encoded stream to multiple destinations simultaneously.
    // clients= accepts "host1:port,host2:port,..." for zero-cost fan-out.
    std::string clients = build_multiudp_clients(cfg.dest_ip, cfg.dest_port);

    // H.265 (HEVC) uses omxh265enc/h265parse/rtph265pay — same OMX HW, ~50% less bandwidth
    // than H.264 at equal visual quality. Enable via QGC_CODEC=h265 env var.
    // VBR (variable bitrate) distributes bits more efficiently than CBR: static scenes use
    // fewer bits, motion peaks use more, average bitrate stays at target.
    const char* enc_elem  = cfg.use_h265 ? "omxh265enc" : "omxh264enc";
    const char* parse_elem= cfg.use_h265 ? "h265parse"  : "h264parse";
    const char* pay_elem  = cfg.use_h265 ? "rtph265pay" : "rtph264pay";

    char pipeline_str[1024];
    snprintf(pipeline_str, sizeof(pipeline_str),
        "appsrc name=src format=time is-live=true block=false do-timestamp=true "
        "! video/x-raw,format=BGR,width=%d,height=%d,framerate=%d/1 "
        "! videoconvert "
        "! video/x-raw,format=I420 "
        "! queue max-size-buffers=2 max-size-time=0 max-size-bytes=0 leaky=2 "
        "! %s target-bitrate=%d control-rate=variable b-frames=0 periodicity-idr=15 "
        "! %s "
        "! queue max-size-buffers=2 max-size-time=0 max-size-bytes=0 leaky=2 "
        "! %s config-interval=-1 pt=96 perfect-rtptime=false "
        "! multiudpsink clients=%s sync=false async=false",
        frame_w, frame_h, cfg.fps,
        enc_elem, cfg.bitrate,
        parse_elem,
        pay_elem,
        clients.c_str());

    H264_LOG("Pipeline: %s", pipeline_str);

    GError* err = nullptr;
    pipeline_ = gst_parse_launch(pipeline_str, &err);
    if (!pipeline_ || err) {
        H264_ERR("gst_parse_launch failed: %s", err ? err->message : "unknown error");
        if (err) g_error_free(err);
        delete[] draw_buf_; draw_buf_ = nullptr;
        return false;
    }

    appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
    if (!appsrc_) {
        H264_ERR("Could not find appsrc element in pipeline");
        gst_object_unref(pipeline_); pipeline_ = nullptr;
        delete[] draw_buf_; draw_buf_ = nullptr;
        return false;
    }

    g_object_set(appsrc_,
        "stream-type", 0,          // GST_APP_STREAM_TYPE_STREAM
        "format",      GST_FORMAT_TIME,
        "is-live",     TRUE,
        nullptr);

    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        H264_ERR("Failed to set pipeline to PLAYING — check omxh264enc / OMX libs");
        gst_object_unref(appsrc_);  appsrc_   = nullptr;
        gst_object_unref(pipeline_); pipeline_ = nullptr;
        delete[] draw_buf_; draw_buf_ = nullptr;
        return false;
    }

    H264_LOG("%s QGC stream ready → [%s]  %d kbps  %dfps",
            cfg.use_h265 ? "H.265" : "H.264",
            clients.c_str(), cfg.bitrate / 1000, cfg.fps);
    return true;
}

void GstH264Streamer::Shutdown()
{
    if (pipeline_) {
        // Send EOS so encoder flushes its last frames before we stop
        gst_element_send_event(pipeline_, gst_event_new_eos());
        GstBus* bus = gst_element_get_bus(pipeline_);
        if (bus) {
            // Wait up to 500 ms for EOS or error
            gst_bus_timed_pop_filtered(bus, 500 * GST_MSECOND,
                (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            gst_object_unref(bus);
        }
        gst_element_set_state(pipeline_, GST_STATE_NULL);
    }
    if (appsrc_) {
        gst_object_unref(appsrc_);
        appsrc_ = nullptr;
    }
    if (pipeline_) {
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    if (draw_buf_) {
        delete[] draw_buf_;
        draw_buf_ = nullptr;
    }
    H264_LOG("Shutdown — sent %" PRIu64 " frames", frame_id_);
}

bool GstH264Streamer::SendFrame(const uint8_t* bgr_buf, int width, int height,
                                 const std::vector<Detection>& dets,
                                 int model_size, float inference_ms)
{
    if (!pipeline_ || !appsrc_ || !draw_buf_) return false;

    frame_counter_++;
    if (cfg_.skip_frames > 1 && (frame_counter_ % cfg_.skip_frames != 0))
        return false;

    const size_t frame_bytes = (size_t)width * height * 3;

    // Draw bounding boxes onto a copy of the frame
    memcpy(draw_buf_, bgr_buf, frame_bytes);
    draw_detections_mapped(draw_buf_, width, height, dets, model_size);

    // Stats overlay (top-left): "AI:59MS 2DET"
    // Shows DRP-AI inference latency and detection count (stream always runs at camera FPS)
    char overlay[64];
    snprintf(overlay, sizeof(overlay), "AI:%dMS %dDET",
             (int)inference_ms, (int)dets.size());
    int ow = (int)strlen(overlay) * 6 + 4;
    for (int y = 2; y < 12; ++y)
        for (int x = 2; x < ow + 2 && x < width; ++x)
            draw_pixel(draw_buf_, width, height, x, y, 0, 0, 0);
    draw_text(draw_buf_, width, height, 4, 3, overlay, 0, 255, 0);

    // Wrap frame data in a GstBuffer (zero-copy not possible without DMA buf)
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, frame_bytes, nullptr);
    if (!buf) return false;

    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buf);
        return false;
    }
    memcpy(map.data, draw_buf_, frame_bytes);
    gst_buffer_unmap(buf, &map);

    // PTS is assigned by appsrc do-timestamp=true at push time (pipeline running_time).
    // This guarantees zero offset between PTS and pipeline clock — no accumulation possible.
    // Hint DURATION so downstream elements know the nominal frame interval.
    GST_BUFFER_DURATION(buf) = GST_SECOND / (guint64)cfg_.fps;

    GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);
    // buf is now owned by GStreamer regardless of return value
    if (flow != GST_FLOW_OK) {
        // Downstream error (encoder stalled, network error, etc.) — not fatal
        H264_ERR("gst_app_src_push_buffer returned %d", (int)flow);
        return false;
    }

    frame_id_++;
    return true;
}

} // namespace AI
