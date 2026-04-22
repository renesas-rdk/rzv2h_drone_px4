/**
 * @file ai_camera_test.cpp
 * @brief Standalone test for AI Camera pipeline on RZ/V2H
 *
 * Tests each stage independently:
 *   1. V4L2 camera capture (YUYV -> BGR)
 *   2. YOLOv8n preprocessing (letterbox + normalise)
 *   3. DRP-AI inference (if model available)
 *   4. Postprocessing + obstacle avoidance logic
 *
 * Build:  (added to CMakeLists.txt as ai_camera_test target)
 * Usage:  ./ai_camera_test [--camera /dev/video0] [--model /path/to/model] [--frames N]
 */
#include "camera_capture.h"
#include "yolo_detector.h"
#include "obstacle_processor.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <ctime>

static volatile bool g_running = true;

static void signal_handler(int) { g_running = false; }

static void print_usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n"
        "  --camera DEV    Camera device (default: /dev/video0)\n"
        "  --model  DIR    DRP-AI model directory (skip inference if omitted)\n"
        "  --frames N      Number of frames to capture (default: 100, 0=infinite)\n"
        "  --save          Save first frame (and annotated detections) as PPM\n"
        "  --save-every N  Save annotated frame every N frames (default: 0=off)\n"
        "  --help          Show this help\n",
        prog);
}

