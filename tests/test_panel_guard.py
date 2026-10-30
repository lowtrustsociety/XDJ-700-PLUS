# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
import importlib.util
from pathlib import Path
import shutil
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_checker():
    spec = importlib.util.spec_from_file_location("check_panel_guard", ROOT / "tools" / "check_panel_guard.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(shutil.which("cc"), "no C compiler")
class PanelGuardTests(unittest.TestCase):
    def test_restarts_only_a_stuck_exchange(self):
        report = load_checker().check()
        self.assertTrue(report["passed"], report["failed"])
        self.assertGreaterEqual(report["checks"], 18)


if __name__ == "__main__":
    unittest.main()
