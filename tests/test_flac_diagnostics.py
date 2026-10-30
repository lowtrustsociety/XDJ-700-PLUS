# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_checker():
    spec = importlib.util.spec_from_file_location(
        "check_flac_diagnostics", ROOT / "tools" / "check_flac_diagnostics.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FlacDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        if shutil.which("ffmpeg") is None:
            self.skipTest("ffmpeg is required for FLAC generation and tone decoding")
        self.header = ROOT / "src" / "dr_flac.h"
        if not self.header.is_file():
            self.skipTest("dr_flac.h is not available locally")

    def test_episode_timing_and_report_round_trip(self):
        report = load_checker().check(self.header)

        episodes = report["episodes"]
        self.assertTrue(episodes.get("passed"), episodes.get("stderr"))
        self.assertGreaterEqual(episodes.get("checks", 0), 60)
        for case in report["report_round_trips"]:
            self.assertTrue(
                case["matched"],
                f"reports rendered at {case['render_rate']} Hz and played at "
                f"{case['playback_rate']} Hz must decode exactly: {case['fields']}",
            )
        paced = report["paced_fetch"]
        self.assertTrue(paced.get("passed"), paced.get("stderr"))
        self.assertGreaterEqual(paced.get("checks", 0), 40)
        self.assertTrue(report["all_passed"])


if __name__ == "__main__":
    unittest.main()
