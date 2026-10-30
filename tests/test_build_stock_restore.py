# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
from __future__ import annotations

import hashlib
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from build_stock_restore import build_restore  # noqa: E402


# Restore files that were flashed and verified on a physical player.
HARDWARE_VERIFIED_RESTORES = {
    "1.20": "fb883cd80d3d0d6e593de59dc3b78defba9c9202126c7e9d5040ce76c9846c23",
    "1.24": "2604a823209f4242f795f5f9f2eb25d9da8ac4509b5a94112d9a4e6ec6f141a5",
}


class StockRestoreTests(unittest.TestCase):
    def setUp(self):
        path = ROOT / "firmware" / "original" / "XDJ700.UPD"
        if not path.is_file():
            self.skipTest("official XDJ700.UPD is not available locally")
        self.stock = path.read_bytes()

    def test_reproduces_hardware_verified_restores(self):
        for version, expected in HARDWARE_VERIFIED_RESTORES.items():
            restore = build_restore(self.stock, version)
            self.assertEqual(hashlib.sha256(restore).hexdigest(), expected, version)

    def test_default_restore_tag(self):
        restore = build_restore(self.stock, "1.17")
        self.assertIn(b"MAINVer1.17\x00", restore[:128])
        self.assertEqual(len(restore), len(self.stock))

    def test_rejects_tags_the_updater_would_skip(self):
        for version in ("1.15", "1.10"):
            with self.assertRaises(ValueError):
                build_restore(self.stock, version)


if __name__ == "__main__":
    unittest.main()
