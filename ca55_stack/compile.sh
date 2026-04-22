#!/bin/bash
# Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
#
# SPDX-License-Identifier: BSD-3-Clause

# compile.sh — CA55 Stack build orchestrator
# Builds: XRCE-DDS Agent + ROS2 ActiveTrack workspace
#
# Usage:
#   ./compile.sh agent         # Build CustomXRCEAgent only
#   ./compile.sh ros2          # Build ROS2 workspace (ActiveTrack pipeline)
#   ./compile.sh all           # Build both
#   ./compile.sh clean         # Clean all build dirs
#   ./compile.sh deploy        # Deploy to RDK board (set RZV_TARGET_HOST first)
#   ./compile.sh status        # Show build status

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AGENT_DIR="$SCRIPT_DIR/xrce_dds_agent"
ROS2_WS_DIR="$SCRIPT_DIR/ros2_ws"
PREBUILT_DIR="$SCRIPT_DIR/third_party_bin"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC}  $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step()  { echo -e "${BLUE}[STEP]${NC}  $1"; }

RZV_TARGET_HOST="${RZV_TARGET_HOST:-${RDK_IP:-192.168.1.150}}"
RZV_TARGET_USER="${RZV_TARGET_USER:-root}"
RZV_SSH_OPTIONS="${RZV_SSH_OPTIONS:--o StrictHostKeyChecking=no}"

# ── Build XRCE-DDS Agent ──────────────────────────────────────────────────────
build_agent() {
    log_step "Building CustomXRCEAgent..."
    bash "$AGENT_DIR/compile_agent.sh" build
    log_info "✓ CustomXRCEAgent built: $AGENT_DIR/src/build/CustomXRCEAgent"
}

# ── Build ROS2 workspace (cross-compile with Poky SDK) ───────────────────────
build_ros2() {
    log_step "Building ROS2 workspace (ActiveTrack pipeline)..."

    if [ -z "${POKY_ENVIRONMENT_SETUP:-}" ]; then
        POKY_ENVIRONMENT_SETUP="/opt/toolchains/poky/3.1.31/environment-setup-aarch64-poky-linux"
        log_warn "POKY_ENVIRONMENT_SETUP not set, defaulting to $POKY_ENVIRONMENT_SETUP"
    fi

    if [ ! -f "$POKY_ENVIRONMENT_SETUP" ]; then
        log_error "Poky environment not found: $POKY_ENVIRONMENT_SETUP"
        log_error "Install Poky SDK or set POKY_ENVIRONMENT_SETUP env var."
        exit 1
    fi

    # ROS2 Humble environment — check either system or Docker
    if [ -f "/opt/ros/humble/setup.bash" ]; then
        ROS2_SETUP="/opt/ros/humble/setup.bash"
        log_info "ROS2 Humble found at $ROS2_SETUP"
    else
        log_warn "ROS2 Humble not found at /opt/ros/humble/setup.bash"
        log_warn "You may need Docker container or meta-ros Yocto build."
        log_warn "See docs/BUILD.md for setup instructions."
        # Attempt anyway — colcon might be available
        ROS2_SETUP=""
    fi

    # Source environments and run colcon
    local colcon_cmd="colcon build --merge-install \
        --cmake-args \
          -DCMAKE_BUILD_TYPE=Release \
          -DCA55_PREBUILT_DIR=$PREBUILT_DIR \
          -DCMAKE_TOOLCHAIN_FILE=$AGENT_DIR/cross.cmake \
        --install-base $ROS2_WS_DIR/install"

    log_info "Running colcon in $ROS2_WS_DIR ..."
    (
        source "$POKY_ENVIRONMENT_SETUP"
        [ -n "$ROS2_SETUP" ] && source "$ROS2_SETUP"
        cd "$ROS2_WS_DIR"
        eval "$colcon_cmd"
    )
    log_info "✓ ROS2 workspace built: $ROS2_WS_DIR/install/"
}

# ── Clean ─────────────────────────────────────────────────────────────────────
clean_all() {
    log_step "Cleaning all build artifacts..."
    bash "$AGENT_DIR/compile_agent.sh" clean 2>/dev/null || true
    rm -rf "$ROS2_WS_DIR/build" "$ROS2_WS_DIR/install" "$ROS2_WS_DIR/log"
    log_info "✓ Clean done"
}

