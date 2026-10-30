#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Design the half-band low-pass filters the bridge halves 88.2 and 96 kHz sources with.

Each filter is a Kaiser-windowed sinc with its cutoff at a quarter of the
source rate. In such a filter the centre tap is one half, every second tap is
zero, and the rest come in equal pairs at odd distances from the centre, so
one side's coefficients describe it. They are quantized to Q31 and the first
one is adjusted so one side sums to exactly a quarter (gain one at DC).

The table printed here is the one in src/flac_decoder.c
(XDJ_HALFBAND_TERMS); `--check` compares the two.

    python3 tools/design_halfband.py            # coefficients and responses
    python3 tools/design_halfband.py --check    # fail if the source differs
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "flac_decoder.c"

# Taps and the Kaiser window's beta, chosen for the trade the table in the
# source describes: fewer taps cost less and reject less.
DESIGNS = {47: 6.0, 31: 5.0, 23: 4.5, 15: 3.5}


def bessel_i0(x: float) -> float:
    total = term = 1.0
    k = 1
    while term > 1e-12 * total:
        term *= (x / (2 * k)) ** 2
        total += term
        k += 1
    return total


def one_side(taps: int, beta: float) -> list[int]:
    """Q31 coefficients at distances 1, 3, 5, ... from the centre."""
    centre = (taps - 1) // 2
    side = []
    for distance in range(1, centre + 1, 2):
        ideal = math.sin(math.pi * distance / 2) / (math.pi * distance)
        window = bessel_i0(beta * math.sqrt(1 - (distance / centre) ** 2)) / bessel_i0(beta)
        side.append(round(ideal * window * 2**31))
    side[0] += 2**29 - sum(side)
    return side


def response_db(side: list[int], frequency: float, rate: float) -> float:
    """Gain at a frequency of the source, in dB."""
    gain = 0.5 + 2 * sum(
        value / 2**31 * math.cos(2 * math.pi * frequency / rate * (2 * index + 1)) for index, value in enumerate(side)
    )
    return 20 * math.log10(max(abs(gain), 1e-9))


def worst_from(side: list[int], start: float, rate: float) -> float:
    """The least rejection from a frequency up to half the source rate."""
    return max(response_db(side, frequency, rate) for frequency in range(int(start), int(rate / 2) + 1, 100))


def source_terms() -> dict[int, list[int]]:
    """The coefficient lists in the C source, by tap count."""
    text = SOURCE.read_text()
    found = {}
    for match in re.finditer(
        r"#(?:el)?if XDJ_HALFBAND_TAPS == (\d+)\n#define XDJ_HALFBAND_TERMS\(T\)((?:.*\\\n)*.*)\n", text
    ):
        found[int(match.group(1))] = [int(value) for value in re.findall(r"T\(\d+, (-?\d+)\)", match.group(2))]
    return found


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="compare with the coefficients in the source")
    parser.add_argument("--rate", type=float, default=96000.0, help="source rate for the response table")
    args = parser.parse_args()

    in_source = source_terms()
    mismatch = False
    for taps, beta in DESIGNS.items():
        side = one_side(taps, beta)
        scale = args.rate / 96000.0
        print(f"{taps} taps (beta {beta}): {side}")
        print(
            "    "
            + ", ".join(f"{int(f * scale / 100) / 10:g} kHz {response_db(side, f * scale, args.rate):+.2f} dB"
                        for f in (16000, 18000, 20000, 22000))
        )
        print(
            "    least rejection of what folds back, from "
            + ", ".join(f"{int(f * scale / 100) / 10:g} kHz: {worst_from(side, f * scale, args.rate):.0f} dB"
                        for f in (28000, 30000, 34000))
        )
        if args.check and in_source.get(taps) != side:
            print(f"    MISMATCH: the source has {in_source.get(taps)}")
            mismatch = True
    if args.check:
        if mismatch or set(in_source) != set(DESIGNS):
            raise SystemExit("the source's coefficients differ from this design")
        print("the source's coefficients match")


if __name__ == "__main__":
    main()
