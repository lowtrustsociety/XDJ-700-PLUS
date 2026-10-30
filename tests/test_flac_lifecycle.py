# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_checker():
    spec = importlib.util.spec_from_file_location("check_flac_lifecycle", ROOT / "tools" / "check_flac_lifecycle.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FlacLifecycleTests(unittest.TestCase):
    def setUp(self):
        if shutil.which("ffmpeg") is None:
            self.skipTest("ffmpeg is required to generate the test files")
        self.header = ROOT / "src" / "dr_flac.h"
        if not self.header.is_file():
            self.skipTest("dr_flac.h is not available locally")

    def test_track_switches_never_fail_or_mix_tracks(self):
        report = load_checker().check(self.header)
        for seed, result in report["runs"].items():
            self.assertTrue(result["passed"], (seed, result))
            self.assertGreater(result["commands"], 20)
            self.assertGreater(result["exact_reads"], 1000)
            self.assertGreater(result["switch_calls"], 0)
        for seed, result in report["network_runs"].items():
            self.assertTrue(result["passed"], ("network", seed, result))
        self.assertTrue(report["evictions"]["passed"], report["evictions"])
        self.assertEqual(report["evictions"]["tracks"], 8)
        # Trusting every handle, as diag8 and diag9 did, must fail here as it did on the player.
        self.assertFalse(report["trusting_every_handle"]["passed"])
        # A stock file as long as one of our tracks is left to the stock reader, over LINK too.
        for filesystem, result in report["same_length_twin"].items():
            self.assertTrue(result["passed"], (filesystem, result))
        self.assertTrue(report["all_passed"])


if __name__ == "__main__":
    unittest.main()
