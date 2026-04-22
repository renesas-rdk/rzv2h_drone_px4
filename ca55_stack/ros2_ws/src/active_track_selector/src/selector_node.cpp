/**
 * selector_node.cpp
 *
 * DJI ActiveTrack-style target selector:
 *   - Subscribe /active_track/tracks (TrackedObjectArray)
 *   - Pick ONE target according to policy
 *   - Publish /active_track/target (TrackedObject, track_id=0 = lost)
 *   - Service /active_track/set_target to change class/track_id/mode at runtime
 *
 * Modes:
 *   0 = CLASS_FILTER: auto-pick best track of given COCO class
 *   1 = TRACK_ID:     lock to explicit track_id
 *   2 = CLEAR:        publish lost (track_id=0)
 *
 * Policies (CLASS_FILTER): "highest_confidence" | "nearest_center" | "largest_bbox"
 */

#include <rclcpp/rclcpp.hpp>
#include <active_track_msgs/msg/tracked_object.hpp>
#include <active_track_msgs/msg/tracked_object_array.hpp>
#include <active_track_msgs/srv/set_target_selection.hpp>

#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>

using TrackedObject      = active_track_msgs::msg::TrackedObject;
using TrackedObjectArray = active_track_msgs::msg::TrackedObjectArray;
using SetTargetSelection = active_track_msgs::srv::SetTargetSelection;

using namespace std::chrono_literals;

class SelectorNode : public rclcpp::Node {
public:
    SelectorNode() : Node("active_track_selector")
    {
        declare_parameter("default_class_id",  0);         // 0 = person
        declare_parameter("default_policy",    std::string("highest_confidence"));
        declare_parameter("lock_timeout_s",    2.0);       // seconds before sticky lock releases

        class_filter_  = get_parameter("default_class_id").as_int();
        policy_        = get_parameter("default_policy").as_string();
        lock_timeout_  = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(get_parameter("lock_timeout_s").as_double()));

        rclcpp::QoS qos_in(rclcpp::KeepLast(5));
        qos_in.best_effort();
        rclcpp::QoS qos_out(rclcpp::KeepLast(1));
        qos_out.best_effort();

        sub_ = create_subscription<TrackedObjectArray>(
            "/active_track/tracks", qos_in,
            std::bind(&SelectorNode::tracks_cb, this, std::placeholders::_1));

        pub_ = create_publisher<TrackedObject>("/active_track/target", qos_out);

        srv_ = create_service<SetTargetSelection>(
            "/active_track/set_target",
            std::bind(&SelectorNode::set_target_cb, this,
                      std::placeholders::_1, std::placeholders::_2));

        // Timer to publish "lost" when no tracks received for > lock_timeout
        timer_ = create_wall_timer(100ms, std::bind(&SelectorNode::timer_cb, this));

        RCLCPP_INFO(get_logger(),
            "SelectorNode ready: class=%u, policy=%s, timeout=%.1fs",
            class_filter_, policy_.c_str(),
            get_parameter("lock_timeout_s").as_double());
    }

