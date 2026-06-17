# Storage

This project splits firmware, configuration, and logs across SD card paths used by the CR8 firmware and the CA55 Linux stack.

---

## SD Card Layout

The important runtime paths are:

| Path | Purpose | Updated by |
|------|---------|------------|
| `/boot/cr8_data/` | CR8 firmware binaries loaded by U-Boot | `./compile.sh build` / `./compile.sh deploy-cr8` |
| `/drone-data/cr8_data/` | Persistent PX4 data on the SD card | QGC, PX4 runtime, scripts |
| `/drone-data/cr8_data/etc/` | Config files and startup extras | Manual edits or deployment scripts |
| `/drone-data/cr8_data/log/` | PX4 flight logs (`.ulg`) | PX4 logger |

The boot partition is used for firmware images, while the data partition holds mutable state.

---

## Firmware Paths

### CR8 firmware

The build outputs for the CR8 target are copied to:

- `/boot/cr8_data/rzv2h_px4_freertos_itcm.bin`
- `/boot/cr8_data/rzv2h_px4_freertos_sdram.bin`

These are the images loaded by the boot flow before PX4 starts.

### CA55 agent

The CA55 XRCE-DDS agent is deployed separately and normally lives at:

- `/usr/bin/CustomXRCEAgent`

It is not stored under the CR8 data partition.

---

## Runtime Data

PX4 reads persistent files from `/drone-data/cr8_data/`.

Common files are:

- `/drone-data/cr8_data/params` — saved PX4 parameters (BSON format, created automatically by PX4 on first successful boot — **do not pre-create this file**)
- `/drone-data/cr8_data/etc/config.txt` — optional parameter overrides
- `/drone-data/cr8_data/etc/extras.txt` — optional extra startup commands
- `/drone-data/cr8_data/log/` — flight log output

If you want to reset runtime state, remove only the specific file or directory you no longer need.

**`/drone-data` lives on partition 3 (p3), which you must create yourself.** The stock
Yocto image ships only p1 (boot) + p2 (rootfs) — `/dev/mmcblk0p3` does **not** exist on a
fresh card. Run this **portable** snippet once on the running board *before* mounting it.
It auto-detects the boot disk (`mmcblk0` / `sda` / `nvme0n1`, MBR or GPT), is idempotent
(safe to re-run), formats p3, writes the fstab entry, and mounts it:
```bash
set -e
# 1) Detect the disk holding the running rootfs (no hard-coded device name)
rootpart=$(findmnt -no SOURCE /)
disk=/dev/$(lsblk -no PKNAME "$rootpart")
# 2) Create p3 in the free space after p2 — only if it does not exist yet
if ! { [ -b "${disk}p3" ] || [ -b "${disk}3" ]; }; then
    echo ',,L' | sfdisk --append "$disk"          # default start=after p2, size=rest
    partprobe "$disk"; udevadm settle 2>/dev/null || true; sync
fi
# 3) Resolve p3 node and format ext4 only if it has no filesystem yet
for d in "${disk}p3" "${disk}3"; do [ -b "$d" ] && p3="$d"; done
blkid "$p3" >/dev/null 2>&1 || mkfs.ext4 -F -L drone-data "$p3"
# 4) Persist mount by LABEL + nofail (a missing card can never block boot)
mkdir -p /drone-data
grep -q ' /drone-data ' /etc/fstab || \
  echo 'LABEL=drone-data  /drone-data  ext4  defaults,noatime,nofail,commit=5  0 2' >> /etc/fstab
mount /drone-data
```

The resulting fstab line mounts **by LABEL** (not `/dev/mmcblk0p3`) so it survives
device-name changes across boards/readers, and `nofail` guarantees a missing/absent p3
never blocks boot (otherwise the system drops to an emergency shell):
```
LABEL=drone-data  /drone-data  ext4  defaults,noatime,nofail,commit=5  0 2
```

---

## U-Boot Partition References

The boot flow on this board expects the SD card to be exposed through U-Boot `mmc` partitions:

- `mmc 0:2` maps to the boot/rootfs area used for firmware under `/boot/cr8_data/`
- `mmc 0:3` maps to the data partition used for `/drone-data/cr8_data/`

That mapping is reflected in the board setup and boot scripts.

---

## Deployment Checklist

When updating firmware:

1. Build the CR8 firmware.
2. Copy the new `.bin` files into `/boot/cr8_data/`.
3. Reboot the board.
4. Check logs in `/drone-data/cr8_data/log/` if the boot or flight behavior changed.

---

## Related Files

- [README.md](../README.md)
- [SETUP.md](SETUP.md)
- [debugging.md](debugging.md)
