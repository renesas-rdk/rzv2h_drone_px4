/**
 * @file yolo_detector.cpp
 * @brief YOLOv8n DRP-AI inference implementation using V2H Runtime
 *
 * Cut model with 6 raw Conv outputs (Detect head removed).
 * CPU postprocessing: DFL decode + sigmoid + anchor-free bbox decode + NMS.
 *
 * NEON FP16 optimization (Session 7):
 *   - Class scan uses float16x8_t (8 anchors per NEON instruction)
 *   - No FP16→FP32 conversion during scan (only for passing anchors)
 *   - Eliminates ~672K unnecessary conversions per frame
 */
#include "yolo_detector.h"
#include "drpai_helpers.h"
#include <builtin_fp16.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <cstring>
#include <sys/time.h>

#if defined(__ARM_FEATURE_FP16_VECTOR_ARITHMETIC)
#include <arm_neon.h>
#define USE_NEON_FP16 1
#else
#define USE_NEON_FP16 0
#endif

static float float16_to_float32(uint16_t a)
{
    return __extendXfYf2__<uint16_t, uint16_t, 10, float, uint32_t, 23>(a);
}

namespace AI {

/* ── Static constants ──────────────────────────────────────────────── */

constexpr int YoloDetector::STRIDES[NUM_SCALES];
constexpr int YoloDetector::GRID_SIZES[NUM_SCALES];

/* ── Helpers ────────────────────────────────────────────────────────── */

static double now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static float iou(const BBox& a, const BBox& b)
{
    const float ix1 = std::max(a.x1, b.x1);
    const float iy1 = std::max(a.y1, b.y1);
    const float ix2 = std::min(a.x2, b.x2);
    const float iy2 = std::min(a.y2, b.y2);
    const float iw  = std::max(0.0f, ix2 - ix1);
    const float ih  = std::max(0.0f, iy2 - iy1);
    const float inter = iw * ih;
    const float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    const float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    return inter / (area_a + area_b - inter + 1e-6f);
}

/* ── Constructor / Destructor ──────────────────────────────────────── */

YoloDetector::YoloDetector()
    : input_size_(640)
    , num_classes_(80)
    , num_anchors_(8400)  // 80*80 + 40*40 + 20*20
    , conf_thresh_(0.25f)
    , nms_iou_thresh_(0.45f)
    , last_inference_ms_(0.0f)
{
}

YoloDetector::~YoloDetector()
{
}

/* ── Init ──────────────────────────────────────────────────────────── */

bool YoloDetector::Init(const std::string& model_dir)
{
    std::cout << "[YoloDetector] Initialising DRP-AI Runtime..." << std::endl;

    // Get DRP-AI memory start address via ioctl (required on V2H)
    uint64_t drpai_start_addr = 0;
    {
        int fd = open(DRPAI_DEV, O_RDWR);
        if (fd < 0) {
            std::cerr << "[ERROR] Failed to open " << DRPAI_DEV << ": " << strerror(errno) << std::endl;
            return false;
        }
        drpai_data_t drpai_area;
        if (ioctl(fd, DRPAI_GET_DRPAI_AREA, &drpai_area) != 0) {
            std::cerr << "[ERROR] DRPAI_GET_DRPAI_AREA failed: " << strerror(errno) << std::endl;
            close(fd);
            return false;
        }
        close(fd);
        drpai_start_addr = drpai_area.address;
        std::cout << "[YoloDetector] DRP-AI memory start: 0x" << std::hex << drpai_start_addr << std::dec << std::endl;
    }

    // Load PreRuntime FIRST (claims END of DRP-AI memory)
    std::string pre_dir = model_dir + "/preprocess";
    std::cout << "[YoloDetector] Loading PreRuntime from: " << pre_dir << std::endl;
    uint8_t ret = preruntime_.Load(pre_dir);
    if (ret != PRE_SUCCESS) {
        std::cerr << "[ERROR] Failed to load PreRuntime from " << pre_dir << std::endl;
        return false;
    }
    // Load DRP-AI Model SECOND (occupies START of DRP-AI memory)
    if (!runtime_.LoadModel(model_dir, drpai_start_addr)) {
        std::cerr << "[ERROR] Failed to load model via MeraDrpRuntimeWrapper" << std::endl;
        return false;
    }
    // Verify output count (cut model should have 6 outputs)
    int num_out = runtime_.GetNumOutput();
    std::cout << "[YoloDetector] Model loaded. Outputs: " << num_out << std::endl;
    if (num_out != 6) {
        std::cerr << "[WARN] Expected 6 outputs (cut model), got " << num_out
                  << ". Postprocess may fail!" << std::endl;
    }

    std::cout << "[YoloDetector] Model and PreRuntime loaded successfully." << std::endl;
    return true;
}

/* ── RunOnYUYV ─────────────────────────────────────────────────────── */

std::vector<Detection> YoloDetector::RunOnYUYV(const uint8_t* yuyv_data, int src_w, int src_h)
{
    if (!yuyv_data) return {};

    double t0 = now_ms();

    // ── Hardware Preprocessing (DRP-AI) ─────────────────────────────────
    s_preproc_param_t pre_param;
    pre_param.pre_in_shape_w = src_w;
    pre_param.pre_in_shape_h = src_h;
    pre_param.pre_in_addr    = (uintptr_t)yuyv_data;

    void* output_ptr = nullptr;
    uint32_t out_size = 0;
    uint8_t ret = preruntime_.Pre(&pre_param, &output_ptr, &out_size);
    double t_after_pre = now_ms();

    if (ret != PRE_SUCCESS) {
        std::cerr << "[ERROR] PreRuntime::Pre (YUYV) failed" << std::endl;
        return {};
    }

    // PreRuntime returns element count (not byte count)
    const uint32_t expected_elements = input_size_ * input_size_ * 3;
    if (out_size != expected_elements) {
        std::cerr << "[ERROR] PreRuntime output size mismatch: " << out_size
                  << " vs expected " << expected_elements << std::endl;
        return {};
    }

    // ── Set Input and Run Inference ─────────────────────────────────────
    double t_before_setinput = now_ms();
    runtime_.SetInput(0, (float*)output_ptr);
    double t_before_run = now_ms();
    runtime_.Run();
    double t_after_run = now_ms();

    last_inference_ms_ = static_cast<float>(t_after_run - t0);

    // Print timing (first 5 frames, then every 50th frame)
    // static int timing_cnt = 0;
    // if (++timing_cnt <= 5 || timing_cnt % 50 == 0) {
    //     fprintf(stderr, "[PERF] Pre=%.1fms SetInput=%.1fms Run=%.1fms\n",
    //             t_after_pre - t0,
    //             t_before_run - t_before_setinput,
    //             t_after_run - t_before_run);
    // }

    // ── Postprocess 6 outputs ───────────────────────────────────────────
    // double t_before_post = now_ms();
    auto dets = PostprocessMultiOutput();
    // double t_after_post = now_ms();

    // if (timing_cnt <= 5 || timing_cnt % 50 == 0) {
    //     fprintf(stderr, "[PERF] Post=%.1fms (dets=%zu) Total=%.1fms\n",
    //             t_after_post - t_before_post, dets.size(),
    //             t_after_post - t0);
    // }

    return dets;
}

/* ── DFL Decode (single anchor) ─────────────────────────────────── */

void YoloDetector::DflDecode(const float* bbox_feat, int n_anchors, float* ltrb_out)
{
    // Decode DFL for a SINGLE anchor.
    // bbox_feat points to start of bbox features for this scale: [64, n_anchors]
    // n_anchors = anchor index (overloaded: used as stride between channels)
    // ltrb_out receives 4 decoded distance values
    //
    // Layout: feat[(d*16+b) * stride + anchor_idx]
    // But here we call with bbox_feat already offset to the right anchor.

    // NOTE: This function is now called per-anchor with:
    //   bbox_feat = scale_bbox_data (full [64, H*W] array)
    //   n_anchors = H*W (stride between channels)
    //   ltrb_out  = 4-element output for this anchor
    // The anchor index is baked into bbox_feat offset.
    // Actually, let's keep it flexible with an anchor index parameter.
    // Caller should pass anchor_idx via ltrb_out context.

    // This overload is no longer used. See DflDecodeOne below.
    (void)bbox_feat;
    (void)n_anchors;
    (void)ltrb_out;
}

/** Decode DFL for a single anchor at index 'a' from bbox features [64, n_total]. */
static void DflDecodeOne(const float* bbox_feat, int n_total, int a, float ltrb[4])
{
    for (int d = 0; d < 4; ++d) {
        float vals[16];
        float max_val = -1e9f;
        for (int b = 0; b < 16; ++b) {
            vals[b] = bbox_feat[(d * 16 + b) * n_total + a];
            if (vals[b] > max_val) max_val = vals[b];
        }

        float sum_exp = 0.0f;
        for (int b = 0; b < 16; ++b) {
            vals[b] = expf(vals[b] - max_val);
            sum_exp += vals[b];
        }

        float dist = 0.0f;
        float inv_sum = 1.0f / sum_exp;
        for (int b = 0; b < 16; ++b) {
            dist += (vals[b] * inv_sum) * (float)b;
        }
        ltrb[d] = dist;
    }
}

/* ── Postprocess Multi-Output ──────────────────────────────────────── */

std::vector<Detection> YoloDetector::PostprocessMultiOutput()
{
    /*
     * DRP-AI outputs 6 tensors (cut model). Order detected by size:
     *   bbox (64-ch): cv2.x at each scale [1,64,H,W]
     *   class (80-ch): cv3.x at each scale [1,80,H,W]
     *
     * Optimized pipeline per scale:
     *   1. Scan class FP16 directly with NEON (no FP32 conversion!)
     *   2. Only for anchors passing raw threshold: convert bbox FP16→FP32, DFL decode
     *   3. NMS on concatenated results
     */

    // Get all 6 raw outputs (keep FP16 pointers for lazy conversion)
    struct RawOutput {
        InOutDataType type;
        void* ptr;
        int64_t count;
        int channels;
        int grid_size;
    };
    RawOutput raw_outputs[6];

    int bbox_idx[3] = {-1, -1, -1};
    int cls_idx[3]  = {-1, -1, -1};
    int bbox_count = 0, cls_count = 0;

    for (int i = 0; i < 6; ++i) {
        auto [type, ptr, count] = runtime_.GetOutput(i);
        raw_outputs[i] = {type, ptr, count, 0, 0};

        bool is_bbox = false;
        int grid = 0;
        for (int s = 0; s < NUM_SCALES; ++s) {
            int gs = GRID_SIZES[s];
            if (count == 64 * gs * gs) { is_bbox = true; grid = gs; break; }
            if (count == 80 * gs * gs) { is_bbox = false; grid = gs; break; }
        }
        if (grid == 0) continue;

        raw_outputs[i].channels = is_bbox ? 64 : 80;
        raw_outputs[i].grid_size = grid;

        if (is_bbox) {
            int scale = (grid == 80) ? 0 : (grid == 40) ? 1 : 2;
            bbox_idx[scale] = i;
            bbox_count++;
        } else {
            int scale = (grid == 80) ? 0 : (grid == 40) ? 1 : 2;
            cls_idx[scale] = i;
            cls_count++;
        }
    }

    if (bbox_count != 3 || cls_count != 3) {
        fprintf(stderr, "[ERROR] Expected 3 bbox + 3 class outputs, got %d + %d\n",
                bbox_count, cls_count);
        return {};
    }

    // Raw threshold: sigmoid(x) > conf_thresh ⟺ x > log(thresh / (1 - thresh))
    const float raw_thresh = logf(conf_thresh_ / (1.0f - conf_thresh_));

    std::vector<Detection> all_dets;
    all_dets.reserve(256);

    // Reusable buffers (avoid repeated allocation)
    static thread_local std::vector<uint16_t> max_scores_fp16;
    static thread_local std::vector<int16_t>  max_classes_i16;
    // Fallback buffers (non-NEON path)
    static thread_local std::vector<float>    max_scores;
    static thread_local std::vector<int>      max_classes;
    static thread_local std::vector<float>    channel_buf;

    for (int s = 0; s < NUM_SCALES; ++s) {
        const int gs       = GRID_SIZES[s];
        const int stride   = STRIDES[s];
        const int n_anchor = gs * gs;
        const int ci       = cls_idx[s];
        const int bi       = bbox_idx[s];

        const auto& cls_out = raw_outputs[ci];
        const uint16_t* cls_fp16 = (cls_out.type == InOutDataType::FLOAT16)
                                   ? static_cast<const uint16_t*>(cls_out.ptr) : nullptr;
        const float* cls_fp32 = (cls_out.type != InOutDataType::FLOAT16)
                                ? static_cast<const float*>(cls_out.ptr) : nullptr;

        const auto& bbox_out = raw_outputs[bi];
        const uint16_t* bbox_fp16 = (bbox_out.type == InOutDataType::FLOAT16)
                                    ? static_cast<const uint16_t*>(bbox_out.ptr) : nullptr;
        const float* bbox_fp32 = (bbox_out.type != InOutDataType::FLOAT16)
                                 ? static_cast<const float*>(bbox_out.ptr) : nullptr;

#if USE_NEON_FP16
        if (cls_fp16) {
            // ══════════════════════════════════════════════════════════════
            // NEON FP16 CLASS SCAN — Process 8 anchors per instruction
            // No FP16→FP32 conversion during scan phase!
            // ══════════════════════════════════════════════════════════════

            // Pad n_anchor to multiple of 8 for NEON
            const int n_anchor_pad = (n_anchor + 7) & ~7;

            max_scores_fp16.resize(n_anchor_pad);
            max_classes_i16.resize(n_anchor_pad);

            // Initialize: FP16 -inf = 0xFC00
            {
                const float16x8_t neg_inf = vdupq_n_f16(-65504.0f);
                const int16x8_t   zero_cls = vdupq_n_s16(0);
                for (int a = 0; a < n_anchor_pad; a += 8) {
                    vst1q_f16(reinterpret_cast<float16_t*>(&max_scores_fp16[a]), neg_inf);
                    vst1q_s16(&max_classes_i16[a], zero_cls);
                }
            }

            // Scan all 80 classes: find per-anchor max score + class ID
            for (int c = 0; c < num_classes_; ++c) {
                const float16_t* src = reinterpret_cast<const float16_t*>(cls_fp16 + c * n_anchor);
                const int16x8_t class_vec = vdupq_n_s16(static_cast<int16_t>(c));

                for (int a = 0; a < n_anchor; a += 8) {
                    float16x8_t vals = vld1q_f16(src + a);
                    float16x8_t maxs = vld1q_f16(reinterpret_cast<const float16_t*>(&max_scores_fp16[a]));

                    // Compare: vals > maxs
                    uint16x8_t mask = vcgtq_f16(vals, maxs);

                    // Update max scores: select new where vals > maxs
                    vst1q_f16(reinterpret_cast<float16_t*>(&max_scores_fp16[a]),
                              vbslq_f16(mask, vals, maxs));

                    // Update class IDs: select new class where vals > maxs
                    int16x8_t old_cls = vld1q_s16(&max_classes_i16[a]);
                    vst1q_s16(&max_classes_i16[a],
                              vbslq_s16(mask, class_vec, old_cls));
                }
            }

            // Convert threshold to FP16 for NEON comparison
            const float16_t raw_thresh_f16 = static_cast<float16_t>(raw_thresh);
            const float16x8_t thresh_vec = vdupq_n_f16(raw_thresh_f16);

            // ── Step 2: Check threshold and decode passing anchors ──
            for (int a_base = 0; a_base < n_anchor; a_base += 8) {
                float16x8_t scores = vld1q_f16(reinterpret_cast<const float16_t*>(&max_scores_fp16[a_base]));
                uint16x8_t pass_mask = vcgtq_f16(scores, thresh_vec);

                // Quick reject: if no anchor in this group passes, skip entirely
                if (vmaxvq_u16(pass_mask) == 0) continue;

                // Process individual passing anchors (rare: typically <1% pass)
                const int a_end = std::min(a_base + 8, n_anchor);
                for (int a = a_base; a < a_end; ++a) {
                    // Convert only passing anchors to FP32
                    float score_f32 = float16_to_float32(max_scores_fp16[a]);
                    if (score_f32 < raw_thresh) continue;

                    float best_score = 1.0f / (1.0f + expf(-score_f32));
                    int best_class = max_classes_i16[a];

                    int ay = a / gs;
                    int ax = a % gs;

                    // DFL decode for this anchor only (on-demand FP16→FP32)
                    float ltrb[4];
                    for (int d = 0; d < 4; ++d) {
                        float vals[16];
                        float max_val = -1e9f;
                        for (int b = 0; b < 16; ++b) {
                            int idx = (d * 16 + b) * n_anchor + a;
                            float v = bbox_fp16 ? float16_to_float32(bbox_fp16[idx])
                                                : bbox_fp32[idx];
                            vals[b] = v;
                            if (v > max_val) max_val = v;
                        }
                        float sum_exp = 0.0f;
                        for (int b = 0; b < 16; ++b) {
                            vals[b] = expf(vals[b] - max_val);
                            sum_exp += vals[b];
                        }
                        float dist = 0.0f;
                        float inv = 1.0f / sum_exp;
                        for (int b = 0; b < 16; ++b)
                            dist += (vals[b] * inv) * static_cast<float>(b);
                        ltrb[d] = dist;
                    }

                    float cx = (ax + 0.5f) * stride;
                    float cy = (ay + 0.5f) * stride;

                    Detection det;
                    det.bbox.x1    = cx - ltrb[0] * stride;
                    det.bbox.y1    = cy - ltrb[1] * stride;
                    det.bbox.x2    = cx + ltrb[2] * stride;
                    det.bbox.y2    = cy + ltrb[3] * stride;
                    det.confidence = best_score;
                    det.class_id   = best_class;
                    all_dets.push_back(det);
                }
            }
        } else
#endif // USE_NEON_FP16
        {
            // ══════════════════════════════════════════════════════════════
            // SCALAR FALLBACK — Original channel-first scan with FP32
            // ══════════════════════════════════════════════════════════════
            max_scores.assign(n_anchor, -1e9f);
            max_classes.assign(n_anchor, 0);
            channel_buf.resize(n_anchor);

            for (int c = 0; c < num_classes_; ++c) {
                const float* row;
                if (cls_fp16) {
                    const uint16_t* src = cls_fp16 + c * n_anchor;
                    for (int a = 0; a < n_anchor; ++a)
                        channel_buf[a] = float16_to_float32(src[a]);
                    row = channel_buf.data();
                } else {
                    row = cls_fp32 + c * n_anchor;
                }

                for (int a = 0; a < n_anchor; ++a) {
                    if (row[a] > max_scores[a]) {
                        max_scores[a] = row[a];
                        max_classes[a] = c;
                    }
                }
            }

            for (int ay = 0; ay < gs; ++ay) {
                for (int ax = 0; ax < gs; ++ax) {
                    const int a = ay * gs + ax;

                    if (max_scores[a] < raw_thresh)
                        continue;

                    float best_score = 1.0f / (1.0f + expf(-max_scores[a]));

                    float ltrb[4];
                    for (int d = 0; d < 4; ++d) {
                        float vals[16];
                        float max_val = -1e9f;
                        for (int b = 0; b < 16; ++b) {
                            int idx = (d * 16 + b) * n_anchor + a;
                            float v = bbox_fp16 ? float16_to_float32(bbox_fp16[idx])
                                                : bbox_fp32[idx];
                            vals[b] = v;
                            if (v > max_val) max_val = v;
                        }
                        float sum_exp = 0.0f;
                        for (int b = 0; b < 16; ++b) {
                            vals[b] = expf(vals[b] - max_val);
                            sum_exp += vals[b];
                        }
                        float dist = 0.0f;
                        float inv = 1.0f / sum_exp;
                        for (int b = 0; b < 16; ++b)
                            dist += (vals[b] * inv) * static_cast<float>(b);
                        ltrb[d] = dist;
                    }

                    float cx = (ax + 0.5f) * stride;
                    float cy = (ay + 0.5f) * stride;

                    Detection det;
                    det.bbox.x1    = cx - ltrb[0] * stride;
                    det.bbox.y1    = cy - ltrb[1] * stride;
                    det.bbox.x2    = cx + ltrb[2] * stride;
                    det.bbox.y2    = cy + ltrb[3] * stride;
                    det.confidence = best_score;
                    det.class_id   = max_classes[a];
                    all_dets.push_back(det);
                }
            }
        }
    }

    NMS(all_dets, nms_iou_thresh_);
    return all_dets;
}

/* ── NMS ───────────────────────────────────────────────────────────── */

void YoloDetector::NMS(std::vector<Detection>& dets, float iou_thresh)
{
    std::sort(dets.begin(), dets.end(),
              [](const Detection& a, const Detection& b) {
                  return a.confidence > b.confidence;
              });

    std::vector<bool> suppressed(dets.size(), false);
    std::vector<Detection> result;
    result.reserve(dets.size());

    for (size_t i = 0; i < dets.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(dets[i]);

        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (suppressed[j]) continue;
            if (dets[i].class_id != dets[j].class_id) continue;
            if (iou(dets[i].bbox, dets[j].bbox) > iou_thresh)
                suppressed[j] = true;
        }
    }
    dets = std::move(result);
}

} // namespace AI
