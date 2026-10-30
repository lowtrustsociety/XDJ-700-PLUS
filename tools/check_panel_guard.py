#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Host check of the panel guard (src/panel_guard.c, --panel-guard).

Builds the guard and the version label (src/version_label.c) against a pretend
exchange (src/panel_guard_host_runner.c), one millisecond a step, and reads what it
prints. Checked: the stock timer handler is
called at every tick; normal running (a frame every 3 ms) gives no restart and a longest
stand of 1 ms; an exchange stuck with the port's overrun is restarted 200 ms later (the
port's flags cleared, both channels off, the port line up, the byte 0) and runs on; one
stuck without the overrun is restarted only after 2 s; with the exchange not in use, or
with the player's count moving, nothing is done; a stand of 150 ms that ends by itself
is no restart and is kept as the longest; one stuck again at once after every restart is
restarted every 200 ms; nothing happens in the first 20 s after power-on; a player
object outside main memory still lets the overrun rule work.
"""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / "src" / name for name in ("panel_guard_host_runner.c", "panel_guard.c", "version_label.c")]
LABEL = "PLUS V0.90 G"
COUNTED = []


def run(scene: str) -> dict:
    with tempfile.TemporaryDirectory(prefix="xdj700-panel-guard-") as folder:
        runner = Path(folder) / "runner"
        subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DXDJ_HOST_TEST",
             *COUNTED,
             f'-DXDJ_VERSION_LABEL="{LABEL}"', "-I", str(ROOT / "src"), *map(str, SOURCES), "-o", str(runner)],
            check=True,
        )
        result = subprocess.run([str(runner), scene], capture_output=True, text=True, check=True)
    if result.stderr:
        raise AssertionError(f"{scene}: {result.stderr}")
    restarts = []
    labels = []
    end = {}
    for line in result.stdout.splitlines():
        words = line.split()
        if words[0] == "restart":
            restarts.append(tuple(int(word) for word in words[1:]))
        elif words[0] == "label":
            labels.append(line[len("label "):])
        elif words[0] == "end":
            end = {key: value for key, value in (word.split("=") for word in words[1:])}
    return {"restarts": restarts, "labels": labels, "end": end}


def check() -> dict:
    failed = []
    checks = 0

    def expect(condition: bool, what: str) -> None:
        nonlocal checks
        checks += 1
        if not condition:
            failed.append(what)

    normal = run("normal")
    expect(normal["end"]["ticks"] == "120000", f"stock handler calls: {normal['end']['ticks']}")
    expect(normal["restarts"] == [], f"normal: restarts {normal['restarts']}")
    expect(normal["end"]["longest"] == "1", f"normal: longest {normal['end']['longest']}")

    stuck = run("stuck")
    expect(stuck["restarts"] == [(60199, 1, 0)], f"stuck: restarts {stuck['restarts']}")
    end = stuck["end"]
    expect(end["lsr"] == "0" and end["fsr"] == "96", f"stuck: port flags {end['lsr']} {end['fsr']}")
    expect(end["rx"] == "40004800" and end["tx"] == "40001800", f"stuck: channels {end['rx']} {end['tx']}")
    expect(end["fast"] == "1" and end["slow"] == "0", f"stuck: counts {end['fast']} {end['slow']}")

    nooverrun = run("nooverrun")
    expect(nooverrun["restarts"] == [(61999, 0, 1)], f"nooverrun: restarts {nooverrun['restarts']}")
    expect(nooverrun["end"]["busy"] in ("0", "1") and nooverrun["end"]["slow"] == "1", "nooverrun: end")

    off = run("off")
    expect(off["restarts"] == [] and off["end"]["busy"] == "1", f"off: {off['restarts']} {off['end']}")

    moving = run("moving")
    expect(moving["restarts"] == [] and moving["end"]["busy"] == "1", f"moving: {moving['restarts']}")

    late = run("late")
    expect(late["restarts"] == [], f"late: restarts {late['restarts']}")
    expect(late["end"]["longest"] == "150", f"late: longest {late['end']['longest']}")

    hard = run("hard")
    times = [restart[0] for restart in hard["restarts"]]
    gaps = {b - a for a, b in zip(times, times[1:])}
    expect(len(times) >= 14 and times[0] == 60199 and gaps <= {200, 201}, f"hard: restarts at {times}")
    expect(all(restart[2] == 0 for restart in hard["restarts"]), "hard: slow rule used")

    early = run("early")
    # Armed at 20 s; its first look then takes the count, so the stand counts from the next.
    expect(early["restarts"] == [(20201, 1, 0)], f"early: restarts {early['restarts']}")

    noobject = run("noobject")
    expect(noobject["restarts"] == [(60199, 1, 0)], f"noobject: restarts {noobject['restarts']}")

    expect(hard["labels"][-1].startswith(LABEL), f"hard: label {hard['labels'][-1]}")

    return {"passed": not failed, "checks": checks, "failed": failed}


def main() -> None:
    report = check()
    print(json.dumps(report, indent=2))
    sys.exit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()
