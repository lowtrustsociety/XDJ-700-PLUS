#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Build and verify the modified XDJ-700 firmware update file."""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from check_dispatch_trampoline import (
    APPLICATION_BASE,
    CHIP_FLAC_EVIDENCE,
    CHIP_STOCK_CONVERTER,
    CHIP_STOCK_CONVERTER_OFFSET,
    DSP_PROGRAM_EVIDENCE,
    UNPLAYABLE_PATCH_CODE,
    UNPLAYABLE_PATCH_EVIDENCE,
    UNPLAYABLE_PATCH_OFFSET,
    DISPATCH_ENTRY_OFFSET,
    PCM_LITERAL_HOOKS,
    STARTUP_CLEAR_RUNTIME_ADDRESS,
    STOCK_REFERENCED_LIMIT_ADDRESS,
    TAIL_RUNTIME_ADDRESS,
    VERSION_LABEL_HOOKS,
    PLAIN_VERSION_TEXTS,
    apply_entry_jump,
    expected_decoder_lock_bytes,
    named_sections,
    symbol_addresses,
    verify_stock_evidence,
)
from check_lzss_roundtrip import build_encoder, stock_application
from firmware_evidence import verify_regions
from inspect_upd import decode_srecords, decompress_lzss, split_container
from roundtrip_upd import XDJ700_V115_SHA256, encode_record


ROOT = Path(__file__).resolve().parents[1]
SECTOR_0_END_EXCLUSIVE = 0x40000  # 256 KiB
STOCK_APPLICATION_SPAN_BYTES = 7251904 - SECTOR_0_END_EXCLUSIVE  # 6,989,760 bytes
# The updater collects the image in a buffer of this many bytes and erases and
# writes the flash up to this address; the error log's sector starts here. No
# record of an image may reach it.
FLASH_APPLICATION_END_EXCLUSIVE = 0x7E0000


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"command failed: {' '.join(command)}\n{detail}")
    return result


# The decoder unit is built for size like the rest. -O2 was tried: an SH
# simulator counted 11 to 20 % fewer decoding cycles, but on the player it was
# worse than -Os. tools/stack_depth.py uses the same
# setting.
DECODER_OPTIMIZATION = "-Os"

# The paced fetch, in milliseconds per 1/75 s of
# audio: the top-up of the read-ahead at 1.33 times playing speed, the refill
# after a load or a jump at 1.9 times. 0 switches a pace off.
FETCH_PACE_MS = 10
FETCH_REFILL_MS = 7

# The screen's version text (--version-label; VERSION_LABEL_HOOKS in
# tools/check_dispatch_trampoline.py): what VERSION No. in UTILITY and the
# start-up screen show in place of 1.15.
VERSION_LABEL_MOST = 24

# The panel guard (--panel-guard, src/panel_guard.c): the stock 1 ms timer handler's address in its
# interrupt definition (0x080B86FC: attribute, extended information, interrupt 0x4A, handler, level) points
# at the guard, which calls the stock handler first.
PANEL_GUARD_HOOKS = {
    "panel_guard": (0x0B8708, 0x08D632C0, "_xdj_panel_guard_tick"),
}
# What the guard relies on in the stock application, as hashed windows (tools/firmware_evidence.py):
# offset: (length, SHA-256).
PANEL_GUARD_EVIDENCE = {
    0xD632C0: (108, "6e23dee33636bbc63e21deced70efe4cb65b57b09497830e42e18be91fda6900"),  # the timer handler, its pool
    0x0B86F4: (32, "7b1b25c9dedc0743fa01eb1ff211f30ef240620bcf6d8145a85c2dab84889a93"),  # its interrupt definition
    0xCEAE02: (418, "971515af70b5028d4bc1aaf8238cc1e264d1884d499e0a95401d8f4d630fab19"),  # the exchange's start
    0xCEAFA4: (660, "b0b5b7389dcf8a4fda9b5ad6550ea19d8135423a2aa99a949761cb89ae9e1f77"),  # "done" and its pool
    0xD53C5C: (32, "888782136184847403ab5dee5d91c0a8265263b40ec03dc8bb25c2ced5417188"),  # the sender's test
    0xD53D80: (32, "c25fcc353d45fb1bcb81576ab61b574d4bd2ad6f5badad276da201b7c64e53e1"),  # the sender's pool
}

# The v0.9 release (--release): FLAC at every rate decoded on the audio
# processor (88.2 and 96 kHz halved there) with the tuned decoder, room for
# FLAC blocks of 16,384 frames, the stack fix, mono, the cache switched on
# before our stack is used, the
# version label on the screen without "Ver ", the panel guard (the front
# panel's exchange restarted when it hangs), and no diagnostics. The only
# changes to the stock application are the bridge's hooks, the two version
# label addresses, the two "Ver " texts, the 1 ms timer's handler address and
# what is appended after its end.
RELEASE_VERSION_LABEL = "PLUS V0.90"
# The release is the default build.
RELEASE_IS_DEFAULT = True


def build_options(release: bool = RELEASE_IS_DEFAULT) -> dict:
    """The settings of a build that is offered: the release, or (release=False) the
    build with FLAC decoded on the main processor only, the one before v0.9. The
    default is the default build's."""
    options = {"audible_reports": False, "ignore_unplayable": True}
    if release:
        options.update(chip_flac=True, mono=True, own_stack=True, version_label=RELEASE_VERSION_LABEL,
                       chip_fast=True, chip_hires=True, chip_restart=True, chip_tuned=True, chip_direct=True,
                       chip_withdraw=True, plain_version=True, big_blocks=True, chip_early_cache=True,
                       panel_guard=True)
    return options


