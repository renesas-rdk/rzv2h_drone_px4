#!/bin/sh
# Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
#
# SPDX-License-Identifier: BSD-3-Clause
#
# RZ/V2H → QGC H.264 hardware-encoded video stream
# Uses Renesas OMX H.264 encoder (omxh264enc, libomxr_mc_h264e.so)
#
# Usage:
#   qgc_stream.sh <QGC_HOST_IP> [options]
#   qgc_stream.sh 192.168.1.39
#   qgc_stream.sh 192.168.1.39 --bitrate 1500000 --res 640x480 --fps 30
#
# QGC side: Application Settings → Video → UDP H.264, port 5600
# Receive test (host): ffplay -fflags nobuffer -i /path/to/stream.sdp
#
# Bandwidth guide (H.264 HW encoder vs JPEG UDP):
#   640x480 @ 30fps H.264 2Mbps   → ~1.2 Mbps actual (5× less than JPEG@50%)
#   640x480 @ 30fps H.264 1Mbps   → ~700 kbps  (good for 2.4GHz WiFi)
#   640x480 @ 15fps H.264 800kbps → ~550 kbps  (minimal bandwidth mode)
#
# CPU overhead on RZ/V2H CA55:
#   OMX H.264 encode (HW): ~5-8% CPU  (vs ~25-35% for software x264)
#   videoconvert YUYV→I420: ~6% CPU
#   Total streaming overhead: ~12-15% CPU (leaves plenty for DRP-AI)

set -e

QGC_IP="${1}"
DEVICE="${DEVICE:-/dev/video0}"
BITRATE="${BITRATE:-2000000}"    # bits/s — 2 Mbps default
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-480}"
FPS="${FPS:-30}"
PORT="${PORT:-5600}"             # QGC default UDP video port

# Parse optional flags
shift 2>/dev/null || true
while [ $# -gt 0 ]; do
    case "$1" in
        --bitrate) BITRATE="$2"; shift 2 ;;
        --res)     WIDTH="${2%%x*}"; HEIGHT="${2##*x}"; shift 2 ;;
        --fps)     FPS="$2"; shift 2 ;;
        --device)  DEVICE="$2"; shift 2 ;;
        --port)    PORT="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,20p' "$0" | sed 's/^# //'
            exit 0
            ;;
    esac
done

if [ -z "$QGC_IP" ]; then
    echo "Usage: $0 <QGC_HOST_IP> [--bitrate N] [--res WxH] [--fps N] [--device /dev/videoN]"
    echo "Example: $0 192.168.1.39 --bitrate 1500000"
    exit 1
fi

# Verify camera device exists
if [ ! -e "$DEVICE" ]; then
    echo "ERROR: Camera device $DEVICE not found"
    echo "Available devices:"
    ls /dev/video* 2>/dev/null || echo "  none"
    exit 1
fi

echo "=== RZ/V2H QGC Video Stream ==="
echo "  Camera   : $DEVICE (${WIDTH}x${HEIGHT} @ ${FPS}fps)"
echo "  Encoder  : Renesas OMX H.264 HW (omxh264enc)"
echo "  Bitrate  : $((BITRATE / 1000)) kbps"
echo "  Target   : udp://${QGC_IP}:${PORT}"
echo ""
echo "QGC setup: Application Settings → Video → Source: UDP H.264, Port: ${PORT}"
echo ""
echo "Press Ctrl+C to stop stream"
echo "---"

exec gst-launch-1.0 -e \
    v4l2src device="${DEVICE}" ! \
    "video/x-raw,width=${WIDTH},height=${HEIGHT},framerate=${FPS}/1" ! \
    videoconvert ! \
    "video/x-raw,format=I420" ! \
    omxh264enc target-bitrate="${BITRATE}" control-rate=variable ! \
    "video/x-h264,stream-format=byte-stream,profile=main" ! \
    h264parse ! \
    rtph264pay config-interval=1 pt=96 ! \
    "application/x-rtp,media=video,encoding-name=H264,payload=96" ! \
    udpsink host="${QGC_IP}" port="${PORT}" sync=false
