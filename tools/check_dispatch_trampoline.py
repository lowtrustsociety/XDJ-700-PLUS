#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Build and verify an offline SH-4A type-5 trampoline and tail layout."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

from check_lzss_roundtrip import build_encoder, stock_application
from firmware_evidence import verify_regions
from inspect_upd import decompress_lzss


ROOT = Path(__file__).resolve().parents[1]
# Stock code executes from the cached P0 alias: startup programs CCR to 0x105
# (ICE, CB, OCE) and nearly all code-pointer literals are 0x08xxxxxx. The P2
# alias 0xA8xxxxxx is uncached, so the tail must never be linked there.
APPLICATION_BASE = 0x08000000
DISPATCH_ENTRY_OFFSET = 0xC01E78
TAIL_RUNTIME_ADDRESS = 0x091BD788
STARTUP_CLEAR_RUNTIME_ADDRESS = 0x09600000
# Lowest address above the tail that stock code references (11 literal loads,
# stored as a pointer near 0xD6AD38). Its use is unknown, so tail memory,
# including BSS, must end below it.
STOCK_REFERENCED_LIMIT_ADDRESS = 0x092CFAD8
TASK_DELAY_RUNTIME_ADDRESS = 0x08E0335C
# The entry jump and its literal replace the dispatcher's first 12 bytes (its
# register-saving prologue), which the trampoline re-executes before resuming
# at 0x08C01E84. The guard checks a 32-byte window from the entry.
DISPLACED_PROLOGUE_BYTES = 12
DISPATCH_ENTRY_EVIDENCE = {
    DISPATCH_ENTRY_OFFSET: (32, "f038ec677ea5f28d73be835539840914782a5f4db42d962becfbbbeff6707726"),
}
ENTRY_JUMP_BYTES = bytes.fromhex("01d02b400900")
PCM_LITERAL_HOOKS = {
    "open_seek_1": (0xBF6220, 0x08BE8E58, "_xdj_virtual_seek_r12"),
    "open_seek_2": (0xBF63DC, 0x08BE8E58, "_xdj_virtual_seek_r12"),
    "read_seek": (0xBF6928, 0x08BE8E58, "_xdj_virtual_seek_r12"),
    "seek": (0xBF7208, 0x08BE8E58, "_xdj_virtual_seek_r10"),
    "stream_seek": (0xBF7708, 0x08BE8E58, "_xdj_virtual_seek_r12"),
    "stream_read": (0xBF7C28, 0x08BE8F40, "_xdj_virtual_read_r12"),
    "seek_bf8430": (0xBF8430, 0x08BE8E58, "_xdj_virtual_seek_r12"),
    "seek_bf869c": (0xBF869C, 0x08BE8E58, "_xdj_virtual_seek_r12"),
    "seek_bf8f34": (0xBF8F34, 0x08BE8E58, "_xdj_virtual_seek_r12"),
    "seek_bf91a0": (0xBF91A0, 0x08BE8E58, "_xdj_virtual_seek_r12"),
    "seek_bf9f18": (0xBF9F18, 0x08BE8E58, "_xdj_virtual_seek_r10"),
    "seek_bfa1c0": (0xBFA1C0, 0x08BE8E58, "_xdj_virtual_seek_r10"),
    "seek_bfa96c": (0xBFA96C, 0x08BE8E58, "_xdj_virtual_seek_r12"),
    "stream_read_2": (0xBFA6DC, 0x08BE8F40, "_xdj_virtual_read_r12"),
    "stream_seek_2": (0xBFAC34, 0x08BE8E58, "_xdj_virtual_seek_r12"),
    # The PCM path's end-of-stream test; r12
    # holds the input task's workspace and r4 its file context at all four.
    "eof_command_1": (0xBF6224, 0x08BE8DC2, "_xdj_virtual_eof_r12"),
    "eof_command_1_loop": (0xBF66AC, 0x08BE8DC2, "_xdj_virtual_eof_r12"),
    "eof_bf7528": (0xBF770C, 0x08BE8DC2, "_xdj_virtual_eof_r12"),
    "eof_stream_read": (0xBF7C2C, 0x08BE8DC2, "_xdj_virtual_eof_r12"),
}