def check_version_label(label: str) -> None:
    if not 1 <= len(label) <= VERSION_LABEL_MOST or not all(32 <= ord(c) < 127 and c not in '"\\' for c in label):
        raise ValueError(f"the version label must be 1 to {VERSION_LABEL_MOST} plain characters, no quotes")


# Lengths of the half-band low-pass the decoder unit can be built with
# (XDJ_HALFBAND_TAPS in src/flac_decoder.c).
FILTER_TAPS = (47, 31, 23, 15)


def build_chip_block(c6x_prefix: Path, folder: Path, application: bytes, mono: bool = False, fast: bool = False,
                     hires: bool = False, restart: bool = False, tuned: bool = False,
                     direct: bool = False, withdraw: bool = False, big_blocks: bool = False, ahead: bool = False,
                     short_hold: bool = False, tick_stack: bool = False, tidy: bool = False,
                     early_cache: bool = False,
                     ) -> tuple[bytes, dict]:
    """FLAC decoded on the audio processor: what is appended for the loader, and
    figures for the report.

    Our block is reached through the stock table of PCM converters, which the
    loader changes in memory: the audio processor's stock program stays as it
    is. Its 16-bit stereo converter must be where the block's entry code sends
    everything that is not ours."""
    import chip_flac_block

    verify_regions(application, DSP_PROGRAM_EVIDENCE, "audio processor program evidence")
    verify_regions(application, CHIP_FLAC_EVIDENCE, "stock PCM converter evidence")
    if (chip_flac_block.constants(big_blocks)["XDJ_CHIP_STOCK_CONVERTER"] != CHIP_STOCK_CONVERTER
            or CHIP_STOCK_CONVERTER_OFFSET not in CHIP_FLAC_EVIDENCE):
        raise ValueError("src/chip_flac.h names another stock converter than the one checked")
    built = chip_flac_block.build_block(folder, c6x_prefix, mono=mono, fast=fast, hires=hires, restart=restart,
                                        tuned=tuned, direct=direct, withdraw=withdraw, big_blocks=big_blocks,
                                        ahead=ahead, short_hold=short_hold, tick_stack=tick_stack, tidy=tidy,
                                        early_cache=early_cache,
                                        )
    blob = chip_flac_block.appended(built["block"])
    return blob, {
        "block_bytes": len(built["block"]),
        "block_sections": built["sizes"],
        "block_end_address": built["symbols"]["__xdj_chip_end"],
        "appended_bytes": len(blob),
    }


