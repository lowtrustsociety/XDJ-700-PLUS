#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Host checks for the WAV and AIFF files the bridge serves in place of the stock readers.

The stock readers refuse 88.2 and 96 kHz files and WAV files with the
extensible header (as Bandcamp writes them). The bridge takes exactly those:
the high rates are halved like FLAC at those rates, extensible files at 44.1
and 48 kHz pass through as they are. Every other WAV or AIFF file must be
declined untouched, so the stock reader handles it as before.

The files are written here, byte by byte, so every header form is known:

1. Served, converted: plain and extensible WAV, AIFF, and AIFC ("sowt",
   "in24") at 88.2 and 96 kHz, 16- and 24-bit. Each must give exactly the
   bytes the same audio gives as FLAC (which tools/check_flac_hires.py
   measures), and pass the host runner's seek and split-read tests.
2. Served, passed through: extensible WAV at 44.1 and 48 kHz must come out
   as its own sample bytes.
3. Odd but valid layouts: chunks before the format chunk, an odd-sized chunk,
   the AIFF sound chunk before its format chunk.
4. Declined: plain 44.1 and 48 kHz files, 32-bit, float, 176.4 and
   192 kHz. The adapter must return "declined", leave the file handle where
   it was, take no lock and touch no slot.
5. Mono: FLAC at every rate the bridge serves, WAV and AIFF at 88.2 and
   96 kHz and extensible WAV at 44.1 and 48 kHz, each one channel. The
   bridge hands them out as stereo with the one channel on both sides, so
   each must give exactly the bytes of the same file made stereo that way.
