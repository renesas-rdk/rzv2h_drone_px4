/**
 * @file camera_capture.h
 * @brief V4L2 USB camera capture with mmap zero-copy buffers
 *
 * Captures YUYV frames from a USB webcam and converts to BGR888
 * for the YoloDetector pipeline.  No OpenCV dependency.
 */
#ifndef AI_CAMERA_CAPTURE_H
#define AI_CAMERA_CAPTURE_H

#include <cstdint>
#include <cstddef>

namespace AI {

class CameraCapture {
public:
    CameraCapture();
    ~CameraCapture();

    // Non-copyable
    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    /**
     * Open device, negotiate YUYV format, setup mmap buffers, start streaming.
     * @param device  V4L2 device path (e.g. "/dev/video0")
     * @param width   requested capture width  (e.g. 640)
     * @param height  requested capture height (e.g. 480)
     * @param fps     requested framerate (e.g. 30)
     * @return true on success
     */
    bool Init(const char* device, int width, int height, int fps);

    /**
     * Dequeue a frame, convert YUYV->BGR888, re-queue the buffer.
     *
     * @param out_bgr   caller-owned buffer of at least width*height*3 bytes;
     *                   receives BGR888 pixel data.
     * @return true if a frame was captured, false on timeout/error.
     */
    bool GrabFrame(uint8_t* out_bgr);

    /**
     * Dequeue a frame and copy raw YUYV data.
     * @param out_yuyv  caller-owned buffer of at least width*height*2 bytes.
     * @return true if a frame was captured.
     */
    bool GrabFrameYUYV(uint8_t* out_yuyv);

    /** Stop streaming, unmap buffers, close device. */
    void Release();

    int GetWidth()  const { return width_; }
    int GetHeight() const { return height_; }

    /** Convert a YUYV frame to BGR888. */
    static void YuyvToBgr(const uint8_t* yuyv, uint8_t* bgr, int w, int h);

private:
    static constexpr int NUM_BUFFERS = 4;

    int fd_;
    int width_;
    int height_;

    struct MmapBuffer {
        void*  start;
        size_t length;
    };
    MmapBuffer buffers_[NUM_BUFFERS];
    int        buf_count_;
    bool       streaming_;
};

} // namespace AI

#endif // AI_CAMERA_CAPTURE_H
