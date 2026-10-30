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


def load_corpus_checker():
    spec = importlib.util.spec_from_file_location(
        "check_flac_corpus", ROOT / "tools" / "check_flac_corpus.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FlacCorpusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.checker = load_corpus_checker()
        cls.header_candidates = [
            ROOT / "src" / "dr_flac.h",
            Path("/private/tmp/dr_flac.h"),
            Path("/tmp/dr_flac.h"),
        ]

    def setUp(self):
        if shutil.which("ffmpeg") is None:
            self.skipTest("ffmpeg is required for test file generation")
        if shutil.which("flac") is None:
            self.skipTest("flac CLI tool is required for reference encoder tests")
        self.header = None
        for candidate in self.header_candidates:
            if candidate.is_file():
                self.header = candidate
                break
        if self.header is None:
            self.skipTest("dr_flac.h is not available locally")

    def test_real_world_flac_corpus_matrix(self):
        report = self.checker.check(self.header)

        self.assertTrue(report.get("all_passed"), "all real-world FLAC corpus tests must pass")
        self.assertEqual(report.get("total_corpus_files_tested"), 15)

        results = report.get("results", {})

        # Check standard presets
        self.assertTrue(results["level_0_fastest"]["passed"])
        self.assertTrue(results["level_5_default"]["passed"])
        self.assertTrue(results["level_8_maximum"]["passed"])

        # Check block sizes
        self.assertTrue(results["blocksize_1152"]["passed"])
        self.assertTrue(results["blocksize_2048"]["passed"])
        self.assertTrue(results["blocksize_4096_standard"]["passed"])
        self.assertTrue(results["blocksize_4608_cd_max"]["passed"])

        # Check stereo modes
        self.assertTrue(results["independent_stereo_channel"]["passed"])
        self.assertTrue(results["mid_side_stereo_channel"]["passed"])

        # Check metadata
        self.assertTrue(results["vorbis_comments_metadata"]["passed"])
        self.assertTrue(results["large_embedded_cover_art"]["passed"])
        self.assertTrue(results["custom_seektable_dense"]["passed"])

        # Check studio 24-bit
        self.assertTrue(results["studio_24bit_level_5"]["passed"])
        self.assertTrue(results["studio_24bit_level_8"]["passed"])
        self.assertTrue(results["studio_24bit_with_cover_and_tags"]["passed"])

        # All arena usages must be bounded <= 64 KiB
        for test_name, res in results.items():
            self.assertLessEqual(res["arena_used_bytes"], 65536, f"{test_name} exceeded 64 KiB arena")


if __name__ == "__main__":
    unittest.main()
