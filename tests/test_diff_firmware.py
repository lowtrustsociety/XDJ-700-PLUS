# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


def load_diff_tool():
    spec = importlib.util.spec_from_file_location(
        "diff_firmware", ROOT / "tools" / "diff_firmware.py"
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FirmwareDiffTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.diff_tool = load_diff_tool()
        cls.stock_path = ROOT / "firmware" / "original" / "XDJ700.UPD"
        cls.mod_path = ROOT / "build" / "XDJ700_MOD_FLAC.UPD"

    def setUp(self):
        if not self.stock_path.is_file():
            self.skipTest("official XDJ700.UPD is not available locally")
        if not self.mod_path.is_file():
            self.skipTest("candidate build/XDJ700_MOD_FLAC.UPD has not been generated")

    def test_firmware_differential_invariants(self):
        report = self.diff_tool.compare(self.stock_path, self.mod_path)

        # 1. Overall safety assessment
        assessment = report["safety_assessment"]
        self.assertTrue(assessment["sector_0_untouched"], "Sector 0 must be 100% byte-for-byte identical")
        self.assertTrue(assessment["panel_mcu_untouched"], "Panel MCU component must be 100% byte-for-byte identical")
        self.assertTrue(assessment["tail_safely_appended"], "Tail payload must be appended cleanly")
        self.assertTrue(assessment["all_safety_invariants_held"], "All safety invariants must pass")

        # 2. Sector 0 bootloader preservation
        c0 = report["component_0_main_cpu"]
        s0 = c0["sector_0_bootloader"]
        self.assertTrue(s0["byte_identical"])
        self.assertEqual(s0["bytes_checked"], 262144)
        self.assertEqual(s0["differing_byte_ranges"], 0)
        self.assertTrue(s0["emergency_updater_preserved"])

        # 3. Component 1 panel MCU preservation
        c1 = report["component_1_panel_mcu"]
        self.assertTrue(c1["byte_identical"])
        self.assertEqual(c1["bytes"], 15454)

        # 4. Packed application and diff sites
        app = c0["packed_application"]
        self.assertTrue(app["checksum_is_valid"])
        self.assertLess(app["mod_packed_bytes"], app["stock_packed_bytes"])
        self.assertEqual(app["packed_bytes_delta"], -7513)
        self.assertEqual(app["unpacked_growth_bytes"], 43724)

        # Exactly 18 contiguous diff regions across the 18.6 MB application
        self.assertEqual(app["app_diff_regions_count"], 18)


if __name__ == "__main__":
    unittest.main()