# The audio processor's program inside the application:
# its second block, a flat image for the audio processor's external
# memory at 0xC0000000, which the stock loader sends from here at start-up.
# The window is the program's first fetch packet.
DSP_PROGRAM_OFFSET = 0xE770
DSP_PROGRAM_BYTES = 0x5F4C0
DSP_PROGRAM_BASE = 0xC0000000
DSP_PROGRAM_EVIDENCE = {
    DSP_PROGRAM_OFFSET: (32, "8b2fa7a0be743f7ef206bdf5802a729ed30644f78c762efd80a1b4f1a5b60c80"),
}

# FLAC decoded on the audio processor: nothing of
# the stock program is changed, but our block relies on the stock 16-bit
# stereo converter at 0xC003C558 and on the table of converters whose start
# values (a 32-byte record for 0x1182E900) lie in the program's first block.
# Windows: the converter's first 32 bytes, and the record with its two
# header words.
CHIP_FLAC_EVIDENCE = {
    DSP_PROGRAM_OFFSET + 0x3C558: (32, "dac09f143ee3645290255ae967fdd4049d1ebfbaae99d687044212a55bf04ba6"),
    0x1010 + (0x11805BE0 - 0x11801DA0): (40, "c44bb53bac1b3610a2ab6170bcf097e60568a9785d33187d1258e889d3a974e7"),
}
# The stock converter the block hands everything that is not ours to, as an
# address of the audio processor (XDJ_CHIP_STOCK_CONVERTER in
# src/chip_flac.h), and where it lies in the application.
CHIP_STOCK_CONVERTER = 0xC003C558
CHIP_STOCK_CONVERTER_OFFSET = DSP_PROGRAM_OFFSET + CHIP_STOCK_CONVERTER - DSP_PROGRAM_BASE

# The screen's version text (build option --version-label; the release
# shows "PLUS-v0.9"): two routines put the version on the
# screen, each keeping the address of the stock version routine (0x08DDF518)
# at a place no other code loads: the start-up screen (0x08DD2E74) and
# UTILITY's VERSION No., which makes "Ver %s" of it (0x08DD18E0). Both are
# pointed at src/version_label.c. The version field itself stays
# "1.15" for the update check, LINK and the log.
VERSION_LABEL_HOOKS = {
    "version_label": (0xDD2FEC, 0x08DDF518, "_xdj_version_label"),
    "version_label_utility": (0xDD1A84, 0x08DDF518, "_xdj_version_label"),
}

# The label without "Ver " in front (build option --plain-version, the user's
# word on 2026-10-15): the two stock texts that put it there, each used by one
# place only (the distribution note, 2026-10-15). UTILITY's VERSION No.
# formats "Ver %s" (0x080D6294, loaded at 0x08DD1A88 by 0x08DD18E0); the
# start-up screen puts the text "Ver " (0x080D62A4, in one record, at
# 0x08E63B08) before the version. The log's lines have texts of their own
# ("Ver%s") and keep the version. Offsets into the application: (stock, ours).
PLAIN_VERSION_TEXTS = {
    0x0D6294: (b"Ver %s\0", b"%s\0\0\0\0\0"),
    0x0D62A4: (b"Ver \0", b"\0\0\0\0\0"),
}