"""

from __future__ import annotations

import argparse
import json
import platform
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER_SOURCE = ROOT / "src" / "flac_adapter_host_runner.c"
ADAPTER_SOURCE = ROOT / "src" / "flac_adapter.c"
DECODER_SOURCE = ROOT / "src" / "flac_decoder.c"

PCM_SUBFORMAT_TAIL = bytes.fromhex("000000001000800000aa00389b71")
SECONDS = 12


def run(command: list[str]) -> subprocess.CompletedProcess:
    outcome = subprocess.run(command, capture_output=True, text=True)
    if outcome.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(command)}\n{outcome.stdout}\n{outcome.stderr}")
    return outcome


def build_runner(header: Path, output: Path) -> None:
    command = [
        "cc", "-std=c11", "-Os", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
        "-DXDJ_HOST_TEST",
        "-DXDJ_MONO",  # as v0.9 on: mono served as stereo
        "-include", str(ROOT / "src" / "host_force_32bit_cache.h"),
        "-I", str(header.parent),
        str(RUNNER_SOURCE), str(ADAPTER_SOURCE), str(DECODER_SOURCE),
        "-o", str(output),
    ]
    command.append("-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections")
    run(command)


def chunk(tag: bytes, body: bytes, big_endian: bool = False) -> bytes:
    size = struct.pack(">I" if big_endian else "<I", len(body))
    return tag + size + body + (b"\x00" if len(body) & 1 else b"")


def wav_file(samples: bytes, rate: int, bits: int, channels: int = 2, extensible: bool = False,
             sub_format: int = 1, format_tag: int = 1, before: bytes = b"", after: bytes = b"") -> bytes:
    """A WAV file. samples are little-endian; before/after are extra chunks around the format chunk."""
    align = channels * bits // 8
    base = struct.pack("<HIIHH", channels, rate, rate * align, align, bits)
    if extensible:
        fmt = (struct.pack("<H", 0xFFFE) + base + struct.pack("<HHI", 22, bits, 3) +
               struct.pack("<H", sub_format) + PCM_SUBFORMAT_TAIL)
    else:
        fmt = struct.pack("<H", format_tag) + base
    body = b"WAVE" + before + chunk(b"fmt ", fmt) + chunk(b"data", samples) + after
    return b"RIFF" + struct.pack("<I", len(body)) + body


def extended(rate: int) -> bytes:
    """An 80-bit big-endian float holding an integer rate, as AIFF stores it."""
    exponent = rate.bit_length() - 1
    mantissa = rate << (63 - exponent)
    return struct.pack(">HQ", 16383 + exponent, mantissa)


def aiff_file(samples_le: bytes, rate: int, bits: int, compression: bytes | None = None,
              sound_first: bool = False, extra: bytes = b"", channels: int = 2) -> bytes:
    """An AIFF file, or AIFC when compression is given. Samples are stored big-endian except for "sowt"."""
    width = bits // 8
    frames = len(samples_le) // (channels * width)
    if compression == b"sowt":
        stored = samples_le
    else:
        stored = b"".join(samples_le[i : i + width][::-1] for i in range(0, len(samples_le), width))
    common = struct.pack(">hIh", channels, frames, bits) + extended(rate)
    if compression is not None:
        common += compression + b"\x00\x00"
    comm = chunk(b"COMM", common, big_endian=True)
    ssnd = chunk(b"SSND", struct.pack(">II", 0, 0) + stored, big_endian=True)
    parts = extra + (ssnd + comm if sound_first else comm + ssnd)
    if compression is not None:
        parts = chunk(b"FVER", struct.pack(">I", 0xA2805140), big_endian=True) + parts
    body = (b"AIFC" if compression is not None else b"AIFF") + parts
    return b"FORM" + struct.pack(">I", len(body)) + body


def check(header: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="xdj700-pcm-hires-") as folder_name:
        folder = Path(folder_name)
        runner = folder / "runner"
        build_runner(header, runner)

        def samples_for(rate: int, bits: int) -> bytes:
            raw = folder / f"noise_{rate}_{bits}.raw"
            if not raw.exists():
                run(["sox", "-n", "-b", str(bits), "-r", str(rate), "-c", "2", "-e", "signed", "-L",
                     "-t", "raw", str(raw), "synth", str(SECONDS), "pinknoise", "vol", "0.5"])
            return raw.read_bytes()

        def dump(path: Path) -> bytes:
            out = folder / "dump.pcm"
            run([str(runner), "--dump-pcm", str(path), str(out)])
            return out.read_bytes()

        def test(path: Path) -> dict:
            outcome = subprocess.run([str(runner), "--test", str(path)], capture_output=True, text=True)
            try:
                return json.loads(outcome.stdout)
            except json.JSONDecodeError:
                return {"success": False, "error": outcome.stderr.strip()}

        served = {}
        for rate, bits in ((88200, 16), (96000, 24), (96000, 16), (88200, 24)):
            samples = samples_for(rate, bits)
            plain = folder / f"plain_{rate}_{bits}.wav"
            plain.write_bytes(wav_file(samples, rate, bits))
            # The same audio as FLAC is the reference: one conversion, two ways in.
            flac = folder / f"reference_{rate}_{bits}.flac"
            run(["flac", "-s", "-f", "-5", "-o", str(flac), str(plain)])
            reference = dump(flac)
            variants = {
                "WAV": plain.read_bytes(),
                "WAV, extensible header": wav_file(samples, rate, bits, extensible=True),
                "WAV, chunks around the format (one odd-sized)": wav_file(
                    samples, rate, bits, before=chunk(b"JUNK", bytes(27)) + chunk(b"bext", bytes(602)),
                    after=chunk(b"LIST", b"INFO" + chunk(b"INAM", b"noise"))),
                "AIFF": aiff_file(samples, rate, bits),
                "AIFF, sound before format, extra chunk": aiff_file(
                    samples, rate, bits, sound_first=True, extra=chunk(b"NAME", b"noise", big_endian=True)),
                "AIFC NONE": aiff_file(samples, rate, bits, compression=b"NONE"),
            }
            if bits == 16:
                variants["AIFC sowt (little-endian)"] = aiff_file(samples, rate, bits, compression=b"sowt")
            else:
                variants["AIFC in24"] = aiff_file(samples, rate, bits, compression=b"in24")
            for name, content in variants.items():
                suffix = ".wav" if name.startswith("WAV") else ".aiff"
                path = folder / f"case{suffix}"
                path.write_bytes(content)
                report = test(path)
                fmt = report.get("format", {})
                same = dump(path) == reference
                served[f"{rate} Hz {bits}-bit {name}"] = {
                    "passed": bool(
                        report.get("success") and fmt.get("sample_rate") == rate // 2 and
                        fmt.get("bits_per_sample") == bits and
                        fmt.get("total_frames") == len(samples) // (bits // 4) // 2 and
                        report["seek_tests"]["passed"] and report["seek_tests"]["byte_exact"] and same
                    ),
                    "equals_flac_path": same,
                    "output_rate": fmt.get("sample_rate"),
                    "error": report.get("error"),
                }

        passed_through = {}
        for rate, bits in ((44100, 16), (48000, 24), (44100, 24), (48000, 16)):
            samples = samples_for(rate, bits)
            path = folder / "case.wav"
            path.write_bytes(wav_file(samples, rate, bits, extensible=True))
            report = test(path)
            fmt = report.get("format", {})
            same = dump(path) == samples
            passed_through[f"{rate} Hz {bits}-bit WAV, extensible header"] = {
                "passed": bool(
                    report.get("success") and fmt.get("sample_rate") == rate and
                    fmt.get("bits_per_sample") == bits and report["seek_tests"]["passed"] and
                    report["seek_tests"]["byte_exact"] and same
                ),
                "equals_file_samples": same,
                "error": report.get("error"),
            }

        declined = {}
        short16 = samples_for(44100, 16)[: 44100 * 4]
        short24 = samples_for(48000, 24)[: 48000 * 6]
        hi16 = samples_for(96000, 16)[: 96000 * 4]
        stock_cases = {
            "44.1 kHz 16-bit WAV": (".wav", wav_file(short16, 44100, 16)),
            "48 kHz 24-bit WAV": (".wav", wav_file(short24, 48000, 24)),
            "44.1 kHz 16-bit AIFF": (".aiff", aiff_file(short16, 44100, 16)),
            "48 kHz 24-bit AIFC NONE": (".aiff", aiff_file(short24, 48000, 24, compression=b"NONE")),
            "96 kHz 32-bit WAV": (".wav", wav_file(hi16, 96000, 32)),
            "96 kHz float WAV (extensible)": (".wav", wav_file(hi16, 96000, 32, extensible=True, sub_format=3)),
            "96 kHz float WAV (format 3)": (".wav", wav_file(hi16, 96000, 32, format_tag=3)),
            "192 kHz 16-bit WAV": (".wav", wav_file(hi16, 192000, 16)),
            "176.4 kHz 16-bit AIFF": (".aiff", aiff_file(hi16, 176400, 16)),
            "96 kHz AIFC with a compressed form": (".aiff", aiff_file(hi16, 96000, 16, compression=b"ulaw")),
            "not a WAV at all": (".wav", b"this is not a RIFF file, only some text" * 20),
            "a WAV cut off inside its header": (".wav", wav_file(hi16, 96000, 16)[:30]),
        }
        for name, (suffix, content) in stock_cases.items():
            path = folder / f"case{suffix}"
            path.write_bytes(content)
            outcome = subprocess.run([str(runner), "--expect-decline", str(path)], capture_output=True, text=True)
            try:
                report = json.loads(outcome.stdout)
            except json.JSONDecodeError:
                report = {"error": outcome.stderr.strip()}
            report.pop("file", None)
            report["passed"] = outcome.returncode == 0
            declined[name] = report

        mono = {}

        def mono_samples(rate: int, bits: int) -> bytes:
            raw = folder / f"mono_{rate}_{bits}.raw"
            if not raw.exists():
                run(["sox", "-n", "-b", str(bits), "-r", str(rate), "-c", "1", "-e", "signed", "-L",
                     "-t", "raw", str(raw), "synth", "4", "pinknoise", "vol", "0.5"])
            return raw.read_bytes()

        def doubled(samples: bytes, bits: int) -> bytes:
            """The same audio as stereo, the one channel on both sides."""
            width = bits // 8
            return b"".join(samples[i : i + width] * 2 for i in range(0, len(samples), width))

        def flac_of(wav: bytes, name: str) -> Path:
            source = folder / f"{name}.wav"
            source.write_bytes(wav)
            flac = folder / f"{name}.flac"
            run(["flac", "-s", "-f", "-5", "-o", str(flac), str(source)])
            return flac

        for rate, bits in ((44100, 16), (48000, 24), (88200, 16), (96000, 24)):
            samples = mono_samples(rate, bits)
            stereo = doubled(samples, bits)
            # The reference: the same audio made stereo, through the same bridge.
            reference = dump(flac_of(wav_file(stereo, rate, bits), f"stereo_{rate}_{bits}"))
            cases = {"FLAC": flac_of(wav_file(samples, rate, bits, channels=1), f"mono_{rate}_{bits}")}
            if rate in (88200, 96000):
                wav = folder / f"mono_{rate}_{bits}_plain.wav"
                wav.write_bytes(wav_file(samples, rate, bits, channels=1))
                aiff = folder / f"mono_{rate}_{bits}.aiff"
                aiff.write_bytes(aiff_file(samples, rate, bits, channels=1))
                aifc = folder / f"mono_{rate}_{bits}_c.aiff"
                aifc.write_bytes(aiff_file(samples, rate, bits, channels=1,
                                           compression=b"sowt" if bits == 16 else b"in24"))
                cases.update({"WAV": wav, "AIFF": aiff, "AIFC": aifc})
            else:
                extensible = folder / f"mono_{rate}_{bits}_ext.wav"
                extensible.write_bytes(wav_file(samples, rate, bits, channels=1, extensible=True))
                cases["WAV, extensible header"] = extensible
            for name, path in cases.items():
                report = test(path)
                fmt = report.get("format", {})
                got = dump(path)
                # An extensible WAV at 44.1 or 48 kHz passes through: its samples, made stereo.
                same = got == (stereo if name == "WAV, extensible header" else reference)
                mono[f"{rate} Hz {bits}-bit mono {name}"] = {
                    "passed": bool(
                        report.get("success") and fmt.get("bits_per_sample") == bits and
                        fmt.get("channels") == 2 and report["seek_tests"]["passed"] and
                        report["seek_tests"]["byte_exact"] and same
                    ),
                    "same_as_stereo": same,
                    "output_rate": fmt.get("sample_rate"),
                    "error": report.get("error"),
                }

    passed = (all(entry["passed"] for entry in served.values()) and
              all(entry["passed"] for entry in passed_through.values()) and
              all(entry["passed"] for entry in declined.values()) and
              all(entry["passed"] for entry in mono.values()))
    return {"served_converted": served, "served_as_is": passed_through, "declined": declined, "mono": mono,
            "all_passed": passed}


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
