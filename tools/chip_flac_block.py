#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""The audio processor's part of FLAC decoding: compile our block for it.

The block (src/chip_flac.c with the decoder unit, entered through
src/chip_flac_glue.S, laid out by src/chip_flac_layout.ld) is
code and data for the audio processor's external memory. The firmware build
appends it after the tail's own memory, with a short header, and the loader
on the main processor (src/chip_flac_loader.c) writes it into the
audio processor's memory at the first FLAC open.

Compiling needs the C compiler for the audio processor (TI C6000 family,
target tic6x-elf), made by toolchains/build_c6x.sh into toolchains/installed/c6x.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "chip_flac.h"
C6X_PREFIX = ROOT / "toolchains" / "installed" / "c6x" / "bin" / "tic6x-elf-"
# -O2 here: the main processor's decoder unit is built for size (-O2 was worse there).
C6X_FLAGS = [
    "-march=c674x", "-mlittle-endian", "-msdata=none", "-O2", "-std=c11", "-fomit-frame-pointer",
    "-ffreestanding", "-fno-builtin", "-fno-tree-loop-distribute-patterns", "-ffunction-sections",
    "-fdata-sections", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-DXDJ_HALFBAND_TAPS=47",
    "-I", "src/freestanding_include", "-I", "src",
]
# The routine the stock converter table is pointed at, and the C routine it calls.
ENTRY = "xdj_chip_entry"
UNIT = "xdj_chip_unit"


def constants(big_blocks: bool = False) -> dict[str, int]:
    """The numbers of src/chip_flac.h (with big_blocks, as built with XDJ_BIG_BLOCKS)."""
    if big_blocks:
        text = run(["cc", "-E", "-dM", "-DXDJ_BIG_BLOCKS", str(HEADER)], text=True).stdout
        return {name: int(value, 0) for name, value in re.findall(r"^#define\s+(XDJ_CHIP_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u\s*$", text, re.M)}
    text = HEADER.read_text()
    return {name: int(value, 0) for name, value in re.findall(r"^#define\s+(XDJ_CHIP_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u\b", text, re.M)}


def run(command: list[str], **more) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, **more)
    if result.returncode != 0:
        detail = result.stderr if isinstance(result.stderr, str) else result.stderr.decode(errors="replace")
        raise RuntimeError(f"command failed: {' '.join(command)}\n{detail.strip()}")
    return result


def no_false_headers(code: bytes) -> None:
    """A word that starts 0xE at the end of a fetch packet would be read as a packet header."""
    for index in range(28, len(code) - 3, 32):
        if code[index + 3] >> 4 == 0xE:
            raise ValueError("a packet's last word in our block reads as a header")


# The tuned decoder: the prediction routines inlined, the frame CRC-16
# left out (the header CRC-8 kept), the prediction in a pass of its own.
TUNED_FLAGS = ["-DXDJ_SHARED_ROUTINE=DRFLAC_INLINE", "-DXDJ_NO_CRC16", "-DXDJ_SPLIT_PREDICTION"]


def switch_flags(mono: bool = False, fast: bool = False, hires: bool = False, restart: bool = False,
                 tuned: bool = False, direct: bool = False, withdraw: bool = False, big_blocks: bool = False,
                 ahead: bool = False) -> list[str]:
    """The block's build switches: mono FLAC taken too (v0.9 on); fast, the ring copied by the word and
    leading zeros by the processor's instruction; hires, 88.2 and 96 kHz halved
    here (5.74); restart, a stream the decoder cannot go on in given up and a new one asked for (5.75);
    tuned, the decoder of 5.78; direct, the output written straight (5.80); withdraw, the reading place withdrawn
    when a stream is given up (5.81); big_blocks, room for FLAC blocks of 16,384 frames (5.83); ahead, the next
    FLAC block decoded ahead, a channel per unit, after the unit's output (5.96). Without them
    the block is that of the builds before."""
    return ((["-DXDJ_MONO"] if mono else []) + (["-DXDJ_CHIP_FAST", "-DXDJ_FAST_CLZ=1"] if fast else [])
            + (["-DXDJ_CHIP_HIRES"] if hires else []) + (["-DXDJ_CHIP_RESTART"] if restart else [])
            + (TUNED_FLAGS if tuned else []) + (["-DXDJ_CHIP_DIRECT"] if direct else [])
            + (["-DXDJ_CHIP_WITHDRAW"] if withdraw else []) + (["-DXDJ_BIG_BLOCKS"] if big_blocks else [])
            + (["-DXDJ_CHIP_AHEAD"] if ahead else []))


