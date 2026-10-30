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


def load_reverse_pitch_checker():
    spec = importlib.util.spec_from_file_location(
        "check_flac_reverse_pitch", ROOT / "tools" / "check_flac_reverse_pitch.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FlacReversePitchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.checker = load_reverse_pitch_checker()
        cls.header_candidates = [
            ROOT / "src" / "dr_flac.h",
            Path("/private/tmp/dr_flac.h"),
            Path("/tmp/dr_flac.h"),
        ]

    def setUp(self):
        if shutil.which("ffmpeg") is None:
            self.skipTest("ffmpeg is required for test stream generation")
        self.header = None
        for candidate in self.header_candidates:
            if candidate.is_file():
                self.header = candidate
                break
        if self.header is None:
            self.skipTest("dr_flac.h is not available locally")

    def test_reverse_playback_pitch_fader_and_scratching(self):
        report = self.checker.check(self.header)

        self.assertTrue(report.get("all_passed"), "all reverse and pitch tests must pass")

        # CD 16-bit 44.1k profile
        cd = report["profiles"]["cd_16bit_44k"]
        self.assertTrue(cd["reverse_playback"]["passed"], "reverse chunk streaming must pass")
        self.assertGreaterEqual(cd["reverse_playback"]["chunks_tested"], 100)
        self.assertTrue(cd["variable_pitch"]["passed"], "variable pitch chunk sizing must pass")
        self.assertGreaterEqual(cd["variable_pitch"]["chunks_tested"], 100)
        self.assertTrue(cd["vinyl_scratch"]["passed"], "high-velocity vinyl scratching must pass")
        self.assertEqual(cd["vinyl_scratch"]["cycles_tested"], 1000)
        self.assertTrue(cd["boundary_safety"]["passed"], "boundary underflow/overflow must be safe")
        self.assertTrue(cd["lock_balanced"], "decoder lock must remain balanced")
        self.assertLessEqual(cd["arena_used"], 65536, "arena must stay within 64 KiB limit")

        # Studio 24-bit 48k profile
        studio = report["profiles"]["studio_24bit_48k"]
        self.assertTrue(studio["reverse_playback"]["passed"], "reverse chunk streaming must pass")
        self.assertGreaterEqual(studio["reverse_playback"]["chunks_tested"], 100)
        self.assertTrue(studio["variable_pitch"]["passed"], "variable pitch chunk sizing must pass")
        self.assertGreaterEqual(studio["variable_pitch"]["chunks_tested"], 100)
        self.assertTrue(studio["vinyl_scratch"]["passed"], "high-velocity vinyl scratching must pass")
        self.assertEqual(studio["vinyl_scratch"]["cycles_tested"], 1000)
        self.assertTrue(studio["boundary_safety"]["passed"], "boundary underflow/overflow must be safe")
        self.assertTrue(studio["lock_balanced"], "decoder lock must remain balanced")
        self.assertLessEqual(studio["arena_used"], 65536, "arena must stay within 64 KiB limit")


if __name__ == "__main__":
    unittest.main()
