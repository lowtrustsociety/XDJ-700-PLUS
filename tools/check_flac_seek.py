#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Check the bounded FLAC seek on the host against awkward files.

Builds src/flac_seek_runner.c with the decoder unit, generates FLAC
files that stress the seek (seek points every 10 s, no seek table,
placeholder and sparse seek points, tiny and huge blocks, a large embedded
picture, a tiny file, and a stream with variable block sizes), and checks
that every seek is sample-exact, never falls back to dr_flac's own seek, and
stays within a bounded number of reads.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import struct
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
RUNNER_SOURCE = ROOT / "src" / "flac_seek_runner.c"
DECODER_SOURCE = ROOT / "src" / "flac_decoder.c"

# A seek may read at most this many 4 KB blocks (about 256 KB); dr_flac's own
# seek needed up to 174 on a 90 s file and far more on long tracks.
MAX_READS_PER_SEEK = 64
RANDOM_TARGETS = 300


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=True, capture_output=True, text=True)


def crc8(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crc16(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def utf8_number(data: bytes, pos: int) -> tuple[int, int]:
    """Decode a FLAC UTF-8 coded number; returns (value, length)."""
    first = data[pos]
    if first < 0x80:
        return first, 1
    length = 2
    mask = 0x1F
    while first & (0x80 >> length):
        length += 1
        mask >>= 1
    value = first & mask
    for index in range(1, length):
        value = (value << 6) | (data[pos + index] & 0x3F)
    return value, length


def encode_utf8_number(value: int) -> bytes:
    if value < 0x80:
        return bytes([value])
    for length in range(2, 8):
        if value < 1 << (5 * length + 1):
            break
    out = []
    for _ in range(length - 1):
        out.append(0x80 | (value & 0x3F))
        value >>= 6
    lead = (0xFF << (8 - length)) & 0xFF
    out.append(lead | value)
    return bytes(reversed(out))


def header_length(data: bytes, pos: int) -> int:
    """Length of the frame header at pos, including its CRC-8."""
    block_code = data[pos + 2] >> 4
    rate_code = data[pos + 2] & 0x0F
    _, number_length = utf8_number(data, pos + 4)
    length = 4 + number_length
    length += {6: 1, 7: 2}.get(block_code, 0)
    length += {12: 1, 13: 2, 14: 2}.get(rate_code, 0)
    return length + 1


def split_frames(path: Path) -> tuple[bytes, list[bytes]]:
    """STREAMINFO body and the list of frames of a fixed-blocking FLAC file."""
    data = path.read_bytes()
    assert data[:4] == b"fLaC"
    pos = 4
    streaminfo = b""
    while True:
        last = data[pos] & 0x80
        kind = data[pos] & 0x7F
        size = int.from_bytes(data[pos + 1:pos + 4], "big")
        if kind == 0:
            streaminfo = data[pos + 4:pos + 4 + size]
        pos += 4 + size
        if last:
            break
    frames = []
    start = pos
    number = 0
    while start < len(data):
        assert data[start] == 0xFF and data[start + 1] & 0xFE == 0xF8, "frame sync expected"
        search = start + 2
        end = len(data)
        while True:
            candidate = data.find(b"\xFF", search)
            if candidate < 0 or candidate + 6 > len(data):
                break
            if data[candidate + 1] == 0xF8:
                try:
                    value, _ = utf8_number(data, candidate + 4)
                    length = header_length(data, candidate)
                    if (value == number + 1 and crc8(data[candidate:candidate + length - 1]) == data[candidate + length - 1]
                            and crc16(data[start:candidate - 2]) == int.from_bytes(data[candidate - 2:candidate], "big")):
                        end = candidate
                        break
                except IndexError:
                    pass
            search = candidate + 1
        frame = data[start:end]
        assert crc16(frame[:-2]) == int.from_bytes(frame[-2:], "big"), "frame CRC-16"
        frames.append(frame)
        start = end
        number += 1
    return streaminfo, frames


def renumber_variable(frame: bytes, first_sample: int) -> bytes:
    """Rewrite a fixed-blocking frame as variable blocking starting at first_sample."""
    _, old_number_length = utf8_number(frame, 4)
    old_header = header_length(frame, 0)
    extra = frame[4 + old_number_length:old_header - 1]
    header = bytes([0xFF, 0xF9, frame[2], frame[3]]) + encode_utf8_number(first_sample) + extra
    header += bytes([crc8(header)])
    body = header + frame[old_header:-2]
    return body + crc16(body).to_bytes(2, "big")


def block_size_of(frame: bytes, streaminfo_max: int) -> int:
    code = frame[2] >> 4
    _, number_length = utf8_number(frame, 4)
    if code == 1:
        return 192
    if 2 <= code <= 5:
        return 576 << (code - 2)
    if code == 6:
        return frame[4 + number_length] + 1
    if code == 7:
        return int.from_bytes(frame[4 + number_length:6 + number_length], "big") + 1
    if code >= 8:
        return 256 << (code - 8)
    return streaminfo_max


def make_variable_block(parts: list[Path], output: Path) -> None:
    """Join separately encoded chunks into one variable-blocking stream."""
    frames = []
    sample = 0
    streaminfo = b""
    min_block = 1 << 16
    max_block = 0
    for part in parts:
        info, part_frames = split_frames(part)
        streaminfo = streaminfo or info
        part_max = int.from_bytes(info[2:4], "big")
        part_total = int.from_bytes(info[13:18], "big") & ((1 << 36) - 1)
        part_sample = 0
        for frame in part_frames:
            size = min(block_size_of(frame, part_max), part_total - part_sample)
            frames.append(renumber_variable(frame, sample))
            sample += size
            part_sample += size
            min_block = min(min_block, size)
            max_block = max(max_block, size)
    # STREAMINFO: block sizes, unknown frame sizes, rate/channels/bits from the parts, new total, no MD5.
    rate_channels_bits = int.from_bytes(streaminfo[10:14], "big") >> 4
    packed = (rate_channels_bits << 36) | sample
    body = (struct.pack(">HH", max(min_block, 16), max_block) + b"\0" * 6 +
            packed.to_bytes(8, "big") + b"\0" * 16)
    output.write_bytes(b"fLaC" + bytes([0x80]) + len(body).to_bytes(3, "big") + body + b"".join(frames))


def check(header: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="xdj700-flac-seek-") as folder_name:
        folder = Path(folder_name)
        executable = folder / "seek_runner"
        command = [
            "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-DXDJ_HOST_TEST",
            "-include", str(ROOT / "src" / "host_force_32bit_cache.h"),
            "-I", str(header.parent),
            str(RUNNER_SOURCE), str(DECODER_SOURCE),
            "-o", str(executable),
        ]
        command.append("-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections")
        run(command)

        def wav(name: str, rate: int, bits: int, seconds: float, source: str = "mix") -> Path:
            path = folder / f"{name}.wav"
            fmt = "pcm_s24le" if bits == 24 else "pcm_s16le"
            if source == "mix":
                third = seconds / 3
                filters = (
                    f"anoisesrc=color=white:amplitude=0.4:sample_rate={rate}:duration={third}[a];"
                    f"sine=frequency=440:sample_rate={rate}:duration={third}[b];"
                    f"anoisesrc=color=pink:amplitude=0.5:sample_rate={rate}:duration={third}[c];"
                    f"[a][b][c]concat=n=3:v=0:a=1,aformat=channel_layouts=stereo"
                )
            else:
                filters = f"anoisesrc=color=white:amplitude=0.4:sample_rate={rate}:duration={seconds},aformat=channel_layouts=stereo"
            run(["ffmpeg", "-v", "error", "-y", "-filter_complex", filters, "-c:a", fmt, str(path)])
            return path

        def flac(name: str, source: Path, *options: str) -> Path:
            path = folder / f"{name}.flac"
            run(["flac", "-s", "-f", *options, "-o", str(path), str(source)])
            return path

        long24 = wav("long24", 48000, 24, 90)
        cd16 = wav("cd16", 44100, 16, 60)
        cases = {
            "seek points every 10 s (24-bit, 48 kHz)": flac("dj24", long24, "-5"),
            "no seek table (16-bit, 44.1 kHz)": flac("noseek", cd16, "-5", "--no-seektable"),
            "placeholder seek points, 1152-sample blocks": flac("placeholder", long24, "-5", "-b", "1152", "-S", "X", "-S", "X", "-S", "7s"),
            "sparse seek table (one point per minute)": flac("sparse", long24, "-5", "-S", "60s"),
            "16384-sample blocks": flac("bigblock", cd16, "-5", "--lax", "-b", "16384"),
            "576-sample blocks": flac("smallblock", cd16, "-5", "-b", "576"),
            "tiny file (2 s)": flac("tiny", wav("tiny", 44100, 16, 2, "noise"), "-5"),
        }

        picture = folder / "picture.png"
        run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "nullsrc=s=640x640,geq=random(1)*255:128:128",
             "-frames:v", "1", str(picture)])
        with_picture = flac("picture", cd16, "-5")
        run(["metaflac", f"--import-picture-from=3||||{picture}", str(with_picture)])
        cases["large embedded picture"] = with_picture

        parts = []
        for index, block in enumerate([1152, 4608, 576, 2048, 4096]):
            part_wav = wav(f"part{index}", 48000, 24, 4.3, "noise")
            parts.append(flac(f"part{index}", part_wav, "-5", "-b", str(block), "--no-seektable"))
        variable = folder / "variable.flac"
        make_variable_block(parts, variable)
        cases["variable block sizes (576 to 4608)"] = variable

        results = {}
        all_passed = True
        for index, (name, path) in enumerate(cases.items()):
            outcome = subprocess.run(
                [str(executable), str(path), str(RANDOM_TARGETS), str(index + 1)],
                capture_output=True, text=True,
            )
            try:
                report = json.loads(outcome.stdout)
            except json.JSONDecodeError:
                report = {"error": outcome.stderr.strip() or outcome.stdout.strip()}
            report["file_bytes"] = path.stat().st_size
            passed = (
                outcome.returncode == 0
                and report.get("mismatches") == 0
                and report.get("failures") == 0
                and report.get("fallbacks") == 0
                and report.get("fallback_path_failures") == 0
                and report.get("fallback_path_mismatches") == 0
                and report.get("max_reads", 1 << 30) <= MAX_READS_PER_SEEK
            )
            report["passed"] = passed
            all_passed = all_passed and passed
            results[name] = report

    return {"cases": results, "max_reads_allowed": MAX_READS_PER_SEEK, "all_passed": all_passed}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "header", type=Path, nargs="?", default=ROOT / "src" / "dr_flac.h",
        help="Path to dr_flac.h",
    )
    args = parser.parse_args()
    report = check(args.header)
    print(json.dumps(report, indent=2))
    if not report["all_passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
