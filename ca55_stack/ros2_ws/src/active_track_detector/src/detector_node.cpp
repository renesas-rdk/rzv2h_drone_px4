/**
 * detector_node.cpp
 *
 * ROS2 node: V4L2 camera → DRP-AI YOLOv8n inference → publish DetectionArray
 * Optional: UDP JPEG stream with bbox overlay for host PC debugging.
 *
 * CRITICAL rules (from project_context.md):
 *   - AI thread MUST be pinned to Core 2 (DRP-AI timing isolation)
 *   - Use SIGINT (killall -INT) for clean shutdown; SIGKILL hangs DRP-AI
 *   - YOLO normalize [0,1] — handled internally by DRP-AI PreRuntime
 */

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/header.hpp>
#include <active_track_msgs/msg/detection_array.hpp>

#include "ai/camera_capture.h"
#include "ai/yolo_detector.h"
#include "ai/detection_types.h"
#include "ai/image_streamer.h"

#include <pthread.h>
#include <sched.h>
#include <csignal>
#include <memory>
#include <vector>
#include <chrono>

using namespace std::chrono_literals;

static const char* COCO_NAMES[80] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat",
    "traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat",
    "dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack",
    "umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball",
    "kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket",
    "bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
    "sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair",
    "couch","potted plant","bed","dining table","toilet","tv","laptop","mouse",
    "remote","keyboard","cell phone","microwave","oven","toaster","sink","refrigerator",
    "book","clock","vase","scissors","teddy bear","hair drier","toothbrush"
};

static bool g_shutdown = false;

// YUYV (YUY2) → BGR888 — used only when UDP streaming is enabled
static void yuyv_to_bgr(const uint8_t* yuyv, uint8_t* bgr, int w, int h)
{
    auto clamp = [](int v) -> uint8_t {
        return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
    };
    const int npairs = w * h / 2;
    for (int i = 0; i < npairs; ++i) {
        int y0 = yuyv[i*4+0];
        int u  = yuyv[i*4+1] - 128;
        int y1 = yuyv[i*4+2];
        int v  = yuyv[i*4+3] - 128;
        bgr[i*6+0] = clamp(y0 + (1772*u)/1000);
        bgr[i*6+1] = clamp(y0 - (344*u)/1000 - (714*v)/1000);
        bgr[i*6+2] = clamp(y0 + (1402*v)/1000);
        bgr[i*6+3] = clamp(y1 + (1772*u)/1000);
        bgr[i*6+4] = clamp(y1 - (344*u)/1000 - (714*v)/1000);
        bgr[i*6+5] = clamp(y1 + (1402*v)/1000);
    }
}

class DetectorNode : public rclcpp::Node {
public:
    DetectorNode() : Node("active_track_detector")
    {
        // Camera & inference params
        declare_parameter("device",         "/dev/video0");
        declare_parameter("width",          640);
        declare_parameter("height",         480);
        declare_parameter("fps",            30);
        declare_parameter("model_dir",      "/home/root/ai_models/yolov8n");
        declare_parameter("conf_threshold", 0.25);
        declare_parameter("nms_iou",        0.45);

        // Streaming params
        declare_parameter("stream_enabled",  false);
        declare_parameter("stream_dest_ip",  std::string(""));  // "" = auto-learn via heartbeat
        declare_parameter("stream_port",     50002);
        declare_parameter("stream_hb_port",  50001);
        declare_parameter("stream_quality",  50);
        declare_parameter("stream_skip",     2);   // stream every Nth frame

        rclcpp::QoS qos(rclcpp::KeepLast(1));
        qos.best_effort();
        pub_detections_ = create_publisher<active_track_msgs::msg::DetectionArray>(
            "/active_track/detections", qos);

        // Init camera
        std::string device = get_parameter("device").as_string();
        int width  = get_parameter("width").as_int();
        int height = get_parameter("height").as_int();
        int fps    = get_parameter("fps").as_int();

        if (!camera_.Init(device.c_str(), width, height, fps)) {
            RCLCPP_FATAL(get_logger(), "Failed to init camera: %s", device.c_str());
            throw std::runtime_error("camera init failed");
        }
        RCLCPP_INFO(get_logger(), "Camera: %s %dx%d @%d fps", device.c_str(), width, height, fps);

        // Init detector
        std::string model_dir = get_parameter("model_dir").as_string();
        if (!detector_.Init(model_dir)) {
            RCLCPP_FATAL(get_logger(), "Failed to init YoloDetector from: %s", model_dir.c_str());
            throw std::runtime_error("detector init failed");
        }
        RCLCPP_INFO(get_logger(), "YOLOv8n loaded from: %s", model_dir.c_str());

        frame_w_ = camera_.GetWidth();
        frame_h_ = camera_.GetHeight();
        yuyv_buf_.resize(frame_w_ * frame_h_ * 2);

        // Init optional UDP streamer
        bool stream_enabled = get_parameter("stream_enabled").as_bool();
        if (stream_enabled) {
            AI::StreamConfig scfg = AI::StreamDefaultConfig();
            scfg.dest_ip        = get_parameter("stream_dest_ip").as_string();
            scfg.dest_port      = (uint16_t)get_parameter("stream_port").as_int();
            scfg.heartbeat_port = (uint16_t)get_parameter("stream_hb_port").as_int();
            scfg.jpeg_quality   = get_parameter("stream_quality").as_int();
            scfg.skip_frames    = get_parameter("stream_skip").as_int();

            if (streamer_.Init(scfg, frame_w_, frame_h_)) {
                stream_active_ = true;
                bgr_buf_.resize(frame_w_ * frame_h_ * 3);
                RCLCPP_INFO(get_logger(),
                    "UDP stream enabled → port %d (heartbeat port %d, quality %d, skip %d)",
                    scfg.dest_port, scfg.heartbeat_port, scfg.jpeg_quality, scfg.skip_frames);
            } else {
                RCLCPP_WARN(get_logger(), "UDP streamer init failed — streaming disabled");
            }
        }

        int interval_ms = 1000 / fps;
        timer_ = create_wall_timer(
            std::chrono::milliseconds(interval_ms),
            std::bind(&DetectorNode::timer_cb, this));

        RCLCPP_INFO(get_logger(), "DetectorNode ready, publishing /active_track/detections");
    }

