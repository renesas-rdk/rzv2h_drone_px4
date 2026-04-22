/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file obstacle_processor.h
 * @brief Object detection data structures and obstacle avoidance logic
 *
 * Processes YOLOv8n detection results and generates steering/jump commands
 * sent via HIL UDP (type=2) to PC/RViz.
 */
#ifndef AI_OBSTACLE_PROCESSOR_H
#define AI_OBSTACLE_PROCESSOR_H

#include <cstdint>
#include <cstring>
#include <vector>

namespace AI {

/* ── Detection primitives ───────────────────────────────────────────── */

struct BBox {
    float x1, y1, x2, y2; // pixel coordinates in model input space (640x640)
};

struct Detection {
    BBox    bbox;
    float   confidence;
    int     class_id;      // COCO class index (0-79)
};

/* ── HIL Obstacle Avoidance Packet (type = 2) ───────────────────────── */

static constexpr uint32_t HIL_HEADER   = 0x48494C42; // "HILB"
static constexpr uint8_t  TYPE_OBSTACLE = 2;          // 0=CAN TX, 1=CAN RX, 2=obstacle

struct ObstacleAvoidPacket {
    uint32_t header;           // 0x48494C42
    uint8_t  type;             // 2
    float    angular_z;        // steering cmd (rad/s), positive = turn left
    float    linear_x;         // forward speed override (m/s), 0 = no override
    uint8_t  jump_trigger;     // 1 = request jump
    uint8_t  num_detections;   // total objects detected this frame
    float    nearest_distance; // estimated distance to nearest in-path obstacle (m)
    uint32_t timestamp_ms;     // monotonic clock ms
} __attribute__((packed));

/* ── Tunable parameters ─────────────────────────────────────────────── */

struct ObstacleConfig {
    float roi_left_ratio   = 0.30f;  // center ROI left boundary  (% of width)
    float roi_right_ratio  = 0.70f;  // center ROI right boundary (% of width)
    float conf_threshold   = 0.25f;  // minimum confidence to consider
    float stairs_conf      = 0.70f;  // confidence threshold for stairs->jump
    float steer_gain       = 1.5f;   // proportional steering gain
    float slow_distance    = 1.0f;   // distance (m) below which we slow down
    float slow_speed       = 0.1f;   // crawl speed when close (m/s)
    // COCO class IDs treated as "stairs" (no native stairs class in COCO-80,
    // so we use a list that can be extended with custom-trained models)
    int   stairs_class_ids[4] = {-1, -1, -1, -1}; // set via config; -1 = unused
    bool  person_only = false; // if true, only process class_id == 0 (person)
};

/* ── API ────────────────────────────────────────────────────────────── */

/**
 * Analyse detections and produce a single avoidance command packet.
 *
 * @param objects     vector of detections from YoloDetector::Run()
 * @param frame_w     model input width  (typically 640)
 * @param frame_h     model input height (typically 640)
 * @param cfg         tuning parameters
 * @return            filled ObstacleAvoidPacket ready to sendto()
 */
ObstacleAvoidPacket Process_Obstacles(const std::vector<Detection>& objects,
                                      int frame_w, int frame_h,
                                      const ObstacleConfig& cfg);

/* ── COCO-80 label table ────────────────────────────────────────────── */

const char* coco_label(int class_id);

} // namespace AI

#endif // AI_OBSTACLE_PROCESSOR_H
