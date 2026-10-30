#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Re-serialize pristine XDJ-700 1.15 firmware and require byte identity."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
from pathlib import Path
import re

from inspect_upd import decode_srecords, split_container


XDJ700_V115_SHA256 = "73edec9802da51672257c2599efc04209dc92478fcbaa1a0425b3b122e33f99c"
ADDRESS_BYTES = {
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


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def encode_record(record_type: str, address: int, payload: bytes) -> bytes:
    if record_type not in ADDRESS_BYTES:
        raise ValueError(f"unsupported S-record type S{record_type}")
    address_bytes = ADDRESS_BYTES[record_type]
    if address < 0 or address >= 1 << (8 * address_bytes):
        raise ValueError("S-record address outside type range")
    body = address.to_bytes(address_bytes, "big") + payload
    if len(body) > 254:
        raise ValueError("S-record body is too long")
    raw = bytes([len(body) + 1]) + body
    checksum = 0xFF - (sum(raw) & 0xFF)
    return (
        b"S"
        + record_type.encode("ascii")
        + (raw + bytes([checksum])).hex().upper().encode("ascii")
        + b"\r\n"
    )


def decode_record(line: bytes) -> tuple[str, int, bytes]:
    if not re.fullmatch(rb"S[0-9][0-9A-Fa-f]+", line):
        raise ValueError("invalid S-record line")
    record_type = chr(line[1])
    if record_type not in ADDRESS_BYTES:
        raise ValueError(f"unsupported S-record type S{record_type}")
    raw = bytes.fromhex(line[2:].decode("ascii"))
    address_bytes = ADDRESS_BYTES[record_type]
    if raw[0] != len(raw) - 1 or len(raw) < address_bytes + 2:
        raise ValueError("S-record count mismatch")
    if sum(raw) & 0xFF != 0xFF:
        raise ValueError("S-record checksum mismatch")
    address = int.from_bytes(raw[1 : 1 + address_bytes], "big")
    payload = raw[1 + address_bytes : -1]
    return record_type, address, payload


def split_components(data: bytes) -> list[bytes]:
    lengths, offset = split_container(data)
    components = []
    for length in lengths:
        if length < 34:
            raise ValueError("component is too short")
        components.append(data[offset : offset + length])
        offset += length
    return components


def rebuild_component(component: bytes) -> bytes:
    stored_crc = int.from_bytes(component[-2:], "little")
    computed_crc = binascii.crc_hqx(component[:-2], 0)
    if stored_crc != computed_crc:
        raise ValueError("component CRC-16/XMODEM mismatch")

    original_stream = component[32:-2]
    original_image, original_info = decode_srecords(original_stream)
    encoded_records = []
    for line in original_stream.splitlines():
        encoded_records.append(encode_record(*decode_record(line)))

    rebuilt_without_crc = component[:32] + b"".join(encoded_records)
    rebuilt = rebuilt_without_crc + binascii.crc_hqx(
        rebuilt_without_crc, 0
    ).to_bytes(2, "little")
    rebuilt_image, rebuilt_info = decode_srecords(rebuilt[32:-2])

    if rebuilt_image != original_image or rebuilt_info != original_info:
        raise ValueError("component meaning changed during reconstruction")
    if rebuilt != component:
        raise ValueError("component reconstruction is not byte-identical")
    return rebuilt


def rebuild(data: bytes) -> tuple[bytes, dict]:
    original_sha = sha256(data)
    if original_sha != XDJ700_V115_SHA256:
        raise ValueError("only pristine official XDJ-700 1.15 firmware is accepted")

    components = [rebuild_component(component) for component in split_components(data)]
    rebuilt = (
        b"".join(str(len(component)).encode("ascii") + b"\r\n" for component in components)
        + b"".join(components)
    )
    if rebuilt != data:
        raise ValueError("container reconstruction is not byte-identical")

    return rebuilt, {
        "model": "XDJ-700",
        "version": "1.15",
        "original_sha256": original_sha,
        "roundtrip_sha256": sha256(rebuilt),
        "bytes": len(rebuilt),
        "component_bytes": [len(component) for component in components],
        "byte_identical": True,
        "modified_firmware": False,
        "device_access": False,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("update", type=Path)
    args = parser.parse_args()
    _, report = rebuild(args.update.read_bytes())
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