def build_tail_payload(
    header: Path,
    tool_prefix: Path,
    folder: Path,
    audible_reports: bool = True,
    filter_taps: int = 47,
    fetch_pace_ms: int = FETCH_PACE_MS,
    fetch_pace_ahead: int = 24,
    fetch_refill_ms: int = FETCH_REFILL_MS,
    fetch_refill_ahead: int = 5,
    chip_flac: bool = False,
    version_label: str | None = None,
    panel_guard: bool = False,
    mono: bool = False,
    own_stack: bool = False,
    chip_hires: bool = False,
    chip_restart: bool = False,
    chip_places: bool = False,
    big_blocks: bool = False,
) -> tuple[bytes, dict, dict]:
    compiler = tool_prefix.with_name(tool_prefix.name + "gcc")
    size_tool = tool_prefix.with_name(tool_prefix.name + "size")
    nm_tool = tool_prefix.with_name(tool_prefix.name + "nm")
    objcopy_tool = tool_prefix.with_name(tool_prefix.name + "objcopy")

    decoder_object = folder / "dr_flac.o"
    trampoline_object = folder / "dispatch_trampoline.o"
    pcm_trampolines_object = folder / "pcm_trampolines.o"
    decoder_lock_object = folder / "decoder_lock.o"
    adapter_object = folder / "flac_adapter.o"
    chip_loader_object = folder / "chip_flac_loader.o"
    chip_feed_object = folder / "chip_flac_feed.o"
    chip_flac_define = ["-DXDJ_CHIP_FLAC"] if chip_flac else []
    # Mono files, handed out as stereo (v0.9 on); without it the code of the builds before.
    chip_flac_define += ["-DXDJ_MONO"] if mono else []
    # The file task's reads on our own stack (v0.9 on); likewise.
    chip_flac_define += ["-DXDJ_OWN_STACK"] if own_stack else []
    # Hi-res FLAC fed to the audio processor too; likewise.
    chip_flac_define += ["-DXDJ_CHIP_HIRES"] if chip_hires else []
    # A stream the audio processor cannot go on in started anew on its asking; likewise.
    chip_flac_define += ["-DXDJ_CHIP_RESTART"] if chip_restart else []
    # A place the player comes back to resumes its old stream up to the next FLAC block; likewise.
    chip_flac_define += ["-DXDJ_CHIP_PLACES"] if chip_places else []
    # Room for FLAC blocks of 16,384 frames, on both processors; likewise.
    big_blocks_define = ["-DXDJ_BIG_BLOCKS"] if big_blocks else []
    chip_flac_define += big_blocks_define
    linked_file = folder / "tail.elf"
    payload_file = folder / "tail.bin"

    common = [str(compiler), "-m4a-nofpu", "-ml"]
    run_command(
        common
        + [
            "-std=c11",
            DECODER_OPTIMIZATION,
            "-fomit-frame-pointer",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-function",
            "-ffreestanding",
            "-fno-builtin",
            "-fno-tree-loop-distribute-patterns",
            "-ffunction-sections",
            "-fdata-sections",
            *chip_flac_define,
            f"-DXDJ_HALFBAND_TAPS={filter_taps}",
            "-I",
            str(ROOT / "src" / "freestanding_include"),
            "-I",
            str(header.parent),
            "-c",
            str(ROOT / "src" / "flac_decoder.c"),
            "-o",
            str(decoder_object),
        ]
    )
    run_command(
        common
        + [
            "-c",
            str(ROOT / "src" / "dispatch_trampoline.S"),
            "-o",
            str(trampoline_object),
        ]
    )
    run_command(
        common
        + [
            "-c",
            str(ROOT / "src" / "pcm_route_trampolines.S"),
            "-o",
            str(pcm_trampolines_object),
        ]
    )
    run_command(
        common
        + [
            "-c",
            str(ROOT / "src" / "decoder_lock.S"),
            "-o",
            str(decoder_lock_object),
        ]
    )
    run_command(
        common
        + [
            "-std=c11",
            "-Os",
            "-fomit-frame-pointer",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-ffreestanding",
            "-fno-builtin",
            "-ffunction-sections",
            "-fdata-sections",
            f"-DXDJ_AUDIBLE_REPORTS={1 if audible_reports else 0}",
            # The filter length sets the size of the conversion state both units share.
            f"-DXDJ_HALFBAND_TAPS={filter_taps}",
            *chip_flac_define,
            # Seconds in hand are 1/75 s units in the source.
            *(
                [
                    f"-DXDJ_FETCH_PACE_MS={fetch_pace_ms}",
                    f"-DXDJ_FETCH_PACE_AHEAD_UNITS={fetch_pace_ahead * 75}U",
                    f"-DXDJ_FETCH_REFILL_MS={fetch_refill_ms}",
                    f"-DXDJ_FETCH_REFILL_AHEAD_UNITS={(fetch_refill_ahead if fetch_refill_ms else fetch_pace_ahead) * 75}U",
                ]
                if fetch_pace_ms
                else []
            ),
            "-I",
            str(header.parent),
            "-c",
            str(ROOT / "src" / "flac_adapter.c"),
            "-o",
            str(adapter_object),
        ]
    )
    extra_objects = []
    extra_link = []
    if chip_flac:
        # The loader that puts our block into the audio processor's memory at the first FLAC open.
        run_command(
            common
            + ["-std=c11", "-Os", "-fomit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
               "-fno-builtin", "-ffunction-sections", "-fdata-sections",
               *big_blocks_define,
               "-c", str(ROOT / "src" / "chip_flac_loader.c"), "-o", str(chip_loader_object)]
        )
        extra_objects.append(str(chip_loader_object))
    if version_label:
        check_version_label(version_label)
        run_command(
            common
            + ["-std=c11", "-Os", "-fomit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
               "-fno-builtin", "-ffunction-sections", "-fdata-sections", f'-DXDJ_VERSION_LABEL="{version_label}"',
               "-c", str(ROOT / "src" / "version_label.c"), "-o", str(folder / "version_label.o")]
        )
        extra_objects.append(str(folder / "version_label.o"))
        extra_link.append("-Wl,-u,_xdj_version_label")
    if panel_guard:
        run_command(
            common
            + ["-std=c11", "-Os", "-fomit-frame-pointer", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
               "-fno-builtin", "-ffunction-sections", "-fdata-sections", "-I", str(ROOT / "src"), "-c",
               str(ROOT / "src" / "panel_guard.c"), "-o", str(folder / "panel_guard.o")]
        )
        extra_objects.append(str(folder / "panel_guard.o"))
        extra_link.append("-Wl,-u,_xdj_panel_guard_tick")
    if chip_flac or own_stack:
        # The fed reads, and with own_stack every read of the file task, run on a stack of their own.
        # 
        run_command(common + ["-c", str(ROOT / "src" / "fed_stack.S"), "-o", str(folder / "fed_stack.o")])
        extra_objects.append(str(folder / "fed_stack.o"))
    if chip_flac:
        run_command(
            common
            + [
                "-std=c11",
                "-Os",
                "-fomit-frame-pointer",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-ffreestanding",
                "-fno-builtin",
                "-ffunction-sections",
                "-fdata-sections",
                *(["-DXDJ_CHIP_HIRES"] if chip_hires else []),
                *(["-DXDJ_CHIP_RESTART"] if chip_restart else []),
                *(["-DXDJ_CHIP_PLACES"] if chip_places else []),
                *big_blocks_define,
                "-c",
                str(ROOT / "src" / "chip_flac_feed.c"),
                "-o",
                str(chip_feed_object),
            ]
        )
        extra_objects.append(str(chip_feed_object))
    run_command(
        common
        + [
            "-nostdlib",
            "-Wl,--gc-sections",
            f"-Wl,-T,{ROOT / 'src' / 'tail_layout.ld'}",
            "-Wl,-u,_xdj_flac_open",
            "-Wl,-u,_xdj_flac_read_s16",
            "-Wl,-u,_xdj_flac_seek",
            "-Wl,-u,_xdj_flac_close",
            *extra_link,
            str(trampoline_object),
            str(pcm_trampolines_object),
            str(decoder_lock_object),
            str(adapter_object),
            str(decoder_object),
            *extra_objects,
            "-lgcc",
            "-o",
            str(linked_file),
        ]
    )

    size_output = run_command([str(size_tool), "-A", str(linked_file)]).stdout
    nm_output = run_command([str(nm_tool), "-n", str(linked_file)]).stdout
    run_command([str(objcopy_tool), "-O", "binary", str(linked_file), str(payload_file)])

    payload = payload_file.read_bytes()
    sections = named_sections(size_output)
    symbols = symbol_addresses(nm_output)

    # Verify lock binary matches expectation
    lock_offset = symbols["_xdj_flac_lock_acquire"] - TAIL_RUNTIME_ADDRESS
    lock_bytes = expected_decoder_lock_bytes(symbols["_xdj_flac_context"])
    if payload[lock_offset : lock_offset + len(lock_bytes)] != lock_bytes:
        raise ValueError("decoder lock binary guard failed")
    if symbols["_xdj_flac_lock_release"] != symbols["_xdj_flac_lock_acquire"] + 0x24:
        raise ValueError("decoder lock function layout changed")

    return payload, sections, symbols


