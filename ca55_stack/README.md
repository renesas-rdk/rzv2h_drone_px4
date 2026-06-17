# CA55 Stack — ROS 2 Autonomy + XRCE-DDS Agent

CA55/Linux autonomy stack for the RZ/V2H drone.  
Runs on Cortex-A55 (Yocto/Poky Linux), communicates with PX4 on CR8 via XRCE-DDS over OpenAMP/RPMsg.

---

## Components

| Component | Description |
|-----------|-------------|
| `xrce_dds_agent/` | Custom XRCE-DDS Agent with RPMsg transport (bridges PX4 ↔ ROS 2) |
| `ros2_ws/` | ROS 2 Humble packages: detector, tracker, selector, follower |
| `systemd/` | systemd service units for auto-start on boot |
| `tools/` | Deploy scripts, stream viewer |

---

## Prerequisites

Two supported toolchains (see [SETUP.md](../docs/SETUP.md#ca55-agent-build) for full details):

- **Option A (Docker):** `ghcr.io/renesas-rdk/rzv2h_ubuntu_xbuild:latest` — pulled automatically on first build
- **Option B (Poky SDK):** Poky 3.1.31 built from `core-image-weston` (must include OpenAMP layer;
  a minimal-image SDK is missing `metal/openamp` headers)

- ROS 2 Jazzy workspace sourced on the target board

---

## Build

### XRCE-DDS Agent

```bash
cd xrce_dds_agent
./compile_agent.sh docker-build
# Output: src/build/CustomXRCEAgent
```

#### Build with AI Camera + QGC stream (optional)

Requires the DRP-AI TVM SDK symlink at `../rzv_drp-ai_tvm` — see [DRP-AI TVM SDK](#drp-ai-tvm-sdk) below.

```bash
ENABLE_AI_CAMERA=ON ./compile_agent.sh docker-build
```

### ROS 2 Packages

```bash
cd ros2_ws
colcon build --packages-select \
  active_track_msgs active_track_detector active_track_tracker \
  active_track_selector
```

---

## Deploy

```bash
# Deploy agent binary + restart systemd service
cd xrce_dds_agent
RZV_TARGET_HOST=<BOARD_IP> ./compile_agent.sh deploy-ca55

# Deploy ROS 2 packages (first time: also copy deps)
./compile.sh deploy-ca55
```

| Env var | Default | Purpose |
|---------|---------|---------|
| `RZV_TARGET_HOST` | `192.168.1.xxx` | Board IP address |
| `RZV_TARGET_USER` | `root` | SSH user |
| `RZV_AGENT_SERVICE_NAME` | `custom-xrce-agent` | systemd service |

---

## ROS 2 Topics (PX4 ↔ ROS 2 bridge)

Key topics published by PX4 and available on CA55:

| Topic | Direction | Type |
|-------|-----------|------|
| `/fmu/out/vehicle_local_position` | CR8 → CA55 | `px4_msgs/VehicleLocalPosition` |
| `/fmu/out/vehicle_attitude` | CR8 → CA55 | `px4_msgs/VehicleAttitude` |
| `/fmu/out/vehicle_status` | CR8 → CA55 | `px4_msgs/VehicleStatus` |
| `/fmu/in/trajectory_setpoint` | CA55 → CR8 | `px4_msgs/TrajectorySetpoint` |
| `/fmu/in/offboard_control_mode` | CA55 → CR8 | `px4_msgs/OffboardControlMode` |

Full topic list: `../px4/src/modules/uxrce_dds_client/dds_topics.yaml`

---

## Camera AI & ActiveTrack

DRP-AI YOLOv8n object detection + ByteTrack tracking + target lock.

```bash
# Start CustomXRCEAgent with AI camera + QGC H.264 stream
/usr/bin/CustomXRCEAgent -c 2 -v 6 \
    --qgc-ip <QGC_HOST_IP> \
    --ai-model /home/root/ai_models/yolov8n \
    --ai-camera /dev/video0
```

QGC: **Video Source = UDP H.264, Port 5600** — bounding boxes rendered before HW encode.

**Stop AI processes with SIGINT only** (`killall -INT`). SIGKILL hangs DRP-AI hardware and requires a reboot.

---

## DRP-AI TVM SDK

**What it is:** Renesas DRP-AI TVM is the neural-network runtime that compiles and runs models (YOLOv8n etc.) on the RZ/V2H DRP-AI hardware accelerator.

**Official guide & model conversion:** https://github.com/renesas-rz/rzv_drp-ai_tvm

**Location in repo:** `../rzv_drp-ai_tvm/` — a symlink pointing to a local clone of the SDK.
This path is listed in `.gitignore` (not committed); each developer sets it up once.

**Pre-built runtime libraries** (`obj/build_runtime/v2h/lib/`):

| Library | Purpose |
|---------|---------|
| `libmera2_runtime.so` | Main TVM runtime — model load & inference |
| `libdrp_tvm_rt.so` | DRP-AI TVM bridge |
| `libdrp_rt.so` | DRP-AI hardware driver interface |
| `libacl_rt.so` | ARM Compute Library backend (ACL) |
| `libarm_compute*.so` | ARM Compute Library (NEON/SVE ops) |
| `libmera2_plan_io.so` | Model plan file I/O |

**First-time setup (clone + symlink):**
```bash
# 1. Clone the SDK somewhere on your machine
git clone https://github.com/renesas-rz/rzv_drp-ai_tvm.git /path/to/rzv_drp-ai_tvm

# 2. Create the symlink the build system expects
ln -sf /path/to/rzv_drp-ai_tvm  /path/to/rzv2h_drone_px4/rzv_drp-ai_tvm
```

> The pre-built `.so` files in `obj/build_runtime/v2h/lib/` are ready to use —
> no need to rebuild the SDK for normal development.
> Rebuild only when targeting a different RZ/V board or upgrading the SDK version.
