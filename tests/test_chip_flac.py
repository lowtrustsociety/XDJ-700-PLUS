# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
from pathlib import Path
import re
import shutil
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def load_with_tools(name):
    if str(ROOT / "tools") not in sys.path:
        sys.path.insert(0, str(ROOT / "tools"))
    return load(name)


# The audio processor's status words (xdj_chip_status in src/chip_flac.h).
STATUS_FIELDS = re.findall(r"^\s+unsigned (\w+);", (ROOT / "src" / "chip_flac.h").read_text().split("} xdj_chip_control;")[1], re.M)


def numbers(header):
    text = (ROOT / "src" / header).read_text()
    return {name: int(value, 0) for name, value in re.findall(r"^#define\s+(XDJ_\w+)\s+(0x[0-9A-Fa-f]+|\d+)u\b", text, re.M)}


class ChipFlacTests(unittest.TestCase):
    def test_both_sides_together_give_the_reference_decoders_samples(self):
        if shutil.which("sox") is None or shutil.which("flac") is None:
            self.skipTest("sox and flac are needed")
        report = load("check_chip_flac").check()
        for name, case in report["cases"].items():
            self.assertTrue(case["passed"], (name, case))
        self.assertTrue(report["bridge"]["passed"], report["bridge"])
        self.assertGreaterEqual(len(report["cases"]), 9)
        # The loader as the release builds it: the load, and nothing else.
        self.assertTrue(report["loader"]["passed"], report["loader"])
        self.assertTrue(report["loader"]["nothing_but_the_load"], report["loader"])
        self.assertGreaterEqual(report["loader"]["checks"], 25)

    def test_what_each_side_writes_keeps_to_its_own_cache_lines(self):
        chip = numbers("chip_flac.h")
        # The audio processor writes its status, its block, stack, decoder memory and scratch;
        # the main processor writes its control words with the records, and the ring.
        self.assertEqual(chip["XDJ_CHIP_CONTROL"] % 64, 0)
        self.assertEqual(chip["XDJ_CHIP_RING"] % 64, 0)
        scratch_end = chip["XDJ_CHIP_SCRATCH"] + chip["XDJ_CHIP_SCRATCH_FRAMES"] * 8
        self.assertLessEqual(scratch_end, chip["XDJ_CHIP_CONTROL"])
        self.assertLessEqual(chip["XDJ_CHIP_CONTROL"] + chip["XDJ_CHIP_CONTROL_BYTES"], chip["XDJ_CHIP_RING"])
        records_end = chip["XDJ_CHIP_CONTROL"] + 0x40 + 16 * chip["XDJ_CHIP_RECORD_COUNT"]
        self.assertLessEqual(records_end, chip["XDJ_CHIP_CONTROL"] + chip["XDJ_CHIP_CONTROL_BYTES"])
        self.assertEqual(chip["XDJ_CHIP_RING"] + chip["XDJ_CHIP_RING_BYTES"], 0xC2000000)
        # The ring's size is a power of two (places are taken modulo it), and a unit fits the scratch.
        self.assertEqual(chip["XDJ_CHIP_RING_BYTES"] & (chip["XDJ_CHIP_RING_BYTES"] - 1), 0)
        self.assertLessEqual(chip["XDJ_CHIP_UNIT_MOST"], chip["XDJ_CHIP_SCRATCH_FRAMES"])
        # A tag: two mark bits, ten for the stretch, twenty for the frame.
        self.assertEqual(chip["XDJ_CHIP_TAG_NUMBERS"] << chip["XDJ_CHIP_TAG_NUMBER_SHIFT"], 0x40000000)
        self.assertEqual(chip["XDJ_CHIP_TAG_FRAMES"], 1 << chip["XDJ_CHIP_TAG_NUMBER_SHIFT"])
        self.assertEqual(chip["XDJ_CHIP_RUN_LEAST"], chip["XDJ_CHIP_TAIL_FRAMES"])


    def test_the_block_lies_in_the_free_memory_in_order(self):
        chip = numbers("chip_flac.h")
        # The stretch of the audio processor's memory the stock program never touches.
        self.assertGreaterEqual(chip["XDJ_CHIP_BASE"], 0xC1F3F420)
        self.assertEqual(chip["XDJ_CHIP_MEMORY_END"], 0xC2000000)
        order = ["XDJ_CHIP_BASE", "XDJ_CHIP_BLOCK_LIMIT", "XDJ_CHIP_STACK_TOP", "XDJ_CHIP_SCRATCH", "XDJ_CHIP_CONTROL",
                 "XDJ_CHIP_RING", "XDJ_CHIP_MEMORY_END"]
        values = [chip[name] for name in order]
        self.assertEqual(values, sorted(values))
        self.assertEqual(chip["XDJ_CHIP_ARENA"], chip["XDJ_CHIP_STACK_TOP"])
        self.assertEqual(chip["XDJ_CHIP_ARENA"] + chip["XDJ_CHIP_ARENA_BYTES"], chip["XDJ_CHIP_SCRATCH"])
        # The loader's note lies inside the status words, after the audio processor's own.
        self.assertLessEqual(len(STATUS_FIELDS) * 4, chip["XDJ_CHIP_LOADER_NOTE"])
        self.assertLessEqual(chip["XDJ_CHIP_LOADER_NOTE"] + 20, chip["XDJ_CHIP_STATUS_BYTES"])

    def test_the_linker_script_agrees_with_the_header(self):
        chip = numbers("chip_flac.h")
        script = (ROOT / "src" / "chip_flac_layout.ld").read_text()
        entry = chip["XDJ_CHIP_BASE"] + chip["XDJ_CHIP_STATUS_BYTES"]
        self.assertIn(f".text {entry:#X} :".replace("0X", "0x"), script)
        self.assertIn(f"xdj_chip_stock = {chip['XDJ_CHIP_STOCK_CONVERTER']:#X};".replace("0X", "0x"), script)
        self.assertIn(f"xdj_chip_stack = {chip['XDJ_CHIP_STACK_TOP']:#X} - 8;".replace("0X", "0x"), script)
        self.assertIn(f"ASSERT(__xdj_chip_end <= {chip['XDJ_CHIP_BLOCK_LIMIT']:#X},".replace("0X", "0x"), script)

    def test_what_the_build_appends_is_what_the_loader_checks(self):
        block_module = load("chip_flac_block")
        chip = numbers("chip_flac.h")
        block = bytes(range(256)) * 3
        appended = block_module.appended(block)
        self.assertEqual(appended[16:], block)
        words = [int.from_bytes(appended[index : index + 4], "little") for index in range(0, 16, 4)]
        self.assertEqual(words, [chip["XDJ_CHIP_BLOB_MAGIC"], len(block) // 4, 0, len(block) // 4])
        with self.assertRaises(ValueError):
            block_module.appended(block + b"\0")
        # The stock converter the build checks in the application is the one the block hands over to.
        trampoline = load_with_tools("check_dispatch_trampoline")
        self.assertEqual(trampoline.CHIP_STOCK_CONVERTER, chip["XDJ_CHIP_STOCK_CONVERTER"])
        self.assertIn(trampoline.CHIP_STOCK_CONVERTER_OFFSET, trampoline.CHIP_FLAC_EVIDENCE)

    def test_a_packet_header_in_our_code_is_refused(self):
        block_module = load("chip_flac_block")
        code = bytearray(64)
        block_module.no_false_headers(bytes(code))
        code[31] = 0xE0
        with self.assertRaises(ValueError):
            block_module.no_false_headers(bytes(code))


if __name__ == "__main__":
    unittest.main()
