#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Worst-case stack depth of the bridge's entry points on the SH-4.

Compiles the decoder unit and the adapter with the firmware build's flags plus
-fcallgraph-info=su, reads GCC's per-function frame sizes and call edges, and
prints the deepest call chain from each entry point the player calls. The
bridge runs on the player's input task, whose stack is 2 KB (task definition
at 0x0807A500); the figures exclude the filesystem frames below our reads and
the few bytes of the PCM trampolines.

Example:

    python3 tools/stack_depth.py
    python3 tools/stack_depth.py --tool-prefix /path/to/sh-elf-
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PREFIX = ROOT / "toolchains" / "installed" / "bin" / "sh-elf-"
ENTRY_POINTS = (
    "xdj_flac_virtual_read_format",
    "xdj_flac_virtual_seek_format",
    "xdj_flac_virtual_eof_format",
    "xdj_flac_adapter",
    "xdj_flac_deactivate",
)
COMMON = ["-m4a-nofpu", "-ml", "-std=c11", "-fomit-frame-pointer", "-ffreestanding",
          "-fno-builtin", "-ffunction-sections", "-fdata-sections", "-fcallgraph-info=su"]
# The paced fetch as an everyday build has it (the defaults of tools/build_flac_firmware.py).
PACE_DEFINES = ["-DXDJ_FETCH_PACE_MS=10", "-DXDJ_FETCH_PACE_AHEAD_UNITS=1800U",
                "-DXDJ_FETCH_REFILL_MS=7", "-DXDJ_FETCH_REFILL_AHEAD_UNITS=375U"]
# As in tools/build_flac_firmware.py (-O2 for the decoder was tried and was worse on the player).
DECODER_OPTIMIZATION = "-Os"
ADAPTER_OPTIMIZATION = "-Os"


def parse_callgraph(paths: list[Path]) -> tuple[dict[str, int], dict[str, set[str]]]:
    frames: dict[str, int] = {}
    edges: dict[str, set[str]] = {}
    for path in paths:
        text = path.read_text()
        for match in re.finditer(r'node: \{ title: "([^"]+)" label: "([^"]*)"', text):
            name = match.group(1).split(":")[-1]
            size = re.search(r"\\n(\d+) bytes \((static|dynamic[^)]*)\)", match.group(2))
            frames[name] = int(size.group(1)) if size else 0
        for match in re.finditer(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"', text):
            edges.setdefault(match.group(1).split(":")[-1], set()).add(match.group(2).split(":")[-1])
    return frames, edges


def worst_chain(frames: dict[str, int], edges: dict[str, set[str]], root: str) -> tuple[int, list[str]]:
    memo: dict[str, tuple[int, list[str]]] = {}

    def visit(name: str, path: frozenset) -> tuple[int, list[str]]:
        if name in path:
            return 0, [f"{name}(recursion)"]
        if name in memo:
            return memo[name]
        best = (0, [])
        for callee in edges.get(name, ()):
            depth, chain = visit(callee, path | {name})
            if depth > best[0]:
                best = (depth, chain)
        result = (frames.get(name, 0) + best[0], [f"{name}:{frames.get(name, 0)}"] + best[1])
        memo[name] = result
        return result

    return visit(root, frozenset())


# chip_flac measures the build with FLAC decoded on the audio processor (the
# release): the feed and the loader are compiled in, and the open path
# carries the load of our block (the parser's open runs in
# tsk_DJcontInBufMngSub, 3 KB of stack). The fed reads themselves run on a
# stack of their own (src/fed_stack.S), which this does not follow.
def measure(prefix: Path, header: Path, audible_reports: bool = True, chip_flac: bool = False) -> dict:
    compiler = str(prefix.with_name(prefix.name + "gcc"))
    chip_define = ["-DXDJ_CHIP_FLAC"] if chip_flac else []
    with tempfile.TemporaryDirectory(prefix="xdj700-stack-") as folder_name:
        folder = Path(folder_name)
        subprocess.run([compiler, *COMMON, DECODER_OPTIMIZATION, "-fno-tree-loop-distribute-patterns",
                        "-Wno-unused-function",
                        "-I", str(ROOT / "src" / "freestanding_include"), "-I", str(header.parent),
                        "-c", str(ROOT / "src" / "flac_decoder.c"), "-o", str(folder / "decoder.o")],
                       check=True, capture_output=True, cwd=folder)
        subprocess.run([compiler, *COMMON, ADAPTER_OPTIMIZATION, *PACE_DEFINES,
                        f"-DXDJ_AUDIBLE_REPORTS={1 if audible_reports else 0}",
                        *chip_define,
                        "-I", str(header.parent), "-c", str(ROOT / "src" / "flac_adapter.c"),
                        "-o", str(folder / "adapter.o")], check=True, capture_output=True, cwd=folder)
        if chip_flac:
            # As tools/build_flac_firmware.py builds them for the release.
            for source in ("chip_flac_feed.c", "chip_flac_loader.c"):
                subprocess.run([compiler, *COMMON, "-Os", "-I", str(ROOT / "src"),
                                "-c", str(ROOT / "src" / source), "-o", str(folder / (source[:-2] + ".o"))],
                               check=True, capture_output=True, cwd=folder)
        frames, edges = parse_callgraph(sorted(folder.glob("*.ci")))
    report = {}
    for root in ENTRY_POINTS:
        depth, chain = worst_chain(frames, edges, root)
        report[root] = {"bytes": depth, "chain": chain}
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tool-prefix", type=Path, default=DEFAULT_PREFIX, help="Cross compiler prefix")
    parser.add_argument("--header", type=Path, default=ROOT / "src" / "dr_flac.h")
    parser.add_argument("--no-reports", action="store_true", help="Measure the build without audible reports")
    parser.add_argument("--chip-flac", action="store_true",
                        help="Measure the build with FLAC decoded on the audio processor (the release)")
    args = parser.parse_args()
    print(json.dumps(measure(args.tool_prefix, args.header, not args.no_reports, args.chip_flac), indent=2))


if __name__ == "__main__":
    main()
