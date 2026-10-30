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


def load_concurrency_checker():
    spec = importlib.util.spec_from_file_location(
        "check_flac_concurrency", ROOT / "tools" / "check_flac_concurrency.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FlacAdapterConcurrencyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.checker = load_concurrency_checker()
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

    def test_multi_threaded_concurrency_stress_and_safety_invariants(self):
        # Run 1 second of heavy multi-threaded stress per profile
        report = self.checker.check(self.header, duration_seconds=1)

        self.assertTrue(report.get("all_passed"), "all concurrency stress tests must pass")

        agg = report["aggregate"]
        self.assertGreater(agg["total_lock_acquires"], 10000, "must perform tens of thousands of lock operations")
        self.assertGreater(agg["total_contentions"], 100, "must observe real lock contention and yield resolution")
        self.assertGreater(agg["total_seeks_performed"], 500, "must execute high-frequency jog seeks")
        self.assertGreater(agg["total_track_reloads"], 50, "must execute rapid deactivation and reloads")
        self.assertLessEqual(agg["peak_arena_bytes"], 65536, "arena must stay bounded under concurrency")

        # CD 16-bit 44.1 kHz profile assertions
        cd = report["profiles"]["cd_16bit_44k"]
        self.assertTrue(cd["safety_invariants"]["mutual_exclusion_preserved"])
        self.assertTrue(cd["safety_invariants"]["no_deadlocks"])
        self.assertTrue(cd["safety_invariants"]["arena_within_limit"])
        self.assertTrue(cd["synchronization"]["balanced"])

        # Studio 24-bit 48.0 kHz profile assertions
        studio = report["profiles"]["studio_24bit_48k"]
        self.assertTrue(studio["safety_invariants"]["mutual_exclusion_preserved"])
        self.assertTrue(studio["safety_invariants"]["no_deadlocks"])
        self.assertTrue(studio["safety_invariants"]["arena_within_limit"])
        self.assertTrue(studio["synchronization"]["balanced"])


if __name__ == "__main__":
    unittest.main()
