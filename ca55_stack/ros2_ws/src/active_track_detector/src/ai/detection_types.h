#pragma once
#include <cstdint>
#include <vector>

namespace AI {

struct BBox {
    float x1, y1, x2, y2; // pixel coordinates in model input space (640x640)
};

struct Detection {
    BBox  bbox;
    float confidence;
    int   class_id;   // COCO class index (0-79)
};

// COCO-80 class label lookup
inline const char* coco_label(int class_id) {
    static const char* LABELS[80] = {
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
    if (class_id >= 0 && class_id < 80) return LABELS[class_id];
    return "unknown";
}

} // namespace AI
