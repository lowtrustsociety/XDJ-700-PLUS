#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Host check of FLAC decoded on the audio processor, both sides together.

src/chip_flac_host_runner.c plays the player's part: it reads FLAC
files through the main processor's feed (tags out, FLAC bytes into a ring)
and gives the tags, cut into units, to the audio processor's converter.
This compares every sample the converter wrote with the reference decoder
(`flac -d`), for: a whole track read from start to end; jumps all over a
track; two tracks by turns; a mono track (each sample on both sides),
alone, with jumps and by turns with a stereo one; a track long enough for its tag numbers to run
out and start again; and plain audio, which the converter must leave to the
stock converter. The tags travel at several delays, in units of 588 and 640
frames.

Then the same with the bridge itself in front
(src/flac_adapter_chip_runner.c with the bridge and the loader built
for --chip-flac): a 96 kHz file is never fed and loads nothing; the first
FLAC open that can be fed puts our block in place; 16-bit and 24-bit files
are fed as 16-bit streams, and so is a mono file; fed tracks that lose their slot are opened again
as fed tracks; when the audio processor is started over (its table holds the
stock entry again) no tag is handed out any more and the bridge decodes the
fed tracks, still as 16-bit streams, until the next open puts the block
back. And a load that fails (the table not holding the stock entry, or a
word that does not read back) is not tried again: every track is then
decoded by the bridge at its own depth.

Then all of it again built for hi-res on the audio processor (XDJ_CHIP_HIRES
with XDJ_CHIP_FAST): 88.2 and 96 kHz tracks,
16- and 24-bit and mono, whole, with jumps, by turns with a 44.1 kHz one,
long enough for the tag numbers to start again, must give every frame as
the bridge's own halving does; and through the bridge, a 96 kHz file is fed
at half the rate, put aside and opened again, decoded by the bridge when the
audio processor is started over, and fed again after. And with
XDJ_CHIP_RESTART, a very fast jog: the units of many
reads never reach the converter, the next lie far ahead in the stream; the
converter must ask for a new stream, write silence meanwhile, and be right
again within a few units, at 44.1, 88.2 and 96 kHz; also when the player has
read far ahead and jumps, or gaps come close together (with XDJ_CHIP_WITHDRAW,
5.81). Then the hi-res part again with XDJ_CHIP_AHEAD (5.96), the next FLAC
block decoded ahead: every sample the same, and the blocks mostly whole
before they are needed.

And the loader alone (src/chip_flac_loader.c, run by
src/chip_flac_loader_host_runner.c against a pretend audio
processor), as the release builds it: with nothing but the load.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
import platform
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROTOTYPES = ROOT / "src"
SOURCES = ["chip_flac_host_runner.c", "chip_flac.c", "chip_flac_feed.c", "flac_decoder.c"]
TAIL_FRAMES = 8
LOADER_CASES = ("load", "stuck", "tablestuck", "nostock", "nomark", "badsum", "bigblock", "bigclips")
# What the release's loader may call, and all it may offer: the port, a turn for the other tasks, the
# appended block, the clock; and the load.
LOADER_CALLS = {"xdj_host_dsp_read", "xdj_host_dsp_write", "xdj_host_dsp_write_words", "xdj_host_delay",
                "xdj_host_chip_blob", "xdj_host_ms"}
# What a computer's compiler may make of a plain loop that fills or copies memory
# (Apple's clang turns the loader's zero-filling loop into bzero); not calls of the loader's own.
HOST_COMPILER_CALLS = {"bzero", "memset", "memcpy", "__bzero", "__memset_chk", "__memcpy_chk"}


def run(command: list[str], **more) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, **more)
    if result.returncode != 0:
        detail = result.stderr if isinstance(result.stderr, str) else result.stderr.decode(errors="replace")
        raise RuntimeError(f"command failed: {' '.join(command)}\n{detail.strip()}")
    return result


