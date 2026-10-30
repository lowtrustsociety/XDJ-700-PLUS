# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


def load_firmware_builder():
    spec = importlib.util.spec_from_file_location(
        "build_flac_firmware", ROOT / "tools" / "build_flac_firmware.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class BuildOptionTests(unittest.TestCase):
    """What the offered builds are made of; no firmware needed."""

    def setUp(self):
        self.builder = load_firmware_builder()

    def test_the_everyday_build_and_the_release(self):
        everyday = self.builder.build_options(False)
        self.assertEqual(everyday, {"audible_reports": False, "ignore_unplayable": True})
        release = self.builder.build_options(True)
        self.assertEqual(release, {**everyday, "chip_flac": True, "mono": True, "own_stack": True, "version_label": "PLUS V0.90",
                                   "chip_fast": True, "chip_hires": True, "chip_restart": True, "chip_tuned": True,
                                   "chip_direct": True, "chip_withdraw": True, "plain_version": True,
                                   "big_blocks": True, "chip_early_cache": True, "panel_guard": True})
        self.builder.check_version_label(release["version_label"])
        self.assertEqual(self.builder.build_options(), self.builder.build_options(self.builder.RELEASE_IS_DEFAULT))


def changed_offsets(stock: bytes, mod: bytes) -> set[int]:
    """Offsets inside the stock application where the built one differs."""
    return {index for index in range(len(stock)) if stock[index] != mod[index]}


class FirmwareBuilderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.builder = load_firmware_builder()
        cls.firmware_path = ROOT / "firmware" / "original" / "XDJ700.UPD"
        cls.header_candidates = [
            ROOT / "src" / "dr_flac.h",
            Path("/private/tmp/dr_flac.h"),
            Path("/tmp/dr_flac.h"),
        ]
        cls.tool_prefixes = [
            ROOT / "toolchains" / "installed" / "bin" / "sh-elf-",
        ]

    def setUp(self):
        if not self.firmware_path.is_file():
            self.skipTest("official XDJ700.UPD is not available locally")
        self.header = None
        for candidate in self.header_candidates:
            if candidate.is_file():
                self.header = candidate
                break
        if self.header is None:
            self.skipTest("dr_flac.h is not available locally")

        self.tool_prefix = None
        for prefix in self.tool_prefixes:
            gcc = prefix.with_name(prefix.name + "gcc")
            if gcc.is_file():
                self.tool_prefix = prefix
                break
        if self.tool_prefix is None:
            self.skipTest("SH-4A cross compiler is not available locally")

    def test_build_dry_run_verifies_all_safety_invariants(self):
        report = self.builder.build(
            self.firmware_path,
            self.header,
            self.tool_prefix,
            output_path=None,
        )

        self.assertEqual(report.get("status"), "ready")
        self.assertEqual(report.get("model"), "XDJ-700")
        self.assertEqual(report.get("version"), "1.16-FLAC")
        self.assertFalse(report.get("output_written"))

        # Safety audit assertions
        audit = report["safety_audit"]
        self.assertTrue(audit["sector_0_untouched"], "Sector 0 must be 100% untouched")
        self.assertTrue(audit["bootloader_recovery_preserved"], "Emergency recovery must be preserved")
        self.assertTrue(audit["component_1_untouched"], "PANL component must be untouched")
        self.assertEqual(audit["sector_0_bytes_identical"], 262144)

        # Headroom and layout assertions
        self.assertGreater(audit["remaining_headroom_bytes"], 0, "compressed image must fit stock flash span")
        self.assertEqual(audit["remaining_headroom_bytes"], 3828)
        self.assertGreater(audit["startup_clearance_bytes"], 3000000, "clearance before BSS zero-fill must exceed 3 MB")
        self.assertGreater(audit["stock_reference_margin_bytes"], 0, "tail memory must end below the stock-referenced address")

        # Checksum mathematical validity
        self.assertEqual(audit["ilram_checksum"], 50313)
        self.assertEqual(audit["component_0_crc16"], 13367)

        # Container structure assertions
        container = report["container"]
        self.assertEqual(container["total_bytes"], 17371335)
        self.assertEqual(container["component_0_bytes"], 17355864)
        self.assertEqual(container["component_1_bytes"], 15454)


    def test_ignore_unplayable_build_changes_only_its_four_instructions(self):
        import check_dispatch_trampoline as layout

        plain = self.builder.build(self.firmware_path, self.header, self.tool_prefix, output_path=None)
        report = self.builder.build(
            self.firmware_path, self.header, self.tool_prefix, output_path=None, ignore_unplayable=True
        )
        self.assertEqual(report.get("status"), "ready")
        self.assertTrue(report["ignore_unplayable"])
        self.assertIsNone(plain["patches_applied"]["unplayable_mark"])
        self.assertEqual(
            report["patches_applied"]["unplayable_mark"],
            {"offset": layout.UNPLAYABLE_PATCH_OFFSET, "bytes": 8},
        )
        audit = report["safety_audit"]
        self.assertTrue(audit["sector_0_untouched"])
        self.assertTrue(audit["component_1_untouched"])
        # Same tail and hooks as the plain build.
        self.assertEqual(report["patches_applied"]["pcm_literal_hooks"], plain["patches_applied"]["pcm_literal_hooks"])
        self.assertEqual(report["patches_applied"]["tail_sections"], plain["patches_applied"]["tail_sections"])


    def test_release_changes_only_the_hooks_the_labels_and_the_appended_block(self):
        import chip_flac_block
        from check_dispatch_trampoline import (
            DISPATCH_ENTRY_OFFSET, DSP_PROGRAM_BYTES, DSP_PROGRAM_OFFSET, PLAIN_VERSION_TEXTS, UNPLAYABLE_PATCH_CODE,
            UNPLAYABLE_PATCH_OFFSET, VERSION_LABEL_HOOKS,
        )
        from build_flac_firmware import PANEL_GUARD_HOOKS
        from check_lzss_roundtrip import stock_application
        from make_patcher import unpacked_application

        if not chip_flac_block.C6X_PREFIX.with_name(chip_flac_block.C6X_PREFIX.name + "gcc").is_file():
            self.skipTest("the audio processor's compiler is not available locally")
        with tempfile.TemporaryDirectory(prefix="xdj700-release-test-") as folder:
            everyday_file, release_file = Path(folder) / "everyday.UPD", Path(folder) / "release.UPD"
            everyday = self.builder.build(self.firmware_path, self.header, self.tool_prefix, everyday_file,
                                          **self.builder.build_options(False))
            release = self.builder.build(self.firmware_path, self.header, self.tool_prefix, release_file,
                                         **self.builder.build_options(True))
            everyday_application = unpacked_application(everyday_file.read_bytes())
            release_application = unpacked_application(release_file.read_bytes())
        self.assertEqual(release.get("status"), "ready")
        self.assertTrue(release["chip_flac"])
        self.assertEqual(release["version_label"], self.builder.RELEASE_VERSION_LABEL)
        _, stock, _ = stock_application(self.firmware_path.read_bytes())

        # In the stock application: the everyday build's places (the dispatcher's jump, the bridge's hooks,
        # the unplayable mark), the two addresses of the version label, the two "Ver " texts and the 1 ms
        # timer's handler address (the panel guard), nothing else.
        hooks = release["patches_applied"]["pcm_literal_hooks"]
        self.assertEqual(set(hooks) - set(everyday["patches_applied"]["pcm_literal_hooks"]),
                         set(VERSION_LABEL_HOOKS) | set(PANEL_GUARD_HOOKS))
        allowed = set(range(DISPATCH_ENTRY_OFFSET, DISPATCH_ENTRY_OFFSET + 12))
        allowed |= set(range(UNPLAYABLE_PATCH_OFFSET, UNPLAYABLE_PATCH_OFFSET + len(UNPLAYABLE_PATCH_CODE)))
        for hook in hooks.values():
            allowed |= set(range(hook["offset"], hook["offset"] + 4))
        ver_bytes = {offset + index for offset, (text, _) in PLAIN_VERSION_TEXTS.items() for index in range(len(text))}
        allowed |= ver_bytes
        changed = changed_offsets(stock, release_application)
        self.assertLessEqual(changed, allowed)
        label_bytes = {offset + index for offset, _, _ in VERSION_LABEL_HOOKS.values() for index in range(4)} | ver_bytes
        label_bytes |= {offset + index for offset, _, _ in PANEL_GUARD_HOOKS.values() for index in range(4)}
        extra = changed - changed_offsets(stock, everyday_application)
        self.assertTrue(extra and extra <= label_bytes, sorted(extra))
        # The audio processor's program inside it is stock: our block only goes into its memory at run time.
        program = slice(DSP_PROGRAM_OFFSET, DSP_PROGRAM_OFFSET + DSP_PROGRAM_BYTES)
        self.assertEqual(release_application[program], stock[program])

        # After the stock application: the tail, its memory as zeros, then the block for the audio processor.
        chip = release["patches_applied"]["chip_flac"]
        tail = len(release_application) - len(stock) - chip["tail_memory_loaded_as_zeros"] - chip["appended_bytes"]
        self.assertEqual(tail, release["safety_audit"]["tail_payload_bytes"])
        self.assertEqual(release_application[-chip["appended_bytes"] - chip["tail_memory_loaded_as_zeros"] : -chip["appended_bytes"]],
                         bytes(chip["tail_memory_loaded_as_zeros"]))
        self.assertEqual(chip["appended_bytes"], 16 + chip["block_bytes"])
        self.assertLessEqual(chip["block_end_address"], chip_flac_block.constants()["XDJ_CHIP_BLOCK_LIMIT"])
        self.assertLess(chip["memory_end"], 0x092CFAD8)
        end = release["safety_audit"]["image_end_address"]
        self.assertGreater(end, 0x6EA7C0)
        self.assertLess(end, self.builder.FLASH_APPLICATION_END_EXCLUSIVE)
        self.assertEqual(end % 32, 0)
        self.assertTrue(release["safety_audit"]["sector_0_untouched"])
        self.assertTrue(release["safety_audit"]["component_1_untouched"])

    def test_version_label_changes_only_the_screen_routines_address(self):
        plain = self.builder.build(self.firmware_path, self.header, self.tool_prefix, output_path=None)
        report = self.builder.build(
            self.firmware_path, self.header, self.tool_prefix, output_path=None, version_label="plus-v1.0"
        )
        self.assertEqual(report.get("status"), "ready")
        hooks = report["patches_applied"]["pcm_literal_hooks"]
        self.assertEqual(
            set(hooks) - set(plain["patches_applied"]["pcm_literal_hooks"]), {"version_label", "version_label_utility"}
        )
        self.assertEqual(hooks["version_label"]["offset"], 0xDD2FEC)
        self.assertEqual(hooks["version_label_utility"]["offset"], 0xDD1A84)
        for name in ("version_label", "version_label_utility"):
            self.assertEqual(hooks[name]["stock_target"], 0x08DDF518)
        # An empty label means none; these are refused.
        for bad in ("x" * 25, 'say "hi"', "back\\slash", "gr\u00fc\u00df"):
            with self.assertRaises(ValueError):
                self.builder.build(self.firmware_path, self.header, self.tool_prefix, output_path=None, version_label=bad)

    def test_paced_fetch_is_the_default_and_can_be_left_out(self):
        plain = self.builder.build(self.firmware_path, self.header, self.tool_prefix, output_path=None)
        unpaced = self.builder.build(
            self.firmware_path, self.header, self.tool_prefix, output_path=None, fetch_pace_ms=0
        )
        self.assertEqual((plain["fetch_pace_ms"], plain["fetch_pace_ahead"]), (10, 24))
        self.assertEqual((plain["fetch_refill_ms"], plain["fetch_refill_ahead"]), (7, 5))
        # Without the top-up pace there is no refill pace either, and the code is left out.
        self.assertEqual((unpaced["fetch_pace_ms"], unpaced["fetch_refill_ms"]), (0, 0))
        self.assertLess(
            unpaced["patches_applied"]["tail_sections"][".text"], plain["patches_applied"]["tail_sections"][".text"]
        )
        self.assertEqual(unpaced.get("status"), "ready")
        with self.assertRaises(ValueError):
            self.builder.build(
                self.firmware_path, self.header, self.tool_prefix, output_path=None, fetch_pace_ms=13
            )


if __name__ == "__main__":
    unittest.main()
