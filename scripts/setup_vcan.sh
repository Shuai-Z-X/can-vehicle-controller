#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:-vcan0}"

sudo modprobe vcan

if ip link show "$IFACE" >/dev/null 2>&1; then
    sudo ip link set "$IFACE" up
else
    sudo ip link add dev "$IFACE" type vcan
    sudo ip link set "$IFACE" up
fi

ip -details link show "$IFACE"
