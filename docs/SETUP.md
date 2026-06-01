# Development Environment Setup

Quick guide to set up and deploy firmware on Renesas RZ/V2H. For hardware pinout/wiring, see [HARDWARE.md](HARDWARE.md).

---

## Quick Start

### 1. Prerequisites

- **Host PC**: x86_64 Linux (Ubuntu 20.04+ recommended)
- **Target Board**: Renesas RZ/V2H RDK with SD card flashed
- **ARM GNU Toolchain 13.3**: [Download](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  - Install to `/opt/toolchains/gcc_arm/13_3-Rel1`
  - Set `export TOOLCHAIN_BASE_PATH="/opt/toolchains/gcc_arm/13_3-Rel1"`
- **Build tools**: `sudo apt-get install -y build-essential git cmake ninja-build`
- **Python**: `pip3 install empy pyyaml`
- **Optional**: SEGGER J-Link for debugging

### 2. Clone & Build

```bash
git clone https://github.com/renesas-rdk/rzv2h_drone_px4.git
cd rzv2h_drone_px4
git submodule update --init --recursive

export TOOLCHAIN_BASE_PATH="/opt/toolchains/gcc_arm/13_3-Rel1"
./compile.sh build

# CA55 XRCE-DDS Agent (Linux side) — optional if Poky SDK unavailable
cd ca55_stack && ./compile.sh agent
```

**Outputs:**
- `Debug/rzv2h_px4_freertos_itcm.bin`, `Debug/rzv2h_px4_freertos_sdram.bin` — CR8 firmware
- `ca55_stack/xrce_dds_agent/src/build/CustomXRCEAgent` — CA55 agent

### 3. Configure RDK: DTB + U-Boot

**Step 1: Copy custom DTB to RDK**

This DTB disables SPI, GPT/PWM, UART, I2C, and CANFD on the Linux side so CR8 can
exclusively control these peripherals. **Using the stock RDK DTB causes Linux to claim
UART5/SPI/I2C and conflict with CR8 firmware — the board will boot but CR8 will not
initialise correctly.**

Run from the repo root (the `.dtb` file is checked in at the root):

```bash
scp r9a09g057h4-rdk-ver1-disabled-spi-gpt-serial-i2c-canfd.dtb \
  root@<RDK_IP>:/boot/dtb/renesas/r9a09g057h4-rdk-ver1.dtb
```

**Step 2: Configure U-Boot (at RDK U-Boot prompt)**

Set bootargs:
- Add `mmc rescan` + retry to improve cold-boot stability after power unplug/replug.
- Load kernel/DTB via `kernel_addr_r` and `fdt_addr_r` to avoid overlap and unstable relocation behavior.
- `fdt_high` and `initrd_high` set to `0xffffffffffffffff` to prevent U-Boot from
  relocating the FDT. Without these, U-Boot may produce `Overlap found` warnings and
  misprocess reserved-memory regions required by OpenAMP.
- Fail fast if `run px4` fails, so Linux is not booted with CR8 firmware load errors.
- `panic=1` — the yocto 6.10 kernel has `CONFIG_PROVE_LOCKING=y` (lockdep debug feature) that causes a deterministic NULL-ptr panic in `mm_core_init` on cold power-on due to uninitialized DRAM state. With `panic=1` the kernel auto-reboots 1 s after any panic, self-healing in ≤3 boot cycles. Root fix: rebuild kernel with `CONFIG_PROVE_LOCKING=n` (see Troubleshooting).
- `usbcore.autosuspend=-1` — disables USB auto-suspend globally, preventing mid-flight USB camera disconnects.

**Note:** `${ocaaddr}`, `${ocabin}`, `${codaddr}`, `${codbin}` referenced in `sd0load`
are pre-set in factory U-Boot environment (OpenCV accelerator and codec blobs). Do not
overwrite or clear them.
```bash
setenv bootargs 'rw rootwait earlycon root=/dev/mmcblk0p2 clk-ignore-unused panic=1 usbcore.autosuspend=-1'
setenv bootdelay 1
setenv bootm_size 0x20000000
setenv kernel_addr_r 0x48080000
setenv fdt_addr_r 0x57f00000
setenv fdt_high 0xffffffffffffffff
setenv initrd_high 0xffffffffffffffff
setenv sd0load 'mmc dev 0; mmc rescan; ext4load mmc 0:2 ${ocaaddr} boot/${ocabin}; ext4load mmc 0:2 ${codaddr} boot/${codbin}; ext4load mmc 0:2 ${kernel_addr_r} boot/Image; ext4load mmc 0:2 ${fdt_addr_r} boot/dtb/renesas/r9a09g057h4-rdk-ver1.dtb; run prodsd0bootargs'
setenv bootcmd_check 'run sd0load'
setenv bootimage 'booti ${kernel_addr_r} - ${fdt_addr_r}'
setenv bootcmd 'if run bootcmd_check; then echo sd0load_ok; else echo sd0load_retry; sleep 1; if run bootcmd_check; then echo sd0load_ok_retry; else echo sd0load_failed; reset; fi; fi; if run px4; then sleep 3; dcache flush; run bootimage; else echo px4_load_failed; reset; fi'
saveenv
```

Define `px4` command (copy-paste as single command):
```bash
setenv px4 'dcache off; \
mw.l 0x10420D24 0x04000000; \
mw.l 0x10420600 0xE000E000; \
mw.l 0x10420604 0x00030003; \
mw.l 0x10420908 0x1FFF0000; \
mw.l 0x10420C44 0x003F0000; \
mw.l 0x10420C14 0x00000000; \
mw.l 0x10420908 0x10001000; \
mw.l 0x10420C48 0x00000020; \
mw.l 0x10420908 0x1FFF1FFF; \
mw.l 0x10420C48 0x00000000; \
ext4load mmc 0:2 0x12040000 boot/cr8_data/rzv2h_px4_freertos_itcm.bin; \
ext4load mmc 0:2 0x41008000 boot/cr8_data/rzv2h_px4_freertos_sdram.bin; \
mw.l 0x41700000 0x00000000; \
mw.l 0x41710000 0x00000000; \
if ext4load mmc 0:3 0x41710008 cr8_data/params; then mw.l 0x41710000 ${filesize}; fi; \
mw.l 0x41720000 0x00000000; \
if ext4load mmc 0:3 0x41720008 cr8_data/etc/config.txt; then mw.l 0x41720000 ${filesize}; fi; \
mw.l 0x41730000 0x00000000; \
if ext4load mmc 0:3 0x41730008 cr8_data/etc/extras.txt; then mw.l 0x41730000 ${filesize}; fi; \
mw.l 0x10420C14 0x00000003; \
dcache on;'
```

Save and reboot:
```bash
saveenv
reset
```

### 4. Deploy Firmware

```bash
# Create directories on RDK (first-time only)
ssh root@<RDK_IP> "mkdir -p /boot/cr8_data /drone-data/cr8_data/etc /drone-data/cr8_data/log"

# Add drone-data partition to fstab so it auto-mounts on every boot
ssh root@<RDK_IP> "echo '/dev/mmcblk0p3  /drone-data  ext4  defaults,noatime,commit=5  0 2' >> /etc/fstab"

# Deploy CR8 firmware binaries to SD p2
scp Debug/rzv2h_px4_freertos_itcm.bin root@<RDK_IP>:/boot/cr8_data/
scp Debug/rzv2h_px4_freertos_sdram.bin root@<RDK_IP>:/boot/cr8_data/

# Deploy CA55 agent (if built)
cd ca55_stack/xrce_dds_agent && ./compile_agent.sh deploy-ca55
```

**Do not create `/drone-data/cr8_data/params` manually.** PX4 creates this file
automatically on first successful boot. Pre-creating it as an empty file will cause
U-Boot to fail loading it silently, which is harmless, but an empty file blocks PX4
from writing its parameter set on startup.

**Reboot** — board will now auto-start CR8 PX4 and CA55 Linux.

---

## Storage Layout

CR8 data split across SD partitions:

| Location | SD partition | Content | Updated |
|----------|-------------|---------|---------|
| `/boot/cr8_data/` | p2 (rootfs) | Firmware binaries | On rebuild |
| `/drone-data/cr8_data/` | p3 | Params, config, logs | QGC/flight |

**U-Boot partition refs:**
- `mmc 0:2` = SD p2 → firmware at `boot/cr8_data/`
- `mmc 0:3` = SD p3 → params/config/extras at `cr8_data/`

---

## Customize Boot Without Rebuilding

### Override params (config.txt)

Create on RDK:
```bash
ssh root@<RDK_IP> "mkdir -p /drone-data/cr8_data/etc"
cat > /tmp/config.txt << 'EOF'
param set MC_ROLLRATE_P 0.15
param set MPC_THR_HOVER 0.50
EOF
scp /tmp/config.txt root@<RDK_IP>:/drone-data/cr8_data/etc/config.txt
```

Reboot. BSON params from QGC override this file.

### Add optional modules (extras.txt)

```bash
cat > /tmp/extras.txt << 'EOF'
gyro_calibration start
EOF
scp /tmp/extras.txt root@<RDK_IP>:/drone-data/cr8_data/etc/extras.txt
```

Reboot.

---

## Advanced

### Detailed Toolchain Setup

**ARM GNU Toolchain 13.3:**

```bash
cd /tmp
wget https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz
sudo mkdir -p /opt/toolchains/gcc_arm/13_3-Rel1
sudo tar -xf arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz \
  -C /opt/toolchains/gcc_arm/13_3-Rel1 --strip-components=1
/opt/toolchains/gcc_arm/13_3-Rel1/bin/arm-none-eabi-gcc --version
```

**Poky SDK 3.1.31** (for CA55 agent build):
- Install to `/opt/toolchains/poky/3.1.31`
- Typically provided by Renesas or built from Yocto

### Docker Build (Optional)

```bash
docker compose build
docker compose up -d
docker compose exec dev bash
./compile.sh build
```

### VSCode Debug Setup

1. Install extensions: C/C++ Extension Pack, CMake Tools, Cortex Debug
2. Set `TOOLCHAIN_BASE_PATH` environment variable
3. Run CMake to generate `compile_commands.json`: `cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cross.cmake`
4. Press `F5` to start GDB debugging via J-Link

### Troubleshooting

**CMake can't find toolchain:**
```bash
echo $TOOLCHAIN_BASE_PATH
# Should show: /opt/toolchains/gcc_arm/13_3-Rel1
```

**Missing Python packages:**
```bash
pip3 install --user empy pyyaml
```

**Git submodule errors:**
```bash
git submodule deinit -f .
git submodule update --init --recursive
```

**ccache not working:**
```bash
ccache -C  # Clear
ccache -z  # Reset stats
```

**Kernel panics on cold power-on (hangs at "Starting kernel ..." or at `[0.000000]`):**

The yocto 6.10.14 kernel is built with `CONFIG_PROVE_LOCKING=y` (lockdep). On cold power-on, the lockdep class cache for `cpu_hotplug_lock` contains uninitialized DRAM data, causing a NULL-ptr dereference in `print_lockdep_cache` inside `mm_core_init`. The panic is identical every cold boot:
```
WARNING: bad unlock balance detected!
Kernel panic - not syncing: Attempted to kill the idle task!
```
With `panic=1` in `bootargs` the board self-recovers in ≤3 attempts.

**Root fix** — rebuild kernel without lockdep (add to your Yocto kernel `.cfg` fragment):
```
CONFIG_PROVE_LOCKING=n
CONFIG_LOCKDEP=n
CONFIG_DEBUG_LOCKDEP=n
```
These are debug-only features with no benefit in production flight firmware.

**USB camera: high streaming latency / xHCI DMA errors in dmesg:**
```
xhci-renesas-hcd 15860000.usb: WARN: HC couldn't access mem fast enough for slot 1 ep 2
xhci-renesas-hcd 15860000.usb: ERROR Transfer event TRB DMA ptr not part of current TD
```
Root cause: a Logitech camera in YUYV mode streams **18.4 MB/s** of uncompressed isochronous USB data (640×480 @ 30 fps × 2 bytes/px). The RZ/V2H xHCI controller shares the AXI bus with DRP-AI and the OMX encoder; when all three run concurrently the xHCI DMA starves, stalling isochronous transfers and causing TRB ring corruption.

Fix: the `CustomXRCEAgent` camera driver negotiates **MJPEG** format first (≈2–5 MB/s, 4–8× less bandwidth), falling back to YUYV only if the camera does not support MJPEG. Verify with:
```bash
v4l2-ctl --list-formats-ext -d /dev/video0   # confirm MJPEG listed
dmesg | grep -i "camera\|xhci"               # should see "Negotiated format: MxN MJPEG"
```

---

## References

- [HARDWARE.md](HARDWARE.md) — Pinout, wiring, BOM
- [PX4 Docs](https://docs.px4.io/)
- [Renesas RZ/V2H](https://www.renesas.com/en/products/rz-v2h)
- [ARM GNU Toolchain](https://developer.arm.com/documentation/)
