# RZ/V2H Drone — PX4 on FreeRTOS + ROS 2

<img src="docs/assets/drone_components.png" width="700" alt="Drone Components"/>

[![License](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-RZ%2FV2H-orange.svg)](https://www.renesas.com/en/products/rz-v2h)
[![PX4](https://img.shields.io/badge/PX4-v1.16.0-blue.svg)](https://px4.io)
[![FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green.svg)](https://www.freertos.org)
[![ROS2](https://img.shields.io/badge/ROS2-Humble-blue.svg)](https://docs.ros.org/en/humble/)

Autonomous drone firmware for the **Renesas RZ/V2H RDK** board.  
PX4 v1.16 runs on the Cortex-R8 (FreeRTOS+POSIX). A ROS 2 autonomy stack runs on the Cortex-A55 (Linux/Yocto). Both cores communicate via OpenAMP/RPMsg on the same SoC.

---

## System Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                          RZ/V2H SoC                                    │
│                                                                        │
│  ┌──────────────────────────┐  OpenAMP/RPMsg  ┌──────────────────────┐ │
│  │   Cortex-R8 (CR8)        │◄───────────────►│  Cortex-A55 (CA55)   │ │
│  │   FreeRTOS + POSIX       │                 │  Linux (Yocto/Poky)  │ │
│  │                          │                 │                      │ │
│  │  ┌──────────────────┐    │                 │  ┌────────────────┐  │ │
│  │  │  PX4 Autopilot   │    │                 │  │  ROS 2 Humble  │  │ │
│  │  │  (libpx4.a)      │    │                 │  └────────────────┘  │ │
│  │  └──────────────────┘    │                 │  ┌────────────────┐  │ │
│  │  ┌──────────────────┐    │                 │  │  XRCE-DDS      │  │ │
│  │  │  XRCE-DDS Client │    │                 │  │  Agent         │  │ │
│  │  └──────────────────┘    │                 │  └────────────────┘  │ │
│  └──────────────────────────┘                 └──────────────────────┘ │
│                                                                        │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │   40-pin GPIO Header (Raspberry Pi compatible)                   │  │
│  │   IMU(SPI) │ Baro(I2C) │ GPS(UART) │ LiDAR(UART) │ 4×ESC(PWM)    │  │
│  │   Telemetry(UART) │ RC Receiver(SBUS/UART)                       │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Hardware

| Component | Model |
|-----------|-------|
| Flight Controller | Renesas RZ/V2H RDK |
| Frame | LX450 (recommended) |
| IMU | MPU9250 (SPI) |
| Barometer | BMP280 (I2C) |
| GPS | u-blox M10 (UART) |
| LiDAR | TFmini Plus (UART) |
| Telemetry | Sik V3 433/915 MHz |
| RC Receiver | FlySky fs-a8s (SBUS) |
| ESC | 4× SkyWalker 40A |
| Motor | 4× Sunny Sky X2216 1100kV |
| Propeller | 9450 (LX450) or 1045 (S500) |
| Power | Holybro PM03D + 4S LiPo |
| Camera | USB (CA55/Linux) |

See [docs/HARDWARE.md](docs/HARDWARE.md) for pinout and wiring.

---

## Prerequisites

| Tool | Version |
|------|---------|
| ARM GNU Toolchain (CR8) | 13.3.Rel1 (`arm-none-eabi`) |
| Docker (CA55 agent) | ≥ 20.x (`ghcr.io/renesas-rdk/rzv2h_ubuntu_xbuild`) |
| CMake | ≥ 3.16 |
| Ninja | any |

```bash
export TOOLCHAIN_BASE_PATH="/opt/toolchains/gcc_arm/13_3-Rel1"
```

---

## e2studio Project Setup

The BSP, HAL, and peripheral config are **not stored in this repo**. They must be generated once using [e2studio](https://www.renesas.com/en/software-tool/e-studio) before building with `compile.sh`.

### Steps

1. **Install e2studio** with the RZ/V2H device support pack.

2. **Import the project:**
   ```
   File → Import → Existing Projects into Workspace → select repo root
   ```

3. **Generate BSP and HAL sources:**
   - Open `configuration.xml` in e2studio
   - Click **"Generate Project Content"** (or press the generate button in the FSP configurator toolbar)
   - e2studio will create/populate: `rzv/`, `rzv_cfg/`, `rzv_gen/`, `script/`

4. Proceed to `compile.sh build` — the generated files will be picked up automatically.

---

## Quick Start

### 1. Clone

```bash
git clone --recurse-submodules https://github.com/renesas-rdk/rzv2h_drone_px4.git
cd rzv2h_drone_px4
```

### 2. Build CR8 firmware

```bash
./compile.sh build
# Outputs: Debug/rzv2h_px4_freertos_itcm.bin  Debug/rzv2h_px4_freertos_sdram.bin
```

### 3. Build CA55 stack

```bash
cd ca55_stack/xrce_dds_agent
./compile_agent.sh docker-build   # pulls Docker image on first run (~10 GB), then builds
# Output: xrce_dds_agent/src/build/CustomXRCEAgent
```

### 4. Deploy to board

```bash
# CR8 firmware (default: root@<BOARD_IP>)
./compile.sh deploy-cr8

# CA55 agent
cd ca55_stack/xrce_dds_agent && ./compile_agent.sh deploy-ca55
```

Full setup details: [docs/SETUP.md](docs/SETUP.md)

---

## Project Structure

```
rzv2h_drone_px4/
├── px4/            # PX4 Autopilot v1.16.0 (submodule, branch: feature/rzv2h-freertos)
├── src/            # FreeRTOS app entry + POSIX compatibility layer
├── ca55_stack/     # CA55/Linux stack
│   ├── ros2_ws/    # ROS 2 Humble packages (detector, tracker, follower)
│   └── xrce_dds_agent/
│       ├── Micro-XRCE-DDS-Agent/  # submodule, branch: feature/rzv2h-rpmsg-transport
│       └── *.cpp   # Custom DDS agent with OpenAMP/RPMsg transport
├── docs/           # Hardware, setup, debugging guides
├── configuration.xml  # e2studio project config (source of truth for BSP/HAL generation)
├── compile.sh      # Main build script
├── cross.cmake     # ARM Cortex-R8 toolchain file
└── CMakeLists.txt  # Root CMake
```

---

## Submodules

| Path | Based on | Branch |
|------|----------|--------|
| `px4/` | PX4/PX4-Autopilot v1.16.0 | `feature/rzv2h-freertos` |
| `ca55_stack/xrce_dds_agent/Micro-XRCE-DDS-Agent/` | eProsima/Micro-XRCE-DDS-Agent v3.0.1 | `feature/rzv2h-rpmsg-transport` |

Renesas-specific changes are isolated to dedicated branches on the upstream forks. This allows tracking upstream updates and contributing patches back.

---

## Configuration

- **PX4 parameters:** `px4/boards/renesas/rzv/init/rc.board_defaults.cmds`
- **PX4 features:** `px4/boards/renesas/rzv/default.px4board`
- **DDS topics:** `px4/src/modules/uxrce_dds_client/dds_topics.yaml`
- **Ground Control:** QGroundControl via MAVLink (SiK telemetry on UART)

---

## Adding Sensors / Hardware to PX4

This board has no NuttX interactive shell (no `nsh>`). All sensor and driver configuration is done through **three files** at build time, then flashed to the board.

### The three files

| File | Purpose | When to edit |
|------|---------|-------------|
| [`px4/boards/renesas/rzv/default.px4board`](px4/boards/renesas/rzv/default.px4board) | Build-time feature flags (`CONFIG_DRIVERS_*=y`) | Enable the driver binary for a new sensor |
| [`px4/boards/renesas/rzv/init/rcS`](px4/boards/renesas/rzv/init/rcS) | Startup script — runs driver `start` commands at boot | Start the driver with correct bus/address/rotation |
| [`px4/boards/renesas/rzv/init/rc.board_defaults.cmds`](px4/boards/renesas/rzv/init/rc.board_defaults.cmds) | Factory `param set` defaults | Set sensor-enable flags, calibration seed values |

### Workflow

```
1. Edit default.px4board   — add CONFIG_DRIVERS_<SENSOR> =y
2. Edit rcS                — add "<driver> start -b <bus> [flags]"
3. Edit rc.board_defaults  — add param set SYS_HAS_<TYPE> =1 (if needed)
4. Rebuild CR8 firmware:   ./compile.sh rebuild
5. Deploy:                 ./compile.sh deploy-cr8
6. Verify in QGC:          Analyze Tools → MAVLink Inspector → check sensor topic
```

### Example: replace MPU9250 with ICM-45686 (SPI)

**Step 1 — `default.px4board`**: enable the ICM-45686 driver, disable MPU9250:

```
# Remove or comment out:
# CONFIG_DRIVERS_IMU_INVENSENSE_MPU9250=y

# Add:
CONFIG_DRIVERS_IMU_INVENSENSE_ICM45686=y
```

**Step 2 — `rcS`**: replace the `mpu9250` start line:

```bash
# Remove:
# mpu9250 -s -M -b 0 -R 0 start

# Add (-s = SPI, -b 0 = SPI bus 0, -R 0 = no rotation correction):
icm45686 -s -b 0 -R 0 start
```

> Adjust `-R <value>` for physical mounting orientation.  
> PX4 rotation constants: `ROTATION_NONE=0`, `ROTATION_YAW_180=4`, etc.  

**Step 3 — `rc.board_defaults.cmds`**: update the calibration IDs and sensor priority:

```bash
# Replace CAL_ACC0_ID / CAL_GYRO0_ID with the ICM-45686 device ID.
# Set placeholder priority; recalibrate via QGC after first boot.
param set CAL_ACC0_ID    <device_id>
param set CAL_ACC0_PRIO  50
param set CAL_GYRO0_ID   <device_id>
param set CAL_GYRO0_PRIO 50
```

**Step 4 — rebuild and verify:**

```bash
./compile.sh rebuild
./compile.sh deploy-cr8
```

### Adding a second IMU

To run both MPU9250 and ICM-45686 simultaneously:

```
# default.px4board — keep both enabled:
CONFIG_DRIVERS_IMU_INVENSENSE_MPU9250=y
CONFIG_DRIVERS_IMU_INVENSENSE_ICM45686=y
```

```bash
# rcS — start both:
mpu9250 -s -M -b 0 -R 0 start
# Must use different CS line on the same SPI bus
icm45686 -s -b 0 -R 0 start
```

PX4 will vote between sensors automatically. Set priorities in `rc.board_defaults.cmds`:

```bash
# MPU9250 — primary
param set CAL_GYRO0_PRIO 50
# ICM-45686 — backup
param set CAL_GYRO1_PRIO 20
```

### PX4 documentation references

| Topic | Link |
|-------|------|
| System startup / rcS | https://docs.px4.io/main/en/concept/system_startup.html |
| Board porting guide | https://docs.px4.io/main/en/hardware/porting_guide_config.html |
| Sensor drivers index | https://docs.px4.io/main/en/sensor/ |
| Adding a new driver | https://docs.px4.io/main/en/middleware/drivers.html |
| uORB messaging | https://docs.px4.io/main/en/middleware/uorb.html |
| Parameter system | https://docs.px4.io/main/en/advanced/parameters_and_configurations.html |
| Serial / bus assignment | https://docs.px4.io/main/en/hardware/serial_port_mapping.html |
| Flight modes / `px4board` flags | https://docs.px4.io/main/en/hardware/porting_guide_nuttx.html#px4-board-configuration-kconfig |

---

## Debugging

- **SEGGER RTT** (recommended — no UART needed):
  ```bash
  ./extract_rtt_addr.sh Debug/rzv2h_px4_freertos.map
  # Connect JLinkRTTViewer with the extracted address
  ```
- **GDB:** Press `F5` in VSCode (J-Link config in `.vscode/launch.json`)

See [docs/debugging.md](docs/debugging.md) for full details.

---

## Documentation

| File | Content |
|------|---------|
| [docs/HARDWARE.md](docs/HARDWARE.md) | BOM, pinout, wiring |
| [docs/SETUP.md](docs/SETUP.md) | Toolchain install, U-Boot config |
| [docs/debugging.md](docs/debugging.md) | RTT, GDB, log extraction |
| [docs/storage.md](docs/storage.md) | SD card partitions, firmware paths |
| [ca55_stack/README.md](ca55_stack/README.md) | CA55 ROS 2 stack overview |

---

## License

Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates.  
Custom code in this repository is licensed under the **BSD 3-Clause License** — see [LICENSE](LICENSE) for details.

This repository integrates third-party components under separate licenses:

| Component | License | Notes |
|-----------|---------|-------|
| Custom code (Renesas) | [BSD 3-Clause](LICENSE) | `ca55_stack/xrce_dds_agent/ai/`, `ca55_stack/tools/` |
| PX4 Autopilot | [BSD 3-Clause](https://github.com/PX4/PX4-Autopilot/blob/main/LICENSE) | `px4/` submodule — Copyright (c) 2012–2024 PX4 Development Team |
| Micro XRCE-DDS Agent | [Apache 2.0](https://github.com/eProsima/Micro-XRCE-DDS-Agent/blob/master/LICENSE) | `ca55_stack/xrce_dds_agent/Micro-XRCE-DDS-Agent/` — Copyright eProsima |
| FreeRTOS | [MIT](https://github.com/FreeRTOS/FreeRTOS/blob/main/LICENSE.md) | `rzv/aws/FreeRTOS/` — Copyright Amazon.com, Inc. |
| OpenAMP / libmetal | [BSD 3-Clause](https://github.com/OpenAMP/open-amp/blob/main/LICENSE.md) | `rzv/linaro/` — Copyright Mentor Graphics, Xilinx, eForce |
| Renesas FSP | [BSD 3-Clause](https://github.com/renesas/rz-fsp/blob/main/LICENSE) | `rzv/fsp/` — Copyright (c) 2020–2024 Renesas Electronics Corporation |

---

## Acknowledgments

- [PX4 Autopilot](https://github.com/PX4/PX4-Autopilot)
- [eProsima Micro XRCE-DDS](https://github.com/eProsima/Micro-XRCE-DDS-Agent)
- [OpenAMP](https://github.com/OpenAMP/open-amp)
- [FreeRTOS](https://www.freertos.org/)
- [Renesas RZ/V FSP](https://github.com/renesas/rz-fsp)