# In-place patch (left out with the build flag --keep-unplayable). A
# player that fails to load a track marks it unplayable at the player holding
# the media, which then sends the mark with every list row.
# The database client's record reader (0x08B74474) stores the row's
# last argument as two bytes of the record: +36, the unplayable mark, and +37.
# Four instructions after the one that loads the argument are replaced by
#     shlr8 r4 / shll8 r4 / mov.w r4,@(r0,r10) / nop
# which stores zero at +36 and the same byte as before at +37 (r0 is 36
# there). So this player never sees a track as unplayable.
UNPLAYABLE_PATCH_OFFSET = 0xB74516
UNPLAYABLE_PATCH_CODE = bytes.fromhex("19441844450a0900")
UNPLAYABLE_PATCH_EVIDENCE = {
    0xB74500: (40, "b09b4dfa98e12bbe90c918f88f4f7bf5562e1b607de6377fadd1e7f9c69f3b9c"),
}


# Stock code the patch relies on, as hash-locked windows: offset into the
# application, window length, SHA-256 (tools/firmware_evidence.py).
PCM_FORMAT_EVIDENCE = {
    0xC0049E: (32, "fb4450116ca06b518ccb413ad8fc7a9391683ca1384e059518e20ee9d9c3d79a"),
    0xC004A4: (32, "6be5d0376b754e2716c61567fa6fc57426b5d30600de42ff615c0b04024486d8"),
    0xC004A8: (32, "e5bfc337314dea8a09c4a45f601be2233a8564295b2b784ef90cbf549e38bd1c"),
    0xC01FBC: (32, "9e6ed26d5beb88b0658f6229ae7e90a8d18a2b86896bf8b8836f5554340869ec"),
    0xC01FC0: (32, "feb480a1e2796ab55c714174133ed90d0af482e01767a347de5de56b70efca2a"),
    0xC01FD8: (32, "fcb4da7e558ba4f8fdf6b3b35998e8a879d40153c1d999f789fbda2adbb4e1f7"),
    0xBF6886: (32, "9d78923aedf0aef54bec6aeff64c8de8ff563549de699d4c2c162d1aa9ac2de3"),
    0xBF688E: (32, "55e0bcd40bca7ffa1253876c57463b850236ebb197f56b06d6a6594182366849"),
}

TASK_LIFECYCLE_EVIDENCE = {
    0x7A4B8: (0x08BFAE38).to_bytes(4, "little"),
    0x7A4D4: (0x08C01DDC).to_bytes(4, "little"),
    0x7A4F0: (0x08BF5D94).to_bytes(4, "little"),
    0x7A50C: (0x08C20430).to_bytes(4, "little"),
    0x7A528: (0x08C209C4).to_bytes(4, "little"),
    0xBF5F9C: (0x08BF7410).to_bytes(4, "little"),
}

TASK_NAME_EVIDENCE = {
    0x7A710: b"tsk_DJcontInBufMng\0",
    0x7A724: b"tsk_DJcontInBufMngSub\0",
    0x7A73C: b"tsk_DJcontInBufFile\0",
    0x7A750: b"tsk_DJcontTxDspPCM\0",
    0x7A764: b"tsk_DJcontTxDspDEC\0",
}

# Literal pointing at the input task's workspace.
TASK_ORDER_LITERALS = {
    0xBFE9F4: (0x09D289F0).to_bytes(4, "little"),
}

TASK_ORDER_EVIDENCE = {
    0xBFF584: (32, "652dbcdf152fff83472ba433d73b97e8bcd80069e15816f758d90e36bf2fc18a"),
    0xBFF62E: (32, "23659763d04741253c0f5b977ee045975e665f119b006807816eae645385a032"),
    0xBFF642: (32, "43bd7185f7bda4ee3e78c13b858ff657eab2fa4fb0be35962c867e5c9d3938ad"),
    0xBFF64C: (32, "6f20c7d08cc6e8e42f070d5c75db0a1d7b6023107967af26ff96a91c46a6fcd3"),
    0xBFF674: (32, "ac926743b135571641200a7a8e2da107a15ec9ff0053f4b5b77bb1fe5652ef43"),
    0xBFF6C0: (32, "6f90797100132a86207868a8308f4029107b5d7163f07403040ae24156b2e5d9"),
    0xBFFF58: (34, "b7a1e5c90f70a84e79d19738a059132988629291a613d30f9f7f855a492123f1"),
    0xC001BC: (34, "490e90e16986ddd8a628c0ac6f6510d45901693321791c0cb3ef7ffaf51c1ed4"),
}


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"command failed: {' '.join(command)}\n{detail}")
    return result


