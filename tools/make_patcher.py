#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Write the end-user patcher: patcher/xdj700_plus_patcher.py, and the same as a web page,
patcher/xdj700_plus_patcher.html.

The patcher is tools/patcher_template.py with the mod's own part filled in:
our bytes at the places the mod hooks into the application, our compiled code
that goes after the application's end, and the hashes that tie the result to
one tested build. It holds nothing of Pioneer's: the user supplies the
official 1.15 update file, and the patcher redoes on their machine what
tools/build_flac_firmware.py does here, without a compiler. The page is
tools/web_patcher_template.html with tools/web_patcher_core.js and the same
data: it does the same in the browser, on the user's computer, and hands
out a zip with the mod, the restore file and the steps.

The build it reproduces is the builder's default one (--release or
--everyday choose). Generating needs what that build needs (the official
file in firmware/original, the cross compiler, and for the release the
audio processor's compiler).

Example:

    python3 tools/make_patcher.py
    python3 patcher/xdj700_plus_patcher.py firmware/original/XDJ700.UPD

After a change to the templates alone, `--rerender` writes both files again
from the data of the patcher already there, without building (so without
the official file or a compiler).
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import textwrap
import zlib

from build_flac_firmware import RELEASE_IS_DEFAULT, build, build_options
from build_stock_restore import build_restore
from check_lzss_roundtrip import APPLICATION_OFFSET, stock_application
from inspect_upd import decode_srecords, decompress_lzss, split_container

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "tools" / "patcher_template.py"
OUTPUT = ROOT / "patcher" / "xdj700_plus_patcher.py"
PLACEHOLDER = "PATCH_DATA = None\n"
PAGE_TEMPLATE = ROOT / "tools" / "web_patcher_template.html"
PAGE_CORE = ROOT / "tools" / "web_patcher_core.js"
PAGE_OUTPUT = ROOT / "patcher" / "xdj700_plus_patcher.html"
PAGE_PLACEHOLDERS = ("/*CORE*/", "/*PATCH_DATA*/null", "/*FONTS*/")
# The page's font, put into it as it is (tools/fonts/README.md): file and weight.
PAGE_FONTS = (("IoskeleyMono-Regular.woff2", 400), ("IoskeleyMono-Medium.woff2", 500), ("IoskeleyMono-Bold.woff2", 700))
# What the zip from the page says, before the notice.
HOW_TO = """\
{title}

1_mod/XDJ700.UPD            the mod
2_stock_restore/XDJ700.UPD  the restore file (the official firmware 1.15)

You need a FAT32 USB stick with about 18 MB free.

To install:
1. Copy 1_mod/XDJ700.UPD to the top folder of a FAT32 USB stick.
2. Eject the stick properly, plug it back in, and check the file on the
   stick with the page ("steps and file check").
3. With the player switched off, put the stick in. Hold IN and RELOOP/EXIT,
   switch the player on, and keep holding until the update screen appears.
4. Wait until it has finished and the player has restarted. Do not switch
   it off before that.

To go back to stock: the same steps with 2_stock_restore/XDJ700.UPD. Keep
that file. Both files have the same name, so keep one at a time on the
stick. The update screen shows {mod_tag} for the mod and {restore_tag} for the restore
file: those numbers are only there so that the player accepts the file.

The mod's SHA-256: {mod_sha256}
The restore file's SHA-256: {restore_sha256}

"""
MOD_TAG = "1.16"
RESTORE_TAG = "1.17"
# The mod changes a jump at the dispatcher, 19 call-target literals and an 8-byte check; the release also
# the two addresses of the version label and the two "Ver " texts.
MOST_CHANGED_BYTES = 128
# What the patcher shows of the build it makes: its name, the day it was approved on the player, and what
# is good to know about it (the notice lists it).
EVERYDAY_BUILD = ("paced2", "2026-10-13", [
    "88.2 and 96 kHz files are played at 44.1 and 48 kHz.",
    "Right after loading a FLAC or hi-res file the screen is slow for a few seconds while the player prepares "
    "the hot cues.",
    "On 88.2 and 96 kHz FLAC the waveform is slightly less smooth.",
    "Spinning the jog forward very fast on a FLAC can outrun the decoder; the player then loops a short piece "
    "until it catches up.",
    "32-bit files, 176.4 and 192 kHz files and ALAC do not play.",
    "The player still shows version 1.15 in its menu.",
])
# The day the user approved the release on the player.
RELEASE_BUILD = ("PLUS V0.90", "2026-10-30", [
    "88.2 and 96 kHz files are downsampled to 44.1 and 48 kHz respectively.",
    "Right after loading a FLAC or hi-res file, the screen can be slower than usual for a few moments while "
    "the player buffers. The sound is not affected: no delay, no lag, no gaps. It is only visual.",
    "An edge case with 96 kHz 24-bit FLAC pitched above +50 % with MASTER TEMPO on: spinning the jog wheel "
    "very fast in vinyl mode can give short gaps in the sound. The sound recovers as soon as you stop "
    "spinning. In extreme cases it can end in the player's emergency loop; loading the same or another track "
    "recovers it.",
    "FLAC files encoded with very large blocks (16,384 frames, which is rare): the same applies to very fast "
    "jog spinning, and hitting their hot cues in a frenzy can also give short gaps, so better avoid that. No "
    "emergency loop has been seen with these files, and in normal DJ use they have played flawlessly.",
    "ALAC, 32-bit, 176.4 kHz and 192 kHz files are not supported.",
    "The update check, other players, rekordbox and the error log still see version 1.15, for compatibility.",
])


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def unpacked_application(update: bytes) -> bytes:
    lengths, offset = split_container(update)
    image, _ = decode_srecords(update[offset : offset + lengths[0]][32:-2])
    packed_bytes = int.from_bytes(image[APPLICATION_OFFSET : APPLICATION_OFFSET + 4], "little")
    return decompress_lzss(image[APPLICATION_OFFSET + 4 : APPLICATION_OFFSET + 4 + packed_bytes])


def changes(stock: bytes, mod: bytes) -> list[list]:
    """Runs of bytes that differ inside the stock application's length: [offset, our bytes as hex]."""
    runs = []
    index = 0
    while index < len(stock):
        if stock[index] == mod[index]:
            index += 1
            continue
        end = index
        while end < len(stock) and stock[end] != mod[end]:
            end += 1
        runs.append([index, mod[index:end].hex()])
        index = end
    return runs


def patch_data(firmware: Path, header: Path, tool_prefix: Path, build_name: str, date: str,
               release: bool = RELEASE_IS_DEFAULT, known_issues: list[str] | None = None,
               c6x_prefix: Path | None = None) -> dict:
    stock = firmware.read_bytes()
    with tempfile.TemporaryDirectory(prefix="xdj700-patcher-") as folder:
        built = Path(folder) / "mod.UPD"
        build(firmware, header, tool_prefix, built, version=MOD_TAG, c6x_prefix=c6x_prefix, **build_options(release))
        mod = built.read_bytes()
    _, stock_application_bytes, _ = stock_application(stock)
    mod_application = unpacked_application(mod)
    if len(mod_application) <= len(stock_application_bytes):
        raise ValueError("the built application is not longer than the stock one")
    runs = changes(stock_application_bytes, mod_application)
    if sum(len(run[1]) // 2 for run in runs) > MOST_CHANGED_BYTES:
        raise ValueError("more of the stock application changed than the mod's hooks account for")
    return {
        "build": build_name,
        "date": date,
        "known_issues": known_issues or [],
        "mod_tag": MOD_TAG,
        "restore_tag": RESTORE_TAG,
        "stock_sha256": sha256(stock),
        "stock_application_sha256": sha256(stock_application_bytes),
        "mod_application_sha256": sha256(mod_application),
        "mod_sha256": sha256(mod),
        "restore_sha256": sha256(build_restore(stock, RESTORE_TAG)),
        "changes": runs,
        "tail": base64.b64encode(zlib.compress(mod_application[len(stock_application_bytes) :], 9)).decode("ascii"),
    }


def render(data: dict) -> str:
    template = TEMPLATE.read_text()
    if template.count(PLACEHOLDER) != 1:
        raise ValueError("the template does not have exactly one placeholder")
    tail = data["tail"]
    lines = ["PATCH_DATA = {"]
    for key, value in data.items():
        if key == "tail":
            continue
        lines.append(f"    {json.dumps(key)}: {json.dumps(value)},")
    lines.append('    "tail": (')
    lines.extend(f'        "{tail[index:index + 100]}"' for index in range(0, len(tail), 100))
    lines.append("    ),")
    lines.append("}")
    return template.replace(PLACEHOLDER, "\n".join(lines) + "\n")


def template_notice() -> str:
    """The patcher's notice, read from the template (the page shows the same)."""
    spec = importlib.util.spec_from_file_location("patcher_template", TEMPLATE)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module.NOTICE


def zip_title(data: dict) -> str:
    """The first line of the zip's text: the project's name and the build ("XDJ-700 PLUS V0.90")."""
    shown = build_shown(data)
    return "XDJ-700 " + shown if shown.startswith("PLUS ") else "XDJ-700 PLUS, " + shown


def build_shown(data: dict) -> str:
    """The build's name, with the day it was approved on the player once there is one."""
    return data["build"] + (" (" + data["date"] + ")" if data.get("date") else "")


def page_notice(text: str) -> dict:
    """The notice as the page shows it: the warning line and the points."""
    lines = [line.strip() for line in text.splitlines() if line.strip() and not line.startswith("=")]
    start = next(n for n, line in enumerate(lines) if line.endswith("AT YOUR OWN RISK."))
    first = next(n for n, line in enumerate(lines) if line.startswith("- "))
    warning = " ".join(lines[start:first])
    points: list[str] = []
    for line in lines[first:]:
        if line.startswith("- "):
            points.append(line[2:])
        else:
            points[-1] += " " + line
    return {"warning": warning, "points": points}


def render_page(data: dict) -> str:
    """The web page: the template with the core and the data (and what the page needs besides)."""
    page = PAGE_TEMPLATE.read_text()
    for placeholder in PAGE_PLACEHOLDERS:
        if page.count(placeholder) != 1:
            raise ValueError(f"the page template does not have exactly one {placeholder}")
    notice = template_notice()
    issues = "".join(
        textwrap.fill(issue, width=77, initial_indent="  - ", subsequent_indent="    ") + "\n"
        for issue in data.get("known_issues", [])
    )
    how_to = HOW_TO.format(**data, title=zip_title(data)) + notice + ("\n  good to know:\n" + issues if issues else "") + "=" * 78 + "\n"
    page_data = dict(data, notice=page_notice(notice), how_to=how_to.replace("\n", "\r\n"))
    core = PAGE_CORE.read_text()
    text = json.dumps(page_data, indent=1)
    if "</" in core or not text.isascii() or not how_to.isascii():
        raise ValueError("the page's script or data cannot be put into the page as they are")
    text = text.replace("</", "<\\/")
    fonts = "\n".join(
        '@font-face { font-family: "IoskeleyMono"; font-weight: %d; font-style: normal; font-display: swap; '
        'src: url("data:font/woff2;base64,%s") format("woff2"); }'
        % (weight, base64.b64encode((ROOT / "tools" / "fonts" / name).read_bytes()).decode("ascii"))
        for name, weight in PAGE_FONTS
    )
    return page.replace(PAGE_PLACEHOLDERS[0], core).replace(PAGE_PLACEHOLDERS[1], text).replace(PAGE_PLACEHOLDERS[2], fonts)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--firmware", type=Path, default=ROOT / "firmware" / "original" / "XDJ700.UPD")
    parser.add_argument("--header", type=Path, default=ROOT / "src" / "dr_flac.h")
    parser.add_argument("--tool-prefix", type=Path, default=ROOT / "toolchains" / "installed" / "bin" / "sh-elf-")
    parser.add_argument("--c6x-prefix", type=Path, default=None, help="Prefix of the audio processor's compiler")
    choice = parser.add_mutually_exclusive_group()
    choice.add_argument("--release", action="store_true", help="Make the patcher for the release"
                        + (" (the default)" if RELEASE_IS_DEFAULT else ""))
    choice.add_argument("--everyday", action="store_true", help="Make the patcher for the build before PLUS V0.90"
                        + ("" if RELEASE_IS_DEFAULT else " (the default)"))
    parser.add_argument("--build-name", help="Name of the tested build this reproduces")
    parser.add_argument("--date", help="Date that build was approved on the player")
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--page-output", type=Path, default=PAGE_OUTPUT)
    parser.add_argument("--rerender", action="store_true",
                        help="Write both files again from the data of the patcher at --output, without building")
    args = parser.parse_args()

    if args.rerender:
        spec = importlib.util.spec_from_file_location("existing_patcher", args.output)
        existing = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(existing)
        args.output.write_text(render(existing.PATCH_DATA))
        args.page_output.write_text(render_page(existing.PATCH_DATA))
        print(json.dumps({"output_file": str(args.output), "page_file": str(args.page_output),
                          "build": existing.PATCH_DATA["build"], "mod_sha256": existing.PATCH_DATA["mod_sha256"]}, indent=2))
        return

    release = args.release or (RELEASE_IS_DEFAULT and not args.everyday)
    name, date, issues = RELEASE_BUILD if release else EVERYDAY_BUILD
    data = patch_data(args.firmware, args.header, args.tool_prefix, args.build_name or name, args.date or date,
                      release, issues, args.c6x_prefix)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render(data))
    args.page_output.parent.mkdir(parents=True, exist_ok=True)
    args.page_output.write_text(render_page(data))
    print(json.dumps({
        "output_file": str(args.output),
        "page_file": str(args.page_output),
        "bytes": args.output.stat().st_size,
        "mod_sha256": data["mod_sha256"],
        "restore_sha256": data["restore_sha256"],
        "changed_runs": len(data["changes"]),
        "changed_bytes": sum(len(run[1]) // 2 for run in data["changes"]),
        "tail_bytes": len(zlib.decompress(base64.b64decode(data["tail"]))),
    }, indent=2))


if __name__ == "__main__":
    main()
