#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Comprehensive real-world FLAC corpus and encoder configuration test suite.

Exercises the XDJ-700 FLAC adapter against files produced by the official Xiph
reference `flac` encoder across:
 1. All compression presets (Level 0 through Level 8)
 2. Standard block sizes (1152, 2048, 4096, 4608)
 3. Stereo decorrelation modes (Independent stereo -M, Mid-Side stereo -m)
 4. Embedded metadata blocks (Vorbis comments, 500KB cover art, Padding, Seek tables)
 5. High-resolution 24-bit 48 kHz studio masters with reference libFLAC encoding
 6. Bounded arena memory verification (<= 64 KiB)
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ADAPTER_SOURCE = ROOT / "src" / "flac_adapter.c"
DECODER_SOURCE = ROOT / "src" / "flac_decoder.c"
HOST_RUNNER_SOURCE = ROOT / "src" / "flac_adapter_host_runner.c"


def run_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"command failed: {' '.join(command)}\n{detail}")
    return result


def check(header: Path) -> dict:
    if shutil.which("flac") is None:
        raise RuntimeError("flac command-line tool is required")
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg command-line tool is required")

    with tempfile.TemporaryDirectory(prefix="xdj700-flac-corpus-") as folder_name:
        folder = Path(folder_name)
        runner_exe = folder / "host_runner"

        # Compile host runner
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
            str(HOST_RUNNER_SOURCE),
            str(ADAPTER_SOURCE),
            str(DECODER_SOURCE),
            "-o",
            str(runner_exe),
        ]
        if platform.system() == "Darwin":
            common.append("-Wl,-dead_strip")
        else:
            common.append("-Wl,--gc-sections")
        run_command(common)

        # Generate base 16-bit 44.1k WAV
        wav_16 = folder / "base16.wav"
        run_command([
            "ffmpeg", "-v", "error", "-y", "-f", "lavfi",
            "-i", "sine=frequency=1000:sample_rate=44100:duration=2",
            "-ac", "2", "-c:a", "pcm_s16le", str(wav_16)
        ])

        # Generate base 24-bit 48k WAV
        wav_24 = folder / "base24.wav"
        run_command([
            "ffmpeg", "-v", "error", "-y", "-f", "lavfi",
            "-i", "sine=frequency=1000:sample_rate=48000:duration=2",
            "-ac", "2", "-c:a", "pcm_s24le", str(wav_24)
        ])

        # Generate test cover image (500 KB)
        cover_img = folder / "cover.jpg"
        run_command([
            "ffmpeg", "-v", "error", "-y", "-f", "lavfi",
            "-i", "testsrc=size=1000x1000:rate=1",
            "-vframes", "1", str(cover_img)
        ])

        corpus_cases = [
            ("level_0_fastest", wav_16, ["flac", "-0"]),
            ("level_5_default", wav_16, ["flac", "-5"]),
            ("level_8_maximum", wav_16, ["flac", "-8"]),
            ("blocksize_1152", wav_16, ["flac", "-b", "1152"]),
            ("blocksize_2048", wav_16, ["flac", "-b", "2048"]),
            ("blocksize_4096_standard", wav_16, ["flac", "-b", "4096"]),
            ("blocksize_4608_cd_max", wav_16, ["flac", "-b", "4608"]),
            ("independent_stereo_channel", wav_16, ["flac", "-8", "-M"]),
            ("mid_side_stereo_channel", wav_16, ["flac", "-8", "-m"]),
            ("vorbis_comments_metadata", wav_16, ["flac", "-8", "--tag=TITLE=Pioneer FLAC Mod", "--tag=ARTIST=Antigravity", "--tag=GENRE=House", "--tag=COMMENT=Tested safely"]),
            ("large_embedded_cover_art", wav_16, ["flac", "-8", f"--picture={cover_img}"]),
            ("custom_seektable_dense", wav_16, ["flac", "-8", "--seekpoint=100ms"]),
            ("studio_24bit_level_5", wav_24, ["flac", "-5"]),
            ("studio_24bit_level_8", wav_24, ["flac", "-8"]),
            ("studio_24bit_with_cover_and_tags", wav_24, ["flac", "-8", f"--picture={cover_img}", "--tag=TITLE=Studio 24-bit Track", "--tag=ARTIST=Antigravity"]),
        ]

        results = {}
        all_passed = True

        for test_name, src_wav, flac_cmd in corpus_cases:
            target_flac = folder / f"{test_name}.flac"
            full_flac_cmd = flac_cmd + [str(src_wav), "-o", str(target_flac), "--force"]
            run_command(full_flac_cmd)

            runner_res = run_command([str(runner_exe), "--test", str(target_flac)])
            test_report = json.loads(runner_res.stdout)

            passed = (
                test_report.get("success", False)
                and test_report["arena"]["within_limit"]
                and test_report["sequential_decode"]["passed"]
                and test_report["seek_tests"]["passed"]
                and test_report["lock"]["balanced"]
                and test_report["arena"]["bytes_used"] <= 65536
            )
            if not passed:
                all_passed = False

            results[test_name] = {
                "passed": passed,
                "arena_used_bytes": test_report["arena"]["bytes_used"],
                "sample_rate": test_report["format"]["sample_rate"],
                "bits_per_sample": test_report["format"]["bits_per_sample"],
                "total_frames": test_report["format"]["total_frames"],
            }

        return {
            "all_passed": all_passed,
            "total_corpus_files_tested": len(corpus_cases),
            "results": results,
        }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Verify real-world FLAC corpus files offline.",
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
