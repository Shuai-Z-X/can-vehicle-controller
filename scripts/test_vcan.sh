#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:-vcan0}"

FRAMES=(
    "100#027B009209000505"
    "100#0300009209010621"
    "101#02F4017D005507D4"
    "200#022C010000000934"
)

for frame in "${FRAMES[@]}"; do
    echo "sending $frame"
    cansend "$IFACE" "$frame"
    sleep 0.2
done
