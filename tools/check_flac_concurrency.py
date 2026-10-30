#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Build and run multi-threaded concurrency stress tests on the XDJ-700 FLAC adapter."""

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
CONCURRENCY_RUNNER_SOURCE = ROOT / "src" / "flac_adapter_concurrency_runner.c"


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
    return subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
    )


def check(header: Path, duration_seconds: int = 1) -> dict:
    if header.name != "dr_flac.h":
        raise ValueError("header filename must be dr_flac.h")
    if not header.is_file():
        raise FileNotFoundError(f"dr_flac.h not found at {header}")

    with tempfile.TemporaryDirectory(prefix="xdj700-flac-concurrency-") as folder_name:
        folder = Path(folder_name)
        executable = folder / "concurrency_runner"

        # Compile concurrency runner
        common = [
            "cc",
            "-std=c11",
            "-Os",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-function",
            "-DXDJ_HOST_TEST",
            "-D_DEFAULT_SOURCE",  # glibc hides nanosleep under -std=c11 otherwise
            "-include",
            str(Path(__file__).resolve().parents[1] / "src" / "host_force_32bit_cache.h"),
            "-I",
            str(header.parent),
            str(CONCURRENCY_RUNNER_SOURCE),
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

        # Generate 16-bit 44.1 kHz test stream
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
                "-bits_per_raw_sample",
                "16",
                str(cd_flac),
            ]
        )

        # Run 16-bit concurrency stress test
        cd_out = run_command([str(executable), str(cd_flac), str(duration_seconds)])
        cd_report = json.loads(cd_out.stdout)

        # Generate 24-bit 48.0 kHz test stream
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

        # Run 24-bit concurrency stress test
        studio_out = run_command([str(executable), str(studio_flac), str(duration_seconds)])
        studio_report = json.loads(studio_out.stdout)

        all_passed = (
            cd_report.get("success") is True
            and studio_report.get("success") is True
            and cd_report["safety_invariants"]["mutual_exclusion_preserved"] is True
            and studio_report["safety_invariants"]["mutual_exclusion_preserved"] is True
            and cd_report["safety_invariants"]["no_deadlocks"] is True
            and studio_report["safety_invariants"]["no_deadlocks"] is True
            and cd_report["safety_invariants"]["arena_within_limit"] is True
            and studio_report["safety_invariants"]["arena_within_limit"] is True
            and cd_report["synchronization"]["balanced"] is True
            and studio_report["synchronization"]["balanced"] is True
        )

        return {
            "candidate": "dr_flac",
            "candidate_version": header_version(header),
            "header_sha256": sha256(header),
            "concurrency_engine": "POSIX Threads (pthreads) + C11 stdatomic",
            "threads_per_test": 4,
            "duration_seconds_per_profile": duration_seconds,
            "all_passed": all_passed,
            "profiles": {
                "cd_16bit_44k": cd_report,
                "studio_24bit_48k": studio_report,
            },
            "aggregate": {
                "total_lock_acquires": (
                    cd_report["synchronization"]["lock_acquires"]
                    + studio_report["synchronization"]["lock_acquires"]
                ),
                "total_contentions": (
                    cd_report["synchronization"]["contentions"]
                    + studio_report["synchronization"]["contentions"]
                ),
                "total_frames_decoded": (
                    cd_report["operations"]["frames_decoded"]
                    + studio_report["operations"]["frames_decoded"]
                ),
                "total_seeks_performed": (
                    cd_report["operations"]["seeks_performed"]
                    + studio_report["operations"]["seeks_performed"]
                ),
                "total_track_reloads": (
                    cd_report["operations"]["track_reloads"]
                    + studio_report["operations"]["track_reloads"]
                ),
                "peak_arena_bytes": max(
                    cd_report["safety_invariants"]["arena_peak_bytes"],
                    studio_report["safety_invariants"]["arena_peak_bytes"],
                ),
            },
            "constraints": {
                "host_only": True,
                "modified_firmware": False,
                "output_written": False,
                "device_access": False,
            },
        }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Run multi-threaded concurrency stress tests on the XDJ-700 FLAC adapter."
    )
    parser.add_argument(
        "header",
        type=Path,
        nargs="?",
        default=ROOT / "src" / "dr_flac.h",
        help="Path to dr_flac.h",
    )
    parser.add_argument(
        "--duration",
        type=int,
        default=1,
        help="Duration in seconds for each stress test profile (default 1s)",
    )
    args = parser.parse_args()

    report = check(args.header, args.duration)
    print(json.dumps(report, indent=2))
    if not report.get("all_passed"):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
