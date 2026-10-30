#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Build and run the host test harness for the XDJ-700 FLAC adapter skeleton."""

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
RUNNER_SOURCE = ROOT / "src" / "flac_adapter_host_runner.c"


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


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
    )


def check(header: Path) -> dict:
    if header.name != "dr_flac.h":
        raise ValueError("header filename must be dr_flac.h")
    if not header.is_file():
        raise FileNotFoundError(f"dr_flac.h not found at {header}")

    with tempfile.TemporaryDirectory(prefix="xdj700-flac-adapter-") as folder_name:
        folder = Path(folder_name)
        executable = folder / "flac_adapter_runner"

        # Build host test binary
        common = [
            "cc",
            "-std=c11",
            "-Os",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-function",
            "-DXDJ_HOST_TEST",
            "-DXDJ_MONO",  # as v0.9 on: mono served as stereo
            "-include",
            str(ROOT / "src" / "host_force_32bit_cache.h"),
            "-I",
            str(header.parent),
            str(RUNNER_SOURCE),
            str(ADAPTER_SOURCE),
            str(DECODER_SOURCE),
            "-o",
            str(executable),
        ]
        if platform.system() == "Darwin":
            common.append("-Wl,-dead_strip")
        else:
            common.append("-Wl,--gc-sections")

        run(common)

        test_results = {}
        all_passed = True

        # 1. 16-bit 44.1 kHz CD profile
        cd_flac = folder / "cd_profile.flac"
        cd_ref_pcm = folder / "cd_ref.pcm"
        cd_adapter_pcm = folder / "cd_adapter.pcm"

        run(
            [
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=1000:sample_rate=44100:duration=2",
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
        # Generate reference PCM
        run(
            [
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-i",
                str(cd_flac),
                "-f",
                "s16le",
                str(cd_ref_pcm),
            ]
        )

        test_out = run([str(executable), "--test", str(cd_flac)])
        cd_report = json.loads(test_out.stdout)

        # Dump adapter decoded PCM
        run([str(executable), "--dump-pcm", str(cd_flac), str(cd_adapter_pcm)])
        cd_pcm_match = (
            cd_ref_pcm.read_bytes() == cd_adapter_pcm.read_bytes()
            and cd_report["format"]["bits_per_sample"] == 16
        )
        cd_report["bit_exact_fidelity"] = cd_pcm_match
        test_results["cd_16bit_44k"] = cd_report
        if not (cd_report.get("success") and cd_pcm_match):
            all_passed = False

        # 2. 24-bit 48.0 kHz Studio profile
        studio_flac = folder / "studio_profile.flac"
        studio_ref_pcm = folder / "studio_ref.pcm"
        studio_adapter_pcm = folder / "studio_adapter.pcm"

        run(
            [
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=1000:sample_rate=48000:duration=2",
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
        run(
            [
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-i",
                str(studio_flac),
                "-f",
                "s24le",
                str(studio_ref_pcm),
            ]
        )

        test_out = run([str(executable), "--test", str(studio_flac)])
        studio_report = json.loads(test_out.stdout)

        run([str(executable), "--dump-pcm", str(studio_flac), str(studio_adapter_pcm)])
        # A 24-bit source keeps its depth: packed 24-bit little-endian, like a 24-bit WAV.
        studio_pcm_match = (
            studio_ref_pcm.read_bytes() == studio_adapter_pcm.read_bytes()
            and studio_report["format"]["bits_per_sample"] == 24
        )
        studio_report["bit_exact_fidelity"] = studio_pcm_match
        test_results["studio_24bit_48k"] = studio_report
        if not (studio_report.get("success") and studio_pcm_match):
            all_passed = False

        # Oversized file: the FLAC is larger than its PCM (noise plus a large metadata block)
        oversized_wav = folder / "oversized.wav"
        oversized_flac = folder / "oversized.flac"
        oversized_ref_pcm = folder / "oversized_ref.pcm"
        oversized_adapter_pcm = folder / "oversized_adapter.pcm"
        run(
            [
                "sox", "-n", "-b", "24", "-r", "48000", "-c", "2", str(oversized_wav),
                "synth", "3", "whitenoise", "vol", "0.9",
            ]
        )
        run(["flac", "-s", "-f", "-0", "--padding=300000", "-o", str(oversized_flac), str(oversized_wav)])
        run(["ffmpeg", "-v", "error", "-y", "-i", str(oversized_flac), "-f", "s24le", str(oversized_ref_pcm)])
        oversized_report = json.loads(run([str(executable), "--test", str(oversized_flac)]).stdout)
        run([str(executable), "--dump-pcm", str(oversized_flac), str(oversized_adapter_pcm)])
        oversized_pcm_match = oversized_ref_pcm.read_bytes() == oversized_adapter_pcm.read_bytes()
        oversized_report["bit_exact_fidelity"] = oversized_pcm_match
        test_results["oversized_24bit_48k"] = oversized_report
        if not (
            oversized_report.get("success")
            and oversized_pcm_match
            and oversized_report["descriptor"]["oversized_file"]
        ):
            all_passed = False

        # 3. Mono (1-channel)
        mono_flac = folder / "mono.flac"
        run(
            [
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=1000:sample_rate=44100:duration=1",
                "-ac",
                "1",
                "-c:a",
                "flac",
                str(mono_flac),
            ]
        )
        # Mono is served as stereo, the one channel on both sides (tools/check_pcm_hires.py
        # compares the samples at every rate).
        mono_report = json.loads(run([str(executable), "--test", str(mono_flac)]).stdout)
        test_results["mono_served"] = mono_report
        if not (mono_report.get("success") and mono_report["format"]["channels"] == 2):
            all_passed = False

        # 4. Rejection test: a rate the player cannot be given (rates above 48 kHz
        # are converted down: tools/check_flac_hires.py)
        high_flac = folder / "high_rate.flac"
        run(
            [
                "ffmpeg",
                "-v",
                "error",
                "-y",
                "-f",
                "lavfi",
                "-i",
                "sine=frequency=1000:sample_rate=32000:duration=1",
                "-ac",
                "2",
                "-c:a",
                "flac",
                str(high_flac),
            ]
        )
        high_out = run([str(executable), "--expect-reject", str(high_flac)])
        high_report = json.loads(high_out.stdout)
        test_results["unsupported_rate_rejection"] = high_report
        if not (high_report.get("rejected") and high_report.get("lock_balanced")):
            all_passed = False

        # 5. Rejection test: Corrupted stream
        corrupt_flac = folder / "corrupted.flac"
        corrupt_flac.write_bytes(b"NOT_A_VALID_FLAC_MAGIC_HEADER_DATA_1234567890")
        corrupt_out = run([str(executable), "--expect-reject", str(corrupt_flac)])
        corrupt_report = json.loads(corrupt_out.stdout)
        test_results["corrupt_rejection"] = corrupt_report
        if not (corrupt_report.get("rejected") and corrupt_report.get("lock_balanced")):
            all_passed = False

        # 6. Rejection test: Zero frame count (prevents divide-by-zero)
        zero_flac = folder / "zero_frames.flac"
        zero_data = bytearray(cd_flac.read_bytes())
        zero_data[21] = zero_data[21] & 0xF0
        zero_data[22] = 0
        zero_data[23] = 0
        zero_data[24] = 0
        zero_data[25] = 0
        zero_flac.write_bytes(bytes(zero_data))
        zero_out = run([str(executable), "--expect-reject", str(zero_flac)])
        zero_report = json.loads(zero_out.stdout)
        test_results["zero_frame_rejection"] = zero_report
        if not (zero_report.get("rejected") and zero_report.get("lock_balanced")):
            all_passed = False

        # 7. Rejection test: Huge frame count exceeding signed 32-bit positive limit
        huge_flac = folder / "huge_frames.flac"
        huge_data = bytearray(cd_flac.read_bytes())
        huge_data[21] = (huge_data[21] & 0xF0) | 0x07
        huge_data[22] = 0xFF
        huge_data[23] = 0xFF
        huge_data[24] = 0xFF
        huge_data[25] = 0xFF
        huge_flac.write_bytes(bytes(huge_data))
        huge_out = run([str(executable), "--expect-reject", str(huge_flac)])
        huge_report = json.loads(huge_out.stdout)
        test_results["huge_frame_rejection"] = huge_report
        if not (huge_report.get("rejected") and huge_report.get("lock_balanced")):
            all_passed = False

        return {
            "candidate": "dr_flac",
            "candidate_version": header_version(header),
            "header_sha256": sha256(header),
            "compile_target": platform.machine(),
            "all_passed": all_passed,
            "pcm_fidelity": {
                "cd_16bit_bit_exact": cd_pcm_match,
                "studio_24bit_bit_exact": studio_pcm_match,
            },
            "fixed_arena": {
                "limit_bytes": 98304,
                "cd_16bit_used_bytes": cd_report["arena"]["bytes_used"],
                "studio_24bit_used_bytes": studio_report["arena"]["bytes_used"],
                "within_limit": cd_report["arena"]["within_limit"] and studio_report["arena"]["within_limit"],
            },
            "tests": test_results,
            "constraints": {
                "host_only": True,
                "modified_firmware": False,
                "output_written": False,
                "device_access": False,
            },
        }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Verify the XDJ-700 FLAC adapter skeleton with host tests."
    )
    parser.add_argument(
        "header",
        type=Path,
        nargs="?",
        default=ROOT / "src" / "dr_flac.h",
        help="Path to dr_flac.h",
    )
    args = parser.parse_args()
    report = check(args.header)
    print(json.dumps(report, indent=2))
    if not report.get("all_passed"):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
