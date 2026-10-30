# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
from pathlib import Path
import shutil
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_checker():
    spec = importlib.util.spec_from_file_location("check_flac_hires", ROOT / "tools" / "check_flac_hires.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FlacHiResTests(unittest.TestCase):
    def setUp(self):
        self.header = ROOT / "src" / "dr_flac.h"
        if not self.header.is_file():
            self.skipTest("dr_flac.h is not available locally")
        for tool in ("sox", "flac", "metaflac", "ffmpeg"):
            if shutil.which(tool) is None:
                self.skipTest(f"{tool} is not available locally")

    def test_88k_and_96k_are_converted_down_and_higher_rates_refused(self):
        checker = load_checker()
        report = checker.check(self.header)
        self.assertEqual(len(report["files"]), 4)
        for name, entry in report["files"].items():
            self.assertTrue(entry["passed"], (name, entry))
        self.assertEqual(report["refused"], {"176400 Hz": True, "192000 Hz": True})
        self.assertEqual(len(report["tones"]), 2)
        for name, entry in report["tones"].items():
            self.assertLessEqual(abs(entry["kept_tone_change_db"]), checker.PASSBAND_TOLERANCE_DB, name)
            self.assertLessEqual(entry["alias_level_db"], -checker.MIN_ALIAS_REJECTION_DB, name)
        self.assertTrue(report["all_passed"])


if __name__ == "__main__":
    unittest.main()
