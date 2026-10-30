#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Extract a player's unpacked application image from its official update file.

Decodes component 0's S-records, finds the packed regions (4-byte length,
Okumura LZSS stream, 16-bit additive checksum) and writes the largest one,
the SH-4 application, decompressed. That image is what the disassembly and
the call-graph tools work on; it is Pioneer's code and stays local
(build/, never committed).

Example (XDJ-700 1.15; the stock application's SHA-256 is
1875381b56d065a2b0a97a63b64ead5ce71397c521b7a62713c5bb4a0e055939):

    python3 tools/extract_application.py firmware/original/XDJ700.UPD build/app115.bin
    sh-elf-objdump -D -b binary -m sh4a -EL --adjust-vma=0x08000000 \\
        build/app115.bin > build/app115.dis
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from inspect_upd import (  # noqa: E402
    PACKED_REGION_OFFSETS,
    decode_srecords,
    decompress_lzss,
    inspect_packed_region,
    split_container,
)


def extract(update: Path, offset: int | None = None) -> tuple[bytes, dict]:
    data = update.read_bytes()
    lengths, start = split_container(data)
    component = data[start : start + lengths[0]]
    if int.from_bytes(component[-2:], "little") != binascii.crc_hqx(component[:-2], 0):
        raise ValueError("component 0 CRC-16/XMODEM mismatch")
    image, _ = decode_srecords(component[32:-2])
    offsets = [offset] if offset is not None else list(PACKED_REGION_OFFSETS)
    regions = [region for candidate in offsets if (region := inspect_packed_region(image, candidate)) is not None]
    if not regions:
        raise ValueError("no packed region found")
    region = max(regions, key=lambda item: item["unpacked_bytes"])
    begin = region["image_offset"] + 4
    application = decompress_lzss(image[begin : begin + region["packed_bytes"]])
    info = {
        "update": str(update),
        "update_sha256": hashlib.sha256(data).hexdigest(),
        "label": component[:32].rstrip(b"\x00 ").decode("ascii", "replace"),
        "packed_region_offset": region["image_offset"],
        "packed_bytes": region["packed_bytes"],
        "application_bytes": len(application),
        "application_sha256": hashlib.sha256(application).hexdigest(),
    }
    return application, info


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("update", type=Path, help="Official update file (.UPD)")
    parser.add_argument("output", type=Path, help="Where to write the unpacked application")
    parser.add_argument("--offset", type=lambda text: int(text, 0), help="Packed region offset in the image")
    args = parser.parse_args()
    application, info = extract(args.update, args.offset)
    args.output.write_bytes(application)
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    main()