def synthetic_clips(folder: Path, mono: bool = False) -> list[Path]:
    """Two clips of made-up sound, the same on every run: for the host tests. With mono, a third, mono."""
    clips = []
    shapes = [(44100, 16, 30000, 2), (96000, 24, 26000, 2)] + ([(48000, 16, 21000, 1)] if mono else [])
    for index, (rate, bits, frames, channels) in enumerate(shapes):
        peak = (1 << (bits - 1)) - 1
        state = 12345 + index
        raw = bytearray()
        for frame in range(frames):
            for channel in range(channels):
                state = (state * 1103515245 + 12345) & 0x7FFFFFFF
                noise = (state >> 8) / float(1 << 23) - 0.5
                value = (
                    0.45 * math.sin(frame * (220.0 + 110.0 * channel) * 2.0 * math.pi / rate)
                    + 0.25 * math.sin(frame * 3520.0 * 2.0 * math.pi / rate + channel)
                    + 0.2 * noise * (1.0 + math.sin(frame / 900.0)) / 2.0
                )
                raw += int(value * peak).to_bytes(bits // 8, "little", signed=True)
        clip = folder / f"clip{index}.flac"
        run(
            ["flac", "--totally-silent", "-8", "-f", "--force-raw-format", "--endian=little", "--sign=signed",
             f"--channels={channels}", f"--bps={bits}", f"--sample-rate={rate}", "-o", str(clip), "-"],
            input=bytes(raw),
        )
        clips.append(clip)
    return clips


def reference_samples(clip: Path) -> tuple[int, int, int, bytes]:
    """Rate, bits, channels and the raw little-endian samples, from the reference decoder."""
    info = run(
        ["metaflac", "--show-sample-rate", "--show-bps", "--show-channels", "--show-total-samples", str(clip)], text=True
    ).stdout.split()
    rate, bits, channels, frames = (int(value) for value in info)
    raw = run(["flac", "--totally-silent", "-d", "-c", "--force-raw-format", "--endian=little", "--sign=signed", str(clip)]).stdout
    if len(raw) != frames * channels * (bits // 8):
        raise ValueError(f"{clip.name}: the reference decoder gave an unexpected length")
    return rate, bits, channels, raw


def as_stereo(raw: bytes, width: int, channels: int) -> bytes:
    """A mono track as the player gets it from us: each sample on both sides."""
    if channels == 2:
        return raw
    return b"".join(raw[i : i + width] * 2 for i in range(0, len(raw), width))

def long_clip(folder: Path) -> Path:
    """Thirty seconds of noise and tones at 44.1 kHz 16-bit: more frames than one run of tag numbers holds."""
    clip = folder / "long.flac"
    subprocess.run(
        ["sox", "-R", "-n", "-r", "44100", "-b", "16", "-c", "2", str(clip), "synth", "30", "pinknoise",
         "synth", "30", "sine", "mix", "330", "gain", "-6"],
        check=True, capture_output=True,
    )
    return clip


def reference_floats(clip: Path) -> tuple[int, bytes]:
    """The track as the converter must write it: each sample as a 32-bit float fraction; silence at the very end."""
    rate, bits, channels, raw = reference_samples(clip)
    width = bits // 8
    raw = as_stereo(raw, width, channels)
    channels = 2
    frames = len(raw) // (width * channels)
    scale = float(1 << (bits - 1))
    values = [int.from_bytes(raw[i : i + width], "little", signed=True) / scale for i in range(0, len(raw), width)]
    for index in range((frames - TAIL_FRAMES) * channels, frames * channels):
        values[index] = 0.0
    return frames, struct.pack(f"<{len(values)}f", *values)


def run_case(runner: Path, folder: Path, name: str, mode: str, delay: int, unit: int, clips: list[Path], references,
             least_played: float | None = None) -> dict:
    """With least_played, a piece may be silence (never other sound) as long as that share of them is right:
    FLAC with blocks of 16,384 frames, jumped about with the tags late, outruns the ring."""
    out = folder / f"{name}.out"
    outcome = subprocess.run(
        [str(runner), mode, str(delay), str(unit), str(out), *(str(clip) for clip in clips)], capture_output=True, text=True
    )
    if not outcome.stdout.strip():
        return {"passed": False, "stderr": outcome.stderr.strip()}
    report = json.loads(outcome.stdout)
    data = out.read_bytes()
    position = 0
    compared = 0
    wrong = 0
    silent = 0
    pieces = 0
    while position < len(data):
        track, start, frames = struct.unpack_from("<3I", data, position)
        position += 12
        got = data[position : position + frames * 8]
        position += frames * 8
        if mode != "plain":
            total, expected = references[track]
            want = expected[start * 8 : (start + frames) * 8]
            want += bytes(frames * 8 - len(want))  # past the track's end: silence
            if got != want:
                if least_played is not None and got == bytes(len(got)):
                    silent += 1
                else:
                    wrong += 1
        pieces += 1
        compared += frames
    status = report["status"]
    checks = {
        "no fault seen by the runner": outcome.returncode == 0 and report["faults"] == 0,
        "every piece the reference decoder's samples": wrong == 0 and compared == report["frames_out"] and compared > 0,
        "nothing missing on the audio processor's side": (
            status["no_record"] == 0 and status["lost"] == 0 and status["short_reads"] == 0 and status["open_failures"] == 0
        ) if least_played is None else (silent <= pieces * (1.0 - least_played)),
    }
    if mode == "plain":
        checks["plain audio left to the stock converter"] = report["units_ours"] == 0 and report["units_stock"] == report["units"]
    else:
        checks["units of ours"] = report["units_ours"] > 0 and status["frames"] > 0
    if mode == "units":
        # The ring never runs far ahead of where the decoder reads (it did, 4 MB, before the fix of 5.66).
        checks["the ring stays near the decoder"] = report["most_ahead"] < 128 * 1024
    if "ahead" in report and mode in ("seq", "units") and report["ahead"]["ready"] + report["ahead"]["late"] > 0:
        # Built with XDJ_CHIP_AHEAD: in plain play the next block is mostly whole before it is needed (blocks of
        # 16,384 frames leave no room for a second buffer: decoded when needed, as before).
        checks["blocks decoded ahead"] = report["ahead"]["ready"] > report["ahead"]["late"]
    if mode == "seq":
        track = report["tracks"][0]
        # One stream for the whole track, and the file read hardly more than once.
        checks["one stream, read once"] = (
            status["opens"] == 1 and track["locates"] == 1 and track["file_bytes_read"] <= track["file_bytes"] + 64
        )
    return {"passed": all(checks.values()), "checks": checks, "frames": compared,
            "opens": status["opens"], "skips": status["skips"], "silent_pieces": silent, "pieces": pieces,
            **({"places_log": report["places_log"]} if "places_log" in report else {}),
            **({"ahead": report["ahead"]} if "ahead" in report else {})}


def tone_clip(folder: Path, name: str, rate: int, bits: int, seconds: float, pitch: int, channels: int = 2) -> Path:
    clip = folder / name
    subprocess.run(
        ["sox", "-R", "-n", "-r", str(rate), "-b", str(bits), "-c", str(channels), str(clip), "synth", str(seconds),
         "pinknoise", "synth", str(seconds), "sine", "mix", str(pitch), "gain", "-6"],
        check=True, capture_output=True,
    )
    return clip


def bridge_references(clips: list[Path]) -> list[dict]:
    """For each clip: its samples as fractions at full depth (what our converter writes, the last frames
    silent), at 16 bits (what the stock converter writes from a 16-bit stream the bridge decoded), and raw."""
    references = []
    for clip in clips:
        rate, bits, channels, raw = reference_samples(clip)
        width = bits // 8
        raw = as_stereo(raw, width, channels)
        channels = 2
        frames = len(raw) // (channels * width)
        samples = [int.from_bytes(raw[i : i + width], "little", signed=True) for i in range(0, len(raw), width)]
        full = [value / float(1 << (bits - 1)) for value in samples]
        for position in range((frames - TAIL_FRAMES) * channels, frames * channels):
            full[position] = 0.0
        # dr_flac takes a 24-bit sample to 16 bits by dropping its low byte.
        short = [(value >> (bits - 16)) / 32768.0 for value in samples]
        references.append({"frames": frames, "full": struct.pack(f"<{len(full)}f", *full),
                           "short": struct.pack(f"<{len(short)}f", *short), "raw": raw})
    return references


def compare_bridge_output(data: bytes, references: list[dict], skip: set[int], fed_tails: bool) -> dict:
    """Every piece the runner wrote against the references. In the last frames of a track that was fed,
    the stock converter may have been given the feed's silence or the bridge's samples: both are right."""
    position = 0
    wrong = 0
    pieces: dict[tuple[int, int], int] = {}
    while position < len(data):
        which, form, start, frames = struct.unpack_from("<4I", data, position)
        position += 16
        size = frames * (6 if form == 1 else 8)
        got = data[position : position + size]
        position += size
        pieces[(which, form)] = pieces.get((which, form), 0) + 1
        if which in skip:
            continue
        reference = references[which]
        if form == 1:
            want = reference["raw"][start * 6 :][:size]
            want += bytes(size - len(want))
            wrong += got != want
            continue
        want = (reference["full"] if form == 0 else reference["short"])[start * 8 :][:size]
        want += bytes(size - len(want))
        if got == want:
            continue
        tail = max(0, reference["frames"] - TAIL_FRAMES - start) * 8
        if (form == 2 and fed_tails and tail < size and got[:tail] == want[:tail] and
                all(got[i : i + 8] in (want[i : i + 8], bytes(8)) for i in range(tail, size, 8))):
            continue
        wrong += 1
    return {"wrong": wrong, "pieces": pieces}


def nothing_missing(status: dict) -> bool:
    return status["no_record"] == 0 and status["lost"] == 0 and status["short_reads"] == 0 and status["open_failures"] == 0


def check_bridge(folder: Path) -> dict:
    runner = folder / "bridge_runner"
    subprocess.run(
        ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-DXDJ_HOST_TEST",
         "-DXDJ_CHIP_FLAC", "-DXDJ_MONO", "-I", str(PROTOTYPES), "-include", str(PROTOTYPES / "host_force_32bit_cache.h"),
         "-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections",
         *(str(PROTOTYPES / source) for source in (
             "flac_adapter_chip_runner.c", "flac_adapter.c", "chip_flac.c", "chip_flac_feed.c",
             "chip_flac_loader.c", "flac_decoder.c")),
         "-o", str(runner)],
        check=True, capture_output=True, text=True,
    )
    clips = [
        tone_clip(folder, "fed16.flac", 44100, 16, 8.0, 330),
        tone_clip(folder, "fed24.flac", 48000, 24, 7.0, 440),
        tone_clip(folder, "hires.flac", 96000, 24, 3.0, 550),
    ] + [tone_clip(folder, f"more{index}.flac", 44100, 16, 2.0 + 0.3 * index, 600 + 50 * index) for index in range(7)] + [
        tone_clip(folder, "mono.flac", 44100, 16, 3.0, 700, channels=1),
    ]
    mono = len(clips) - 1
    hires = 2  # converted down by the bridge: its sound is the business of other tests
    references = bridge_references(clips)
    results = {}
    for mode in ("lazy", "nostock", "stuck"):
        out = folder / f"bridge_{mode}.out"
        outcome = subprocess.run([str(runner), mode, str(out), *(str(clip) for clip in clips)],
                                 capture_output=True, text=True)
        if not outcome.stdout.strip():
            results[mode] = {"passed": False, "stderr": outcome.stderr.strip()}
            continue
        report = json.loads(outcome.stdout)
        compared = compare_bridge_output(out.read_bytes(), references, {hires}, fed_tails=mode == "lazy")
        pieces = compared["pieces"]
        fed = report["fed"]
        frame_bytes = report["frame_bytes"]
        checks = {
            "no fault seen by the runner": outcome.returncode == 0 and report["faults"] == 0,
            "every piece the reference decoder's samples": (
                compared["wrong"] == 0 and all(any(key[0] == which for key in pieces) for which in range(len(clips)))
            ),
            "no tag ever given to the stock converter": report["tags_to_stock"] == 0,
            "a mono file played as a 16-bit stereo stream, each sample on both sides": frame_bytes[mono] == 4,
        }
        if mode == "lazy":
            checks.update({
                "a 96 kHz file opened first: not fed, nothing sent": (
                    report["port_writes_before_load"] == 0 and report["table_before_load_stock"]
                    and not fed[hires] and frame_bytes[hires] == 6
                ),
                "the first FLAC that can be fed puts our block in place": report["loaded_at_first_open"],
                "16-bit and 24-bit files fed, as 16-bit streams": fed[0] and fed[1] and frame_bytes[0:2] == [4, 4],
                "a mono file fed": fed[mono],
                "tracks put aside come back fed": (
                    all(fed[3:]) and report["units_ours_at_restart"] > report["units_ours_first_round"]
                ),
                "started over: no more tags, the bridge decodes, the stream stays 16-bit": (
                    report["units_ours_after_restart"] == report["units_ours_at_restart"]
                    and report["loads_before_reload"] == 1 and report["frame_bytes_after_restart"] == 4
                    and pieces.get((0, 2), 0) > 0 and pieces.get((1, 2), 0) > 0
                ),
                "a track opened then puts the block back and is fed": (
                    report["loads"] == 2 and report["table_at_end_ours"] and report["units_ours_new_track"] > 0
                ),
                "tracks opened again after that are fed again": report["units_ours_reopened"] > 0,
                "nothing missing on the audio processor's side": (
                    nothing_missing(report["status_before_restart"]) and nothing_missing(report["status"])
                    and report["status_before_restart"]["units_ours"] > 0 and report["status"]["units_ours"] > 0
                ),
            })
        else:
            checks.update({
                "nothing fed": report["units_ours"] == 0 and not any(fed),
                "every stream at its file's depth": frame_bytes[0] == 4 and frame_bytes[1] == 6 and frame_bytes[hires] == 6,
                "the table left as it was": report["table_unchanged"],
            })
            if mode == "nostock":
                checks["nothing written to the audio processor"] = report["port_writes"] == 0 and report["loads"] == 0
            else:
                checks["the load tried once only"] = report["loads"] == 1
        results[mode] = {"passed": all(checks.values()), "checks": checks, "pieces": sum(pieces.values()),
                         "units_ours": report["units_ours"]}
    return {"passed": all(result["passed"] for result in results.values()), "modes": results}


# With the tuned decoder of 5.78, all but its leaving out the CRC-16: here the main processor's side shares the
# one build of the decoder, and its seek checks frames by it (the check of the decoder's forms covers that part).
HIRES_FLAGS = ["-DXDJ_CHIP_HIRES", "-DXDJ_CHIP_FAST", "-DXDJ_FAST_CLZ=1", "-DXDJ_CHIP_RESTART",
               "-DXDJ_SHARED_ROUTINE=DRFLAC_INLINE", "-DXDJ_SPLIT_PREDICTION", "-DXDJ_CHIP_DIRECT",
               "-DXDJ_CHIP_WITHDRAW", "-DXDJ_BIG_BLOCKS"]


def run_skip_case(runner: Path, folder: Path, name: str, delay: int, unit: int, clip: Path, reference,
                  mode: str = "skip") -> dict:
    """A very fast jog (mode "skip"): the units after a gap lie far ahead in the stream. Every unit must be
    right or silent, the converter must have asked for new streams, and be right again within a few units."""
    out = folder / f"{name}.out"
    outcome = subprocess.run([str(runner), mode, str(delay), str(unit), str(out), str(clip)], capture_output=True,
                             text=True)
    if not outcome.stdout.strip():
        return {"passed": False, "stderr": outcome.stderr.strip()}
    report = json.loads(outcome.stdout)
    data = out.read_bytes()
    total, expected = reference
    position = 0
    right = silent = wrong = 0
    while position < len(data):
        _, start, frames = struct.unpack_from("<3I", data, position)
        position += 12
        got = data[position : position + frames * 8]
        position += frames * 8
        want = expected[start * 8 : (start + frames) * 8]
        want += bytes(frames * 8 - len(want))
        if got == want:
            right += 1
        elif got == bytes(len(got)):
            silent += 1
        else:
            wrong += 1
    gaps = report["skipped"] // 70 if mode != "farjump" else 3
    checks = {
        "no fault seen by the runner": outcome.returncode == 0 and report["faults"] == 0,
        "every unit right or silent": wrong == 0 and right > 0,
        "new streams asked for": (report["asks"] > 0 and gaps > 0) if mode != "farjump" else True,
        "right again within a few units of each gap": silent <= gaps * (delay + 3),
        "nothing lost": report["status"]["lost"] == 0 and report["status"]["open_failures"] == 0,
    }
    return {"passed": all(checks.values()), "checks": checks, "right": right, "silent": silent, "gaps": gaps,
            "asks": report["asks"], "stuck_units": report["stuck_units"]}


def compile_runner(folder: Path, name: str, sources: list[str], flags: list[str]) -> Path:
    runner = folder / name
    subprocess.run(
        ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-DXDJ_HOST_TEST",
         "-DXDJ_CHIP_FLAC", "-DXDJ_MONO", *flags, "-I", str(PROTOTYPES),
         "-include", str(PROTOTYPES / "host_force_32bit_cache.h"),
         "-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections",
         *(str(PROTOTYPES / source) for source in sources), "-o", str(runner)],
        check=True, capture_output=True, text=True,
    )
    return runner


