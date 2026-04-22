std_msgs/Header header
uint32 track_id          # 0 = invalid / lost
uint32 class_id
string class_name
float32 confidence
# Bounding box in pixel coordinates
uint32 bbox_x
uint32 bbox_y
uint32 bbox_w
uint32 bbox_h
# Normalized offsets: 0 = center, -1 = left/top, +1 = right/bottom
float32 bbox_cx_norm
float32 bbox_cy_norm
# Ratio of bbox area to frame area [0, 1]
float32 bbox_size_ratio
# Estimated velocity in pixels/second (from Kalman filter)
float32 vx_px_s
float32 vy_px_s
# Track age and state
uint32 age_frames
bool is_predicted        # true if this frame is Kalman prediction (occluded)