# ── Deploy to RDK ─────────────────────────────────────────────────────────────
deploy_all() {
    local target="${RZV_TARGET_USER}@${RZV_TARGET_HOST}"
    log_step "Deploying to $target ..."

    # Agent binary
    if [ -f "$AGENT_DIR/src/build/CustomXRCEAgent" ]; then
        log_info "Deploying CustomXRCEAgent..."
        scp $RZV_SSH_OPTIONS "$AGENT_DIR/src/build/CustomXRCEAgent" \
            "$target:/usr/bin/CustomXRCEAgent"
    else
        log_warn "CustomXRCEAgent not built, skipping"
    fi

    # ROS2 install tree
    if [ -d "$ROS2_WS_DIR/install" ]; then
        log_info "Deploying ROS2 install tree..."
        rsync -az --progress $RZV_SSH_OPTIONS \
            "$ROS2_WS_DIR/install/" \
            "$target:/home/root/ros2_active_track/"
    else
        log_warn "ROS2 workspace not built, skipping"
    fi

    # DRP-AI prebuilt libs
    if [ -d "$PREBUILT_DIR/lib/aarch64" ]; then
        log_info "Deploying prebuilt DRP-AI libs..."
        rsync -az $RZV_SSH_OPTIONS \
            "$PREBUILT_DIR/lib/aarch64/" "$target:/usr/lib/"
    fi

    # Model weights
    if [ -d "$PREBUILT_DIR/models/yolov8n" ]; then
        log_info "Deploying YOLOv8n model..."
        rsync -az --progress $RZV_SSH_OPTIONS \
            "$PREBUILT_DIR/models/yolov8n/" \
            "$target:/home/root/ai_models/yolov8n/"
    fi

    # systemd units
    if [ -d "$SCRIPT_DIR/systemd" ]; then
        log_info "Deploying systemd units..."
        rsync -az $RZV_SSH_OPTIONS \
            "$SCRIPT_DIR/systemd/" "$target:/etc/systemd/system/"
        ssh $RZV_SSH_OPTIONS "$target" "systemctl daemon-reload"
    fi

    log_info "✓ Deploy complete to $target"
}

# ── Status ────────────────────────────────────────────────────────────────────
show_status() {
    echo "=== CA55 Stack Build Status ==="
    echo ""
    if [ -f "$AGENT_DIR/src/build/CustomXRCEAgent" ]; then
        echo -e "  CustomXRCEAgent: ${GREEN}✓ Built${NC} ($(du -h "$AGENT_DIR/src/build/CustomXRCEAgent" | cut -f1))"
    else
        echo -e "  CustomXRCEAgent: ${RED}✗ Not built${NC}"
    fi
    if [ -d "$ROS2_WS_DIR/install" ]; then
        echo -e "  ROS2 workspace:  ${GREEN}✓ Built${NC} ($ROS2_WS_DIR/install)"
    else
        echo -e "  ROS2 workspace:  ${RED}✗ Not built${NC}"
    fi
    if [ -d "$PREBUILT_DIR/models/yolov8n" ]; then
        echo -e "  YOLOv8n model:   ${GREEN}✓ Present${NC}"
    else
        echo -e "  YOLOv8n model:   ${YELLOW}⚠ Missing${NC} (copy from wheel_legged_robot or see docs/BUILD_YOLOV8_WEIGHTS.md)"
    fi
    if [ -d "$PREBUILT_DIR/lib/aarch64" ] && ls "$PREBUILT_DIR/lib/aarch64"/*.so* >/dev/null 2>&1; then
        echo -e "  DRP-AI libs:     ${GREEN}✓ Present${NC}"
    else
        echo -e "  DRP-AI libs:     ${YELLOW}⚠ Missing${NC} (copy from Renesas RUHMI SDK, see docs/BUILD.md)"
    fi
}

# ── Main ──────────────────────────────────────────────────────────────────────
case "${1:-}" in
    agent)       build_agent   ;;
    ros2)        build_ros2    ;;
    all)         build_agent; build_ros2 ;;
    clean)       clean_all     ;;
    deploy)      deploy_all    ;;
    status)      show_status   ;;
    *)
        echo "Usage: $0 {agent|ros2|all|clean|deploy|status}"
        echo ""
        echo "  agent    Build CustomXRCEAgent (xrce_dds_agent/)"
        echo "  ros2     Build ROS2 ActiveTrack workspace (ros2_ws/)"
        echo "  all      Build both"
        echo "  clean    Remove all build artifacts"
        echo "  deploy   Deploy binaries to RDK board (RZV_TARGET_HOST)"
        echo "  status   Show build status"
        echo ""
        echo "Env vars:"
        echo "  POKY_ENVIRONMENT_SETUP  Path to Poky SDK env script"
        echo "  RZV_TARGET_HOST         Board IP (default: \$RDK_IP or 192.168.1.150)"
        echo "  RZV_TARGET_USER         Board user (default: root)"
        exit 1
        ;;
esac

log_info "Done!"
