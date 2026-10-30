# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
from __future__ import annotations

import importlib.util
from pathlib import Path
import shutil
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_checker():
    spec = importlib.util.spec_from_file_location("check_flac_seek", ROOT / "tools" / "check_flac_seek.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FlacBoundedSeekTests(unittest.TestCase):
    def setUp(self):
        for tool in ("ffmpeg", "flac", "metaflac"):
            if shutil.which(tool) is None:
                self.skipTest(f"{tool} is required to generate the seek test files")
        self.header = ROOT / "src" / "dr_flac.h"
        if not self.header.is_file():
            self.skipTest("dr_flac.h is not available locally")

    def test_every_seek_is_sample_exact_and_bounded(self):
        report = load_checker().check(self.header)

        for name, case in report["cases"].items():
            with self.subTest(case=name):
                self.assertEqual(case.get("mismatches"), 0, case)
                self.assertEqual(case.get("failures"), 0, case)
                self.assertEqual(case.get("fallbacks"), 0, "the bounded search must finish on its own")
                self.assertLessEqual(case.get("max_reads", 1 << 30), report["max_reads_allowed"])
        self.assertTrue(report["all_passed"])


if __name__ == "__main__":
    unittest.main()
