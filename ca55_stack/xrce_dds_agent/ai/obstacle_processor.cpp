/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file obstacle_processor.cpp
 * @brief Obstacle avoidance logic for YOLOv8n detections
 */
#include "obstacle_processor.h"
#include <algorithm>
#include <cmath>
#include <ctime>

namespace AI {

/* ── COCO-80 class names ───────────────────────────────────────────── */

static const char* COCO_NAMES[80] = {
    "person",        "bicycle",      "car",           "motorcycle",
    "airplane",      "bus",          "train",         "truck",
    "boat",          "traffic light","fire hydrant",  "stop sign",
    "parking meter", "bench",        "bird",          "cat",
    "dog",           "horse",        "sheep",         "cow",
    "elephant",      "bear",         "zebra",         "giraffe",
    "backpack",      "umbrella",     "handbag",       "tie",
    "suitcase",      "frisbee",      "skis",          "snowboard",
    "sports ball",   "kite",         "baseball bat",  "baseball glove",
    "skateboard",    "surfboard",    "tennis racket",  "bottle",
    "wine glass",    "cup",          "fork",          "knife",
    "spoon",         "bowl",         "banana",        "apple",
    "sandwich",      "orange",       "broccoli",      "carrot",
    "hot dog",       "pizza",        "donut",         "cake",
    "chair",         "couch",        "potted plant",  "bed",
    "dining table",  "toilet",       "tv",            "laptop",
    "mouse",         "remote",       "keyboard",      "cell phone",
    "microwave",     "oven",         "toaster",       "sink",
    "refrigerator",  "book",         "clock",         "vase",
    "scissors",      "teddy bear",   "hair drier",    "toothbrush"
};

const char* coco_label(int class_id)
{
    if (class_id >= 0 && class_id < 80)
        return COCO_NAMES[class_id];
    return "unknown";
}

/* ── Helper: check if class_id is in the stairs list ───────────────── */

static bool is_stairs_class(int class_id, const ObstacleConfig& cfg)
{
    for (int i = 0; i < 4; ++i) {
        if (cfg.stairs_class_ids[i] < 0) break;
        if (cfg.stairs_class_ids[i] == class_id) return true;
    }
    return false;
}

/* ── Main processing function ──────────────────────────────────────── */

ObstacleAvoidPacket Process_Obstacles(const std::vector<Detection>& objects,
                                      int frame_w, int frame_h,
                                      const ObstacleConfig& cfg)
{
    ObstacleAvoidPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header = HIL_HEADER;
    pkt.type   = TYPE_OBSTACLE;
    pkt.num_detections = static_cast<uint8_t>(std::min<size_t>(objects.size(), 255));

    const float roi_x1 = frame_w * cfg.roi_left_ratio;
    const float roi_x2 = frame_w * cfg.roi_right_ratio;
    const float frame_area = static_cast<float>(frame_w * frame_h);

    float max_area   = 0.0f;
    float nearest_cx = frame_w / 2.0f;
    bool  jump       = false;

    for (const auto& det : objects) {
        if (det.confidence < cfg.conf_threshold)
            continue;

        if (cfg.person_only && det.class_id != 0)
            continue;

        const float cx   = (det.bbox.x1 + det.bbox.x2) * 0.5f;
        const float area = (det.bbox.x2 - det.bbox.x1) * (det.bbox.y2 - det.bbox.y1);

        // Only process objects whose center falls inside the path ROI
        if (cx < roi_x1 || cx > roi_x2)
            continue;

        // Distance estimation from bounding-box area ratio
        // Empirical: area_ratio ~15% => ~0.5 m, ~5% => ~1.5 m, ~1% => ~3 m
        const float area_ratio = area / frame_area;
        float est_dist = 0.5f / (area_ratio / 0.15f + 0.001f);
        est_dist = std::min(est_dist, 5.0f);

        if (area > max_area) {
            max_area   = area;
            nearest_cx = cx;
            pkt.nearest_distance = est_dist;
        }

        // Jump trigger for stairs-like classes
        if (det.confidence > cfg.stairs_conf && is_stairs_class(det.class_id, cfg))
            jump = true;
    }

    // Generate steering command
    if (max_area > 0.0f) {
        // center_offset: -1 (far left) .. 0 (center) .. +1 (far right)
        const float center_offset =
            (nearest_cx - frame_w * 0.5f) / (frame_w * 0.5f);
        // Steer away: obstacle on right -> positive angular_z (turn left)
        pkt.angular_z = -center_offset * cfg.steer_gain;

        // Slow down when obstacle is close
        if (pkt.nearest_distance < cfg.slow_distance)
            pkt.linear_x = cfg.slow_speed;
    }

    pkt.jump_trigger = jump ? 1 : 0;

    // Monotonic timestamp
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pkt.timestamp_ms = static_cast<uint32_t>(
        ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

    return pkt;
}

} // namespace AI