private:
    void tracks_cb(const TrackedObjectArray::SharedPtr msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_tracks_time_ = now();

        if (msg->tracks.empty()) {
            handle_lost(msg->header);
            return;
        }

        switch (mode_) {
        case 2: // CLEAR
            publish_lost(msg->header);
            break;

        case 1: { // TRACK_ID — find locked id
            auto it = std::find_if(msg->tracks.begin(), msg->tracks.end(),
                [this](const TrackedObject& t) { return t.track_id == locked_id_; });
            if (it != msg->tracks.end()) {
                pub_->publish(*it);
                last_target_time_ = now();
            } else {
                handle_lost(msg->header);
            }
            break;
        }

        case 0: { // CLASS_FILTER
            std::vector<TrackedObject> candidates;
            for (const auto& t : msg->tracks) {
                if (t.class_id == class_filter_ && t.track_id != 0)
                    candidates.push_back(t);
            }
            if (candidates.empty()) {
                handle_lost(msg->header);
                break;
            }

            // If we have a sticky lock, prefer that track_id
            if (locked_id_ != 0) {
                auto it = std::find_if(candidates.begin(), candidates.end(),
                    [this](const TrackedObject& t) { return t.track_id == locked_id_; });
                if (it != candidates.end()) {
                    pub_->publish(*it);
                    last_target_time_ = now();
                    break;
                }
                // Sticky lock expired / track gone — pick new best
                locked_id_ = 0;
            }

            // Pick best according to policy
            const TrackedObject* best = pick_best(candidates);
            if (best) {
                locked_id_ = best->track_id;
                pub_->publish(*best);
                last_target_time_ = now();
                RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 3000,
                    "Locked onto track_id=%u (class=%s, conf=%.2f)",
                    best->track_id, best->class_name.c_str(), best->confidence);
            }
            break;
        }
        }
    }

    const TrackedObject* pick_best(const std::vector<TrackedObject>& cands) {
        if (cands.empty()) return nullptr;
        if (policy_ == "nearest_center") {
            return &*std::min_element(cands.begin(), cands.end(),
                [](const TrackedObject& a, const TrackedObject& b) {
                    float da = a.bbox_cx_norm*a.bbox_cx_norm + a.bbox_cy_norm*a.bbox_cy_norm;
                    float db = b.bbox_cx_norm*b.bbox_cx_norm + b.bbox_cy_norm*b.bbox_cy_norm;
                    return da < db;
                });
        } else if (policy_ == "largest_bbox") {
            return &*std::max_element(cands.begin(), cands.end(),
                [](const TrackedObject& a, const TrackedObject& b) {
                    return a.bbox_size_ratio < b.bbox_size_ratio;
                });
        } else { // "highest_confidence"
            return &*std::max_element(cands.begin(), cands.end(),
                [](const TrackedObject& a, const TrackedObject& b) {
                    return a.confidence < b.confidence;
                });
        }
    }

    void handle_lost(const std_msgs::msg::Header& hdr) {
        // Check sticky lock timeout
        auto elapsed = (now() - last_target_time_).to_chrono<std::chrono::nanoseconds>();
        if (elapsed > lock_timeout_) {
            locked_id_ = 0;
        }
        publish_lost(hdr);
    }

    void publish_lost(const std_msgs::msg::Header& hdr) {
        TrackedObject lost;
        lost.header   = hdr;
        lost.track_id = 0;
        pub_->publish(lost);
    }

    void timer_cb() {
        std::lock_guard<std::mutex> lock(mutex_);
        // If no track message for > lock_timeout, publish lost
        auto elapsed = (now() - last_tracks_time_).to_chrono<std::chrono::nanoseconds>();
        if (elapsed > lock_timeout_) {
            TrackedObject lost;
            lost.header.stamp = now();
            lost.track_id = 0;
            pub_->publish(lost);
        }
    }

    void set_target_cb(
        const std::shared_ptr<SetTargetSelection::Request>  req,
              std::shared_ptr<SetTargetSelection::Response> res)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = req->mode;
        switch (req->mode) {
        case 0: // CLASS_FILTER
            class_filter_ = req->class_id;
            if (!req->policy.empty()) policy_ = req->policy;
            locked_id_ = 0;
            res->success = true;
            res->message = "MODE_CLASS_FILTER: class=" + std::to_string(req->class_id)
                         + " policy=" + policy_;
            break;
        case 1: // TRACK_ID
            locked_id_ = req->track_id;
            res->success = true;
            res->message = "MODE_TRACK_ID: locked to " + std::to_string(req->track_id);
            break;
        case 2: // CLEAR
            locked_id_ = 0;
            res->success = true;
            res->message = "MODE_CLEAR: following stopped";
            break;
        default:
            res->success = false;
            res->message = "Unknown mode: " + std::to_string(req->mode);
            return;
        }
        RCLCPP_INFO(get_logger(), "set_target: %s", res->message.c_str());
    }

    std::mutex mutex_;
    uint8_t    mode_         = 0;    // CLASS_FILTER by default
    uint32_t   class_filter_ = 0;   // person
    uint32_t   locked_id_    = 0;   // sticky track_id (0 = none)
    std::string policy_      = "highest_confidence";

    rclcpp::Time last_target_time_{0, 0, RCL_ROS_TIME};
    rclcpp::Time last_tracks_time_{0, 0, RCL_ROS_TIME};
    std::chrono::nanoseconds lock_timeout_{2'000'000'000LL};

    rclcpp::Subscription<TrackedObjectArray>::SharedPtr sub_;
    rclcpp::Publisher<TrackedObject>::SharedPtr         pub_;
    rclcpp::Service<SetTargetSelection>::SharedPtr      srv_;
    rclcpp::TimerBase::SharedPtr                        timer_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<SelectorNode>());
    rclcpp::shutdown();
    return 0;
}
