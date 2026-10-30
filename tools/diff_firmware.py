#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Deep differential analysis between stock and modified XDJ-700 firmware update files."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
from pathlib import Path
import sys

from inspect_upd import decode_srecords, decompress_lzss, split_container


ROOT = Path(__file__).resolve().parents[1]
SECTOR_0_END_EXCLUSIVE = 0x40000  # 256 KiB
APPLICATION_FLASH_OFFSET = 0x40000
DISPATCH_ENTRY_OFFSET = 0xC01E78
TAIL_RUNTIME_ADDRESS = 0xA91BD788


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def diff_bytes(a: bytes, b: bytes) -> list[tuple[int, int, bytes, bytes]]:
    """Return contiguous differing byte ranges: [(offset, length, bytes_a, bytes_b)]."""
    diffs = []
    min_len = min(len(a), len(b))
    idx = 0
    while idx < min_len:
        if a[idx] != b[idx]:
            start = idx
            while idx < min_len and a[idx] != b[idx]:
                idx += 1
            diffs.append((start, idx - start, a[start:idx], b[start:idx]))
        else:
            idx += 1
    if len(a) != len(b):
        diffs.append((min_len, abs(len(a) - len(b)), a[min_len:], b[min_len:]))
    return diffs


def compare(stock_path: Path, modified_path: Path) -> dict:
    stock_data = stock_path.read_bytes()
    mod_data = modified_path.read_bytes()

    stock_lengths, stock_offset = split_container(stock_data)
    mod_lengths, mod_offset = split_container(mod_data)

    stock_c0 = stock_data[stock_offset : stock_offset + stock_lengths[0]]
    mod_c0 = mod_data[mod_offset : mod_offset + mod_lengths[0]]

    stock_c1 = stock_data[stock_offset + stock_lengths[0] : stock_offset + stock_lengths[0] + stock_lengths[1]]
    mod_c1 = mod_data[mod_offset + mod_lengths[0] : mod_offset + mod_lengths[0] + mod_lengths[1]]

    # 1. Container header diff
    container_header_stock = stock_data[:stock_offset]
    container_header_mod = mod_data[:mod_offset]

    # 2. Component 1 diff (PANL MCU)
    c1_identical = stock_c1 == mod_c1
    c1_crc_stock = int.from_bytes(stock_c1[-2:], "little")
    c1_crc_mod = int.from_bytes(mod_c1[-2:], "little")

    # 3. Component 0 diff (MAIN CPU)
    c0_header_identical = stock_c0[:32] == mod_c0[:32]
    c0_crc_stock = int.from_bytes(stock_c0[-2:], "little")
    c0_crc_mod = int.from_bytes(mod_c0[-2:], "little")

    stock_lines = stock_c0[32:-2].splitlines()
    mod_lines = mod_c0[32:-2].splitlines()

    differing_lines = []
    for line_idx in range(min(len(stock_lines), len(mod_lines))):
        if stock_lines[line_idx] != mod_lines[line_idx]:
            differing_lines.append(line_idx)

    # 4. S-record image decoding & Sector 0 analysis
    stock_image, _ = decode_srecords(stock_c0[32:-2])
    mod_image, _ = decode_srecords(mod_c0[32:-2])

    sector0_stock = stock_image[:SECTOR_0_END_EXCLUSIVE]
    sector0_mod = mod_image[:SECTOR_0_END_EXCLUSIVE]
    sector0_diffs = diff_bytes(sector0_stock, sector0_mod)
    sector0_identical = len(sector0_diffs) == 0

    # 5. Packed application analysis
    stock_packed_len = int.from_bytes(stock_image[APPLICATION_FLASH_OFFSET : APPLICATION_FLASH_OFFSET + 4], "little")
    mod_packed_len = int.from_bytes(mod_image[APPLICATION_FLASH_OFFSET : APPLICATION_FLASH_OFFSET + 4], "little")

    stock_lzss = stock_image[APPLICATION_FLASH_OFFSET + 4 : APPLICATION_FLASH_OFFSET + 4 + stock_packed_len]
    mod_lzss = mod_image[APPLICATION_FLASH_OFFSET + 4 : APPLICATION_FLASH_OFFSET + 4 + mod_packed_len]

    stock_csum = int.from_bytes(
        stock_image[APPLICATION_FLASH_OFFSET + 4 + stock_packed_len : APPLICATION_FLASH_OFFSET + 4 + stock_packed_len + 2],
        "little",
    )
    mod_csum = int.from_bytes(
        mod_image[APPLICATION_FLASH_OFFSET + 4 + mod_packed_len : APPLICATION_FLASH_OFFSET + 4 + mod_packed_len + 2],
        "little",
    )

    stock_app = decompress_lzss(stock_lzss)
    mod_app = decompress_lzss(mod_lzss)

    app_diffs = diff_bytes(stock_app, mod_app)
    app_diff_summary = []
    for off, length, b_stock, b_mod in app_diffs:
        runtime_addr = 0xA8000000 + off
        app_diff_summary.append({
            "application_offset": f"0x{off:X}",
            "runtime_address": f"0x{runtime_addr:X}",
            "bytes_modified": length,
            "stock_hex": b_stock[:16].hex() + ("..." if len(b_stock) > 16 else ""),
            "mod_hex": b_mod[:16].hex() + ("..." if len(b_mod) > 16 else ""),
        })

    return {
        "files": {
            "stock": {
                "path": str(stock_path),
                "bytes": len(stock_data),
                "sha256": sha256(stock_data),
            },
            "modified": {
                "path": str(modified_path),
                "bytes": len(mod_data),
                "sha256": sha256(mod_data),
            },
        },
        "container_diff": {
            "header_stock": container_header_stock.decode("ascii", errors="replace").strip(),
            "header_mod": container_header_mod.decode("ascii", errors="replace").strip(),
            "total_bytes_delta": len(mod_data) - len(stock_data),
        },
        "component_1_panel_mcu": {
            "byte_identical": c1_identical,
            "bytes": len(mod_c1),
            "crc16": c1_crc_mod,
            "safe": c1_identical,
        },
        "component_0_main_cpu": {
            "header_identical": c0_header_identical,
            "crc16_stock": c0_crc_stock,
            "crc16_mod": c0_crc_mod,
            "total_lines": len(mod_lines),
            "lines_changed": len(differing_lines),
            "first_line_changed": differing_lines[0] if differing_lines else None,
            "last_line_changed": differing_lines[-1] if differing_lines else None,
            "sector_0_bootloader": {
                "bytes_checked": SECTOR_0_END_EXCLUSIVE,
                "byte_identical": sector0_identical,
                "differing_byte_ranges": len(sector0_diffs),
                "emergency_updater_preserved": sector0_identical,
                "safe": sector0_identical,
            },
            "packed_application": {
                "stock_packed_bytes": stock_packed_len,
                "mod_packed_bytes": mod_packed_len,
                "packed_bytes_delta": mod_packed_len - stock_packed_len,
                "stock_checksum": stock_csum,
                "mod_checksum": mod_csum,
                "checksum_is_valid": mod_csum == (sum(mod_image[APPLICATION_FLASH_OFFSET : APPLICATION_FLASH_OFFSET + 4 + mod_packed_len]) & 0xFFFF),
                "stock_unpacked_bytes": len(stock_app),
                "mod_unpacked_bytes": len(mod_app),
                "unpacked_growth_bytes": len(mod_app) - len(stock_app),
                "app_diff_regions_count": len(app_diffs),
                "app_diffs": app_diff_summary,
            },
        },
        "safety_assessment": {
            "sector_0_untouched": sector0_identical,
            "panel_mcu_untouched": c1_identical,
            "tail_safely_appended": (len(mod_app) - len(stock_app)) > 0,
            "all_safety_invariants_held": (
                sector0_identical
                and c1_identical
                and (mod_csum == (sum(mod_image[APPLICATION_FLASH_OFFSET : APPLICATION_FLASH_OFFSET + 4 + mod_packed_len]) & 0xFFFF))
            ),
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Deep differential analysis of stock vs modified XDJ-700 firmware."
    )
    parser.add_argument("stock", type=Path, help="Pristine stock XDJ-700 firmware")
    parser.add_argument("modified", type=Path, help="Modified XDJ-700 firmware")
    args = parser.parse_args()

    report = compare(args.stock, args.modified)
    print(json.dumps(report, indent=2))
    if not report["safety_assessment"]["all_safety_invariants_held"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
