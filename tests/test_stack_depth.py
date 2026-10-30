# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_tool():
    spec = importlib.util.spec_from_file_location("stack_depth", ROOT / "tools" / "stack_depth.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class StackDepthTests(unittest.TestCase):
    def setUp(self):
        self.tool = load_tool()
        if not self.tool.DEFAULT_PREFIX.with_name(self.tool.DEFAULT_PREFIX.name + "gcc").exists():
            self.skipTest("SH cross compiler is not available")

    def test_entry_points_fit_the_input_task_stack(self):
        # The input task has a 2 KB stack; keep our share well below it, leaving
        # room for the stock callers above and the filesystem frames below.
        for audible in (True, False):
            report = self.tool.measure(self.tool.DEFAULT_PREFIX, ROOT / "src" / "dr_flac.h", audible)
            for entry in ("xdj_flac_virtual_read_format", "xdj_flac_virtual_seek_format",
                          "xdj_flac_virtual_eof_format"):
                self.assertGreater(report[entry]["bytes"], 0, entry)
                self.assertLessEqual(report[entry]["bytes"], 1200, (entry, report[entry]["chain"]))

    def test_the_release_fits_too(self):
        # The release adds the feed to the reads and the load of our block to the open, which runs in
        # tsk_DJcontInBufMngSub (3 KB of stack); hold it to the reads' bound all the same.
        report = self.tool.measure(self.tool.DEFAULT_PREFIX, ROOT / "src" / "dr_flac.h", False, True)
        for entry in ("xdj_flac_virtual_read_format", "xdj_flac_virtual_seek_format",
                      "xdj_flac_virtual_eof_format", "xdj_flac_adapter"):
            self.assertGreater(report[entry]["bytes"], 0, entry)
            self.assertLessEqual(report[entry]["bytes"], 1200, (entry, report[entry]["chain"]))


if __name__ == "__main__":
    unittest.main()
