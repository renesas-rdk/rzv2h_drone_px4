#!/usr/bin/env bash
# stream_test.sh
#
# Quick camera stream debug test — runs AI inference + UDP stream on the board,
# then opens stream_viewer.py on the host.
#
# This uses the STANDALONE binary approach (no ROS2 needed on board):
#   - Requires CustomXRCEAgent binary built WITH --ai flag support
#   - Requires DRP-AI libs + model deployed (run deploy_prebuilt.sh first)
#
# Usage:
#   ./tools/stream_test.sh [board-ip] [camera-device]
#   Defaults: board=192.168.1.150, camera=/dev/video0
#
# NOTE: This test bypasses ROS2. For full pipeline use active_track bringup.
# NOTE: Stop the test with Ctrl+C — the board process receives SIGINT cleanly.

set -e

BOARD_IP="${1:-${RDK_IP:-192.168.1.150}}"
CAMERA="${2:-/dev/video0}"
BOARD="root@${BOARD_IP}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
AGENT_BIN="${3:-/home/root/CustomXRCEAgent}"  # must be AI-capable binary

echo "==> Stream test: board=${BOARD_IP}, camera=${CAMERA}"
echo "    Binary: ${AGENT_BIN}"

# ── Check board reachable ──────────────────────────────────────────────
if ! ping -c 1 -W 2 "${BOARD_IP}" > /dev/null 2>&1; then
    echo "ERROR: Board ${BOARD_IP} unreachable"
    exit 1
fi

# ── Stop any existing AI stream processes on board ────────────────────
echo "--> Stopping existing AI processes on board..."
ssh "${BOARD}" "killall -INT CustomXRCEAgent 2>/dev/null; sleep 1; killall -9 CustomXRCEAgent 2>/dev/null; true" || true
sleep 1

# ── Start AI agent on board ────────────────────────────────────────────
echo "--> Starting AI stream on board (port 50002)..."
LOG=/tmp/ai_stream.log
ssh "${BOARD}" "
  nohup ${AGENT_BIN} \
    --ai \
    --ai-model /home/root/ai_models/yolov8n \
    --ai-camera ${CAMERA} \
    --stream \
    > ${LOG} 2>&1 &
  sleep 2
  grep -E 'pipeline ready|streamer enabled|ERROR|error' ${LOG} || true
" 2>&1

# ── Start viewer on host ───────────────────────────────────────────────
echo "--> Opening stream viewer (Ctrl+C to stop)..."
echo "    Board will switch to unicast once viewer heartbeat is received."

cleanup() {
    echo ""
    echo "--> Stopping AI process on board (SIGINT — clean DRP-AI shutdown)..."
    ssh "${BOARD}" "killall -INT CustomXRCEAgent 2>/dev/null; true" || true
    echo "    Done."
}
trap cleanup EXIT INT TERM

python3 "${SCRIPT_DIR}/stream_viewer.py" --board "${BOARD_IP}"