static bool save_ppm(const char* path, const uint8_t* bgr, int w, int h)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    // PPM is RGB, input is BGR -> swap channels
    for (int i = 0; i < w * h; ++i) {
        uint8_t rgb[3] = { bgr[i*3+2], bgr[i*3+1], bgr[i*3+0] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

/* ── Drawing helpers (shared via draw_utils.h) ────────────────────── */
#include "draw_utils.h"
using AI::draw_pixel;
using AI::draw_rect;
using AI::draw_text;
using AI::draw_detections_on_bgr;

int main(int argc, char** argv)
{
    // Parse args
    const char* camera_dev = "/dev/video0";
    const char* model_dir  = nullptr;
    int   max_frames = 100;
    bool  save_frame = false;
    int   save_every = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            camera_dev = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_dir = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--save") == 0) {
            save_frame = true;
        } else if (strcmp(argv[i], "--save-every") == 0 && i + 1 < argc) {
            save_every = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    fprintf(stderr, "=== AI Camera Test ===\n");
    fprintf(stderr, "Camera: %s\n", camera_dev);
    fprintf(stderr, "Model:  %s\n", model_dir ? model_dir : "(none - camera-only mode)");
    fprintf(stderr, "Frames: %d (0=infinite)\n", max_frames);

    // ── Stage 1: DRP-AI Model (BEFORE camera to avoid DMA conflict) ──
    AI::YoloDetector* detector = nullptr;
    if (model_dir) {
        fprintf(stderr, "\n[Stage 1] Loading DRP-AI model...\n");
        detector = new AI::YoloDetector();
        if (!detector->Init(model_dir)) {
            fprintf(stderr, "FAILED: Could not load model from %s\n", model_dir);
            fprintf(stderr, "Continuing in camera-only mode...\n");
            delete detector;
            detector = nullptr;
        } else {
            fprintf(stderr, "OK: Model loaded, input=%dx%d\n",
                    detector->GetInputSize(), detector->GetInputSize());
        }
    }

    // ── Stage 2: Camera (after DRP-AI init) ──────────────────────────
    fprintf(stderr, "\n[Stage 2] Opening camera...\n");
    AI::CameraCapture camera;
    if (!camera.Init(camera_dev, 640, 480, 30)) {
        fprintf(stderr, "FAILED: Could not open camera %s\n", camera_dev);
        delete detector;
        return 1;
    }
    fprintf(stderr, "OK: Camera %dx%d ready\n", camera.GetWidth(), camera.GetHeight());

    const int cam_w = camera.GetWidth();
    const int cam_h = camera.GetHeight();
    uint8_t* bgr_buf = new uint8_t[cam_w * cam_h * 3];
    // Alloc 3 bytes/pixel even for YUYV because DRP-AI driver might read full RGB size (921600 bytes)
    // regardless of actual format, causing read overflow if we only alloc *2.
    uint8_t* yuyv_buf = new uint8_t[cam_w * cam_h * 3]; // YUYV buffer (padded)

    // ── Stage 3: Capture first frame ─────────────────────────────────
    fprintf(stderr, "\n[Stage 3] Capturing first frame...\n");
    bool got_frame = false;
    for (int retry = 0; retry < 30 && !got_frame; ++retry) {
        got_frame = camera.GrabFrameYUYV(yuyv_buf);
        if (!got_frame) usleep(100000); // 100ms
    }
    if (!got_frame) {
        fprintf(stderr, "FAILED: No frame after 3 seconds\n");
        delete detector;
        delete[] bgr_buf;
        delete[] yuyv_buf;
        return 1;
    }
    fprintf(stderr, "OK: Got frame %dx%d (YUYV)\n", cam_w, cam_h);

    if (save_frame) {
        // Convert to BGR for saving
        AI::CameraCapture::YuyvToBgr(yuyv_buf, bgr_buf, cam_w, cam_h);
        if (save_ppm("/tmp/test_frame.ppm", bgr_buf, cam_w, cam_h)) {
            fprintf(stderr, "OK: Saved /tmp/test_frame.ppm\n");
        }
    }

    // ── Stage 4: Main loop ───────────────────────────────────────────
    fprintf(stderr, "\n[Stage 4] Running main loop...\n");
    AI::ObstacleConfig obs_cfg;

    int frame_count = 0;
    float total_preproc_ms = 0;
    float total_infer_ms = 0;

    while (g_running && (max_frames == 0 || frame_count < max_frames)) {
        if (!camera.GrabFrameYUYV(yuyv_buf)) {
            usleep(1000);
            continue;
        }
        frame_count++;

        if (detector) {
            // Full pipeline: preprocess + inference + postprocess
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);

            // OPTIMIZATION: Check if we need BGR for saving THIS frame
            bool should_save = (save_frame && frame_count == 1) ||
                               (save_every > 0 && frame_count % save_every == 0);

            // FIX: PreRuntime expects 3-channel BGR input (IMG_ICH=3), not YUYV!
            // imagescaler handles BGR→RGB internally (DIN_FORMAT=0, DOUT_RGB_ORDER=1)
            AI::CameraCapture::YuyvToBgr(yuyv_buf, bgr_buf, cam_w, cam_h);

            // Run inference on BGR data (imagescaler converts to RGB for model)
            auto dets = detector->RunOnYUYV(bgr_buf, cam_w, cam_h);

            clock_gettime(CLOCK_MONOTONIC, &t1);
            float total_ms = (t1.tv_sec - t0.tv_sec) * 1000.0f
                           + (t1.tv_nsec - t0.tv_nsec) / 1e6f;
            total_infer_ms += detector->GetLastInferenceMs();
            total_preproc_ms += (total_ms - detector->GetLastInferenceMs());

            // Process obstacles
            auto pkt = AI::Process_Obstacles(dets, 640, 640, obs_cfg);

            if (frame_count % 10 == 0) {
                fprintf(stderr, "[%4d] %zu dets, infer=%.1fms, total=%.1fms, steer=%.2f, jump=%d\n",
                        frame_count, dets.size(),
                        detector->GetLastInferenceMs(), total_ms,
                        pkt.angular_z, pkt.jump_trigger);
                for (const auto& d : dets) {
                    fprintf(stderr, "       -> %s (%.0f%%) [%.0f,%.0f,%.0f,%.0f]\n",
                            AI::coco_label(d.class_id),
                            d.confidence * 100,
                            d.bbox.x1, d.bbox.y1, d.bbox.x2, d.bbox.y2);
                }
            }

            // Save annotated frame with bounding boxes
            if (should_save && !dets.empty()) {
                // bgr_buf already has BGR data from conversion above
                // Copy frame, draw detections, save
                uint8_t* ann_buf = new uint8_t[cam_w * cam_h * 3];
                memcpy(ann_buf, bgr_buf, cam_w * cam_h * 3);
                draw_detections_on_bgr(ann_buf, cam_w, cam_h, dets);

                char path[128];
                snprintf(path, sizeof(path), "/tmp/detect_%04d.ppm", frame_count);
                if (save_ppm(path, ann_buf, cam_w, cam_h)) {
                    fprintf(stderr, "OK: Saved annotated %s (%zu dets)\n",
                            path, dets.size());
                }
                delete[] ann_buf;
            }
        } else {
            // Camera-only: count/fps
             if (frame_count % 30 == 0) {
                fprintf(stderr, "[%4d] Camera OK (no model loaded)\n", frame_count);
            }
        }
    }

    // ── Summary ──────────────────────────────────────────────────────
    fprintf(stderr, "\n=== Summary ===\n");
    fprintf(stderr, "Frames captured: %d\n", frame_count);
    if (detector && frame_count > 0) {
        fprintf(stderr, "Avg preprocess:  %.1f ms\n", total_preproc_ms / frame_count);
        fprintf(stderr, "Avg inference:   %.1f ms\n", total_infer_ms / frame_count);
        fprintf(stderr, "Avg total:       %.1f ms (%.0f fps)\n",
                (total_preproc_ms + total_infer_ms) / frame_count,
                frame_count * 1000.0f / (total_preproc_ms + total_infer_ms));
    }

    delete detector;
    delete[] bgr_buf;
    delete[] yuyv_buf;
    camera.Release();

    fprintf(stderr, "Done.\n");
    return 0;
}