def halved_values(runner: Path, folder: Path, clip: Path) -> list[int]:
    """The clip as the bridge's own halving gives it, every frame from the start: 24-bit values, both channels."""
    out = folder / (clip.stem + ".halved")
    outcome = subprocess.run([str(runner), "halvedref", "0", "588", str(out), str(clip)], capture_output=True, text=True)
    if outcome.returncode != 0:
        raise RuntimeError(f"halving reference failed: {outcome.stderr.strip()}")
    data = out.read_bytes()
    return list(struct.unpack(f"<{len(data) // 4}i", data))


def halved_floats(values: list[int]) -> tuple[int, bytes]:
    """As our converter must write a halved track: fractions of 2 to the 23, the last frames silent."""
    frames = len(values) // 2
    floats = [value / 8388608.0 for value in values]
    for index in range((frames - TAIL_FRAMES) * 2, frames * 2):
        floats[index] = 0.0
    return frames, struct.pack(f"<{len(floats)}f", *floats)


def big_block_clip(folder: Path, source: Path, name: str) -> Path:
    """The source encoded again with FLAC blocks of 16,384 frames, the largest the usual subset allows above
    48 kHz."""
    clip = folder / name
    subprocess.run(["flac", "--totally-silent", "-f", "-8", "-b", "16384", "-o", str(clip), str(source)],
                   check=True, capture_output=True)
    return clip