def named_sections(size_output: str) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in size_output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0] in (
            ".text",
            ".rodata",
            ".data",
            ".bss",
        ):
            result[fields[0]] = int(fields[1])
    return result


def symbol_addresses(nm_output: str) -> dict[str, int]:
    result = {}
    for line in nm_output.splitlines():
        fields = line.split()
        if len(fields) == 3:
            try:
                result[fields[2]] = int(fields[0], 16)
            except ValueError:
                continue
    return result


def bsr_calls_to(application: bytes, target_offset: int) -> list[int]:
    calls = []
    for offset in range(0, len(application) - 1, 2):
        opcode = int.from_bytes(application[offset : offset + 2], "little")
        if opcode >> 12 != 0xB:
            continue
        displacement = opcode & 0xFFF
        if displacement & 0x800:
            displacement -= 0x1000
        if offset + 4 + displacement * 2 == target_offset:
            calls.append(offset)
    return calls


def apply_entry_jump(image: bytearray, offset: int, target: int) -> dict:
    literal_offset = ((offset + 4) & ~3) + 4
    image[offset : offset + len(ENTRY_JUMP_BYTES)] = ENTRY_JUMP_BYTES
    image[literal_offset : literal_offset + 4] = target.to_bytes(4, "little")
    return {
        "entry_runtime_address": APPLICATION_BASE + offset,
        "entry_jump_bytes": ENTRY_JUMP_BYTES.hex(),
        "literal_runtime_address": APPLICATION_BASE + literal_offset,
        "literal_value": target,
        "resume_runtime_address": APPLICATION_BASE + literal_offset + 4,
    }


def expected_decoder_lock_bytes(context_address: int) -> bytes:
    return bytes.fromhex(
        "862f224f05d82c781b48048904d00b4001e4f9af0900264f0b00f668"
    ) + context_address.to_bytes(4, "little") + TASK_DELAY_RUNTIME_ADDRESS.to_bytes(
        4, "little"
    ) + bytes.fromhex(
        "03d100e02c710021ab000b0009000900"
    ) + context_address.to_bytes(4, "little")


def verify_stock_evidence(application: bytes) -> None:
    """Check the stock code and data the patch relies on (hash-locked windows)."""
    try:
        verify_regions(application, DISPATCH_ENTRY_EVIDENCE, "dispatcher entry")
    except ValueError:
        raise ValueError("dispatcher entry expected-byte guard failed") from None
    verify_regions(application, PCM_FORMAT_EVIDENCE, "PCM format evidence")
    for offset, expected in TASK_LIFECYCLE_EVIDENCE.items():
        if application[offset : offset + len(expected)] != expected:
            raise ValueError(f"task lifecycle evidence mismatch at 0x{offset:x}")
    for offset, expected in TASK_NAME_EVIDENCE.items():
        if application[offset : offset + len(expected)] != expected:
            raise ValueError(f"task name evidence mismatch at 0x{offset:x}")
    for offset, expected in TASK_ORDER_LITERALS.items():
        if application[offset : offset + len(expected)] != expected:
            raise ValueError(f"task order evidence mismatch at 0x{offset:x}")
    verify_regions(application, TASK_ORDER_EVIDENCE, "task order evidence")
    dispatcher_calls = bsr_calls_to(application, DISPATCH_ENTRY_OFFSET)
    if dispatcher_calls != [0xC01E02]:
        raise ValueError("unexpected playback dispatcher callers")