def build_block(folder: Path, c6x_prefix: Path = C6X_PREFIX, mono: bool = False, fast: bool = False,
                hires: bool = False, restart: bool = False, tuned: bool = False, direct: bool = False,
                withdraw: bool = False, big_blocks: bool = False, ahead: bool = False, short_hold: bool = False,
                tick_stack: bool = False, tidy: bool = False, early_cache: bool = False,
                ) -> dict:
    """Compile and link the block (with the switches of switch_flags). Returns the block ("block": code and data,
    from the end of the status words on, to a 4-byte edge), the length of its
    code ("text_bytes"), the section sizes and the symbols."""
    numbers = constants(big_blocks)

    def tool(name: str) -> str:
        return str(c6x_prefix.with_name(c6x_prefix.name + name))

    # The compiler's driver does not take paths with spaces: sources by relative path, from the top folder.
    if " " in str(folder):
        raise ValueError("the build folder must not contain spaces")
    objects = []
    extra = []
    # short_hold: the entry code writes the cache back again before holding interrupts.
    glue_extra = ["--defsym", "XDJ_SHORT_HOLD=1"] if short_hold else []
    # tick_stack: the output interrupt runs on the stock stack while our unit runs.
    glue_extra += ["--defsym", "XDJ_TICK_STACK=1"] if tick_stack else []
    extra += ["-DXDJ_CHIP_TICK_STACK"] if tick_stack else []
    # tidy: the decoder's changed lines written back as it goes.
    extra += ["-DXDJ_CHIP_TIDY"] if tidy else []
    # early_cache: the entry code makes our memory cacheable before our stack is used.
    glue_extra += ["--defsym", "XDJ_EARLY_CACHE=1"] if early_cache else []
    extra += ["-DXDJ_CHIP_EARLY_CACHE"] if early_cache else []
    for source in ("flac_decoder.c", "chip_flac.c"):
        objects.append(str(folder / (source + ".o")))
        run([tool("gcc"), *C6X_FLAGS, *switch_flags(mono, fast, hires, restart, tuned, direct, withdraw, big_blocks, ahead), *extra, "-c", f"src/{source}", "-o", objects[-1]],
            cwd=ROOT, text=True)
    glue_object = str(folder / "chip_flac_glue.o")
    run([tool("as"), "-march=c674x", "-mlittle-endian", *glue_extra, "-o", glue_object, "src/chip_flac_glue.S"],
        cwd=ROOT, text=True)
    linked = str(folder / "chip_flac.elf")
    run(
        [tool("gcc"), "-march=c674x", "-mlittle-endian", "-nostdlib", "-Wl,--gc-sections",
         f"-Wl,-T,src/{'chip_flac_big_layout.ld' if big_blocks else 'chip_flac_layout.ld'}", f"-Wl,-u,{UNIT}",
         f"-Wl,-u,{ENTRY}",
         f"-Wl,-e,{UNIT}", glue_object, *objects, "-lgcc", "-o", linked],
        cwd=ROOT, text=True,
    )

    binary = folder / "chip_flac.bin"
    run([tool("objcopy"), "-O", "binary", "-j", ".text", "-j", ".rodata", "-j", ".data", linked, str(binary)], text=True)
    symbols = {}
    for line in run([tool("nm"), "-n", linked], text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)
    sizes = {}
    for line in run([tool("size"), "-A", linked], text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0].startswith("."):
            sizes[parts[0]] = (int(parts[1]), int(parts[2]))

    block = binary.read_bytes()
    block += bytes(-len(block) % 4)
    base = numbers["XDJ_CHIP_BASE"] + numbers["XDJ_CHIP_STATUS_BYTES"]
    if sizes[".text"][1] != base or base + len(block) < symbols["__xdj_chip_load_end"]:
        raise ValueError("the block is not where src/chip_flac.h says")
    if symbols["__xdj_chip_end"] > numbers["XDJ_CHIP_BLOCK_LIMIT"]:
        raise ValueError("the block does not fit its place")
    if symbols[ENTRY] != base or symbols["xdj_chip_stack"] != numbers["XDJ_CHIP_STACK_TOP"] - 8:
        raise ValueError("the entry code or the stack is not where src/chip_flac.h says")
    no_false_headers(block[: sizes[".text"][0]])
    return {
        "block": block,
        "text_bytes": sizes[".text"][0],
        "sizes": {name: size for name, (size, _) in sizes.items()},
        "symbols": symbols,
    }


def appended(block: bytes) -> bytes:
    """What the build appends for the loader: the mark, the block's length in words, 0, the same length
    again (xdj_chip_load checks all four), then the block."""
    if not block or len(block) % 4:
        raise ValueError("the block must be whole words")
    words = len(block) // 4
    header = (constants()["XDJ_CHIP_BLOB_MAGIC"], words, 0, words)
    return b"".join(value.to_bytes(4, "little") for value in header) + block