def build(
    firmware_path: Path,
    header_path: Path,
    tool_prefix: Path,
    output_path: Path | None = None,
    version: str = "1.16",
    audible_reports: bool = True,
    ignore_unplayable: bool = False,
    filter_taps: int = 47,
    fetch_pace_ms: int = FETCH_PACE_MS,
    fetch_pace_ahead: int = 24,
    fetch_refill_ms: int = FETCH_REFILL_MS,
    fetch_refill_ahead: int = 5,
    c6x_prefix: Path | None = None,
    chip_flac: bool = False,
    version_label: str | None = None,
    panel_guard: bool = False,
    mono: bool = False,
    own_stack: bool = False,
    plain_version: bool = False,
    chip_fast: bool = False,
    chip_hires: bool = False,
    chip_restart: bool = False,
    chip_places: bool = False,
    chip_tuned: bool = False,
    chip_direct: bool = False,
    chip_withdraw: bool = False,
    big_blocks: bool = False,
    chip_ahead: bool = False,
    chip_short_hold: bool = False,
    chip_tick_stack: bool = False,
    chip_tidy: bool = False,
    chip_early_cache: bool = False,
) -> dict:
    if filter_taps not in FILTER_TAPS:
        raise ValueError(f"filter_taps must be one of {FILTER_TAPS}")
    # An empty label means none.
    if version_label:
        check_version_label(version_label)
    if chip_withdraw and not chip_restart:
        raise ValueError("--chip-withdraw belongs to --chip-restart")
    if (chip_fast or chip_hires or chip_restart or chip_tuned or chip_direct or chip_withdraw or chip_places
            or chip_ahead or chip_short_hold or chip_tick_stack or chip_tidy or chip_early_cache) and not chip_flac:
        raise ValueError("--chip-fast, --chip-hires, --chip-restart and --chip-tuned change FLAC on the audio processor: "
                         "they need it")
    if plain_version and not version_label:
        raise ValueError('--plain-version drops the "Ver " before a version label: it needs one')
    # 13 ms per 1/75 s is playing speed: the fetch must stay faster than that.
    if not 0 <= fetch_pace_ms <= 12:
        raise ValueError("fetch_pace_ms must be 0 (off) to 12")
    if not 1 <= fetch_pace_ahead <= 29:
        raise ValueError("fetch_pace_ahead must be 1 to 29 seconds")
    if not fetch_pace_ms:
        # Without the top-up pace there is no refill pace either.
        fetch_refill_ms = 0
    if not 0 <= fetch_refill_ms <= fetch_pace_ms:
        raise ValueError("fetch_refill_ms must be 0 (off) up to fetch_pace_ms")
    if not 1 <= fetch_refill_ahead < fetch_pace_ahead:
        raise ValueError("fetch_refill_ahead must be at least 1 second and below fetch_pace_ahead")
    stock_firmware_bytes = firmware_path.read_bytes()
    firmware_sha = sha256(stock_firmware_bytes)
    if firmware_sha != XDJ700_V115_SHA256:
        raise ValueError("input firmware is not pristine official XDJ-700 1.15")

    stock_packed, application, _ = stock_application(stock_firmware_bytes)
    if APPLICATION_BASE + len(application) != TAIL_RUNTIME_ADDRESS:
        raise ValueError("stock application end does not match tail layout")

    # Invariant guards
    verify_stock_evidence(application)
    if panel_guard:
        verify_regions(application, PANEL_GUARD_EVIDENCE, "panel guard evidence")

    with tempfile.TemporaryDirectory(prefix="xdj700-firmware-build-") as folder_name:
        folder = Path(folder_name)
        payload, sections, symbols = build_tail_payload(
            header_path,
            tool_prefix,
            folder,
            audible_reports=audible_reports,
            filter_taps=filter_taps,
            fetch_pace_ms=fetch_pace_ms,
            fetch_pace_ahead=fetch_pace_ahead,
            fetch_refill_ms=fetch_refill_ms,
            fetch_refill_ahead=fetch_refill_ahead,
            chip_flac=chip_flac,
            version_label=version_label,
            panel_guard=panel_guard,
            mono=mono,
            own_stack=own_stack,
            chip_hires=chip_hires,
            chip_restart=chip_restart,
            chip_places=chip_places,
            big_blocks=big_blocks,
        )

        runtime_bytes = sum(sections.values())
        clearance_bytes = STARTUP_CLEAR_RUNTIME_ADDRESS - (TAIL_RUNTIME_ADDRESS + runtime_bytes)
        if clearance_bytes <= 0:
            raise ValueError("tail payload overlaps startup zero-fill area")
        stock_reference_margin_bytes = STOCK_REFERENCED_LIMIT_ADDRESS - (
            TAIL_RUNTIME_ADDRESS + runtime_bytes
        )
        if stock_reference_margin_bytes <= 0:
            raise ValueError("tail memory reaches a stock-referenced address")

        # Patch application in memory
        patched_app = bytearray(application)
        dispatcher_patch = apply_entry_jump(
            patched_app,
            DISPATCH_ENTRY_OFFSET,
            TAIL_RUNTIME_ADDRESS,
        )
        pcm_hook_patches = {}
        hooks = dict(PCM_LITERAL_HOOKS)
        if version_label:
            hooks.update(VERSION_LABEL_HOOKS)
        if panel_guard:
            hooks.update(PANEL_GUARD_HOOKS)
        for name, (offset, stock_target, trampoline) in hooks.items():
            expected = stock_target.to_bytes(4, "little")
            if application[offset : offset + 4] != expected:
                raise ValueError(f"PCM {name} literal expected-byte guard failed")
            target = symbols[trampoline] & 0x1FFFFFFF
            patched_app[offset : offset + 4] = target.to_bytes(4, "little")
            pcm_hook_patches[name] = {
                "offset": offset,
                "stock_target": stock_target,
                "replacement_target": target,
            }

        plain_version_patch = None
        if plain_version:
            for offset, (stock, ours) in PLAIN_VERSION_TEXTS.items():
                if application[offset : offset + len(stock)] != stock or len(ours) != len(stock):
                    raise ValueError(f"the version text at 0x{offset:x} is not the stock one")
                patched_app[offset : offset + len(ours)] = ours
            plain_version_patch = {f"{offset:#x}": len(stock) for offset, (stock, _) in PLAIN_VERSION_TEXTS.items()}

        unplayable_patch = None
        if ignore_unplayable:
            verify_regions(application, UNPLAYABLE_PATCH_EVIDENCE, "unplayable mark evidence")
            end = UNPLAYABLE_PATCH_OFFSET + len(UNPLAYABLE_PATCH_CODE)
            patched_app[UNPLAYABLE_PATCH_OFFSET:end] = UNPLAYABLE_PATCH_CODE
            unplayable_patch = {"offset": UNPLAYABLE_PATCH_OFFSET, "bytes": len(UNPLAYABLE_PATCH_CODE)}

        chip_flac_patch = None
        chip_flac_blob = b""
        if chip_flac:
            chip_flac_blob, chip_flac_patch = build_chip_block(
                c6x_prefix or ROOT / "toolchains" / "installed" / "c6x" / "bin" / "tic6x-elf-", folder, application, mono,
                chip_fast, chip_hires, chip_restart, chip_tuned, chip_direct, chip_withdraw, big_blocks, chip_ahead,
                chip_short_hold, chip_tick_stack, chip_tidy, chip_early_cache,
            )

        augmented_app = bytes(patched_app) + payload
        application_span_bytes = STOCK_APPLICATION_SPAN_BYTES
        if chip_flac:
            # What the loader sends to the audio processor comes after the tail's own
            # memory, which is loaded as zeros so that nothing of the tail moves or
            # starts differently; the loader finds it at the tail's end. The unpacked
            # application and the packed image both grow.
            tail_end = symbols["__xdj_tail_bss_end"] & 0x1FFFFFFF
            zeros = tail_end - (TAIL_RUNTIME_ADDRESS + len(payload))
            if zeros < 0 or tail_end % 4 or tail_end + len(chip_flac_blob) >= STOCK_REFERENCED_LIMIT_ADDRESS:
                raise ValueError("the audio processor's block is misplaced or reaches a stock-referenced address")
            augmented_app += bytes(zeros) + chip_flac_blob
            chip_flac_patch["tail_memory_loaded_as_zeros"] = zeros
            chip_flac_patch["memory_end"] = tail_end + len(chip_flac_blob)

        # Compress augmented application using LZSS encoder
        compress = build_encoder(folder)
        recompressed_stock = compress(application)
        recompressed_augmented = compress(augmented_app)

        if decompress_lzss(recompressed_augmented) != augmented_app:
            raise ValueError("patched layout compression did not roundtrip")

        recompressed_bytes = len(recompressed_augmented)
        packed_limit_bytes = len(stock_packed)
        longer_image = chip_flac
        if longer_image:
            if recompressed_bytes <= len(stock_packed):
                raise ValueError("the image with the audio processor's block did not come out longer than stock")
            # The image may run past the stock end: as far as it needs, to a whole record.
            application_span_bytes = -(-(recompressed_bytes + 6) // 32) * 32
            packed_limit_bytes = recompressed_bytes
        if recompressed_bytes > packed_limit_bytes:
            raise ValueError(
                f"compressed patched application ({recompressed_bytes} bytes) exceeds "
                f"stock span ({len(stock_packed)} bytes)"
            )
        if SECTOR_0_END_EXCLUSIVE + application_span_bytes > FLASH_APPLICATION_END_EXCLUSIVE:
            raise ValueError("the image would reach the updater's limit at 0x7E0000")

        # Assemble the packed application region for flash:
        # [4 bytes length LE] + [LZSS data] + [2 bytes additive checksum LE]
        packed_body = recompressed_bytes.to_bytes(4, "little") + recompressed_augmented
        additive_csum = sum(packed_body) & 0xFFFF
        packed_region = packed_body + additive_csum.to_bytes(2, "little")

        if len(packed_region) > application_span_bytes:
            raise ValueError("packed application block exceeds flash sector span")

        # Pad with 0xFF to preserve identical total span size
        padding_len = application_span_bytes - len(packed_region)
        full_application_flash_span = packed_region + b"\xFF" * padding_len

        # Extract and unpack components from original firmware container
        lengths, offset = split_container(stock_firmware_bytes)
        c0_stock = stock_firmware_bytes[offset : offset + lengths[0]]
        c1_stock = stock_firmware_bytes[offset + lengths[0] : offset + lengths[0] + lengths[1]]

        c0_lines = c0_stock[32:-2].splitlines(keepends=True)
        # In stock C0, lines 0..4080 cover Sector 0 (address 0x000000 to 0x03FFFF)
        sector_0_lines = c0_lines[:4081]
        c0_terminator = c0_lines[-1]

        # Generate S-records for the new application span (0x040000 to 0x6EA7C0)
        num_s2_records = application_span_bytes // 32
        new_app_s2_records = []
        for i in range(num_s2_records):
            record_addr = SECTOR_0_END_EXCLUSIVE + i * 32
            record_payload = full_application_flash_span[i * 32 : (i + 1) * 32]
            new_app_s2_records.append(encode_record("2", record_addr, record_payload))

        version_tag = f"MAINVer{version}\x00".encode("ascii")
        c0_header = c0_stock[:32].replace(b"MAINVer1.15\x00", version_tag)
        if len(c0_header) != 32:
            raise ValueError(f"Component 0 header line length is {len(c0_header)}, expected 32 bytes")

        new_c0_body = (
            c0_header
            + b"".join(sector_0_lines)
            + b"".join(new_app_s2_records)
            + c0_terminator
        )
        new_c0_crc = binascii.crc_hqx(new_c0_body, 0).to_bytes(2, "little")
        new_c0 = new_c0_body + new_c0_crc

        # Construct new container
        new_container = (
            str(len(new_c0)).encode("ascii")
            + b"\r\n"
            + str(len(c1_stock)).encode("ascii")
            + b"\r\n"
            + new_c0
            + c1_stock
        )

        # In-depth validation of the built container
        v_lengths, v_offset = split_container(new_container)
        v_c0 = new_container[v_offset : v_offset + v_lengths[0]]
        v_c1 = new_container[v_offset + v_lengths[0] :]

        # 1. CRC-16 of both components
        if binascii.crc_hqx(v_c0[:-2], 0) != int.from_bytes(v_c0[-2:], "little"):
            raise ValueError("Component 0 CRC-16 validation failed")
        if binascii.crc_hqx(v_c1[:-2], 0) != int.from_bytes(v_c1[-2:], "little"):
            raise ValueError("Component 1 CRC-16 validation failed")

        # 2. Sector 0 byte-identity verification: Sector 0 MUST be 100% identical to stock
        stock_c0_image, _ = decode_srecords(c0_stock[32:-2])
        built_c0_image, built_c0_info = decode_srecords(v_c0[32:-2])
        if built_c0_image[:SECTOR_0_END_EXCLUSIVE] != stock_c0_image[:SECTOR_0_END_EXCLUSIVE]:
            raise ValueError("CRITICAL: Sector 0 (bootloader / recovery) was modified!")

        # 3. Component 1 byte-identity verification
        if v_c1 != c1_stock:
            raise ValueError("CRITICAL: Component 1 (PANL) was modified!")

        # 4. Packed application checksum validation
        stored_csum = int.from_bytes(
            built_c0_image[
                SECTOR_0_END_EXCLUSIVE
                + 4
                + recompressed_bytes : SECTOR_0_END_EXCLUSIVE
                + 4
                + recompressed_bytes
                + 2
            ],
            "little",
        )
        computed_csum = (
            sum(
                built_c0_image[
                    SECTOR_0_END_EXCLUSIVE : SECTOR_0_END_EXCLUSIVE + 4 + recompressed_bytes
                ]
            )
            & 0xFFFF
        )
        if stored_csum != computed_csum or stored_csum != additive_csum:
            raise ValueError("Packed application 16-bit additive checksum verification failed")

        output_written = False
        if output_path is not None:
            output_path.write_bytes(new_container)
            output_written = True

        return {
            "model": "XDJ-700",
            "version": f"{version}-FLAC",
            "status": "ready",
            "safety_audit": {
                "sector_0_untouched": True,
                "bootloader_recovery_preserved": True,
                "component_1_untouched": True,
                "sector_0_bytes_identical": SECTOR_0_END_EXCLUSIVE,
                "tail_payload_bytes": len(payload),
                "tail_runtime_address": TAIL_RUNTIME_ADDRESS,
                "startup_clearance_bytes": clearance_bytes,
                "stock_reference_margin_bytes": stock_reference_margin_bytes,
                "recompressed_packed_bytes": recompressed_bytes,
                "stock_packed_bytes": len(stock_packed),
                "remaining_headroom_bytes": len(stock_packed) - recompressed_bytes,
                "image_end_address": SECTOR_0_END_EXCLUSIVE + application_span_bytes,
                "ilram_checksum": additive_csum,
                "component_0_crc16": int.from_bytes(new_c0[-2:], "little"),
            },
            "patches_applied": {
                "playback_dispatcher_entry_jump": dispatcher_patch,
                "pcm_literal_hooks": pcm_hook_patches,
                "unplayable_mark": unplayable_patch,
                "chip_flac": chip_flac_patch,
                "tail_sections": sections,
            },
            "container": {
                "stock_sha256": firmware_sha,
                "built_sha256": sha256(new_container),
                "total_bytes": len(new_container),
                "component_0_bytes": len(new_c0),
                "component_1_bytes": len(v_c1),
            },
            "output_file": str(output_path) if output_path else None,
            "audible_reports": audible_reports,
            "ignore_unplayable": ignore_unplayable,
            "chip_flac": chip_flac,
            "version_label": version_label,
            "panel_guard": panel_guard,
            "mono": mono,
            "own_stack": own_stack,
            "plain_version": plain_version_patch,
            "chip_fast": chip_fast,
            "chip_hires": chip_hires,
            "chip_restart": chip_restart,
            "chip_places": chip_places,
            "chip_tuned": chip_tuned,
            "chip_direct": chip_direct,
            "chip_withdraw": chip_withdraw,
            "big_blocks": big_blocks,
            "chip_ahead": chip_ahead,
            "chip_short_hold": chip_short_hold,
            "chip_tick_stack": chip_tick_stack,
            "chip_tidy": chip_tidy,
            "chip_early_cache": chip_early_cache,
            "filter_taps": filter_taps,
            "fetch_pace_ms": fetch_pace_ms,
            "fetch_pace_ahead": fetch_pace_ahead if fetch_pace_ms else None,
            "fetch_refill_ms": fetch_refill_ms,
            "fetch_refill_ahead": fetch_refill_ahead if fetch_refill_ms else None,
            "output_written": output_written,
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--firmware",
        type=Path,
        default=ROOT / "firmware" / "original" / "XDJ700.UPD",
        help="Path to stock XDJ-700 1.15 firmware update file",
    )
    parser.add_argument(
        "--header",
        type=Path,
        default=ROOT / "src" / "dr_flac.h",
        help="Path to dr_flac.h",
    )
    parser.add_argument(
        "--tool-prefix",
        type=Path,
        default=ROOT / "toolchains" / "installed" / "bin" / "sh-elf-",
        help="Cross compiler prefix",
    )
    parser.add_argument(
        "--version",
        default="1.16",
        help="Firmware version tag (default: 1.16)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Optional path to write the modified firmware container",
    )
    # The build in use is the default: no audible reports, and the "unplayable"
    # mark of another player ignored. The two older switches for that are still
    # accepted, so the commands in the notes keep working.
    parser.add_argument(
        "--reports",
        action="store_true",
        help="Diagnostic: build in the audible reports (tones that tell how long each call took)",
    )
    parser.add_argument(
        "--keep-unplayable",
        action="store_true",
        help="Leave the stock handling of the \"unplayable\" mark another player set",
    )
    parser.add_argument("--no-reports", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--ignore-unplayable", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument(
        "--release",
        action="store_true",
        help=f"The v0.9 release: FLAC at every rate decoded on the audio processor, the start-up screen and "
        f"VERSION No. showing {RELEASE_VERSION_LABEL}, no diagnostics" + (" (the default)" if RELEASE_IS_DEFAULT else ""),
    )
    parser.add_argument(
        "--everyday",
        action="store_true",
        help="The build before v0.9: all FLAC decoded on the main processor, the version shown as 1.15"
        + ("" if RELEASE_IS_DEFAULT else " (the default)"),
    )
    parser.add_argument(
        "--version-label",
        metavar="TEXT",
        help="What VERSION No. in UTILITY and the start-up screen show in place of 1.15 (at most 24 plain "
        "characters). Only the screen: the update check, LINK and the log keep 1.15",
    )
    parser.add_argument(
        "--chip-fast",
        action="store_true",
        help="FLAC on the audio processor: the ring's bytes copied by the word and leading zeros counted by the "
        "processor's instruction",
    )
    parser.add_argument(
        "--chip-hires",
        action="store_true",
        help="FLAC on the audio processor at 88.2 and 96 kHz too, halved there",
    )
    parser.add_argument(
        "--chip-restart",
        action="store_true",
        help="FLAC on the audio processor: a stream it cannot go on in (too far behind, or its bytes gone) is "
        "given up and the main processor starts a new one there, as a jump does",
    )
    parser.add_argument(
        "--chip-tuned",
        action="store_true",
        help="FLAC on the audio processor: the tuned decoder (prediction inlined and in a pass of its own, the "
        "frame CRC-16 left out, the header CRC-8 kept)",
    )
    parser.add_argument(
        "--chip-direct",
        action="store_true",
        help="FLAC on the audio processor: the decoder writes straight into the converter's two channels "
        "",
    )
    parser.add_argument(
        "--chip-places",
        action="store_true",
        help="FLAC on the audio processor: a place the player's reads come back to (after reading another place "
        "or track) resumes its old stream up to the next FLAC block, so the new stream drops nothing "
        "",
    )
    parser.add_argument(
        "--chip-withdraw",
        action="store_true",
        help="With --chip-restart: a stream given up also withdraws the decoder's reading place, so that the ring "
        "is filled for the next one",
    )
    parser.add_argument(
        "--chip-ahead",
        action="store_true",
        help="FLAC on the audio processor: the next FLAC block is decoded ahead, a channel per unit, after the "
        "unit's output is written, so no unit stops in the middle of its output",
    )
    parser.add_argument(
        "--chip-early-cache",
        action="store_true",
        help="FLAC on the audio processor: the entry code makes our memory cacheable before our stack is used, so "
        "that no output interrupt runs on an uncached stack",
    )
    parser.add_argument(
        "--chip-short-hold",
        action="store_true",
        help="FLAC on the audio processor: after each unit the cache is written back a second time before "
        "interrupts are held, so that the hold stays short and the output interrupt keeps its turn "
        "",
    )
    parser.add_argument(
        "--chip-tick-stack",
        action="store_true",
        help="FLAC on the audio processor: the output interrupt, which runs the player's audio engine, runs on "
        "the stock stack in internal memory when it comes while our unit runs, not on our stack in external "
        "memory",
    )
    parser.add_argument(
        "--chip-tidy",
        action="store_true",
        help="FLAC on the audio processor: the decoder writes its changed data back from the cache as it goes, "
        "so that the output interrupt never finds the cache full of it",
    )
    parser.add_argument(
        "--big-blocks",
        action="store_true",
        help="Room for FLAC blocks of up to 16,384 frames on both processors",
    )
    parser.add_argument(
        "--panel-guard",
        action="store_true",
        help="Restart the front panel's exchange when it has stood still: 200 ms with the port's overrun, 2 s "
        "without (src/panel_guard.c)",
    )
    parser.add_argument(
        "--plain-version",
        action="store_true",
        help='With --version-label: the screens show the label alone, without "Ver " in front',
    )
    parser.add_argument(
        "--c6x-prefix",
        type=Path,
        default=None,
        help="Prefix of the audio processor's compiler (default toolchains/installed/c6x/bin/tic6x-elf-)",
    )
    parser.add_argument(
        "--filter-taps",
        type=int,
        choices=FILTER_TAPS,
        default=47,
        help="Length of the low-pass used when halving 88.2 and 96 kHz sources (default 47; shorter is faster)",
    )
    parser.add_argument(
        "--fetch-pace",
        type=int,
        default=FETCH_PACE_MS,
        metavar="MS",
        help="Spread the top-up of the read-ahead, forward and in reverse, MS milliseconds per 1/75 s "
        f"of audio (default {FETCH_PACE_MS}: 1.33 times playing speed; 0 switches all pacing off)",
    )
    parser.add_argument(
        "--fetch-pace-ahead",
        type=int,
        default=24,
        metavar="SECONDS",
        help="The top-up pace applies while this much audio is ahead of the playing position (default 24)",
    )
    parser.add_argument(
        "--fetch-refill",
        type=int,
        default=FETCH_REFILL_MS,
        metavar="MS",
        help="Pace of the refill after a load or a jump, MS milliseconds per 1/75 s "
        f"(default {FETCH_REFILL_MS}: 1.9 times playing speed; 0 leaves it at full speed)",
    )
    parser.add_argument(
        "--fetch-refill-ahead",
        type=int,
        default=5,
        metavar="SECONDS",
        help="Full speed until this much audio is in hand (default 5)",
    )
    args = parser.parse_args()
    if args.release and args.everyday:
        parser.error("--release and --everyday: one or the other")
    release = args.release or (RELEASE_IS_DEFAULT and not args.everyday)
    options = build_options(release)
    if args.version_label is not None:
        options["version_label"] = args.version_label
    if args.plain_version:
        options["plain_version"] = True
    if args.panel_guard:
        options["panel_guard"] = True
    if args.chip_fast:
        options["chip_fast"] = True
    if args.chip_hires:
        options["chip_hires"] = True
    if args.chip_restart:
        options["chip_restart"] = True
    if args.chip_places:
        options["chip_places"] = True
    if args.chip_tuned:
        options["chip_tuned"] = True
    if args.chip_direct:
        options["chip_direct"] = True
    if args.chip_withdraw:
        options["chip_withdraw"] = True
    if args.big_blocks:
        options["big_blocks"] = True
    if args.chip_ahead:
        options["chip_ahead"] = True
    if args.chip_short_hold:
        options["chip_short_hold"] = True
    if args.chip_tick_stack:
        options["chip_tick_stack"] = True
    if args.chip_tidy:
        options["chip_tidy"] = True
    if args.chip_early_cache:
        options["chip_early_cache"] = True

    options["audible_reports"] = args.reports and not args.no_reports
    options["ignore_unplayable"] = not args.keep_unplayable
    report = build(
        args.firmware,
        args.header,
        args.tool_prefix,
        args.output,
        version=args.version,
        c6x_prefix=args.c6x_prefix,
        **options,
        filter_taps=args.filter_taps,
        fetch_pace_ms=args.fetch_pace,
        fetch_pace_ahead=args.fetch_pace_ahead,
        fetch_refill_ms=args.fetch_refill,
        fetch_refill_ahead=args.fetch_refill_ahead,
    )
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
