#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Host checks for 88.2 and 96 kHz FLAC sources, which the adapter converts down.

Their rate is halved, so the player gets 44.1 or 48 kHz at the source's bit
depth. Higher rates (176.4 and 192 kHz) are refused. The checks:

1. Files from the reference encoder and from ffmpeg (which uses larger blocks
   at high rates) open, declare the converted rate and length, and pass the
   host runner's tests: every seek and every split read returns exactly the
   bytes straight playing gives, so the filters' state never depends on how a
   position was reached. 176.4 and 192 kHz files are refused at the open.
2. Filter quality, measured on tones: the level in the audible band is kept,
   and a tone above the new Nyquist frequency is suppressed instead of
   folding into the audible band.
"""

from __future__ import annotations

import argparse
import json
import math
import platform
import subprocess
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER_SOURCE = ROOT / "src" / "flac_adapter_host_runner.c"
ADAPTER_SOURCE = ROOT / "src" / "flac_adapter.c"
DECODER_SOURCE = ROOT / "src" / "flac_decoder.c"

# (source rate, bits)
PROFILES = ((88200, 16), (96000, 24))
REFUSED_RATES = (176400, 192000)
PASSBAND_TOLERANCE_DB = 0.1
MIN_ALIAS_REJECTION_DB = 55.0


def run(command: list[str]) -> subprocess.CompletedProcess:
    outcome = subprocess.run(command, capture_output=True, text=True)
    if outcome.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(command)}\n{outcome.stdout}\n{outcome.stderr}")
    return outcome


def build_runner(header: Path, output: Path) -> None:
    command = [
        "cc", "-std=c11", "-Os", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-DXDJ_HOST_TEST",
        "-include", str(ROOT / "src" / "host_force_32bit_cache.h"),
        "-I", str(header.parent),
        str(RUNNER_SOURCE), str(ADAPTER_SOURCE), str(DECODER_SOURCE),
        "-o", str(output),
    ]
    command.append("-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections")
    run(command)


def source_frames(flac: Path) -> int:
    return int(run(["metaflac", "--show-total-samples", str(flac)]).stdout.strip())


def write_tones(path: Path, rate: int, bits: int, left_hz: float, right_hz: float, seconds: int = 3) -> None:
    """A stereo WAV with one sine per channel at half of full scale.

    Written here, sample by sample: sox's synth does not produce tones above
    24 kHz as asked.
    """
    width = bits // 8
    peak = 0.5 * ((1 << (bits - 1)) - 1)
    data = bytearray()
    for index in range(rate * seconds):
        for frequency in (left_hz, right_hz):
            value = round(peak * math.sin(2.0 * math.pi * frequency * index / rate))
            data += value.to_bytes(width, "little", signed=True)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(width)
        output.setframerate(rate)
        output.writeframes(bytes(data))


def channel_samples(pcm: bytes, bytes_per_sample: int, channel: int) -> list[int]:
    step = bytes_per_sample * 2
    start = channel * bytes_per_sample
    return [
        int.from_bytes(pcm[offset : offset + bytes_per_sample], "little", signed=True)
        for offset in range(start, len(pcm) - step + 1, step)
    ]


def tone_level_db(samples: list[int], frequency: float, rate: int, full_scale: int) -> float:
    """Level of one frequency relative to full scale (Goertzel over whole cycles)."""
    count = len(samples)
    coefficient = 2.0 * math.cos(2.0 * math.pi * frequency / rate)
    previous = before = 0.0
    for sample in samples:
        previous, before = sample + coefficient * previous - before, previous
    power = previous * previous + before * before - coefficient * previous * before
    amplitude = 2.0 * math.sqrt(max(power, 0.0)) / count
    return 20.0 * math.log10(max(amplitude / full_scale, 1e-12))


def check(header: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="xdj700-flac-hires-") as folder_name:
        folder = Path(folder_name)
        runner = folder / "runner"
        build_runner(header, runner)

        files = {}
        tones = {}
        for rate, bits in PROFILES:
            out_rate = rate // 2
            noise = folder / f"noise_{rate}.wav"
            run(["sox", "-n", "-b", str(bits), "-r", str(rate), "-c", "2", str(noise),
                 "synth", "20", "pinknoise", "vol", "0.5"])
            encoded = {
                "reference encoder": folder / f"reference_{rate}.flac",
                "ffmpeg": folder / f"ffmpeg_{rate}.flac",
            }
            run(["flac", "-s", "-f", "-5", "-o", str(encoded["reference encoder"]), str(noise)])
            depth = ["-sample_fmt", "s32", "-bits_per_raw_sample", "24"] if bits == 24 else ["-sample_fmt", "s16"]
            run(["ffmpeg", "-v", "error", "-y", "-i", str(noise), "-c:a", "flac", *depth, str(encoded["ffmpeg"])])
            for encoder, flac in encoded.items():
                outcome = subprocess.run([str(runner), "--test", str(flac)], capture_output=True, text=True)
                try:
                    report = json.loads(outcome.stdout)
                except json.JSONDecodeError:
                    report = {"success": False, "error": outcome.stderr.strip()}
                block = int(run(["metaflac", "--show-max-blocksize", str(flac)]).stdout.strip())
                expected_frames = source_frames(flac) // 2
                fmt = report.get("format", {})
                passed = bool(
                    report.get("success")
                    and fmt.get("sample_rate") == out_rate
                    and fmt.get("bits_per_sample") == bits
                    and fmt.get("total_frames") == expected_frames
                    and report["seek_tests"]["passed"]
                    and report["seek_tests"]["byte_exact"]
                )
                files[f"{rate} Hz {bits}-bit, {encoder}"] = {
                    "passed": passed,
                    "max_block_frames": block,
                    "output_rate": fmt.get("sample_rate"),
                    "output_bits": fmt.get("bits_per_sample"),
                    "output_frames": fmt.get("total_frames"),
                    "expected_frames": expected_frames,
                    "arena_bytes": report.get("arena", {}).get("bytes_used"),
                    "error": report.get("error"),
                }

            # Left: a tone that must pass. Right: a tone that would fold to 18 kHz (or 16.5 kHz).
            keep = 20000.0 * out_rate / 48000.0
            fold = out_rate - 18000.0 * out_rate / 48000.0
            tone_wav = folder / f"tone_{rate}.wav"
            tone_flac = folder / f"tone_{rate}.flac"
            tone_pcm = folder / f"tone_{rate}.pcm"
            write_tones(tone_wav, rate, bits, keep, fold)
            run(["flac", "-s", "-f", "-5", "-o", str(tone_flac), str(tone_wav)])
            run([str(runner), "--dump-pcm", str(tone_flac), str(tone_pcm)])
            pcm = tone_pcm.read_bytes()
            bytes_per_sample = bits // 8
            full_scale = 1 << (bits - 1)
            # One second from the middle: whole cycles of every tone measured.
            window = slice(out_rate, 2 * out_rate)
            left = channel_samples(pcm, bytes_per_sample, 0)[window]
            right = channel_samples(pcm, bytes_per_sample, 1)[window]
            kept_db = tone_level_db(left, keep, out_rate, full_scale) - 20.0 * math.log10(0.5)
            alias_db = tone_level_db(right, out_rate - fold, out_rate, full_scale) - 20.0 * math.log10(0.5)
            tones[f"{rate} Hz {bits}-bit"] = {
                "kept_tone_hz": keep,
                "kept_tone_change_db": round(kept_db, 3),
                "folding_tone_hz": fold,
                "alias_hz": out_rate - fold,
                "alias_level_db": round(alias_db, 1),
                "passed": abs(kept_db) <= PASSBAND_TOLERANCE_DB and alias_db <= -MIN_ALIAS_REJECTION_DB,
            }

        refused = {}
        for rate in REFUSED_RATES:
            wav = folder / f"high_{rate}.wav"
            flac = folder / f"high_{rate}.flac"
            run(["sox", "-n", "-b", "24", "-r", str(rate), "-c", "2", str(wav), "synth", "2", "pinknoise", "vol", "0.5"])
            run(["flac", "-s", "-f", "-5", "-o", str(flac), str(wav)])
            report = json.loads(run([str(runner), "--expect-reject", str(flac)]).stdout)
            refused[f"{rate} Hz"] = bool(report.get("rejected") and report.get("lock_balanced"))

    passed = (all(entry["passed"] for entry in files.values()) and
              all(entry["passed"] for entry in tones.values()) and all(refused.values()))
    return {"files": files, "tones": tones, "refused": refused, "all_passed": passed}


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
