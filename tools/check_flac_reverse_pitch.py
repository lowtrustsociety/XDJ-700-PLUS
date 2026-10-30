#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Build and run reverse playback, variable pitch fader, and vinyl scratching tests."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ADAPTER_SOURCE = ROOT / "src" / "flac_adapter.c"
DECODER_SOURCE = ROOT / "src" / "flac_decoder.c"
REVERSE_PITCH_RUNNER_SOURCE = ROOT / "src" / "flac_adapter_reverse_pitch_runner.c"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def header_version(header: Path) -> str:
    text = header.read_text(encoding="utf-8", errors="strict")
    values = []
    for name in ("MAJOR", "MINOR", "REVISION"):
        match = re.search(rf"#define DRFLAC_VERSION_{name}\s+(\d+)", text)
        if match is None:
            raise ValueError(f"missing dr_flac version field: {name}")
        values.append(match.group(1))
    return ".".join(values)


def run_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed (rc={result.returncode}): {' '.join(command)}\n"
            f"STDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}"
        )
    return result


def check(header: Path) -> dict:
    if header.name != "dr_flac.h":
        raise ValueError("header filename must be dr_flac.h")
    if not header.is_file():
        raise FileNotFoundError(f"dr_flac.h not found at {header}")

    with tempfile.TemporaryDirectory(prefix="xdj700-flac-reverse-pitch-") as folder_name:
        folder = Path(folder_name)
        executable = folder / "reverse_pitch_runner"

        # Compile runner
        common = [
            "cc",
            "-std=c11",
            "-Os",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-function",
            "-DXDJ_HOST_TEST",
            "-include",
            str(Path(__file__).resolve().parents[1] / "src" / "host_force_32bit_cache.h"),
            "-I",
            str(header.parent),
            str(REVERSE_PITCH_RUNNER_SOURCE),
            str(ADAPTER_SOURCE),
            str(DECODER_SOURCE),
            "-o",
            str(executable),
        ]
        if platform.system() == "Darwin":
            common.append("-Wl,-dead_strip")
        else:
            common.append("-Wl,--gc-sections")

        run_command(common)

        # Generate 16-bit 44.1 kHz test stream (3 seconds = 132,300 frames)
        cd_flac = folder / "cd_stream.flac"
        run_command(
            [
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=1000:sample_rate=44100:duration=3",
                "-ac",
                "2",
                "-c:a",
                "flac",
                "-sample_fmt",
                "s16",
                str(cd_flac),
            ]
        )

        # Generate 24-bit 48 kHz test stream (3 seconds = 144,000 frames)
        studio_flac = folder / "studio_stream.flac"
        run_command(
            [
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=1000:sample_rate=48000:duration=3",
                "-ac",
                "2",
                "-c:a",
                "flac",
                "-sample_fmt",
                "s32",
                "-bits_per_raw_sample",
                "24",
                str(studio_flac),
            ]
        )

        # Run tests on 16-bit 44.1k
        cd_result = run_command([str(executable), str(cd_flac)])
        cd_report = json.loads(cd_result.stdout)

        # Run tests on 24-bit 48k
        studio_result = run_command([str(executable), str(studio_flac)])
        studio_report = json.loads(studio_result.stdout)

        all_passed = (
            cd_report["reverse_playback"]["passed"]
            and cd_report["variable_pitch"]["passed"]
            and cd_report["vinyl_scratch"]["passed"]
            and cd_report["boundary_safety"]["passed"]
            and cd_report["lock_balanced"]
            and cd_report["arena_used"] <= 65536
            and studio_report["reverse_playback"]["passed"]
            and studio_report["variable_pitch"]["passed"]
            and studio_report["vinyl_scratch"]["passed"]
            and studio_report["boundary_safety"]["passed"]
            and studio_report["lock_balanced"]
            and studio_report["arena_used"] <= 65536
        )

        return {
            "header": {
                "path": str(header),
                "sha256": sha256(header),
                "version": header_version(header),
            },
            "all_passed": all_passed,
            "profiles": {
                "cd_16bit_44k": cd_report,
                "studio_24bit_48k": studio_report,
            },
        }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Verify reverse playback, variable pitch fader, and vinyl scratching offline.",
    )
    parser.add_argument(
        "header",
        type=Path,
        help="Path to official dr_flac.h header",
    )
    args = parser.parse_args()
    print(json.dumps(check(args.header), indent=2))


if __name__ == "__main__":
    main()