def check_hires(folder: Path, more: list[str] = [], tag: str = "") -> dict:
    """With more and tag: the same built with more switches (tag names its files)."""
    flags = HIRES_FLAGS + more
    runner = compile_runner(folder, "hires_runner" + tag, SOURCES, flags)
    short = synthetic_clips(folder, mono=True)
    h96 = short[1]
    h88 = tone_clip(folder, "h88.flac", 88200, 16, 4.0, 500)
    h96mono = tone_clip(folder, "h96mono.flac", 96000, 24, 3.0, 450, channels=1)
    # Long enough at half the rate for the tag numbers to start again (1,048,576 frames).
    long88 = tone_clip(folder, "long88.flac", 88200, 24, 25.0, 300)
    h96big = big_block_clip(folder, tone_clip(folder, "h96src.flac", 96000, 24, 6.0, 470), "h96big.flac")
    h88big = big_block_clip(folder, tone_clip(folder, "h88src.flac", 88200, 16, 6.0, 390), "h88big.flac")
    references = {clip: halved_floats(halved_values(runner, folder, clip))
                  for clip in (h96, h88, h96mono, long88, h96big, h88big)}
    references[short[0]] = reference_floats(short[0])
    cases = {}
    for name, mode, delay, unit, clips in (
        ("whole 44.1 kHz track, sped up", "seq", 2, 588, [short[0]]),
        ("44.1 kHz jumps, sped up", "seeks", 3, 588, [short[0]]),
        ("whole 96 kHz 24-bit track", "seq", 2, 640, [h96]),
        ("whole 88.2 kHz 16-bit track, tags late", "seq", 6, 588, [h88]),
        ("96 kHz jumps", "seeks", 4, 640, [h96]),
        ("88.2 kHz jumps, tags late", "seeks", 9, 588, [h88]),
        ("96 kHz mono, jumps", "seeks", 3, 640, [h96mono]),
        ("96 kHz and 44.1 kHz by turns", "two", 3, 588, [h96, short[0]]),
        ("long 88.2 kHz track: tag numbers start again", "seq", 4, 588, [long88]),
        ("long 88.2 kHz track, one unit at a time", "units", 2, 588, [long88]),
        ("long 88.2 kHz track, jumps", "seeks", 3, 588, [long88]),
        ("96 kHz 24-bit, blocks of 16,384 frames, whole", "seq", 3, 640, [h96big]),
    ):
        cases[name] = run_case(runner, folder, "hires" + tag + "_" + name.replace(" ", "_").replace(",", "").replace(":", ""),
                               mode, delay, unit, clips, [references[clip] for clip in clips])
    # Jumps on blocks of 16,384 frames: each new stream needs a whole block in the ring, and with the tags
    # late older streams are overwritten before they play. Silence then, never other sound (5.83).
    for name, mode, delay, unit, clips, least in (
        ("96 kHz 24-bit, blocks of 16,384 frames, jumps", "seeks", 2, 640, [h96big], 0.9),
        ("88.2 kHz 16-bit, blocks of 16,384 frames, jumps, tags late", "seeks", 9, 588, [h88big], 0.6),
    ):
        cases[name] = run_case(runner, folder, "hires" + tag + "_" + name.replace(" ", "_").replace(",", "").replace(":", ""),
                               mode, delay, unit, clips, [references[clip] for clip in clips], least)
    long44 = long_clip(folder)
    references[long44] = reference_floats(long44)
    h96long = tone_clip(folder, "h96long.flac", 96000, 24, 10.0, 520)
    references[h96long] = halved_floats(halved_values(runner, folder, h96long))
    for name, delay, unit, clip in (
        ("very fast jog: 44.1 kHz, far ahead in the stream", 2, 588, long44),
        ("very fast jog: 44.1 kHz, tags late", 8, 588, long44),
        ("very fast jog: 88.2 kHz, far ahead in the stream", 2, 588, long88),
        ("very fast jog: 96 kHz, tags late", 6, 640, h96long),
    ):
        cases[name] = run_skip_case(runner, folder, "skip" + tag + "_" + str(len(cases)), delay, unit, clip, references[clip])
    # The player reads seconds ahead, so the ring fills up to its limit; then a jump throws the reads ahead away,
    # or two gaps come close together.
    for name, mode, delay, clip in (
        ("far reads, then jumps: 44.1 kHz", "farjump", 15, long44),
        ("far reads, then jumps: 88.2 kHz", "farjump", 15, long88),
        ("far reads, gaps close together: 88.2 kHz", "skipfar", 15, long88),
        ("two gaps close together: 44.1 kHz", "skip2", 3, long44),
    ):
        cases[name] = run_skip_case(runner, folder, "skip" + tag + "_" + str(len(cases)), delay, 588, clip, references[clip], mode)

    bridge_runner = compile_runner(folder, "hires_bridge_runner" + tag, [
        "flac_adapter_chip_runner.c", "flac_adapter.c", "chip_flac.c", "chip_flac_feed.c",
        "chip_flac_loader.c", "flac_decoder.c"], flags)
    clips = [
        tone_clip(folder, "hfed16.flac", 44100, 16, 6.0, 330),
        tone_clip(folder, "hfed24.flac", 48000, 24, 5.0, 440),
        # Blocks of 16,384 frames: fed, and decoded by the bridge itself after the restart.
        big_block_clip(folder, tone_clip(folder, "hhires_src.flac", 96000, 24, 5.0, 550), "hhires.flac"),
    ] + [tone_clip(folder, f"hmore{index}.flac", 44100, 16, 2.0 + 0.3 * index, 600 + 50 * index) for index in range(7)]
    hires = 2
    references_bridge = bridge_references(clips)
    values = halved_values(runner, folder, clips[hires])
    frames, full = halved_floats(values)
    # The bridge's own 16-bit stream at half the rate: the 24-bit value without its low byte.
    short16 = struct.pack(f"<{len(values)}f", *((value >> 8) / 32768.0 for value in values))
    references_bridge[hires] = {"frames": frames, "full": full, "short": short16, "raw": b""}
    out = folder / f"bridge_hires{tag}.out"
    outcome = subprocess.run([str(bridge_runner), "hires", str(out), *(str(clip) for clip in clips)],
                             capture_output=True, text=True)
    if not outcome.stdout.strip():
        return {"passed": False, "cases": cases, "bridge": {"passed": False, "stderr": outcome.stderr.strip()}}
    report = json.loads(outcome.stdout)
    compared = compare_bridge_output(out.read_bytes(), references_bridge, set(), fed_tails=True)
    pieces = compared["pieces"]
    checks = {
        "no fault seen by the runner": outcome.returncode == 0 and report["faults"] == 0,
        "every piece the expected samples": compared["wrong"] == 0 and len(pieces) > 0,
        "no tag ever given to the stock converter": report["tags_to_stock"] == 0,
        "the 96 kHz file's open puts our block in place": report["loaded_at_hires_open"],
        "fed at half the rate as a 16-bit stream": report["fed"][hires] and report["frame_bytes"][hires] == 4
                                                   and report["hires_units_ours_first"] > 0,
        "put aside and opened again, fed": report["hires_units_ours_at_restart"] > report["hires_units_ours_first"],
        "started over: the bridge halves it, a 16-bit stream still": (
            report["hires_units_ours_after_restart"] == report["hires_units_ours_at_restart"]
            and pieces.get((hires, 2), 0) > 0),
        "opened again: the block back, fed again": (
            report["loads"] == 2 and report["hires_frame_bytes_after_restart"] == 4
            and report["hires_units_ours_reloaded"] > 0),
        "nothing missing on the audio processor's side": nothing_missing(report["status"]),
    }
    bridge = {"passed": all(checks.values()), "checks": checks, "pieces": sum(pieces.values())}
    return {"passed": all(case["passed"] for case in cases.values()) and bridge["passed"], "cases": cases,
            "bridge": bridge}


