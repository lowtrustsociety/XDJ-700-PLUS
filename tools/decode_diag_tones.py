#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Decode the diagnostic build's audible episode reports from a recording.

Each report (src/flac_adapter.c, xdj_encode_report) is two
4 kHz marker tones and then 27 field tones at 400 Hz + 40 Hz per unit. Tones
last 60 ms with 20 ms gaps, and the music is muted meanwhile. A pitch fader
away from zero scales frequencies and durations alike; the measured marker
frequency corrects both.

Frequencies come from zero crossings, interpolated between samples and
measured over whole cycles, so no FFT library is needed.
"""

from __future__ import annotations

import argparse
from array import array
from bisect import bisect_left, bisect_right
import json
from pathlib import Path
import statistics
import subprocess


RATE = 16000
MARK_HZ = 4000.0
BASE_HZ = 400.0
STEP_HZ = 40.0
FIELDS = 27
SYMBOL_S = 0.080
TONE_S = 0.060
WINDOW_S = 0.020
HOP_S = 0.005
MIN_MARK_WINDOWS = 5
MAX_IRREGULARITY = 0.08

FIELD_NAMES = [
    "sequence", "kind", "wall", "first_read", "busy", "reopen", "flac_seek",
    "decode", "fs_read", "fs_seek", "wait", "calls", "fs_reads",
    "physical_seeks", "probes", "max_wall", "failure", "seek_failure",
    "fs_errors", "fs_error", "fs_operation", "positions", "sequence_breaks", "stale_calls",
    "unreadable_size", "eof_ends", "checksum",
]
KINDS = {1: "jump", 2: "open", 3: "fail"}
FLAG_FALLBACK = 4
FLAG_CACHE_HIT = 8
FLAG_NO_SIZE = 16
FLAG_FAILED = 32
FAILURES = {0: "", 1: "read", 2: "seek", 3: "reopen", 4: "opening"}
SEEK_FAILURES = {
    0: "", 1: "bounds", 2: "first frame", 3: "decode", 4: "span", 5: "probes", 6: "io", 7: "verify",
}
SEEK_UNKNOWN = 16
# Parser wrapper operation codes stored with the error (file context +36).
OPERATIONS = {0: "", 3: "seek", 4: "read", 7: "tell", 9: "size"}
TIMES = {"wall", "first_read", "busy", "reopen", "flac_seek", "decode", "fs_read", "fs_seek", "wait", "max_wall"}
COUNTS = {"calls", "fs_reads", "physical_seeks", "probes", "fs_errors", "sequence_breaks", "stale_calls", "unreadable_size", "eof_ends"}


def time_ms(value: int) -> int:
    """10 ms steps up to 310 ms, then 80 ms steps from 320 ms; 63 means 2,800 ms or more."""
    return value * 10 if value < 32 else 320 + (value - 32) * 80


def count(value: int) -> int:
    """Exact up to 31, then steps of 8 from 32; 63 means 280 or more."""
    return value if value < 32 else 32 + (value - 32) * 8


def load_samples(path: Path) -> array:
    result = subprocess.run(
        [
            "ffmpeg", "-v", "error", "-i", str(path), "-ac", "1", "-ar", str(RATE),
            "-f", "s16le", "-acodec", "pcm_s16le", "-",
        ],
        check=True,
        capture_output=True,
    )
    samples = array("h")
    samples.frombytes(result.stdout)
    return samples


def zero_crossings(samples: array) -> array:
    """Upward zero crossings, in fractional sample positions."""
    crossings = array("d")
    previous = samples[0] if samples else 0
    for index in range(1, len(samples)):
        current = samples[index]
        if previous < 0 <= current:
            crossings.append(index - 1 + previous / (previous - current))
        previous = current
    return crossings


def tone(crossings: array, start: float, end: float) -> tuple[float, float] | None:
    """Frequency and irregularity of the tone between two sample positions."""
    first = bisect_left(crossings, start)
    last = bisect_right(crossings, end)
    if last - first < 4:
        return None
    periods = [crossings[index + 1] - crossings[index] for index in range(first, last - 1)]
    mean = sum(periods) / len(periods)
    if mean <= 0:
        return None
    irregularity = statistics.pstdev(periods) / mean
    return RATE / mean, irregularity


def marker_runs(crossings: array, total: int) -> list[tuple[float, float]]:
    """Runs of windows holding a steady tone near 4 kHz: (start seconds, frequency)."""
    window = WINDOW_S * RATE
    hop = HOP_S * RATE
    runs = []
    current: list[tuple[float, float]] = []
    position = 0.0
    while position + window <= total:
        found = tone(crossings, position, position + window)
        if found is not None and 3500.0 <= found[0] <= 4600.0 and found[1] <= MAX_IRREGULARITY:
            current.append((position / RATE, found[0]))
        else:
            if len(current) >= MIN_MARK_WINDOWS:
                runs.append((current[0][0], statistics.median(f for _, f in current)))
            current = []
        position += hop
    if len(current) >= MIN_MARK_WINDOWS:
        runs.append((current[0][0], statistics.median(f for _, f in current)))
    return runs


SNAPSHOT_MARK = 63
SNAPSHOT_REASONS = {1: "baseline", 2: "not a FLAC track", 3: "size unreadable", 4: "zeroed handle"}


def u32(fields: list[int]) -> int:
    value = 0
    for field in fields:
        value = (value << 6) | field
    return value & 0xFFFFFFFF


def describe_snapshot(fields: list[int]) -> dict:
    """A file snapshot (kind field 63): the handle a call read and the file expected."""
    flags = fields[24]
    return {
        "raw": fields,
        "snapshot": True,
        "number": fields[0],
        "reason": SNAPSHOT_REASONS.get(fields[2], str(fields[2])),
        "fs_type": fields[3],
        "device_byte": (fields[4] << 6) | fields[5],
        "call_file_bytes": u32(fields[6:12]),
        "expected_file_bytes": u32(fields[12:18]),
        "context_data_bytes": u32(fields[18:24]),
        "size_known": bool(flags & 1),
        "expected_a_track": bool(flags & 2),
        "expected_evicted": bool(flags & 4),
        "tracks_with_call_size": fields[25],
        "checksum_ok": (sum(fields[:-1]) & 63) == fields[-1],
    }


def describe(fields: list[int]) -> dict:
    if len(fields) > 1 and fields[1] == SNAPSHOT_MARK:
        return describe_snapshot(fields)
    report = {"raw": fields}
    for name, value in zip(FIELD_NAMES, fields):
        if name in TIMES:
            report[name + "_ms"] = time_ms(value)
        elif name in COUNTS:
            report[name] = count(value)
        elif name == "kind":
            report[name] = KINDS.get(value & 3, str(value & 3))
            report["fallback"] = bool(value & FLAG_FALLBACK)
            report["cache_hit"] = bool(value & FLAG_CACHE_HIT)
            report["no_size"] = bool(value & FLAG_NO_SIZE)
            report["failed"] = bool(value & FLAG_FAILED)
        elif name == "failure":
            report[name] = FAILURES.get(value, str(value))
        elif name == "seek_failure":
            if value == SEEK_UNKNOWN:
                report[name] = "unknown"
            else:
                reason = SEEK_FAILURES.get(value & 7, str(value & 7))
                report[name] = reason + (" + fallback" if value & 8 else "")
        elif name == "fs_error":
            report[name] = -value
        elif name == "fs_operation":
            report[name] = OPERATIONS.get(value, str(value))
        elif name == "positions":
            # Handle found elsewhere after our seek (low 3 bits) and after our read (next 3), capped at 7.
            report["misplaced"] = value & 7
            report["moved"] = value >> 3
        else:
            report[name] = value
    report["checksum_ok"] = (sum(fields[:-1]) & 63) == fields[-1]
    report["capped"] = [name for name, value in zip(FIELD_NAMES[:-1], fields[:-1]) if value == 63 and name != "sequence"]
    return report


def decode_samples(samples: array) -> list[dict]:
    crossings = zero_crossings(samples)
    runs = marker_runs(crossings, len(samples))
    reports = []
    index = 0
    skip_until = -1.0
    while index + 1 < len(runs):
        first, second = runs[index], runs[index + 1]
        if first[0] < skip_until:
            index += 1
            continue
        scale = (first[1] + second[1]) / 2.0 / MARK_HZ
        spacing = second[0] - first[0]
        if not (abs(first[1] - second[1]) < 60.0 and abs(spacing * scale - SYMBOL_S) < 0.015):
            index += 1
            continue
        period = SYMBOL_S / scale
        fields = []
        valid = True
        for symbol in range(FIELDS):
            start = second[0] + (symbol + 1) * period
            found = tone(
                crossings,
                (start + 0.012 / scale) * RATE,
                (start + (TONE_S - 0.012) / scale) * RATE,
            )
            if found is None or found[1] > MAX_IRREGULARITY:
                valid = False
                fields.append(-1)
                continue
            value = round((found[0] / scale - BASE_HZ) / STEP_HZ)
            if not 0 <= value <= 63:
                valid = False
            fields.append(value)
        report = {"time_s": round(first[0], 3), "pitch_percent": round((scale - 1.0) * 100.0, 2)}
        if valid:
            report.update(describe(fields))
        else:
            report.update({"fields": fields, "checksum_ok": False, "unreadable": True})
        reports.append(report)
        skip_until = second[0] + (FIELDS + 1) * period
        index += 2
    return reports


def decode(path: Path) -> list[dict]:
    return decode_samples(load_samples(path))


def summary_line(report: dict) -> str:
    if report.get("unreadable"):
        return f"{report['time_s']:9.3f}s  unreadable fields {report['fields']}"
    if report.get("snapshot"):
        flag = "" if report["checksum_ok"] else "  CHECKSUM BAD"
        return (
            f"{report['time_s']:9.3f}s  SNAPSHOT {report['number']} {report['reason']}: fs type {report['fs_type']} "
            f"device {report['device_byte']}  call file {report['call_file_bytes']} bytes "
            f"(known {report['size_known']})  expected {report['expected_file_bytes']} "
            f"(track {report['expected_a_track']}, evicted {report['expected_evicted']})  "
            f"context data {report['context_data_bytes']}  tracks with that size {report['tracks_with_call_size']}{flag}"
        )
    flag = "" if report["checksum_ok"] else "  CHECKSUM BAD"
    capped = f"  capped: {','.join(report['capped'])}" if report["capped"] else ""
    notes = ("  FALLBACK" if report["fallback"] else "") + ("  cache" if report["cache_hit"] else "")
    notes += "  NO SIZE" if report["no_size"] else ""
    if report["failed"]:
        notes += f"  FAILED {report['failure']}"
        notes += f" (seek: {report['seek_failure']})" if report["seek_failure"] else ""
    if report["fs_errors"]:
        notes += f"  fs errors {report['fs_errors']} last {report['fs_error']} on {report['fs_operation'] or '?'}"
    if report["misplaced"] or report["moved"]:
        notes += f"  HANDLE misplaced {report['misplaced']} moved {report['moved']}"
    if report["stale_calls"]:
        notes += f"  stale {report['stale_calls']}"
    if report["unreadable_size"]:
        notes += f"  unreadable size {report['unreadable_size']}"
    if report["eof_ends"]:
        notes += f"  ended {report['eof_ends']}"
    if report["sequence_breaks"]:
        notes += f"  FRAME BREAKS {report['sequence_breaks']}"
    if report["seek_failure"] and not report["failed"]:
        notes += f"  search gave up: {report['seek_failure']}"
    return (
        f"{report['time_s']:9.3f}s  #{report['sequence']:2d} {report['kind']:4s} "
        f"wall {report['wall_ms']:4d} ms  first read {report['first_read_ms']:4d}  "
        f"busy {report['busy_ms']:4d}  reopen {report['reopen_ms']:4d}  "
        f"seek {report['flac_seek_ms']:4d}  decode {report['decode_ms']:4d}  "
        f"fs read {report['fs_read_ms']:4d}  fs seek {report['fs_seek_ms']:4d}  "
        f"wait {report['wait_ms']:4d}  calls {report['calls']:3d}  reads {report['fs_reads']:3d}  "
        f"phys seeks {report['physical_seeks']:3d}  probes {report['probes']:3d}  "
        f"max {report['max_wall_ms']:4d}{notes}{flag}{capped}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("recording", type=Path)
    parser.add_argument("--json", action="store_true", help="print JSON instead of one line per report")
    args = parser.parse_args()
    reports = decode(args.recording)
    if args.json:
        print(json.dumps(reports, indent=2))
        return
    for report in reports:
        print(summary_line(report))
    print(f"{len(reports)} reports")


if __name__ == "__main__":
    main()
