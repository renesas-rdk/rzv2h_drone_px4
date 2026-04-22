#pragma once
/**
 * Kalman filter for object tracking.
 * State vector: [cx, cy, aspect_ratio, height, vx, vy, va, vh]
 * Measurement:  [cx, cy, aspect_ratio, height]
 *
 * Based on ByteTrack/SORT formulation — written from scratch.
 */
#include <array>
#include <cmath>

namespace tracker {

struct BBox {
    float x, y, w, h; // center x,y + width, height
    float x1() const { return x - w * 0.5f; }
    float y1() const { return y - h * 0.5f; }
    float x2() const { return x + w * 0.5f; }
    float y2() const { return y + h * 0.5f; }
    float area() const { return w * h; }
};

inline float iou(const BBox& a, const BBox& b) {
    float ix = std::max(0.0f, std::min(a.x2(), b.x2()) - std::max(a.x1(), b.x1()));
    float iy = std::max(0.0f, std::min(a.y2(), b.y2()) - std::max(a.y1(), b.y1()));
    float inter = ix * iy;
    float uni = a.area() + b.area() - inter;
    return (uni > 0.0f) ? (inter / uni) : 0.0f;
}

// 8-state, 4-measurement linear Kalman filter (simplified, no Eigen dep)
struct KalmanTrack {
    // State: [cx, cy, a, h, vx, vy, va, vh]  a = aspect = w/h
    float x[8] = {};     // state estimate
    float P[8][8] = {};  // covariance

    static constexpr float Q_pos  = 1.0f;    // process noise: position
    static constexpr float Q_vel  = 0.01f;   // process noise: velocity
    static constexpr float R_meas = 1.0f;    // measurement noise

    void init(float cx, float cy, float w, float h) {
        x[0] = cx;
        x[1] = cy;
        x[2] = (h > 0) ? (w / h) : 1.0f;
        x[3] = h;
        x[4] = x[5] = x[6] = x[7] = 0.0f;
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                P[i][j] = (i == j) ? ((i < 4) ? 10.0f : 100.0f) : 0.0f;
    }

    void predict() {
        // State transition: cx += vx, cy += vy, a += va, h += vh
        x[0] += x[4];
        x[1] += x[5];
        x[2] += x[6];
        x[3] += x[7];
        // P = F*P*Ft + Q  (F is constant velocity model)
        // Simplified diagonal update
        for (int i = 0; i < 4; ++i) {
            P[i][i] += P[i+4][i+4] + Q_pos;
            P[i+4][i+4] += Q_vel;
        }
    }

    void update(float cx, float cy, float w, float h) {
        float z[4] = {cx, cy, (h > 0) ? (w / h) : 1.0f, h};
        // Kalman gain: simplified diagonal (uncorrelated meas)
        for (int i = 0; i < 4; ++i) {
            float K = P[i][i] / (P[i][i] + R_meas);
            float innov = z[i] - x[i];
            x[i]   += K * innov;
            x[i+4] += K * innov * 0.3f; // velocity correction
            P[i][i]   *= (1.0f - K);
            P[i+4][i+4] *= (1.0f - K * 0.3f);
        }
    }

    BBox get_bbox() const {
        float h = std::max(1.0f, x[3]);
        float w = x[2] * h;
        return {x[0], x[1], w, h};
    }
};

} // namespace tracker
