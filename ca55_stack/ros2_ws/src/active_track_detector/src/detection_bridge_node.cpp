/**
 * detection_bridge_node.cpp
 *
 * Receives UDP detection packets from CustomXRCEAgent_AI (port 50010)
 * and publishes active_track_msgs/DetectionArray to /active_track/detections.
 *
 * Packet format (little-endian):
 *   uint32_t magic    = 0x44455401
 *   int32_t  frame_w
 *   int32_t  frame_h
 *   uint8_t  num_dets
 *   per detection:
 *     float x1, y1, x2, y2  (pixel coords in model space, typically 640x640)
 *     float confidence
 *     int32_t class_id
 */
#include <rclcpp/rclcpp.hpp>
#include <active_track_msgs/msg/detection_array.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <thread>

static constexpr uint32_t DET_MAGIC    = 0x44455401;
static constexpr uint16_t DET_UDP_PORT = 50010;

class DetectionBridgeNode : public rclcpp::Node
{
public:
    DetectionBridgeNode() : Node("detection_bridge")
    {
        pub_ = create_publisher<active_track_msgs::msg::DetectionArray>(
            "/active_track/detections", 10);

        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ < 0) {
            RCLCPP_FATAL(get_logger(), "Failed to create UDP socket");
            return;
        }

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(DET_UDP_PORT);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            RCLCPP_FATAL(get_logger(), "Failed to bind UDP port %d", DET_UDP_PORT);
            close(sock_);
            sock_ = -1;
            return;
        }

        // Set receive timeout so thread can check shutdown flag
        struct timeval tv{1, 0};
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        running_ = true;
        recv_thread_ = std::thread(&DetectionBridgeNode::recv_loop, this);
        RCLCPP_INFO(get_logger(), "Listening for AI detections on UDP port %d", DET_UDP_PORT);
    }

    ~DetectionBridgeNode()
    {
        running_ = false;
        if (recv_thread_.joinable()) recv_thread_.join();
        if (sock_ >= 0) close(sock_);
    }

private:
    void recv_loop()
    {
        constexpr int BUF_SIZE = 4 + 4 + 4 + 1 + 50 * (5 * 4 + 4);
        uint8_t buf[BUF_SIZE];

        while (running_) {
            ssize_t n = recv(sock_, buf, sizeof(buf), 0);
            if (n < 0) continue; // timeout or error
            if (n < 13) continue; // too short

            const uint8_t* p = buf;
            uint32_t magic;
            memcpy(&magic, p, 4); p += 4;
            if (magic != DET_MAGIC) continue;

            int32_t frame_w, frame_h;
            memcpy(&frame_w, p, 4); p += 4;
            memcpy(&frame_h, p, 4); p += 4;
            uint8_t num = *p++;

            if ((ssize_t)(13 + num * 24) > n) continue;

            auto msg = active_track_msgs::msg::DetectionArray();
            msg.header.stamp    = now();
            msg.header.frame_id = "camera";
            msg.frame_w         = (uint32_t)frame_w;
            msg.frame_h         = (uint32_t)frame_h;

            for (int i = 0; i < num; i++) {
                float x1, y1, x2, y2, conf;
                int32_t class_id;
                memcpy(&x1,      p, 4); p += 4;
                memcpy(&y1,      p, 4); p += 4;
                memcpy(&x2,      p, 4); p += 4;
                memcpy(&y2,      p, 4); p += 4;
                memcpy(&conf,    p, 4); p += 4;
                memcpy(&class_id,p, 4); p += 4;

                auto det = active_track_msgs::msg::Detection();
                det.class_id   = (uint32_t)class_id;
                det.confidence = conf;
                // bbox in pixel coords relative to frame (model space = frame_w x frame_h)
                det.bbox_x     = (uint32_t)(x1 < 0 ? 0 : x1);
                det.bbox_y     = (uint32_t)(y1 < 0 ? 0 : y1);
                det.bbox_w     = (uint32_t)(x2 - x1 > 0 ? x2 - x1 : 0);
                det.bbox_h     = (uint32_t)(y2 - y1 > 0 ? y2 - y1 : 0);
                msg.detections.push_back(det);
            }

            pub_->publish(msg);
        }
    }

    rclcpp::Publisher<active_track_msgs::msg::DetectionArray>::SharedPtr pub_;
    int         sock_    = -1;
    bool        running_ = false;
    std::thread recv_thread_;
};

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<DetectionBridgeNode>());
    rclcpp::shutdown();
    return 0;
}
