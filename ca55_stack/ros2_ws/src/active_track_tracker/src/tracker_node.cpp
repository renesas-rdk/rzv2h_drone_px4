/**
 * tracker_node.cpp
 *
 * ByteTrack-lite: subscribe DetectionArray, assign persistent track IDs,
 * publish TrackedObjectArray.
 *
 * Track lifecycle: TENTATIVE(min_hits frames) → CONFIRMED → LOST(max_age) → DELETED
 * 2-stage IoU matching (high-conf first, then low-conf for unmatched tracks).
 */

#include <rclcpp/rclcpp.hpp>
#include <active_track_msgs/msg/detection_array.hpp>
#include <active_track_msgs/msg/tracked_object_array.hpp>
#include <active_track_msgs/msg/tracked_object.hpp>

#include "kalman_track.hpp"

#include <vector>
#include <algorithm>
#include <limits>
#include <mutex>
#include <memory>

using DetectionArray    = active_track_msgs::msg::DetectionArray;
using TrackedObjectArray= active_track_msgs::msg::TrackedObjectArray;
using TrackedObject     = active_track_msgs::msg::TrackedObject;

namespace {

enum class TrackState { TENTATIVE, CONFIRMED, LOST };

struct Track {
    uint32_t          id;
    uint32_t          class_id;
    std::string       class_name;
    float             confidence;
    tracker::KalmanTrack kf;
    TrackState        state;
    int               hit_streak;
    int               age;
    int               frames_since_update;
    bool              is_predicted;

    tracker::BBox bbox() const { return kf.get_bbox(); }

    TrackedObject to_msg(const std_msgs::msg::Header& hdr,
                          uint32_t frame_w, uint32_t frame_h) const {
        TrackedObject m;
        m.header     = hdr;
        m.track_id   = id;
        m.class_id   = class_id;
        m.class_name = class_name;
        m.confidence = confidence;
        auto bb = bbox();
        m.bbox_x = static_cast<uint32_t>(std::max(0.0f, bb.x1()));
        m.bbox_y = static_cast<uint32_t>(std::max(0.0f, bb.y1()));
        m.bbox_w = static_cast<uint32_t>(bb.w);
        m.bbox_h = static_cast<uint32_t>(bb.h);

        float cx = bb.x;
        float cy = bb.y;
        float fw = static_cast<float>(frame_w);
        float fh = static_cast<float>(frame_h);
        m.bbox_cx_norm   = (fw > 0) ? (2.0f * cx / fw - 1.0f) : 0.0f;
        m.bbox_cy_norm   = (fh > 0) ? (2.0f * cy / fh - 1.0f) : 0.0f;
        m.bbox_size_ratio= (fw > 0 && fh > 0) ? (bb.w * bb.h / (fw * fh)) : 0.0f;

        // velocity in px/s from Kalman state (vx, vy indices 4,5)
        m.vx_px_s   = kf.x[4];
        m.vy_px_s   = kf.x[5];
        m.age_frames= static_cast<uint32_t>(age);
        m.is_predicted = is_predicted;
        return m;
    }
};

// Greedy IoU matching: returns assignment[det_idx] = track_idx, -1 if unmatched
std::vector<int> greedy_iou_match(
    const std::vector<tracker::BBox>& det_bboxes,
    const std::vector<tracker::BBox>& trk_bboxes,
    float iou_threshold)
{
    std::vector<int> assignment(det_bboxes.size(), -1);
    std::vector<bool> trk_used(trk_bboxes.size(), false);

    for (size_t d = 0; d < det_bboxes.size(); ++d) {
        float best_iou = iou_threshold;
        int best_t = -1;
        for (size_t t = 0; t < trk_bboxes.size(); ++t) {
            if (trk_used[t]) continue;
            float s = tracker::iou(det_bboxes[d], trk_bboxes[t]);
            if (s > best_iou) {
                best_iou = s;
                best_t   = static_cast<int>(t);
            }
        }
        if (best_t >= 0) {
            assignment[d] = best_t;
            trk_used[best_t] = true;
        }
    }
    return assignment;
}

} // anonymous namespace

