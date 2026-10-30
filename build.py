#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Build XDJ-700 PLUS from the official 1.15 file: build/XDJ700.UPD.

The same as tools/build_flac_firmware.py with --output build/XDJ700.UPD. Other
options are passed on to it (for example --everyday).
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(ROOT, "build", "XDJ700.UPD")

if __name__ == "__main__":
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    builder = os.path.join(ROOT, "tools", "build_flac_firmware.py")
    sys.exit(subprocess.call([sys.executable, builder, "--output", OUTPUT] + sys.argv[1:]))
