#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Check that two tasks playing two FLAC tracks at once never mix them up.

Builds src/flac_adapter_two_track_runner.c with the adapter, generates
two noise FLAC files of different lengths, and runs two threads that seek and
read their own track through the bridge, checking every sample. Each call
carries its own format object, as the PCM trampolines pass it, so no call
may ever be served by the other track's decoder. (Calls whose format object
and file disagree are stale windows, covered by the lifecycle check.)
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
RUNNER_SOURCE = ROOT / "src" / "flac_adapter_two_track_runner.c"
ADAPTER_SOURCE = ROOT / "src" / "flac_adapter.c"
DECODER_SOURCE = ROOT / "src" / "flac_decoder.c"
SECONDS = "4"
MODES = ("format",)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=True, capture_output=True, text=True)


def check(header: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="xdj700-flac-two-") as folder_name:
        folder = Path(folder_name)
        executable = folder / "two_track_runner"
        command = [
            "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-DXDJ_HOST_TEST", "-D_DEFAULT_SOURCE",  # glibc hides nanosleep under -std=c11 otherwise
            "-include", str(ROOT / "src" / "host_force_32bit_cache.h"),
            "-I", str(header.parent),
            str(RUNNER_SOURCE), str(ADAPTER_SOURCE), str(DECODER_SOURCE),
            "-lpthread", "-o", str(executable),
        ]
        command.append("-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections")
        run(command)
        files = []
        for name, seconds in (("a", 20), ("b", 27)):
            path = folder / f"{name}.flac"
            run([
                "ffmpeg", "-v", "error", "-y", "-f", "lavfi",
                "-i", f"anoisesrc=color=pink:amplitude=0.5:sample_rate=44100:duration={seconds}",
                "-ac", "2", "-c:a", "flac", "-sample_fmt", "s16", str(path),
            ])
            files.append(str(path))
        results = {}
        for mode in MODES:
            outcome = subprocess.run([str(executable), *files, SECONDS, mode], capture_output=True, text=True)
            try:
                results[mode] = json.loads(outcome.stdout)
            except json.JSONDecodeError:
                results[mode] = {"error": outcome.stderr.strip()}
    passed = all(results[mode].get("passed") is True for mode in MODES)
    return {"runs": results, "all_passed": passed}


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
