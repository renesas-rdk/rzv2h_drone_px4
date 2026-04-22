/**
 * @file camera_capture.cpp
 * @brief V4L2 USB camera capture implementation
 */
#include "camera_capture.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <linux/videodev2.h>

#define CAM_LOG(fmt, ...) fprintf(stderr, "[Camera] " fmt "\n", ##__VA_ARGS__)
#define CAM_ERR(fmt, ...) fprintf(stderr, "[Camera ERROR] " fmt ": %s\n", ##__VA_ARGS__, strerror(errno))

namespace AI {

/* ── Helpers ────────────────────────────────────────────────────────── */

static int xioctl(int fd, unsigned long request, void* arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static inline uint8_t clamp_u8(int v)
{
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* ── Constructor / Destructor ──────────────────────────────────────── */

CameraCapture::CameraCapture()
    : fd_(-1), width_(0), height_(0), buf_count_(0), streaming_(false)
{
    memset(buffers_, 0, sizeof(buffers_));
}

CameraCapture::~CameraCapture()
{
    Release();
}

/* ── Init ──────────────────────────────────────────────────────────── */

bool CameraCapture::Init(const char* device, int width, int height, int fps)
{
    fd_ = open(device, O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        CAM_ERR("Failed to open %s", device);
        return false;
    }

    // Query capabilities
    struct v4l2_capability cap;
    if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        CAM_ERR("VIDIOC_QUERYCAP");
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        CAM_LOG("%s does not support video capture", device);
        return false;
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        CAM_LOG("%s does not support streaming", device);
        return false;
    }

    // Set format: YUYV 4:2:2
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        CAM_ERR("VIDIOC_S_FMT YUYV %dx%d", width, height);
        return false;
    }
    width_  = fmt.fmt.pix.width;
    height_ = fmt.fmt.pix.height;
    CAM_LOG("Negotiated format: %dx%d YUYV", width_, height_);

    // Set framerate
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    if (xioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
        CAM_LOG("Warning: could not set %d fps", fps);
    }

    // Request mmap buffers
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = NUM_BUFFERS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
        CAM_ERR("VIDIOC_REQBUFS");
        return false;
    }
    buf_count_ = std::min(static_cast<int>(req.count), NUM_BUFFERS);

    // Map buffers
    for (int i = 0; i < buf_count_; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            CAM_ERR("VIDIOC_QUERYBUF %d", i);
            return false;
        }
        buffers_[i].length = buf.length;
        buffers_[i].start  = mmap(NULL, buf.length,
                                  PROT_READ | PROT_WRITE, MAP_SHARED,
                                  fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED) {
            CAM_ERR("mmap buffer %d", i);
            return false;
        }
    }

    // Queue all buffers
    for (int i = 0; i < buf_count_; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            CAM_ERR("VIDIOC_QBUF %d", i);
            return false;
        }
    }

    // Start streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        CAM_ERR("VIDIOC_STREAMON");
        return false;
    }
    streaming_ = true;

    CAM_LOG("Streaming started: %dx%d @ %dfps, %d buffers",
            width_, height_, fps, buf_count_);
    return true;
}

/* ── GrabFrame ─────────────────────────────────────────────────────── */

bool CameraCapture::GrabFrame(uint8_t* out_bgr)
{
    if (!streaming_ || fd_ < 0)
        return false;

    // Poll with 100ms timeout
    struct pollfd pfd = { fd_, POLLIN, 0 };
    int ret = poll(&pfd, 1, 100);
    if (ret <= 0)
        return false;

    // Dequeue buffer
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN)
            return false;
        CAM_ERR("VIDIOC_DQBUF");
        return false;
    }

    // Convert YUYV -> BGR888
    YuyvToBgr(static_cast<const uint8_t*>(buffers_[buf.index].start),
              out_bgr, width_, height_);

    // Re-queue buffer
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        CAM_ERR("VIDIOC_QBUF re-queue");
    }

    return true;
}

bool CameraCapture::GrabFrameYUYV(uint8_t* out_yuyv)
{
    if (!streaming_ || fd_ < 0)
        return false;

    // Poll with 100ms timeout
    struct pollfd pfd = { fd_, POLLIN, 0 };
    int ret = poll(&pfd, 1, 100);
    if (ret <= 0)
        return false;

    // Dequeue buffer
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN)
            return false;
        CAM_ERR("VIDIOC_DQBUF");
        return false;
    }

    // Copy raw YUYV data (width * height * 2 bytes)
    memcpy(out_yuyv, buffers_[buf.index].start, width_ * height_ * 2);

    // Re-queue buffer
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        CAM_ERR("VIDIOC_QBUF re-queue");
    }

    return true;
}

/* ── Release ───────────────────────────────────────────────────────── */

void CameraCapture::Release()
{
    if (streaming_ && fd_ >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }

    for (int i = 0; i < buf_count_; ++i) {
        if (buffers_[i].start && buffers_[i].start != MAP_FAILED) {
            munmap(buffers_[i].start, buffers_[i].length);
            buffers_[i].start = nullptr;
        }
    }
    buf_count_ = 0;

    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }

    CAM_LOG("Camera released");
}

/* ── YUYV to BGR conversion ────────────────────────────────────────── */

void CameraCapture::YuyvToBgr(const uint8_t* yuyv, uint8_t* bgr, int w, int h)
{
    // YUYV: 2 pixels per 4 bytes (Y0 U Y1 V)
    const int total = w * h;
    for (int i = 0; i < total; i += 2) {
        const int yi = i * 2;
        const int Y0 = yuyv[yi + 0];
        const int U  = yuyv[yi + 1] - 128;
        const int Y1 = yuyv[yi + 2];
        const int V  = yuyv[yi + 3] - 128;

        // ITU-R BT.601 conversion
        // B = Y + 1.772*U
        // G = Y - 0.344*U - 0.714*V
        // R = Y + 1.402*V
        const int bi = i * 3;
        bgr[bi + 0] = clamp_u8(Y0 + ((454 * U) >> 8));                        // B
        bgr[bi + 1] = clamp_u8(Y0 - ((88 * U + 183 * V) >> 8));              // G
        bgr[bi + 2] = clamp_u8(Y0 + ((359 * V) >> 8));                        // R

        bgr[bi + 3] = clamp_u8(Y1 + ((454 * U) >> 8));                        // B
        bgr[bi + 4] = clamp_u8(Y1 - ((88 * U + 183 * V) >> 8));              // G
        bgr[bi + 5] = clamp_u8(Y1 + ((359 * V) >> 8));                        // R
    }
}

} // namespace AI