def check(header: Path, tool_prefix: Path, firmware: Path) -> dict:
    compiler = tool_prefix.with_name(tool_prefix.name + "gcc")
    size_tool = tool_prefix.with_name(tool_prefix.name + "size")
    nm_tool = tool_prefix.with_name(tool_prefix.name + "nm")
    objcopy_tool = tool_prefix.with_name(tool_prefix.name + "objcopy")
    for tool in (compiler, size_tool, nm_tool, objcopy_tool):
        if not tool.is_file():
            raise FileNotFoundError(tool)
    if header.name != "dr_flac.h":
        raise ValueError("header filename must be dr_flac.h")

    stock_packed, application, _ = stock_application(firmware.read_bytes())
    if APPLICATION_BASE + len(application) != TAIL_RUNTIME_ADDRESS:
        raise ValueError("stock application end no longer matches tail layout")
    verify_stock_evidence(application)

    with tempfile.TemporaryDirectory(prefix="xdj700-dispatch-tail-") as folder_name:
        folder = Path(folder_name)
        decoder_object = folder / "decoder.o"
        trampoline_object = folder / "trampoline.o"
        pcm_trampolines_object = folder / "pcm_trampolines.o"
        decoder_lock_object = folder / "decoder_lock.o"
        adapter_object = folder / "adapter.o"
        linked_file = folder / "tail.elf"
        payload_file = folder / "tail.bin"
        common = [str(compiler), "-m4a-nofpu", "-ml"]
        run(
            common
            + [
                "-std=c11",
                "-O2",
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
        run(
            common
            + [
                "-c",
                str(ROOT / "src" / "dispatch_trampoline.S"),
                "-o",
                str(trampoline_object),
            ]
        )
        run(
            common
            + [
                "-c",
                str(ROOT / "src" / "pcm_route_trampolines.S"),
                "-o",
                str(pcm_trampolines_object),
            ]
        )
        run(
            common
            + [
                "-c",
                str(ROOT / "src" / "decoder_lock.S"),
                "-o",
                str(decoder_lock_object),
            ]
        )
        run(
            common
            + [
                "-std=c11",
                "-O2",
                "-fomit-frame-pointer",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-ffreestanding",
                "-fno-builtin",
                "-ffunction-sections",
                "-fdata-sections",
                "-I",
                str(header.parent),
                "-c",
                str(ROOT / "src" / "flac_adapter.c"),
                "-o",
                str(adapter_object),
            ]
        )
        run(
            common
            + [
                "-nostdlib",
                "-Wl,--gc-sections",
                f"-Wl,-T,{ROOT / 'src' / 'tail_layout.ld'}",
                "-Wl,-u,_xdj_flac_open",
                "-Wl,-u,_xdj_flac_read_s16",
                "-Wl,-u,_xdj_flac_seek",
                "-Wl,-u,_xdj_flac_close",
                str(trampoline_object),
                str(pcm_trampolines_object),
                str(decoder_lock_object),
                str(adapter_object),
                str(decoder_object),
                "-lgcc",
                "-o",
                str(linked_file),
            ]
        )
        undefined = [
            line.strip()
            for line in run([str(nm_tool), "-u", str(linked_file)]).stdout.splitlines()
            if line.strip()
        ]
        if undefined:
            raise ValueError(f"tail has undefined symbols: {undefined}")
        symbols = symbol_addresses(
            run([str(nm_tool), "-n", str(linked_file)]).stdout
        )
        if symbols.get("_xdj_dispatch_trampoline") != TAIL_RUNTIME_ADDRESS:
            raise ValueError("trampoline is not linked at the application tail")
        for lock_symbol in (
            "_xdj_flac_lock_acquire",
            "_xdj_flac_lock_release",
        ):
            if lock_symbol not in symbols:
                raise ValueError(f"missing decoder lock symbol: {lock_symbol}")
        sections = named_sections(
            run([str(size_tool), "-A", str(linked_file)]).stdout
        )
        run(
            [
                str(objcopy_tool),
                "-O",
                "binary",
                "-j",
                ".text",
                "-j",
                ".rodata",
                "-j",
                ".data",
                str(linked_file),
                str(payload_file),
            ]
        )
        payload = payload_file.read_bytes()
        runtime_bytes = sum(sections.values())
        lock_offset = symbols["_xdj_flac_lock_acquire"] - TAIL_RUNTIME_ADDRESS
        lock_bytes = expected_decoder_lock_bytes(symbols["_xdj_flac_context"])
        if payload[lock_offset : lock_offset + len(lock_bytes)] != lock_bytes:
            raise ValueError("decoder lock expected-byte guard failed")
        if (
            symbols["_xdj_flac_lock_release"]
            != symbols["_xdj_flac_lock_acquire"] + 0x24
        ):
            raise ValueError("decoder lock function layout changed")

        patched = bytearray(application)
        dispatcher_patch = apply_entry_jump(
            patched,
            DISPATCH_ENTRY_OFFSET,
            TAIL_RUNTIME_ADDRESS,
        )
        pcm_hook_patches = {}
        for name, (offset, stock_target, trampoline) in PCM_LITERAL_HOOKS.items():
            expected = stock_target.to_bytes(4, "little")
            if application[offset : offset + 4] != expected:
                raise ValueError(f"PCM {name} literal expected-byte guard failed")
            target = symbols[trampoline] & 0x1FFFFFFF
            patched[offset : offset + 4] = target.to_bytes(4, "little")
            pcm_hook_patches[name] = {
                "literal_offset": offset,
                "literal_runtime_address": APPLICATION_BASE + offset,
                "stock_target_address": stock_target,
                "replacement_target_address": target,
                "trampoline_runtime_address": symbols[trampoline],
                "stock_path_tailcalls_original_target": True,
            }
        augmented = bytes(patched) + payload
        compress = build_encoder(folder)
        recompressed_stock = compress(application)
        recompressed_augmented = compress(augmented)
        if decompress_lzss(recompressed_augmented) != augmented:
            raise ValueError("patched layout compression did not roundtrip")

    return {
        "target": "sh-elf, SH-4A, little-endian, no FPU",
        "compiler": run([str(compiler), "--version"]).stdout.splitlines()[0],
        "dispatcher_patch": {
            **dispatcher_patch,
            "displaced_prologue_bytes": DISPLACED_PROLOGUE_BYTES,
            "non_flac_resume_runtime_address": 0x08C01E84,
            "flac_postprocess_runtime_address": 0x08C01ECC,
            "non_flac_path_reconstructs_displaced_prologue": True,
        },
        "pcm_route_hooks": {
            "inline_format_context_offset_in_output_descriptor": 340,
            "format_context_runtime_address": symbols["_xdj_flac_context"],
            "request_format_offset_in_flac_context": 0,
            "active_slot_count_offset_in_flac_context": 4,
            "decoder_slots": 3,
            "format_context_state_offset": 92,
            "format_context_layout": {
                "bytes_per_75_hz_frame_offset": 0,
                "little_endian_flag_offset": 4,
                "channel_count_offset": 5,
                "sample_rate_khz_offset": 6,
                "bytes_per_sample_offset": 7,
            },
            "virtual_pcm_format": "signed 16-bit little-endian stereo",
            "hooks": pcm_hook_patches,
            "stock_pcm_task_is_retained": True,
            "shared_low_level_file_functions_are_unmodified": True,
            "stock_routine_entered_directly_when_no_flac_slot_is_active": True,
            "flac_slot_selected_by_task_state_format_context": True,
            "decoder_io_follows_input_file_task_handle": True,
        },
        "task_lifecycle": {
            "playback_dispatch_direct_call_offsets": dispatcher_calls,
            "input_buffer_manager_entry_offset": 0xBFAE38,
            "input_buffer_subtask_entry_offset": 0xC01DDC,
            "input_file_worker_entry_offset": 0xBF5D94,
            "pcm_dsp_transmit_worker_entry_offset": 0xC20430,
            "compressed_dsp_transmit_worker_entry_offset": 0xC209C4,
            "parser_and_file_workers_are_distinct": True,
            "input_file_command_5_handler_offset": 0xBF7410,
            "command_5_is_not_used_as_a_cleanup_hook": True,
            "decoder_operations_are_serialized_across_tasks": True,
            "lock_primitive": "SH-4A TAS.B with task-delay contention yield",
            "lock_expected_bytes": lock_bytes.hex(),
            "input_file_command_before_parser_dispatch": {
                "manager_function_offset": 0xBFE8D2,
                "input_file_state_address": 0x09D289F0,
                "input_file_command_values": [2, 3],
                "input_file_command_send_offset": 0xBFF674,
                "input_file_completion_receive_offset": 0xBFF6C0,
                "parser_command_value": 1,
                "parser_command_send_offsets": [0xBFFF60, 0xC001C4],
                "parser_completion_receive_offsets": [0xBFFF6C, 0xC001D0],
                "ordering_is_linear_in_one_manager_function": True,
                "input_file_worker_calls_handlers_synchronously": True,
                "input_file_is_quiescent_before_dispatcher_runs": True,
            },
        },
        "tail": {
            "runtime_start": TAIL_RUNTIME_ADDRESS,
            "runtime_end_exclusive": TAIL_RUNTIME_ADDRESS + len(payload),
            "runtime_allocation_end_exclusive": (
                TAIL_RUNTIME_ADDRESS + runtime_bytes
            ),
            "payload_bytes": len(payload),
            "runtime_bytes_including_bss": runtime_bytes,
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
            "sections": sections,
            "trampoline_runtime_address": symbols["_xdj_dispatch_trampoline"],
            "adapter_runtime_address": symbols["_xdj_flac_adapter"],
            "undefined_symbols": undefined,
            "bytes_before_startup_zero_fill": (
                STARTUP_CLEAR_RUNTIME_ADDRESS
                - (TAIL_RUNTIME_ADDRESS + runtime_bytes)
            ),
        },
        "compression_fit": {
            "stock_packed_span_bytes": len(stock_packed),
            "recompressed_stock_bytes": len(recompressed_stock),
            "recompressed_patched_bytes": len(recompressed_augmented),
            "compressed_growth_bytes": (
                len(recompressed_augmented) - len(recompressed_stock)
            ),
            "remaining_packed_span_bytes": (
                len(stock_packed) - len(recompressed_augmented)
            ),
            "fits_stock_packed_span": (
                len(recompressed_augmented) <= len(stock_packed)
            ),
        },
        "constraints": {
            "adapter_populates_provisional_pcm_descriptor": True,
            "decoder_is_wired_to_hash_locked_device_file_io": True,
            "decoder_is_wired_as_virtual_pcm_read_and_seek": True,
            "virtual_read_returns_decoded_byte_count": True,
            "virtual_read_fifth_argument_is_preserved_as_opaque": True,
            "a_dispatch_releases_only_the_slot_of_its_own_descriptor": True,
            "decoders_of_other_descriptors_survive_a_dispatch": True,
            "slot_release_is_serialized_with_pcm_calls_by_decoder_lock": True,
            "fixed_arena_allocator_is_experimental": True,
            "fixed_arena_allocator_is_protected_by_decoder_lock": True,
            "input_file_quiescence_before_descriptor_replacement_is_proven": True,
            "link_layout_is_offline_only": True,
            "hardware_safety_proven": False,
        },
        "modified_firmware": False,
        "output_written": False,
        "device_access": False,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("header", type=Path)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("--tool-prefix", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(check(args.header, args.tool_prefix, args.firmware), indent=2))


if __name__ == "__main__":
    main()
