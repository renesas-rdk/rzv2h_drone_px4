/**
 * @file camera_capture.cpp
 * @brief V4L2 USB camera capture implementation
 *
 * Negotiates MJPEG format first to reduce USB isochronous bandwidth from
 * ~18 MB/s (YUYV 640×480@30fps) to ~2–5 MB/s, preventing xHCI DMA starvation
 * on the RZ/V2H AXI bus shared with DRP-AI and the OMX H.264 encoder.
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
#include <jpeglib.h>
#include <setjmp.h>
#include <arm_neon.h>

#define CAM_LOG(fmt, ...) fprintf(stderr, "[Camera] " fmt "\n", ##__VA_ARGS__)
#define CAM_ERR(fmt, ...) fprintf(stderr, "[Camera ERROR] " fmt ": %s\n", ##__VA_ARGS__, strerror(errno))

namespace AI {

/* ── Lenient JPEG error handler ─────────────────────────────────────── */
// USB camera MJPEG streams often have extraneous bytes before RST markers
// (padding added to align USB isochronous packet boundaries). libjpeg-turbo
// treats these as fatal errors in strict mode. Suppress warnings so the
// decoder continues; only fatal decode failures call longjmp.
struct LenientJpegErr {
    struct jpeg_error_mgr pub;
    jmp_buf               jbuf;
};
static void lenient_error_exit(j_common_ptr cinfo)
{
    LenientJpegErr* e = reinterpret_cast<LenientJpegErr*>(cinfo->err);
    longjmp(e->jbuf, 1);
}
static void lenient_emit_message(j_common_ptr, int) {}  // suppress all warnings

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
    : fd_(-1), width_(0), height_(0), buf_count_(0), streaming_(false),
      fmt_mjpeg_(false), tj_handle_(nullptr), yuv_tmp_(nullptr)
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

    // Try MJPEG first — Logitech cameras produce MJPEG natively at ~2–5 MB/s
    // (vs 18 MB/s for YUYV 640×480@30fps), preventing xHCI DMA starvation.
    // Fall back to YUYV for cameras that do not support MJPEG.
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if (xioctl(fd_, VIDIOC_S_FMT, &fmt) == 0 &&
        fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG) {
        fmt_mjpeg_ = true;
    } else {
        // MJPEG rejected — retry with YUYV
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
        fmt_mjpeg_ = false;
        CAM_LOG("MJPEG not supported, falling back to YUYV");
    }
    width_  = fmt.fmt.pix.width;
    height_ = fmt.fmt.pix.height;
    CAM_LOG("Negotiated format: %dx%d %s", width_, height_,
            fmt_mjpeg_ ? "MJPEG" : "YUYV");

    // MJPEG decode now uses libjpeg directly (no TurboJPEG handle needed).

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

    bool ok = false;
    if (fmt_mjpeg_) {
        const uint8_t* src      = static_cast<const uint8_t*>(buffers_[buf.index].start);
        unsigned long  data_len = buf.bytesused;
        if (data_len == 0)
            data_len = buf.length;

        // Use libjpeg directly with a lenient error handler so that
        // extraneous bytes before RST markers (USB packet-alignment padding)
        // are treated as warnings and suppressed rather than aborting the decode.
        LenientJpegErr jerr;
        struct jpeg_decompress_struct cinfo;
        cinfo.err               = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit     = lenient_error_exit;
        jerr.pub.emit_message   = lenient_emit_message;

        if (setjmp(jerr.jbuf)) {
            jpeg_destroy_decompress(&cinfo);
            CAM_LOG("MJPEG decode fatal error (len=%lu)", data_len);
        } else {
            jpeg_create_decompress(&cinfo);
            jpeg_mem_src(&cinfo, src, data_len);
            jpeg_read_header(&cinfo, TRUE);
            // JCS_RGB is universally available (standard libjpeg); JCS_EXT_BGR is
            // a libjpeg-turbo extension (value 13) absent on some runtime builds.
            // Decode to RGB then swap R↔B in-place — one extra pass, ~1 ms.
            cinfo.out_color_space = JCS_RGB;
            jpeg_start_decompress(&cinfo);

            const int out_w      = (int)cinfo.output_width;
            const int out_h      = (int)cinfo.output_height;
            const int row_stride = out_w * 3;
            while (cinfo.output_scanline < (JDIMENSION)out_h) {
                JSAMPROW row = out_bgr + cinfo.output_scanline * row_stride;
                jpeg_read_scanlines(&cinfo, &row, 1);
            }
            jpeg_finish_decompress(&cinfo);
            jpeg_destroy_decompress(&cinfo);

            // Swap R↔B to convert RGB → BGR.
            // NEON vld3q_u8 de-interleaves 16 pixels into {R[16], G[16], B[16]};
            // swap R↔B vectors then vst3q_u8 re-interleaves — 16× fewer iterations.
            const int npix = out_w * out_h;
            int i = 0;
            for (; i + 16 <= npix; i += 16) {
                uint8x16x3_t px = vld3q_u8(out_bgr + i * 3);
                uint8x16_t   tmp = px.val[0];
                px.val[0] = px.val[2];
                px.val[2] = tmp;
                vst3q_u8(out_bgr + i * 3, px);
            }
            for (; i < npix; ++i) {   // scalar tail (remainder < 16 pixels)
                uint8_t* p = out_bgr + i * 3;
                uint8_t  t = p[0]; p[0] = p[2]; p[2] = t;
            }
            ok = true;
        }
    } else {
        // YUYV path — direct conversion
        YuyvToBgr(static_cast<const uint8_t*>(buffers_[buf.index].start),
                  out_bgr, width_, height_);
        ok = true;
    }

    // Re-queue buffer
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        CAM_ERR("VIDIOC_QBUF re-queue");
    }

    return ok;
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

    delete[] yuv_tmp_;
    yuv_tmp_   = nullptr;
    fmt_mjpeg_ = false;

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
