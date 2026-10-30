#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Hash-locked evidence about stock firmware code.

The tools check that a stock image holds the code they rely on without
carrying that code. Each piece of evidence is an offset into the unpacked
application, a window length of at least 32 bytes and the SHA-256 of that
window. A window that long cannot be recovered from its digest, so the
repository holds no Pioneer machine code, only where it is and a fingerprint.
Addresses, names and strings the docs quote stay readable.

To make or refresh a table from your own unpacked application
(tools/extract_application.py):

    python3 tools/firmware_evidence.py build/analysis/app115.bin 0xC01E78 0xBF6886

It prints one entry per offset, ready to paste.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

MIN_WINDOW_BYTES = 32


def region_digest(image: bytes, offset: int, length: int = MIN_WINDOW_BYTES) -> str:
    if length < MIN_WINDOW_BYTES:
        raise ValueError(f"evidence windows must be at least {MIN_WINDOW_BYTES} bytes")
    if offset < 0 or offset + length > len(image):
        raise ValueError(f"evidence window at 0x{offset:x} is outside the image")
    return hashlib.sha256(image[offset : offset + length]).hexdigest()


def region_matches(image: bytes, offset: int, length: int, digest: str) -> bool:
    return offset + length <= len(image) and region_digest(image, offset, length) == digest


def verify_regions(image: bytes, regions: dict[int, tuple[int, str]], label: str) -> None:
    """Raise ValueError unless every (offset: (length, digest)) window matches."""
    for offset, (length, digest) in regions.items():
        if not region_matches(image, offset, length, digest):
            raise ValueError(f"{label} mismatch at 0x{offset:x}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("application", type=Path, help="unpacked application image")
    parser.add_argument("offsets", nargs="+", type=lambda text: int(text, 0))
    parser.add_argument("--length", type=int, default=MIN_WINDOW_BYTES)
    args = parser.parse_args()
    image = args.application.read_bytes()
    for offset in args.offsets:
        digest = region_digest(image, offset, args.length)
        print(f'    0x{offset:X}: ({args.length}, "{digest}"),')


if __name__ == "__main__":
    main()
