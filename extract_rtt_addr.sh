#!/bin/bash
# Copyright (c) 2025 Renesas Electronics Corporation and/or its affiliates
#
# SPDX-License-Identifier: BSD-3-Clause


# Script to extract SEGGER_RTT address from map file
# Usage: extract_rtt_addr.sh <map_file> <output_file>

MAP_FILE="$1"
OUTPUT_FILE="$2"

if [ $# -ne 2 ]; then
    echo "Usage: $0 <map_file> <output_file>"
    exit 1
fi

if [ ! -f "$MAP_FILE" ]; then
    echo "Error: Map file '$MAP_FILE' not found"
    exit 1
fi

# Extract SEGGER_RTT address using grep and awk
SEGGER_ADDR=$(grep '^[[:space:]]*0x[0-9a-fA-F]*[[:space:]]*_SEGGER_RTT$' "$MAP_FILE" | awk '{print $1}' | head -1)

if [ -n "$SEGGER_ADDR" ]; then
    echo "SEGGER_RTT Address: $SEGGER_ADDR"
    echo "$SEGGER_ADDR" > "$OUTPUT_FILE"
    echo "Address written to $OUTPUT_FILE"
    exit 0
else
    echo "Warning: SEGGER_RTT symbol not found in map file"
    exit 1
fi
