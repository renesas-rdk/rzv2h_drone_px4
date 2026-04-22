/*
 * Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file yolo_detector.h
 * @brief YOLOv8n object detector using Renesas DRP-AI V2H Runtime
 *
 * Cut model (Detect head removed): 6 raw Conv outputs per inference.
 * CPU postprocessing: DFL decode + sigmoid + anchor-free bbox decode + NMS.
 */
#ifndef AI_YOLO_DETECTOR_H
#define AI_YOLO_DETECTOR_H

#include "obstacle_processor.h" // Detection, BBox
#include "MeraDrpRuntimeWrapper.h"
#include "PreRuntime.h"
#include <vector>
#include <string>

namespace AI {

class YoloDetector {
public:
    YoloDetector();
    ~YoloDetector();

    // Non-copyable
    YoloDetector(const YoloDetector&) = delete;
    YoloDetector& operator=(const YoloDetector&) = delete;

    /**
     * Load a DRP-AI compiled model using MeraDrpRuntime.
     * @param model_dir  directory containing mera.plan / preprocess / etc.
     * @return true on success
     */
    bool Init(const std::string& model_dir);

    /**
     * Run inference on raw YUYV (YUY2) image.
     * Fastest path for V4L2 camera input.
     *
     * @param yuyv_data pointer to YUYV pixel data (w * h * 2 bytes)
     * @param src_w     source image width
     * @param src_h     source image height
     * @return          vector of detections
     */
    std::vector<Detection> RunOnYUYV(const uint8_t* yuyv_data, int src_w, int src_h);

    /** Last inference time in milliseconds (DRP-AI Run() only). */
    float GetLastInferenceMs() const { return last_inference_ms_; }

    /** Model input dimension (always 640 for YOLOv8n). */
    int GetInputSize() const { return input_size_; }

private:
    /* DRP-AI runtime wrapper (TVM-based) */
    MeraDrpRuntimeWrapper runtime_;
    /* DRP-AI Preprocessing Runtime */
    PreRuntime preruntime_;

    /* Model parameters */
    int      input_size_;      // 640
    int      num_classes_;     // 80
    int      num_anchors_;     // 8400 = 80*80 + 40*40 + 20*20

    /* NMS thresholds */
    float    conf_thresh_;     // 0.25
    float    nms_iou_thresh_;  // 0.45

    float    last_inference_ms_;

    /* ── Scale info for 3 detection heads ─────────────────── */
    static constexpr int NUM_SCALES = 3;
    static constexpr int STRIDES[NUM_SCALES] = {8, 16, 32};
    static constexpr int GRID_SIZES[NUM_SCALES] = {80, 40, 20}; // 640/stride
    static constexpr int REG_MAX = 16;  // DFL register_max

    /* ── Internal methods ─────────────────────────────────────── */

    /**
     * Decode 6 raw Conv outputs from cut model + apply NMS.
     * Output order from DRP-AI (matches cut model):
     *   0: cv2.0 [1,64,80,80]  bbox features, stride 8
     *   1: cv2.1 [1,64,40,40]  bbox features, stride 16
     *   2: cv2.2 [1,64,20,20]  bbox features, stride 32
     *   3: cv3.0 [1,80,80,80]  class features, stride 8
     *   4: cv3.1 [1,80,40,40]  class features, stride 16
     *   5: cv3.2 [1,80,20,20]  class features, stride 32
     */
    std::vector<Detection> PostprocessMultiOutput();

    /** DFL decode: [64, n_anchors] -> [4, n_anchors] (ltrb distances). */
    static void DflDecode(const float* bbox_feat, int n_anchors, float* ltrb_out);

    /** Non-Maximum Suppression (greedy, per-class). */
    static void NMS(std::vector<Detection>& dets, float iou_thresh);
};

} // namespace AI

#endif // AI_YOLO_DETECTOR_H
