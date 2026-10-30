#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Recompress the stock XDJ-700 application and verify lossless roundtrip."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from inspect_upd import decode_srecords, decompress_lzss, split_container
from roundtrip_upd import XDJ700_V115_SHA256


APPLICATION_OFFSET = 0x40000
ROOT = Path(__file__).resolve().parents[1]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stock_application(update: bytes) -> tuple[bytes, bytes, dict]:
    if sha256(update) != XDJ700_V115_SHA256:
        raise ValueError("only pristine official XDJ-700 1.15 firmware is accepted")
    lengths, offset = split_container(update)
    main_component = update[offset : offset + lengths[0]]
    main_image, _ = decode_srecords(main_component[32:-2])

    packed_bytes = int.from_bytes(
        main_image[APPLICATION_OFFSET : APPLICATION_OFFSET + 4], "little"
    )
    packed_start = APPLICATION_OFFSET + 4
    packed_end = packed_start + packed_bytes
    if packed_end + 2 > len(main_image):
        raise ValueError("packed application exceeds MAIN image")
    stored_checksum = int.from_bytes(main_image[packed_end : packed_end + 2], "little")
    computed_checksum = sum(main_image[APPLICATION_OFFSET:packed_end]) & 0xFFFF
    if stored_checksum != computed_checksum:
        raise ValueError("packed application checksum mismatch")

    packed = main_image[packed_start:packed_end]
    application = decompress_lzss(packed)
    return packed, application, {
        "main_image_bytes": len(main_image),
        "application_offset": APPLICATION_OFFSET,
        "stock_packed_bytes": packed_bytes,
        "stock_packed_sha256": sha256(packed),
        "application_bytes": len(application),
        "application_sha256": sha256(application),
        "bytes_after_stock_block": len(main_image) - (packed_end + 2),
    }


def build_encoder(folder: Path):
    library = folder / "lzss-encoder.dylib"
    subprocess.run(
        [
            "cc",
            "-std=c99",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-shared",
            "-fPIC",
            str(ROOT / "tools" / "lzss_encode.c"),
            "-o",
            str(library),
        ],
        check=True,
        capture_output=True,
    )
    handle = ctypes.CDLL(str(library))
    function = handle.xdj_lzss_encode
    function.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    function.restype = ctypes.c_size_t

    def compress(data: bytes) -> bytes:
        source = ctypes.create_string_buffer(data)
        capacity = len(data) + len(data) // 8 + 1
        output = ctypes.create_string_buffer(capacity)
        count = function(source, len(data), output, capacity)
        if data and not count:
            raise ValueError("LZSS compression failed")
        return output.raw[:count]

    return compress


def check(path: Path) -> dict:
    stock_packed, application, report = stock_application(path.read_bytes())
    with tempfile.TemporaryDirectory(prefix="xdj700-lzss-") as folder:
        recompressed = build_encoder(Path(folder))(application)
    decoded = decompress_lzss(recompressed)
    if decoded != application:
        raise ValueError("recompressed application does not decode byte-identically")

    report.update(
        {
            "recompressed_bytes": len(recompressed),
            "recompressed_sha256": sha256(recompressed),
            "decoded_roundtrip_sha256": sha256(decoded),
            "decoded_roundtrip_byte_identical": True,
            "compressed_byte_identical": recompressed == stock_packed,
            "fits_stock_packed_span": len(recompressed) <= len(stock_packed),
            "packed_headroom_bytes": len(stock_packed) - len(recompressed),
            "output_written": False,
            "device_access": False,
        }
    )
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("update", type=Path)
    args = parser.parse_args()
    print(json.dumps(check(args.update), indent=2))


if __name__ == "__main__":
    main()
