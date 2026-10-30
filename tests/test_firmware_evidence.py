# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
from pathlib import Path
import re
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import firmware_evidence  # noqa: E402


def load(name: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


# Byte literals that are ours: the dispatcher entry jump and the decoder lock
# code compiled from src/, which the build compares against its output.
OWN_HEX_LITERALS = {
    "01d02b400900",
    "862f224f05d82c781b48048904d00b4001e4f9af0900264f0b00f668",
    "03d100e02c710021ab000b0009000900",
    # The four instructions that make the player ignore the "unplayable" mark.
    "19441844450a0900",
    # Public file-format constants in the WAV and AIFF test files: the tail of
    # the PCM sub-format GUID, and 96000 as an 80-bit float.
    "000000001000800000aa00389b71",
    "400fbb80000000000000",
}


class FirmwareEvidenceTests(unittest.TestCase):
    def test_windows_shorter_than_32_bytes_are_refused(self):
        with self.assertRaises(ValueError):
            firmware_evidence.region_digest(bytes(64), 0, 31)
        with self.assertRaises(ValueError):
            firmware_evidence.region_digest(bytes(64), 40, 32)

    def test_verify_regions_reports_the_offset(self):
        image = bytes(range(256))
        good = {16: (32, firmware_evidence.region_digest(image, 16, 32))}
        firmware_evidence.verify_regions(image, good, "sample")
        bad = {17: good[16]}
        with self.assertRaisesRegex(ValueError, "sample mismatch at 0x11"):
            firmware_evidence.verify_regions(image, bad, "sample")

    def test_no_stock_code_bytes_in_tools_or_tests(self):
        literal = re.compile(r'fromhex\(\s*"([0-9a-fA-F]+)"')
        found = []
        for folder in ("tools", "tests"):
            for path in sorted((ROOT / folder).glob("*.py")):
                for match in literal.finditer(path.read_text()):
                    value = match.group(1).lower()
                    if not any(value in own for own in OWN_HEX_LITERALS):
                        found.append(f"{path.relative_to(ROOT)}: {match.group(1)}")
        self.assertEqual(found, [])

    def test_evidence_tables_hold_windows_and_digests(self):
        tables = [
            load("check_dispatch_trampoline").PCM_FORMAT_EVIDENCE,
        ]
        for table in tables:
            self.assertTrue(table)
            for length, digest in table.values():
                self.assertGreaterEqual(length, firmware_evidence.MIN_WINDOW_BYTES)
                self.assertRegex(digest, r"^[0-9a-f]{64}$")


if __name__ == "__main__":
    unittest.main()
