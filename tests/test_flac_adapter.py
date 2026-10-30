# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


def load_adapter_checker():
    spec = importlib.util.spec_from_file_location(
        "check_flac_adapter", ROOT / "tools" / "check_flac_adapter.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FlacAdapterHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.checker = load_adapter_checker()
        cls.header_candidates = [
            ROOT / "src" / "dr_flac.h",
            Path("/private/tmp/dr_flac.h"),
            Path("/tmp/dr_flac.h"),
        ]

    def setUp(self):
        if shutil.which("ffmpeg") is None:
            self.skipTest("ffmpeg is required for FLAC test generation")
        self.header = None
        for candidate in self.header_candidates:
            if candidate.is_file():
                self.header = candidate
                break
        if self.header is None:
            self.skipTest("dr_flac.h is not available locally")

    def test_flac_adapter_full_host_lifecycle_and_fidelity(self):
        report = self.checker.check(self.header)

        self.assertTrue(report.get("all_passed"), "all adapter tests must pass")

        # Bit-exact PCM fidelity
        pcm = report.get("pcm_fidelity", {})
        self.assertTrue(pcm.get("cd_16bit_bit_exact"), "16-bit PCM must match ffmpeg byte-for-byte")
        self.assertTrue(pcm.get("studio_24bit_bit_exact"), "24-bit PCM must match ffmpeg byte-for-byte")

        # Fixed arena memory limit
        arena = report.get("fixed_arena", {})
        self.assertTrue(arena.get("within_limit"), "allocator must remain within fixed arena")
        self.assertLessEqual(arena.get("cd_16bit_used_bytes", 999999), 98304)
        self.assertLessEqual(arena.get("studio_24bit_used_bytes", 999999), 98304)

        # Seek, lifecycle, lock balance on CD profile
        cd_test = report["tests"]["cd_16bit_44k"]
        self.assertTrue(cd_test["descriptor"]["pcm_format_valid"])
        self.assertTrue(cd_test["sequential_decode"]["passed"])
        self.assertTrue(cd_test["seek_tests"]["passed"])
        self.assertTrue(cd_test["lifecycle"]["passed"])
        self.assertTrue(cd_test["lock"]["balanced"])

        # Seek, lifecycle, lock balance on Studio profile
        studio_test = report["tests"]["studio_24bit_48k"]
        self.assertTrue(studio_test["descriptor"]["pcm_format_valid"])
        self.assertTrue(studio_test["sequential_decode"]["passed"])
        self.assertTrue(studio_test["seek_tests"]["passed"])
        self.assertTrue(studio_test["lifecycle"]["passed"])
        self.assertTrue(studio_test["lock"]["balanced"])

        # Rejection of unsupported profiles
        # Mono is served, as stereo with the one channel on both sides.
        self.assertTrue(report["tests"]["mono_served"]["success"])
        self.assertEqual(report["tests"]["mono_served"]["format"]["channels"], 2)
        self.assertTrue(report["tests"]["unsupported_rate_rejection"]["rejected"])
        self.assertTrue(report["tests"]["unsupported_rate_rejection"]["lock_balanced"])
        self.assertTrue(report["tests"]["corrupt_rejection"]["rejected"])
        self.assertTrue(report["tests"]["corrupt_rejection"]["lock_balanced"])
        self.assertTrue(report["tests"]["zero_frame_rejection"]["rejected"])
        self.assertTrue(report["tests"]["zero_frame_rejection"]["lock_balanced"])
        self.assertTrue(report["tests"]["huge_frame_rejection"]["rejected"])
        self.assertTrue(report["tests"]["huge_frame_rejection"]["lock_balanced"])


if __name__ == "__main__":
    unittest.main()
