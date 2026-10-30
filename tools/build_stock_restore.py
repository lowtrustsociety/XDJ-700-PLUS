#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Build a stock XDJ-700 1.15 restore container with a spoofed version tag.

The player always reports the installed version as 1.15 (the string at
application offset 0x728, which no build modifies), and the updater only
writes flash when the USB header tag is higher. Any tag above 1.15 therefore
forces a full rewrite. Only the Component 0 header tag and CRC-16 change;
every S-record and all of Component 1 stay byte-identical to the official file.
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
from pathlib import Path

from inspect_upd import decode_srecords, split_container
from roundtrip_upd import XDJ700_V115_SHA256


ROOT = Path(__file__).resolve().parents[1]
STOCK_VERSION_TAG = b"MAINVer1.15\x00"


def build_restore(stock: bytes, version: str) -> bytes:
    if hashlib.sha256(stock).hexdigest() != XDJ700_V115_SHA256:
        raise ValueError("input firmware is not pristine official XDJ-700 1.15")
    if float(version) <= 1.15:
        raise ValueError("version tag must be above 1.15 or the updater skips the write")

    lengths, offset = split_container(stock)
    c0 = stock[offset : offset + lengths[0]]
    c1 = stock[offset + lengths[0] : offset + lengths[0] + lengths[1]]
    header = c0[:32].replace(STOCK_VERSION_TAG, f"MAINVer{version}\x00".encode("ascii"))
    if len(header) != 32 or header == c0[:32]:
        raise ValueError("version tag replacement failed")

    body = header + c0[32:-2]
    new_c0 = body + binascii.crc_hqx(body, 0).to_bytes(2, "little")
    restore = stock[:offset] + new_c0 + c1

    if len(restore) != len(stock):
        raise ValueError("restore container size changed")
    if decode_srecords(new_c0[32:-2])[0] != decode_srecords(c0[32:-2])[0]:
        raise ValueError("CRITICAL: flash image differs from stock")
    if restore[offset + lengths[0] :] != c1:
        raise ValueError("CRITICAL: Component 1 (PANL) was modified")
    return restore


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--firmware",
        type=Path,
        default=ROOT / "firmware" / "original" / "XDJ700.UPD",
    )
    parser.add_argument("--version", default="1.17", help="Version tag (default: 1.17)")
    parser.add_argument("--output", type=Path, default=None)
    args = parser.parse_args()

    restore = build_restore(args.firmware.read_bytes(), args.version)
    output = args.output or ROOT / "build" / f"XDJ700_RESTORE_STOCK_{args.version}.UPD"
    output.write_bytes(restore)
    print(json.dumps({
        "version": args.version,
        "output_file": str(output),
        "sha256": hashlib.sha256(restore).hexdigest(),
    }, indent=2))


if __name__ == "__main__":
    main()
