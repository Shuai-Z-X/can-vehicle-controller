#!/usr/bin/env python3
"""Validate the CAN matrix and the golden test vectors.

Run this from the repository root:

    python tools/check_can_db.py

Install the dependency first:

    python -m pip install cantools
"""

from __future__ import annotations

import json
from pathlib import Path

try:
    import cantools
except ImportError as exc:  # pragma: no cover - handled for a friendly message
    raise SystemExit(
        "cantools is required. Install it with: python -m pip install cantools"
    ) from exc


ROOT = Path(__file__).resolve().parents[1]
DBC_PATH = ROOT / "protocols" / "can_matrix.dbc"
VECTORS_PATH = ROOT / "protocols" / "test_vectors.json"


def crc8(data: bytes) -> int:
    """CRC-8 with polynomial 0x07, init 0x00, no reflection, xorout 0x00."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def load_database() -> cantools.database.Database:
    if not DBC_PATH.exists():
        raise SystemExit(f"DBC file not found: {DBC_PATH}")
    return cantools.database.load_file(DBC_PATH)


def check_vectors(db: cantools.database.Database) -> None:
    vectors = json.loads(VECTORS_PATH.read_text(encoding="utf-8"))
    frames = vectors["frames"]
    passed = 0

    for item in frames:
        message = db.get_message_by_name(item["name"])
        frame_id = int(item["id"], 16)
        if message.frame_id != frame_id:
            raise AssertionError(
                f"{item['name']}: DBC ID 0x{message.frame_id:03X} "
                f"does not match vector 0x{frame_id:03X}"
            )

        data = bytes.fromhex(item["data"].replace(" ", ""))
        if len(data) != 8:
            raise AssertionError(f"{item['name']}: DLC must be 8")
        expected_crc = data[7]
        actual_crc = crc8(data[:7])
        if actual_crc != expected_crc:
            raise AssertionError(
                f"{item['name']}: CRC mismatch, "
                f"expected 0x{expected_crc:02X}, calculated 0x{actual_crc:02X}"
            )

        decoded = message.decode(data, decode_choices=False)
        for signal_name, expected in item["decoded"].items():
            actual = decoded[signal_name]
            if isinstance(expected, float):
                if abs(actual - expected) > 1e-6:
                    raise AssertionError(
                        f"{item['name']}.{signal_name}: "
                        f"{actual} != {expected}"
                    )
            elif actual != expected:
                raise AssertionError(
                    f"{item['name']}.{signal_name}: {actual} != {expected}"
                )

        print(f"OK 0x{frame_id:03X} {item['name']}")
        passed += 1

    print(f"all {passed} vectors passed")


def main() -> None:
    db = load_database()
    print(f"messages: {len(db.messages)}")
    for message in db.messages:
        print(f"0x{message.frame_id:03X} {message.name} dlc={message.length}")
    check_vectors(db)


if __name__ == "__main__":
    main()
