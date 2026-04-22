#!/usr/bin/env bash
# deploy_prebuilt.sh
#
# Copies DRP-AI runtime libs + YOLOv8n model to the RDK board.
# Run once before first use, or when updating libs/model.
#
# Usage:
#   ./tools/deploy_prebuilt.sh [board-ip]
#   Default board IP: $RDK_IP env var, or 192.168.1.150
#
# Expects on host (beside this script):
#   ../third_party_bin/lib/aarch64/*.so   — DRP-AI runtime shared libs
#   ../third_party_bin/models/yolov8n/    — compiled model files

set -e

BOARD_IP="${1:-${RDK_IP:-192.168.1.150}}"
BOARD="root@${BOARD_IP}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREBUILT_DIR="${SCRIPT_DIR}/../third_party_bin"

echo "==> Deploying to ${BOARD}"

# ── DRP-AI shared libs → /usr/lib/ ─────────────────────────────────────
LIB_DIR="${PREBUILT_DIR}/lib/aarch64"
if [ -d "${LIB_DIR}" ]; then
    echo "--> Copying DRP-AI libs..."
    scp "${LIB_DIR}"/*.so "${BOARD}:/usr/lib/"
    ssh "${BOARD}" "ldconfig"
    echo "    OK"
else
    echo "WARN: ${LIB_DIR} not found — skipping libs (must be present on board already)"
fi

# ── YOLOv8n model → /home/root/ai_models/yolov8n/ ─────────────────────
MODEL_DIR="${PREBUILT_DIR}/models/yolov8n"
if [ -d "${MODEL_DIR}" ]; then
    echo "--> Copying YOLOv8n model..."
    ssh "${BOARD}" "mkdir -p /home/root/ai_models/yolov8n"
    scp -r "${MODEL_DIR}/"* "${BOARD}:/home/root/ai_models/yolov8n/"
    echo "    OK"
else
    echo "WARN: ${MODEL_DIR} not found — skipping model"
fi

echo "==> Done. Board ${BOARD_IP} is ready for AI inference."
echo "    Test with: ssh ${BOARD} 'ls /home/root/ai_models/yolov8n && ldconfig -p | grep mera'"
