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

- `/drone-data/cr8_data/params` — saved PX4 parameters
- `/drone-data/cr8_data/etc/config.txt` — optional parameter overrides
- `/drone-data/cr8_data/etc/extras.txt` — optional extra startup commands
- `/drone-data/cr8_data/log/` — flight log output

If you want to reset runtime state, remove only the specific file or directory you no longer need.

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
