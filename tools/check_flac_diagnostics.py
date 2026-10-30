#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Check the adapter's episode timing and the audible report round trip.

Builds src/flac_adapter_diag_runner.c against the adapter on the host,
runs its episode checks on a generated FLAC file, then renders reports and
decodes them back with tools/decode_diag_tones.py at several playback speeds.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import decode_diag_tones  # noqa: E402


RUNNER_SOURCE = ROOT / "src" / "flac_adapter_diag_runner.c"
ADAPTER_SOURCE = ROOT / "src" / "flac_adapter.c"
DECODER_SOURCE = ROOT / "src" / "flac_decoder.c"

# Reports covering every value range: a failed jump, a small open, and a failed read.
SAMPLE_REPORTS = [
    [5, 1 | 4 | 8 | 16 | 32, 63, 0, 10, 20, 30, 40, 50, 60, 62, 7, 8, 9, 33, 44, 2, 15, 3, 31, 4, 1 | (7 << 3), 5, 2, 9, 1],
    [6, 2, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0, 16, 0, 0, 0, 63, 0, 0, 0, 0],
    [7, 3 | 32, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 2, 5, 0, 63, 63, 63, 0, 63, 63, 63, 63],
    # A file snapshot: kind 63, reason, type, device byte, three 32-bit sizes, flags, matches.
    [1, 63, 2, 3, 1, 5, 0, 2, 26, 11, 5, 9, 0, 0, 63, 1, 2, 3, 0, 1, 0, 0, 0, 0, 7, 0],
]
# Render rate and WAV header rate: equal, and pitch +3 % and -7 %.
RENDER_CASES = [(44100, 44100), (48000, 48000), (48000, 49440), (48000, 44640)]


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=True, capture_output=True, text=True)


def with_checksum(fields: list[int]) -> list[int]:
    return fields + [sum(fields) & 63]


def check(header: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="xdj700-flac-diag-") as folder_name:
        folder = Path(folder_name)
        executable = folder / "diag_runner"
        command = [
            "cc", "-std=c11", "-Os", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
            "-DXDJ_HOST_TEST",
            "-include", str(ROOT / "src" / "host_force_32bit_cache.h"),
            "-I", str(header.parent),
            str(RUNNER_SOURCE), str(ADAPTER_SOURCE), str(DECODER_SOURCE),
            "-o", str(executable),
        ]
        command.append("-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections")
        run(command)
        # The same runner with the paced fetch built in (the builder's --fetch-pace 10 --fetch-refill 7).
        paced_executable = folder / "diag_runner_paced"
        run(
            [part if part != str(executable) else str(paced_executable) for part in command]
            + ["-DXDJ_FETCH_PACE_MS=10", "-DXDJ_FETCH_REFILL_MS=7"]
        )

        # Noise does not compress, so every seek and decode reads the file.
        flac = folder / "noise.flac"
        run([
            "ffmpeg", "-v", "error", "-y", "-f", "lavfi",
            "-i", "anoisesrc=color=white:amplitude=0.5:sample_rate=44100:duration=20",
            "-ac", "2", "-c:a", "flac", "-sample_fmt", "s16", str(flac),
        ])
        other = folder / "other.flac"
        run([
            "ffmpeg", "-v", "error", "-y", "-f", "lavfi",
            "-i", "anoisesrc=color=pink:amplitude=0.5:sample_rate=44100:duration=13",
            "-ac", "2", "-c:a", "flac", "-sample_fmt", "s16", str(other),
        ])
        # Six more tracks, so opening them evicts the first one.
        extras = []
        for index in range(6):
            extra = folder / f"extra{index}.flac"
            run([
                "ffmpeg", "-v", "error", "-y", "-f", "lavfi",
                "-i", f"anoisesrc=color=pink:amplitude=0.3:sample_rate=44100:duration={5 + index}",
                "-ac", "2", "-c:a", "flac", "-sample_fmt", "s16", str(extra),
            ])
            extras.append(str(extra))
        episodes = subprocess.run(
            [str(executable), "--episodes", str(flac), str(other), *extras], capture_output=True, text=True
        )
        episode_report = json.loads(episodes.stdout)
        episode_report["stderr"] = episodes.stderr.strip()

        pace = subprocess.run([str(paced_executable), "--pace", str(flac)], capture_output=True, text=True)
        pace_report = json.loads(pace.stdout) if pace.stdout.strip() else {"passed": False}
        pace_report["stderr"] = pace.stderr.strip()

        expected = [with_checksum(fields) for fields in SAMPLE_REPORTS]
        argument = ";".join(",".join(str(value) for value in fields) for fields in expected)
        round_trips = []
        for rate, header_rate in RENDER_CASES:
            wav = folder / f"report_{rate}_{header_rate}.wav"
            run([str(executable), "--render", str(wav), str(rate), str(header_rate), argument])
            decoded = decode_diag_tones.decode(wav)
            fields = [report.get("raw", report.get("fields")) for report in decoded]
            matched = len(decoded) == len(expected) and all(
                report.get("checksum_ok") and not report.get("unreadable") for report in decoded
            ) and all(report["raw"] == want for report, want in zip(decoded, expected))
            round_trips.append({
                "render_rate": rate,
                "playback_rate": header_rate,
                "reports_decoded": len(decoded),
                "pitch_percent": [report.get("pitch_percent") for report in decoded],
                "fields": fields,
                "matched": matched,
            })

    all_passed = episodes.returncode == 0 and episode_report.get("passed") and all(
        case["matched"] for case in round_trips
    ) and pace.returncode == 0 and pace_report.get("passed")
    return {
        "episodes": episode_report,
        "paced_fetch": pace_report,
        "report_round_trips": round_trips,
        "all_passed": bool(all_passed),
    }


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
