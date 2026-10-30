# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_checker():
    spec = importlib.util.spec_from_file_location("check_flac_two_tracks", ROOT / "tools" / "check_flac_two_tracks.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FlacTwoTrackTests(unittest.TestCase):
    def setUp(self):
        if shutil.which("ffmpeg") is None:
            self.skipTest("ffmpeg is required to generate the test files")
        self.header = ROOT / "src" / "dr_flac.h"
        if not self.header.is_file():
            self.skipTest("dr_flac.h is not available locally")

    def test_two_tasks_never_mix_their_tracks(self):
        report = load_checker().check(self.header)
        runs = report["runs"]
        for mode in ("format",):
            self.assertTrue(runs[mode]["passed"], runs[mode])
            for track in runs[mode]["tracks"]:
                self.assertGreater(track["jumps"], 100)
        self.assertTrue(report["all_passed"])


if __name__ == "__main__":
    unittest.main()