    ~DetectorNode() {
        if (stream_active_) streamer_.Shutdown();
        camera_.Release();
    }

private:
    void timer_cb() {
        if (g_shutdown) {
            rclcpp::shutdown();
            return;
        }

        if (!camera_.GrabFrameYUYV(yuyv_buf_.data())) {
            RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000, "Frame grab failed");
            return;
        }

        auto start = std::chrono::steady_clock::now();
        std::vector<AI::Detection> dets = detector_.RunOnYUYV(
            yuyv_buf_.data(), frame_w_, frame_h_);
        float inference_ms = (float)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        // Optional streaming — convert YUYV→BGR for annotation + UDP send
        if (stream_active_) {
            yuyv_to_bgr(yuyv_buf_.data(), bgr_buf_.data(), frame_w_, frame_h_);
            streamer_.SendFrame(bgr_buf_.data(), frame_w_, frame_h_, dets,
                                detector_.GetInputSize(), inference_ms);
        }

        // Build and publish DetectionArray
        auto msg = active_track_msgs::msg::DetectionArray();
        msg.header.stamp    = now();
        msg.header.frame_id = "camera";
        msg.frame_w = static_cast<uint32_t>(frame_w_);
        msg.frame_h = static_cast<uint32_t>(frame_h_);

        for (const auto& d : dets) {
            active_track_msgs::msg::Detection det;
            det.header     = msg.header;
            det.class_id   = static_cast<uint32_t>(d.class_id);
            det.class_name = (d.class_id >= 0 && d.class_id < 80)
                             ? COCO_NAMES[d.class_id] : "unknown";
            det.confidence = d.confidence;
            float sx = static_cast<float>(frame_w_)  / detector_.GetInputSize();
            float sy = static_cast<float>(frame_h_) / detector_.GetInputSize();
            det.bbox_x = static_cast<uint32_t>(std::max(0.0f, d.bbox.x1 * sx));
            det.bbox_y = static_cast<uint32_t>(std::max(0.0f, d.bbox.y1 * sy));
            det.bbox_w = static_cast<uint32_t>((d.bbox.x2 - d.bbox.x1) * sx);
            det.bbox_h = static_cast<uint32_t>((d.bbox.y2 - d.bbox.y1) * sy);
            msg.detections.push_back(det);
        }

        pub_detections_->publish(msg);

        RCLCPP_DEBUG(get_logger(),
            "Frame: %zu dets, infer %.1f ms",
            dets.size(), inference_ms);
    }

    AI::CameraCapture     camera_;
    AI::YoloDetector      detector_;
    AI::UdpImageStreamer  streamer_;
    bool                  stream_active_ = false;

    std::vector<uint8_t>  yuyv_buf_;
    std::vector<uint8_t>  bgr_buf_;   // allocated only when stream_active_
    int                   frame_w_ = 0, frame_h_ = 0;

    rclcpp::Publisher<active_track_msgs::msg::DetectionArray>::SharedPtr pub_detections_;
    rclcpp::TimerBase::SharedPtr timer_;
};

static void sigint_handler(int) {
    g_shutdown = true;
}

int main(int argc, char** argv)
{
    // Pin this process to Core 2 — mandatory for DRP-AI timing isolation
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0) {
        fprintf(stderr, "[detector_node] WARN: failed to pin to Core 2, DRP-AI may be unstable\n");
    }

    signal(SIGINT,  sigint_handler);
    signal(SIGTERM, sigint_handler);

    rclcpp::init(argc, argv);
    rclcpp::executors::SingleThreadedExecutor executor;

    try {
        auto node = std::make_shared<DetectorNode>();
        executor.add_node(node);
        executor.spin();
    } catch (const std::exception& e) {
        fprintf(stderr, "[detector_node] Fatal: %s\n", e.what());
        rclcpp::shutdown();
        return 1;
    }

    rclcpp::shutdown();
    return 0;
}
