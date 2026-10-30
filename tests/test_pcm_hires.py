# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
from pathlib import Path
import shutil
import struct
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_checker():
    spec = importlib.util.spec_from_file_location("check_pcm_hires", ROOT / "tools" / "check_pcm_hires.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class PcmHiResTests(unittest.TestCase):
    def setUp(self):
        self.header = ROOT / "src" / "dr_flac.h"
        if not self.header.is_file():
            self.skipTest("dr_flac.h is not available locally")
        for tool in ("sox", "flac"):
            if shutil.which(tool) is None:
                self.skipTest(f"{tool} is not available locally")

    def test_file_builders_write_the_headers_they_claim(self):
        checker = load_checker()
        samples = bytes(range(48)) * 2
        plain = checker.wav_file(samples, 96000, 24)
        self.assertEqual(plain[:4] + plain[8:16], b"RIFFWAVEfmt ")
        self.assertEqual(struct.unpack_from("<IHHI", plain, 16), (16, 1, 2, 96000))
        extensible = checker.wav_file(samples, 96000, 24, extensible=True)
        self.assertEqual(struct.unpack_from("<IH", extensible, 16), (40, 0xFFFE))
        self.assertEqual(struct.unpack_from("<H", extensible, 44)[0], 1)
        aiff = checker.aiff_file(samples, 96000, 24)
        self.assertEqual(aiff[:4] + aiff[8:16], b"FORMAIFFCOMM")
        # 96000 as an 80-bit float: exponent 16, mantissa 0xBB80 followed by zeros.
        self.assertEqual(aiff[28:38], bytes.fromhex("400fbb80000000000000"))
        # Big-endian samples: the first 24-bit sample 00 01 02 is stored as 02 01 00.
        self.assertEqual(aiff[aiff.index(b"SSND") + 16 :][:3], bytes([2, 1, 0]))

    def test_served_files_match_the_flac_path_and_stock_files_are_declined(self):
        checker = load_checker()
        report = checker.check(self.header)
        self.assertEqual(len(report["served_converted"]), 28)
        for name, entry in report["served_converted"].items():
            self.assertTrue(entry["equals_flac_path"], name)
            self.assertTrue(entry["passed"], (name, entry))
        self.assertEqual(len(report["served_as_is"]), 4)
        for name, entry in report["served_as_is"].items():
            self.assertTrue(entry["equals_file_samples"], name)
            self.assertTrue(entry["passed"], (name, entry))
        # Mono FLAC, WAV and AIFF are served as stereo, each sample on both sides.
        self.assertEqual(len(report["mono"]), 12)
        for name, entry in report["mono"].items():
            self.assertTrue(entry["passed"], (name, entry))
        self.assertEqual(len(report["declined"]), 12)
        for name, entry in report["declined"].items():
            self.assertTrue(entry.get("declined"), (name, entry))
            self.assertTrue(entry.get("handle_restored"), name)
            self.assertTrue(entry.get("slots_untouched"), name)
            self.assertEqual(entry.get("locks_taken"), 0, name)
        self.assertTrue(report["all_passed"])


if __name__ == "__main__":
    unittest.main()