def check_loader(folder: Path, figures: bool = False) -> dict:
    """The loader as the release builds it."""
    name, define = "loader", []
    common = ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-DXDJ_HOST_TEST",
              *define, "-I", str(PROTOTYPES)]
    runner = folder / name
    subprocess.run([*common, *(str(PROTOTYPES / source) for source in ("chip_flac_loader_host_runner.c", "chip_flac_loader.c")),
                    "-o", str(runner)], check=True, capture_output=True, text=True)
    report = {"passed": True, "checks": 0, "cases": {}, "stderr": ""}
    for case in LOADER_CASES:
        outcome = subprocess.run([str(runner), case], capture_output=True, text=True)
        result = json.loads(outcome.stdout) if outcome.stdout.strip() else {"passed": False, "checks": 0}
        report["cases"][case] = result
        report["checks"] += result.get("checks", 0)
        report["passed"] = report["passed"] and outcome.returncode == 0 and bool(result.get("passed"))
        report["stderr"] += outcome.stderr
    # What the object offers and calls: the release's loader has the load and nothing else.
    loader_object = folder / f"{name}.o"
    subprocess.run([*common, "-c", str(PROTOTYPES / "chip_flac_loader.c"), "-o", str(loader_object)],
                   check=True, capture_output=True, text=True)
    offered, called = set(), set()
    for line in run(["nm", "-g", str(loader_object)], text=True).stdout.splitlines():
        parts = line.split()
        symbol = parts[-1][1:] if platform.system() == "Darwin" else parts[-1]
        (called if parts[-2] == "U" else offered).add(symbol)
    report["offered"], report["called"] = sorted(offered), sorted(called)
    report["nothing_but_the_load"] = offered == {"xdj_chip_load"} and called <= LOADER_CALLS | HOST_COMPILER_CALLS
    report["passed"] = report["passed"] and (figures or report["nothing_but_the_load"])
    return report


