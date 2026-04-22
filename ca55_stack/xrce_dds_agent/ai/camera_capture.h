/**
 * @file camera_capture.h
 * @brief V4L2 USB camera capture with mmap zero-copy buffers
 *
 * Negotiates MJPEG format first (≈2–5 MB/s vs 18 MB/s for YUYV), falling back
 * to YUYV if the camera does not support MJPEG.  MJPEG frames are decoded via
 * libjpeg-turbo before the YUYV→BGR conversion so the GrabFrame() interface is
 * unchanged for callers.
 */
#ifndef AI_CAMERA_CAPTURE_H
#define AI_CAMERA_CAPTURE_H

#include <cstdint>
#include <cstddef>
#include <turbojpeg.h>

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

    int  GetWidth()    const { return width_; }
    int  GetHeight()   const { return height_; }
    bool IsMjpeg()     const { return fmt_mjpeg_; }

    /** Convert a YUYV frame to BGR888. */
    static void YuyvToBgr(const uint8_t* yuyv, uint8_t* bgr, int w, int h);

private:
    // 2 buffers are sufficient for double-buffered isochronous capture and
    // reduce the DMA footprint on the shared AXI bus.
    static constexpr int NUM_BUFFERS = 2;

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
    bool       fmt_mjpeg_;   // true when camera negotiated MJPEG (lower USB BW)
    tjhandle   tj_handle_;   // libjpeg-turbo decompressor (valid when fmt_mjpeg_)
    uint8_t*   yuv_tmp_;     // scratch buffer for MJPEG→YUV decode (width*height*2)
};

} // namespace AI

#endif // AI_CAMERA_CAPTURE_H