class TrackerNode : public rclcpp::Node {
public:
    TrackerNode() : Node("active_track_tracker")
    {
        declare_parameter("max_age_frames",     30);   // keep LOST track for N frames
        declare_parameter("min_hits",           3);    // frames before CONFIRMED
        declare_parameter("iou_threshold",      0.3);  // high-conf match threshold
        declare_parameter("low_conf_threshold", 0.1);  // 2nd-stage match threshold

        max_age_  = get_parameter("max_age_frames").as_int();
        min_hits_ = get_parameter("min_hits").as_int();
        iou_th_   = static_cast<float>(get_parameter("iou_threshold").as_double());
        low_conf_ = static_cast<float>(get_parameter("low_conf_threshold").as_double());

        rclcpp::QoS qos_sub(rclcpp::KeepLast(1));
        qos_sub.best_effort();
        rclcpp::QoS qos_pub(rclcpp::KeepLast(5));
        qos_pub.best_effort();

        sub_ = create_subscription<DetectionArray>(
            "/active_track/detections", qos_sub,
            std::bind(&TrackerNode::detections_cb, this, std::placeholders::_1));

        pub_ = create_publisher<TrackedObjectArray>(
            "/active_track/tracks", qos_pub);

        RCLCPP_INFO(get_logger(), "TrackerNode ready: max_age=%d, min_hits=%d, iou=%.2f",
            max_age_, min_hits_, iou_th_);
    }

private:
    void detections_cb(const DetectionArray::SharedPtr msg) {
        std::lock_guard<std::mutex> lock(mutex_);

        uint32_t frame_w = msg->frame_w;
        uint32_t frame_h = msg->frame_h;

        // Predict all existing tracks
        for (auto& trk : tracks_) {
            trk.kf.predict();
            trk.frames_since_update++;
            trk.is_predicted = true;
        }

        // Convert detections to BBoxes
        auto make_bbox = [&](const auto& d) -> tracker::BBox {
            float cx = d.bbox_x + d.bbox_w * 0.5f;
            float cy = d.bbox_y + d.bbox_h * 0.5f;
            return {cx, cy, static_cast<float>(d.bbox_w), static_cast<float>(d.bbox_h)};
        };

        // Split into high/low confidence
        std::vector<size_t> high_idx, low_idx;
        for (size_t i = 0; i < msg->detections.size(); ++i) {
            if (msg->detections[i].confidence >= iou_th_)
                high_idx.push_back(i);
            else if (msg->detections[i].confidence >= low_conf_)
                low_idx.push_back(i);
        }

        // Stage 1: match high-conf detections to all tracks
        std::vector<tracker::BBox> trk_bboxes;
        for (auto& t : tracks_) trk_bboxes.push_back(t.bbox());

        std::vector<tracker::BBox> high_bboxes;
        for (auto i : high_idx) high_bboxes.push_back(make_bbox(msg->detections[i]));

        auto assign_high = greedy_iou_match(high_bboxes, trk_bboxes, iou_th_);
        std::vector<bool> trk_matched(tracks_.size(), false);
        std::vector<size_t> unmatched_high;

        for (size_t di = 0; di < high_idx.size(); ++di) {
            int ti = assign_high[di];
            if (ti >= 0) {
                auto& det = msg->detections[high_idx[di]];
                auto  bb  = high_bboxes[di];
                tracks_[ti].kf.update(bb.x, bb.y, bb.w, bb.h);
                tracks_[ti].confidence        = det.confidence;
                tracks_[ti].frames_since_update = 0;
                tracks_[ti].hit_streak++;
                tracks_[ti].age++;
                tracks_[ti].is_predicted = false;
                trk_matched[ti] = true;
            } else {
                unmatched_high.push_back(high_idx[di]);
            }
        }

        // Stage 2: match low-conf detections to unmatched tracks
        std::vector<size_t> unmatched_trk;
        for (size_t ti = 0; ti < tracks_.size(); ++ti)
            if (!trk_matched[ti]) unmatched_trk.push_back(ti);

        if (!low_idx.empty() && !unmatched_trk.empty()) {
            std::vector<tracker::BBox> low_bboxes, unm_bboxes;
            for (auto i : low_idx) low_bboxes.push_back(make_bbox(msg->detections[i]));
            for (auto ti : unmatched_trk) unm_bboxes.push_back(tracks_[ti].bbox());

            auto assign_low = greedy_iou_match(low_bboxes, unm_bboxes, low_conf_);
            for (size_t di = 0; di < low_idx.size(); ++di) {
                int ui = assign_low[di];
                if (ui >= 0) {
                    auto& det = msg->detections[low_idx[di]];
                    auto  bb  = low_bboxes[di];
                    size_t ti = unmatched_trk[ui];
                    tracks_[ti].kf.update(bb.x, bb.y, bb.w, bb.h);
                    tracks_[ti].confidence        = det.confidence;
                    tracks_[ti].frames_since_update = 0;
                    tracks_[ti].hit_streak++;
                    tracks_[ti].age++;
                    tracks_[ti].is_predicted = false;
                    trk_matched[ti] = true;
                }
            }
        }

        // Spawn new tracks from unmatched high-conf detections
        for (auto di : unmatched_high) {
            auto& det = msg->detections[di];
            auto  bb  = make_bbox(det);
            Track t;
            t.id          = next_id_++;
            t.class_id    = det.class_id;
            t.class_name  = det.class_name;
            t.confidence  = det.confidence;
            t.state       = TrackState::TENTATIVE;
            t.hit_streak  = 1;
            t.age         = 1;
            t.frames_since_update = 0;
            t.is_predicted = false;
            t.kf.init(bb.x, bb.y, bb.w, bb.h);
            tracks_.push_back(t);
        }

        // Update track states, prune dead tracks
        for (auto& trk : tracks_) {
            if (trk.frames_since_update == 0) {
                trk.state = (trk.hit_streak >= min_hits_)
                            ? TrackState::CONFIRMED : TrackState::TENTATIVE;
            } else {
                trk.state = TrackState::LOST;
                trk.hit_streak = 0;
            }
        }
        tracks_.erase(
            std::remove_if(tracks_.begin(), tracks_.end(),
                [this](const Track& t) {
                    return t.frames_since_update > max_age_;
                }),
            tracks_.end());

        // Publish confirmed (+ recently lost) tracks
        auto out = TrackedObjectArray();
        out.header = msg->header;
        for (const auto& trk : tracks_) {
            if (trk.state != TrackState::TENTATIVE) {
                out.tracks.push_back(trk.to_msg(msg->header, frame_w, frame_h));
            }
        }
        pub_->publish(out);
    }

    std::vector<Track> tracks_;
    uint32_t           next_id_ = 1;
    std::mutex         mutex_;

    int   max_age_;
    int   min_hits_;
    float iou_th_;
    float low_conf_;

    rclcpp::Subscription<DetectionArray>::SharedPtr    sub_;
    rclcpp::Publisher<TrackedObjectArray>::SharedPtr   pub_;
};

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<TrackerNode>());
    rclcpp::shutdown();
    return 0;
}
