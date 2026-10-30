#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Inspect Pioneer XDJ two-component S-record update containers offline."""

from __future__ import annotations

import argparse
import binascii
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


MAX_IMAGE_BYTES = 64 * 1024 * 1024
PACKED_REGION_OFFSETS = (0x10000, 0x40000)
SEARCH_TERMS = (b"FLAC", b"flac", b"fLaC", b"ALAC", b"alac", b"Apple Lossless")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def decode_srecords(data: bytes) -> tuple[bytes, dict]:
    records: list[tuple[int, bytes]] = []
    types: Counter[str] = Counter()
    entry = None
    count_record = None
    lines = data.splitlines()

    for line_number, line in enumerate(lines, 1):
        if not re.fullmatch(rb"S[0-9][0-9A-Fa-f]+", line):
            raise ValueError(f"invalid S-record line {line_number}")
        record_type = chr(line[1])
        address_sizes = {
            "0": 2,
            "1": 2,
            "2": 3,
            "3": 4,
            "5": 2,
            "6": 3,
            "7": 4,
            "8": 3,
            "9": 2,
        }
        if record_type not in address_sizes:
            raise ValueError(f"unsupported S-record type S{record_type}")

        raw = bytes.fromhex(line[2:].decode("ascii"))
        count = raw[0]
        address_size = address_sizes[record_type]
        if count != len(raw) - 1 or count < address_size + 1:
            raise ValueError(f"S-record count mismatch on line {line_number}")
        if sum(raw) & 0xFF != 0xFF:
            raise ValueError(f"S-record checksum mismatch on line {line_number}")

        address = int.from_bytes(raw[1 : 1 + address_size], "big")
        payload = raw[1 + address_size : -1]
        types[record_type] += 1

        if record_type in "123":
            if not payload or address + len(payload) > MAX_IMAGE_BYTES:
                raise ValueError("S-record data outside analysis limit")
            records.append((address, payload))
        elif record_type in "789":
            if entry is not None or payload or line_number != len(lines):
                raise ValueError("invalid S-record termination")
            entry = address
        elif record_type in "56":
            if count_record is not None or payload:
                raise ValueError("invalid S-record count")
            count_record = address

    if not records or entry is None:
        raise ValueError("missing S-record data or termination")
    if count_record is not None and count_record != len(records):
        raise ValueError("incorrect S-record count")

    records.sort()
    ranges: list[list[int]] = []
    end = 0
    for address, payload in records:
        if address < end:
            raise ValueError("overlapping S-record data")
        if ranges and address == end:
            ranges[-1][1] = address + len(payload)
        else:
            ranges.append([address, address + len(payload)])
        end = address + len(payload)

    base = records[0][0]
    image = bytearray(b"\xff" * (end - base))
    for address, payload in records:
        offset = address - base
        image[offset : offset + len(payload)] = payload

    return bytes(image), {
        "base_address": base,
        "end_address_exclusive": end,
        "entry_address": entry,
        "data_bytes": sum(len(payload) for _, payload in records),
        "ranges": ranges,
        "record_types": dict(types),
    }


def split_container(data: bytes) -> tuple[list[int], int]:
    first, second, remainder = data.split(b"\r\n", 2)
    lengths = [int(first), int(second)]
    offset = len(data) - len(remainder)
    if offset + sum(lengths) != len(data):
        raise ValueError("container length mismatch")
    return lengths, offset


def decompress_lzss(source: bytes) -> bytes:
    """Decode the Okumura-style LZSS stream used by this firmware family."""
    ring = bytearray(b" " * (4096 - 18) + b"\0" * 18)
    cursor = 4096 - 18
    output = bytearray()
    position = 0

    while position < len(source):
        flags = source[position]
        position += 1
        if position == len(source):
            raise ValueError("LZSS flags without a token")
        for bit in range(8):
            if position == len(source):
                break
            if flags & (1 << bit):
                value = source[position]
                position += 1
                output.append(value)
                ring[cursor] = value
                cursor = (cursor + 1) % 4096
            else:
                if position + 2 > len(source):
                    raise ValueError("truncated LZSS back-reference")
                low, high = source[position : position + 2]
                position += 2
                source_position = low | ((high & 0xF0) << 4)
                length = (high & 0x0F) + 3
                for index in range(length):
                    value = ring[(source_position + index) % 4096]
                    output.append(value)
                    ring[cursor] = value
                    cursor = (cursor + 1) % 4096
            if len(output) > MAX_IMAGE_BYTES:
                raise ValueError("LZSS output outside analysis limit")
    return bytes(output)


def inspect_packed_region(image: bytes, offset: int) -> dict | None:
    if offset + 6 > len(image):
        return None
    packed_bytes = int.from_bytes(image[offset : offset + 4], "little")
    end = offset + 4 + packed_bytes
    if not packed_bytes or end + 2 > len(image):
        return None
    stored_checksum = int.from_bytes(image[end : end + 2], "little")
    computed_checksum = sum(image[offset:end]) & 0xFFFF
    if stored_checksum != computed_checksum:
        return None

    decoded = decompress_lzss(image[offset + 4 : end])
    term_hits = {
        term.decode("ascii"): [
            match.start() for match in re.finditer(re.escape(term), decoded)
        ]
        for term in SEARCH_TERMS
        if term in decoded
    }
    return {
        "image_offset": offset,
        "packed_bytes": packed_bytes,
        "checksum": stored_checksum,
        "unpacked_bytes": len(decoded),
        "unpacked_sha256": sha256(decoded),
        "search_term_offsets": term_hits,
    }


def inspect(path: Path) -> dict:
    data = path.read_bytes()
    lengths, offset = split_container(data)
    components = []

    for index, size in enumerate(lengths):
        component = data[offset : offset + size]
        label = component[:32].rstrip(b"\x00 ").decode("ascii")
        stored_crc = int.from_bytes(component[-2:], "little")
        computed_crc = binascii.crc_hqx(component[:-2], 0)
        if stored_crc != computed_crc:
            raise ValueError(f"component {index} CRC-16/XMODEM mismatch")
        image, record_info = decode_srecords(component[32:-2])
        packed_regions = [
            result
            for packed_offset in PACKED_REGION_OFFSETS
            if (result := inspect_packed_region(image, packed_offset)) is not None
        ]
        components.append(
            {
                "index": index,
                "label": label,
                "container_offset": offset,
                "container_bytes": size,
                "component_sha256": sha256(component),
                "crc16_xmodem": stored_crc,
                "image_bytes": len(image),
                "image_sha256": sha256(image),
                "packed_regions": packed_regions,
                **record_info,
            }
        )
        offset += size

    return {
        "file": path.name,
        "bytes": len(data),
        "sha256": sha256(data),
        "components": components,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("updates", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps([inspect(path) for path in args.updates], indent=2))


if __name__ == "__main__":
    main()
