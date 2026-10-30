#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Check the bridge against the player's track lifecycle.

Builds src/flac_adapter_lifecycle_runner.c with the adapter and runs it
on three generated FLAC tracks: a manager thread switches tracks the way the
load manager does (zeroed context, closed and reopened handle in the same
context, a wait, then the new format object and command) while a stream
thread keeps reading through the workspace. Every read outside a switch must
be sample-exact and no call may fail. The same runner built to trust every
handle (as diag8 and diag9 did) must fail, which shows the check can see the
device's failure. A last run uses seven tracks for the six decoder slots, so
switches also evict and reopen tracks.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
RUNNER_SOURCE = ROOT / "src" / "flac_adapter_lifecycle_runner.c"
ADAPTER_SOURCE = ROOT / "src" / "flac_adapter.c"
DECODER_SOURCE = ROOT / "src" / "flac_decoder.c"
SECONDS = "3"
SEEDS = ("1", "2", "3")


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=True, capture_output=True, text=True)


def build(header: Path, output: Path, *defines: str) -> None:
    command = [
        "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-DXDJ_HOST_TEST", *defines, "-D_DEFAULT_SOURCE",  # glibc hides nanosleep under -std=c11 otherwise
        "-include", str(ROOT / "src" / "host_force_32bit_cache.h"),
        "-I", str(header.parent),
        str(RUNNER_SOURCE), str(ADAPTER_SOURCE), str(DECODER_SOURCE),
        "-lpthread", "-o", str(output),
    ]
    command.append("-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections")
    run(command)


def check(header: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="xdj700-flac-life-") as folder_name:
        folder = Path(folder_name)
        runner = folder / "lifecycle"
        trusting = folder / "lifecycle_trusting"
        network = folder / "lifecycle_network"
        build(header, runner)
        build(header, trusting, "-DXDJ_HOST_TRUST_EVERY_HANDLE")
        # A network filesystem (LINK): tracks identified by the context's data
        # length, sizes checked once per handle, handle objects reused.
        build(header, network, "-DXDJ_HOST_NETWORK_FS", "-DXDJ_HOST_CONTEXT_DATA_BYTES")
        tracks = []
        for name, seconds, color in (("a", 70, "pink"), ("b", 90, "white"), ("c", 60, "brown")):
            path = folder / f"{name}.flac"
            run([
                "ffmpeg", "-v", "error", "-y", "-f", "lavfi",
                "-i", f"anoisesrc=color={color}:amplitude=0.2:sample_rate=44100:duration={seconds}",
                "-ac", "2", "-c:a", "flac", "-sample_fmt", "s16", str(path),
            ])
            tracks.append(str(path))

        def execute(executable: Path, seed: str) -> dict:
            outcome = subprocess.run([str(executable), SECONDS, seed, *tracks], capture_output=True, text=True)
            try:
                return json.loads(outcome.stdout)
            except json.JSONDecodeError:
                return {"error": outcome.stderr.strip(), "passed": False}

        runs = {seed: execute(runner, seed) for seed in SEEDS}
        network_runs = {seed: execute(network, seed) for seed in SEEDS}
        trusting_run = execute(trusting, SEEDS[0])

        # A stock file with the same audio length as an open track of ours
        # (the same recording in two formats) must go to the stock reader, on
        # both kinds of filesystem.
        def twin(executable: Path) -> dict:
            outcome = subprocess.run(
                [str(executable), "--twin", tracks[0], tracks[1]], capture_output=True, text=True
            )
            try:
                return {**json.loads(outcome.stdout), "stderr": outcome.stderr.strip()}
            except json.JSONDecodeError:
                return {"error": outcome.stderr.strip(), "passed": False}

        twin_runs = {"local": twin(runner), "network": twin(network)}
        # Eight tracks for six decoder slots: switches also evict and reopen.
        # The added ones mix depths, rates and kinds: 16-bit and 24-bit FLAC
        # share the slots with FLAC converted down from 96 kHz and with a WAV
        # and an AIFF file the bridge serves (96 and 88.2 kHz).
        for name, seconds, bits, rate, kind in (("d", 40, 16, 48000, "flac"), ("e", 45, 24, 48000, "flac"),
                                                ("f", 35, 24, 96000, "flac"), ("g", 30, 16, 88200, "aiff"),
                                                ("h", 30, 24, 96000, "wav")):
            source = f"anoisesrc=color=pink:amplitude=0.1:sample_rate={rate}:duration={seconds}"
            if kind == "aiff":
                path = folder / f"{name}.aiff"
                run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", source, "-ac", "2",
                     "-c:a", "pcm_s24be" if bits == 24 else "pcm_s16be", "-f", "aiff", str(path)])
            else:
                path = folder / f"{name}.wav"
                run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", source, "-ac", "2",
                     "-c:a", "pcm_s24le" if bits == 24 else "pcm_s16le", str(path)])
                if kind == "flac":
                    # The reference encoder keeps 4096-frame blocks at every rate.
                    wav, path = path, folder / f"{name}.flac"
                    run(["flac", "-s", "-f", "-5", "-o", str(path), str(wav)])
            tracks.append(str(path))
        eviction_run = execute(runner, "7")
    passed = (all(result.get("passed") is True for result in runs.values()) and
              all(result.get("passed") is True for result in network_runs.values()) and
              eviction_run.get("passed") is True and trusting_run.get("passed") is False and
              all(result.get("passed") is True for result in twin_runs.values()))
    return {"runs": runs, "network_runs": network_runs, "evictions": eviction_run,
            "trusting_every_handle": trusting_run, "same_length_twin": twin_runs, "all_passed": passed}


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