def check() -> dict:
    with tempfile.TemporaryDirectory(prefix="xdj700-chip-flac-") as folder_name:
        folder = Path(folder_name)
        runner = folder / "runner"
        subprocess.run(
            ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-DXDJ_HOST_TEST",
             "-DXDJ_CHIP_FLAC", "-DXDJ_MONO", "-I", str(PROTOTYPES), "-include", str(PROTOTYPES / "host_force_32bit_cache.h"),
             "-Wl,-dead_strip" if platform.system() == "Darwin" else "-Wl,--gc-sections",
             *(str(PROTOTYPES / source) for source in SOURCES), "-o", str(runner)],
            check=True, capture_output=True, text=True,
        )
        short = synthetic_clips(folder, mono=True)  # 44.1 kHz 16-bit, 96 kHz 24-bit (the depth is what matters here), mono
        long = long_clip(folder)
        references = {clip: reference_floats(clip) for clip in (*short, long)}
        cases = {}
        for name, mode, delay, unit, clips in (
            ("whole track", "seq", 0, 588, [short[0]]),
            ("whole track, tags late", "seq", 5, 588, [short[0]]),
            ("whole 24-bit track, units of 640", "seq", 2, 640, [short[1]]),
            ("jumps", "seeks", 2, 588, [short[0]]),
            ("jumps, tags late", "seeks", 9, 588, [short[1]]),
            ("two tracks by turns", "two", 3, 588, short[:2]),
            ("mono track", "seq", 3, 588, [short[2]]),
            ("mono track, jumps", "seeks", 4, 640, [short[2]]),
            ("a mono and a stereo track by turns", "two", 3, 588, [short[2], short[0]]),
            ("long track: tag numbers start again", "seq", 4, 588, [long]),
            ("long track, jumps", "seeks", 3, 640, [long]),
            ("long track read one unit at a time, as the player does", "units", 2, 588, [long]),
            ("the same, tags very late", "units", 12, 588, [long]),
            ("plain audio", "plain", 0, 588, [short[0]]),
        ):
            cases[name] = run_case(runner, folder, name.replace(" ", "_").replace(",", "").replace(":", ""), mode, delay,
                                   unit, clips, [references[clip] for clip in clips])
        bridge = check_bridge(folder)
        hires = check_hires(folder)
        # The next FLAC block decoded ahead (XDJ_CHIP_AHEAD): every sample the same.
        ahead = check_hires(folder, ["-DXDJ_CHIP_AHEAD"], "_ahead")
        ahead["passed"] = ahead["passed"] and ahead["cases"]["long 88.2 kHz track: tag numbers start again"]["ahead"]["ready"] > 0
        # The prediction pass in pieces, the cache tidied after each (XDJ_CHIP_TIDY).
        tidy = check_hires(folder, ["-DXDJ_CHIP_TIDY"], "_tidy")
        loader = check_loader(folder)
    return {"cases": cases, "bridge": bridge, "hires": hires, "ahead": ahead, "tidy": tidy, "loader": loader,
            "all_passed": all(case["passed"] for case in cases.values()) and bridge["passed"] and hires["passed"]
            and ahead["passed"] and tidy["passed"]
            and loader["passed"]
            }


def main() -> None:
    report = check()
    print(json.dumps(report, indent=2))
    if not report["all_passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
