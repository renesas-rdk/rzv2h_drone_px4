#!/usr/bin/env python3
"""
Quick validation helper for Renesas RZ/V2H FreeRTOS builds.

Checks:
  - FreeRTOS heap reservation (HeapLimit - HeapBase matches configTOTAL_HEAP_SIZE)
  - IRQ/FIQ stack sizes (0x8000 / 0x4000 bytes)
  - Absence of legacy BSP heap symbol (g_heap)
  - BSS footprint and OpenAMP VRing size (from link map)
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Optional, Tuple


HEX_RE = re.compile(r"^([0-9a-fA-F]+)\s+\w\s+(\S+)$")
READELF_RE = re.compile(r"\s+\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+\S+\s+\S+\s+\S+\s+\d+\s+(\S+)")

# Expected memory reservations after relocating heap/stacks to DDR.
# Heap size is usually taken from configTOTAL_HEAP_SIZE.
FIQ_STACK_DEFAULT_BYTES = 0x00004000  # 16 KB
IRQ_STACK_DEFAULT_BYTES = 0x00008000  # 32 KB
DEFAULT_HEAP_BYTES = 0x00400000  # Fallback if config parsing fails
# Allowable slack for miscellaneous .bss outside the VRing + FreeRTOS heap.
OTHER_BSS_WARN_MAX = 4_000_000  # 4 MB headroom for drivers/globals


def run_command(cmd: list[str]) -> str:
    """Run a command and return stdout as text; raise with helpful message on failure."""
    try:
        completed = subprocess.run(
            cmd,
            check=True,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError as exc:
        raise RuntimeError(f"Command not found: {cmd[0]}") from exc
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            f"Command failed: {' '.join(cmd)}\nstdout:\n{exc.stdout}\nstderr:\n{exc.stderr}"
        ) from exc
    return completed.stdout


def parse_heap_bounds(nm_output: str) -> Tuple[Optional[int], Optional[int], bool]:
    heap_base = None
    heap_limit = None
    g_heap_present = False

    for line in nm_output.splitlines():
        match = HEX_RE.match(line.strip())
        if not match:
            continue
        value_str, symbol = match.groups()
        value = int(value_str, 16)

        if symbol == "__HeapBase":
            heap_base = value
        elif symbol == "__HeapLimit":
            heap_limit = value
        elif symbol == "g_heap":
            g_heap_present = True

    return heap_base, heap_limit, g_heap_present


def parse_stack_sizes(readelf_output: str) -> Tuple[Optional[int], Optional[int]]:
    irq_size = None
    fiq_size = None

    for line in readelf_output.splitlines():
        match = READELF_RE.match(line)
        if not match:
            continue
        _value_str, size_str, symbol = match.groups()
        if symbol == "g_irq_stack":
            irq_size = int(size_str, 10)
        elif symbol == "g_fiq_stack":
            fiq_size = int(size_str, 10)

    return irq_size, fiq_size


def parse_size_output(size_output: str) -> Optional[int]:
    lines = size_output.strip().splitlines()
    if not lines:
        return None
    last = lines[-1].split()
    if len(last) < 4:
        return None
    try:
        return int(last[2])
    except ValueError:
        return None


def parse_vring_size(map_path: Path) -> Optional[int]:
    if not map_path.exists():
        return None

    vring_pattern = re.compile(r"\.vring\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)")
    with map_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            match = vring_pattern.search(line)
            if match:
                return int(match.group(1), 16)
    return None
def parse_bsp_stack_sizes(cfg_path: Path) -> tuple[Optional[int], Optional[int]]:
    """Extract BSP stack sizes for IRQ/FIQ from bsp_cfg.h if available."""
    if not cfg_path.exists():
        return None, None

    patterns = {
        "BSP_CFG_STACK_IRQ_BYTES": None,
        "BSP_CFG_STACK_FIQ_BYTES": None,
    }

    macro_re = re.compile(r"#define\s+(BSP_CFG_STACK_(?:IRQ|FIQ)_BYTES)\s+\((0x[0-9A-Fa-f]+|\d+)\)")

    with cfg_path.open("r", encoding="utf-8") as cfg:
        for line in cfg:
            match = macro_re.search(line)
            if match:
                name, value = match.groups()
                try:
                    patterns[name] = int(value, 0)
                except ValueError:
                    patterns[name] = None

    return patterns["BSP_CFG_STACK_IRQ_BYTES"], patterns["BSP_CFG_STACK_FIQ_BYTES"]


def parse_config_total_heap(config_path: Path) -> Optional[int]:
    """Extract configTOTAL_HEAP_SIZE from the provided FreeRTOSConfig header."""
    if not config_path.exists():
        return None

    pattern = re.compile(r"#define\s+configTOTAL_HEAP_SIZE\s+\((0x[0-9A-Fa-f]+|\d+)\)")

    with config_path.open("r", encoding="utf-8") as cfg:
        for line in cfg:
            match = pattern.search(line)
            if match:
                value = match.group(1)
                try:
                    return int(value, 0)
                except ValueError:
                    return None
    return None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate FreeRTOS heap/stack/linker constraints."
    )
    parser.add_argument("--elf", required=True, type=Path, help="Path to ELF (rzv2h_px4_freertos.elf)")
    parser.add_argument("--nm", default="arm-none-eabi-nm", help="nm tool (default: %(default)s)")
    parser.add_argument("--size", default="arm-none-eabi-size", help="size tool (default: %(default)s)")
    parser.add_argument(
        "--readelf", default="arm-none-eabi-readelf", help="readelf tool (default: %(default)s)"
    )
    parser.add_argument(
        "--map",
        default="Debug/rzv2h_px4_freertos.map",
        type=Path,
        help="Optional map file for VRing lookup (default: %(default)s)",
    )
    parser.add_argument(
        "--heap-size",
        type=lambda v: int(v, 0),
        help="Expected FreeRTOS heap size in bytes (overrides configTOTAL_HEAP_SIZE)",
    )
    parser.add_argument(
        "--quiet", action="store_true", help="Only exit with status code, suppress summary output"
    )
    args = parser.parse_args()

    elf_path: Path = args.elf
    if not elf_path.exists():
        print(f"[ERROR] ELF not found: {elf_path}", file=sys.stderr)
        return 2

    heap_expected_bytes: int
    if args.heap_size is not None:
        heap_expected_bytes = args.heap_size
    else:
        cfg_path = Path("rzv_cfg/aws/FreeRTOSConfig.h")
        heap_expected_bytes = parse_config_total_heap(cfg_path) or DEFAULT_HEAP_BYTES

    irq_expected = IRQ_STACK_DEFAULT_BYTES
    fiq_expected = FIQ_STACK_DEFAULT_BYTES

    irq_cfg, fiq_cfg = parse_bsp_stack_sizes(Path("rzv_cfg/fsp_cfg/bsp/bsp_cfg.h"))
    if irq_cfg:
        irq_expected = irq_cfg
    if fiq_cfg:
        fiq_expected = fiq_cfg

    status_ok = True
    messages: list[str] = []

    # Heap bounds & g_heap presence
    nm_output = run_command([args.nm, "--defined-only", str(elf_path)])
    heap_base, heap_limit, g_heap_present = parse_heap_bounds(nm_output)

    if heap_base is None or heap_limit is None:
        messages.append("Heap span FAIL: missing __HeapBase or __HeapLimit")
        status_ok = False
    else:
        span = heap_limit - heap_base
        if span == heap_expected_bytes:
            messages.append(
                f"Heap span OK (0x{span:08X}, base=0x{heap_base:08X}, limit=0x{heap_limit:08X})"
            )
        else:
            messages.append(
                f"Heap span FAIL (0x{span:08X}, expected 0x{heap_expected_bytes:08X}, "
                f"base=0x{heap_base:08X}, limit=0x{heap_limit:08X})"
            )
            status_ok = False

    if g_heap_present:
        messages.append("BSP heap symbol: present (OK)")
    else:
        messages.append("BSP heap symbol: not found")

    # Stack sizes
    readelf_output = run_command([args.readelf, "-s", str(elf_path)])
    irq_size, fiq_size = parse_stack_sizes(readelf_output)

    if irq_size == irq_expected:
        messages.append(f"IRQ stack size OK (0x{irq_expected:08X})")
    else:
        observed = f"{irq_size} bytes" if irq_size is not None else "missing"
        messages.append(
            f"IRQ stack size FAIL ({observed}, expected {irq_expected})"
        )
        status_ok = False

    if fiq_size == fiq_expected:
        messages.append(f"FIQ stack size OK (0x{fiq_expected:08X})")
    else:
        observed = f"{fiq_size} bytes" if fiq_size is not None else "missing"
        messages.append(
            f"FIQ stack size FAIL ({observed}, expected {fiq_expected})"
        )
        status_ok = False

    # BSS footprint
    size_output = run_command([args.size, str(elf_path)])
    bss_total = parse_size_output(size_output)
    if bss_total is None:
        messages.append("[WARN] Could not parse BSS size from size output")
    else:
        vring_size = parse_vring_size(args.map)
        if vring_size is None:
            messages.append(f"BSS total: {bss_total/1024/1024:.2f} MB (VRing size unavailable)")
        else:
            other_bss = bss_total - vring_size - heap_expected_bytes
            messages.append(
                "BSS total: "
                f"{bss_total/1024/1024:.2f} MB "
                f"(VRing {vring_size/1024/1024:.2f} MB, "
                f"FreeRTOS heap {heap_expected_bytes/1024/1024:.2f} MB, "
                f"other {other_bss/1024/1024:.2f} MB)"
            )
            # Warn if miscellaneous BSS seems excessive (might indicate leaks)
            if other_bss < 0 or other_bss > OTHER_BSS_WARN_MAX:
                messages.append(
                    "BSS other WARN: "
                    f"{other_bss/1024/1024:.2f} MB outside 0–{OTHER_BSS_WARN_MAX/1024/1024:.0f} MB window – verify allocations"
                )

    if not args.quiet:
        print("\n".join(messages))

    return 0 if status_ok else 1


if __name__ == "__main__":
    sys.exit(main())
