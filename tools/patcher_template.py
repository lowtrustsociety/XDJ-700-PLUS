#!/usr/bin/env python3
"""XDJ-700 PLUS patcher.

Adds FLAC, hi-res WAV and AIFF and Bandcamp WAV playback to a Pioneer DJ
XDJ-700 by patching YOUR OWN copy of the official firmware 1.15.

    python3 xdj700_plus_patcher.py XDJ700.UPD

The argument is your own official XDJ-700 firmware version 1.15 UPD file.
Two files are written, each named XDJ700.UPD in a folder of its own: the
mod, and the restore file, which puts the official firmware 1.15 back.

This file contains no Pioneer code. It holds only the mod's own code and the
places where it goes, and it refuses any file that is not the official
XDJ-700 firmware version 1.15 UPD file. It needs Python 3.8 or later and
nothing else.

UNOFFICIAL AND EXPERIMENTAL. FLASHING FIRMWARE IS AT YOUR OWN RISK.
It shows the full notice before it builds; the README has it too.

Copyright (C) 2026 low trust society

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, version 3.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
details: <https://www.gnu.org/licenses/gpl-3.0.html>. The source of the mod's
code that this file carries in compiled form is in the project's repository,
under the same licence. It includes dr_flac by David Reid (public domain or
MIT No Attribution).
"""

# SPDX-License-Identifier: GPL-3.0-only

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import io
import os
import re
import sys
import textwrap
import zipfile
import zlib

# The mod's own code and where it goes. Filled in by tools/make_patcher.py.
PATCH_DATA = None

NOTICE = """\
==============================================================================
  XDJ-700 PLUS, UNOFFICIAL AND EXPERIMENTAL

  FLASHING FIRMWARE IS AT YOUR OWN RISK.
  It can leave a player unusable. There is no warranty of any kind, and no
  one who made or shared it is responsible for any damage or loss.

  - This is not a Pioneer DJ or AlphaTheta product. They have nothing to do
    with it, do not support it, and may refuse service on a modified player.
  - It is free software under the GNU General Public License version 3,
    provided as is, with no warranty of any kind.
  - You use it entirely at your own risk. In no case is anyone who made or
    shared it responsible for any damage or loss, to your player, your
    media, your gig or anything else.
  - It has been tested at length on the author's XDJ-700s, in sessions of up
    to 18 hours, and has been very stable. That is not proof that it works
    the same on yours. Treat it as experimental, and do not rely on it for a
    show before you have tried it yourself at home.
  - A failed or interrupted update can leave a player unusable, as with any
    firmware update. Never switch the player off during an update.
  - The boot and recovery region and the front panel firmware stay as
    Pioneer made them, so the player's own update can always put the
    official firmware back.
  - Keep the restore file that the patcher writes, in case you want to go
    back to stock.
  - Only for the XDJ-700, and only from your own official XDJ-700 firmware
    version 1.15 UPD file.
"""
NOTICE_END = "=" * 78 + "\n"

APPLICATION_OFFSET = 0x40000           # the packed application inside the MAIN image
MAIN_IMAGE_BYTES = 7251904
APPLICATION_SPAN_BYTES = MAIN_IMAGE_BYTES - APPLICATION_OFFSET
# The updater erases and writes the flash up to here: an image may grow up to it, never into it.
FLASH_LIMIT = 0x7E0000
BOOT_REGION_LINES = 4081               # S-records of the boot region, kept as they are
RECORD_BYTES = 32
WINDOW = 4096
MAX_MATCH = 18
STOCK_TAG = b"MAINVer1.15\x00"
ADDRESS_BYTES = {"0": 2, "1": 2, "2": 3, "3": 4, "5": 2, "6": 3, "7": 4, "8": 3, "9": 2}


class PatchError(Exception):
    """Something is not as it must be; nothing is written."""


# Errors that can only come from our side: each is raised only once the input is the official file.
OURS = ("the packed application is damaged", "the packed application's checksum is wrong",
        "the unpacked application is not the expected one", "the version tag was not found", "internal error:")
REPORT = "this should not happen with the official file: please report it at https://github.com/lowtrustsociety/XDJ-700-PLUS/issues"


def stopped(error, written="nothing was written."):
    """The red line for a refusal, with the report line when it is our fault."""
    text = "\nstopped: %s. %s" % (str(error).rstrip("."), written)
    if str(error).startswith(OURS):
        text += "\n" + REPORT
    return text


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def notice():
    """The notice, with what is good to know about the build this patcher makes."""
    text = NOTICE
    if PATCH_DATA is not None and PATCH_DATA.get("known_issues"):
        text += "\n  good to know:\n"
        for issue in PATCH_DATA["known_issues"]:
            text += textwrap.fill(issue, width=77, initial_indent="  - ", subsequent_indent="    ") + "\n"
    return text + NOTICE_END


def application_span(block_bytes):
    """Bytes of flash the packed application takes: the stock length, or past it to a whole
    record when the mod's code needs more (as the build does)."""
    return max(APPLICATION_SPAN_BYTES, -(-block_bytes // RECORD_BYTES) * RECORD_BYTES)


def split_container(data):
    """The update file: two length lines, then the MAIN and the PANEL component."""
    try:
        first, second, remainder = data.split(b"\r\n", 2)
        lengths = [int(first), int(second)]
    except ValueError:
        raise PatchError("this is not an XDJ-700 update file")
    offset = len(data) - len(remainder)
    if offset + sum(lengths) != len(data):
        raise PatchError("the update file is damaged (lengths do not add up)")
    return lengths, offset


def decode_records(text):
    """The flash image a block of Motorola S-records describes."""
    image = bytearray(b"\xff" * FLASH_LIMIT)
    end = 0
    for line in text.splitlines():
        kind = chr(line[1])
        raw = bytes.fromhex(line[2:].decode("ascii"))
        if line[:1] != b"S" or kind not in ADDRESS_BYTES or raw[0] != len(raw) - 1 or sum(raw) & 0xFF != 0xFF:
            raise PatchError("the update file is damaged (bad S-record)")
        if kind in "123":
            size = ADDRESS_BYTES[kind]
            address = int.from_bytes(raw[1:1 + size], "big")
            payload = raw[1 + size:-1]
            if address + len(payload) > FLASH_LIMIT:
                raise PatchError("the update file is damaged (data outside the image)")
            image[address:address + len(payload)] = payload
            end = max(end, address + len(payload))
    return bytes(image[:end])


def encode_record(address, payload):
    body = address.to_bytes(3, "big") + payload
    raw = bytes([len(body) + 1]) + body
    return b"S2" + (raw + bytes([0xFF - (sum(raw) & 0xFF)])).hex().upper().encode("ascii") + b"\r\n"


def lzss_decode(source):
    """Unpack the firmware's LZSS stream (4,096-byte window, matches of 3 to 18 bytes)."""
    ring = bytearray(b" " * (WINDOW - MAX_MATCH) + b"\0" * MAX_MATCH)
    cursor = WINDOW - MAX_MATCH
    output = bytearray()
    position = 0
    size = len(source)
    while position < size:
        flags = source[position]
        position += 1
        for bit in range(8):
            if position >= size:
                break
            if flags & (1 << bit):
                value = source[position]
                position += 1
                output.append(value)
                ring[cursor] = value
                cursor = (cursor + 1) & (WINDOW - 1)
            else:
                if position + 2 > size:
                    raise PatchError("the packed application is damaged")
                low = source[position]
                high = source[position + 1]
                position += 2
                start = low | ((high & 0xF0) << 4)
                for index in range((high & 0x0F) + 3):
                    value = ring[(start + index) & (WINDOW - 1)]
                    output.append(value)
                    ring[cursor] = value
                    cursor = (cursor + 1) & (WINDOW - 1)
    return bytes(output)


def lzss_encode(data, progress=None):
    """Pack data as the firmware expects it.

    Makes the same choices as the encoder the tested build was made with, so
    the result is the same bytes: the longest match (3 to 18 bytes) starting
    in the last 4,096 positions, the most recent one on a tie, and a single
    byte instead when the next position has a longer match.
    """
    size = len(data)
    rfind = data.rfind
    output = bytearray()
    append = output.append
    position = 0
    next_report = 0

    def find(at, newest):
        limit = min(size - at, MAX_MATCH)
        if limit < 3:
            return 0, 0
        oldest = max(at - WINDOW, 0)
        where = rfind(data[at:at + 3], oldest, newest + 3)
        if where < 0:
            return 0, 0
        best = 3
        low, high = 4, limit
        while low <= high:
            middle = (low + high) >> 1
            found = rfind(data[at:at + middle], oldest, newest + middle)
            if found < 0:
                high = middle - 1
            else:
                best, where = middle, found
                low = middle + 1
        return best, where

    while position < size:
        if progress is not None and position >= next_report:
            progress(position, size)
            next_report = position + size // 50 + 1
        flags_at = len(output)
        append(0)
        flags = 0
        for bit in range(8):
            if position >= size:
                break
            best, where = find(position, position - 1)
            if 3 <= best < MAX_MATCH and size - position > 1 and find(position + 1, position - 1)[0] > best:
                best = 0
            if best >= 3:
                ring = (WINDOW - MAX_MATCH + where) & (WINDOW - 1)
                append(ring & 0xFF)
                append(((ring >> 4) & 0xF0) | (best - 3))
                position += best
            else:
                flags |= 1 << bit
                append(data[position])
                position += 1
        output[flags_at] = flags
    if progress is not None:
        progress(size, size)
    return bytes(output)


def read_official_file(path):
    """The update file's bytes, from the file itself or from the zip Pioneer ships it in."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:2] == b"PK":
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                names = [name for name in archive.namelist() if name.upper().endswith("XDJ700.UPD")]
                if len(names) != 1:
                    raise PatchError("no XDJ700.UPD in this zip file")
                data = archive.read(names[0])
        except zipfile.BadZipFile:
            raise PatchError("this zip file cannot be read")
    return data


OFFICIAL = "the official XDJ-700 firmware version 1.15 UPD file"
HEADER = re.compile(rb"\r\n([ -~]{1,16}?) *MAINVer([0-9]{1,2})\.([0-9]{1,3})\x00")


def refusal(data):
    """Why a file given as the official one is refused, saying what it is when that can be told; None if it is the official file."""
    digest = sha256(data)
    if digest == PATCH_DATA["stock_sha256"]:
        return None
    if digest == PATCH_DATA["mod_sha256"]:
        return ("this is already the mod (" + PATCH_DATA["build"] + "). nothing to build: copy it as XDJ700.UPD to the "
                "top folder of a FAT32 USB stick. to build it again, with the restore file, provide " + OFFICIAL)
    if digest == PATCH_DATA["restore_sha256"]:
        return ("this is the restore file this patcher makes: it puts the official firmware 1.15 back and cannot be "
                "patched. please provide " + OFFICIAL + ", unchanged")
    header = HEADER.search(data[:160])
    if header:
        player = header.group(1).decode("ascii").strip()
        major, minor = int(header.group(2)), int(header.group(3))
        version = "%d.%s" % (major, header.group(3).decode("ascii"))
        if player != "XDJ-700":
            return "this is the update file of another player (" + player + "). please provide " + OFFICIAL
        if version == PATCH_DATA["mod_tag"]:
            return ("this says it is the mod (version " + version + "), but it is not byte for byte the tested build ("
                    + PATCH_DATA["build"] + "): it is damaged, changed or another build. DO NOT flash it. to build it "
                    "again, provide " + OFFICIAL)
        if (major, minor) > (1, 15):
            return ("this XDJ-700 file says version " + version + ": it has already been modified (a mod or a restore "
                    "file), so it cannot be patched. please provide " + OFFICIAL + ", unchanged")
        if (major, minor) < (1, 15):
            return "this is an older XDJ-700 firmware (version " + version + "). please provide " + OFFICIAL
        return ("this file says XDJ-700 version 1.15, but it is not the official file unchanged: it has been modified "
                "or damaged. please provide " + OFFICIAL + ", unchanged")
    return "this is not " + OFFICIAL + ". please provide that, unchanged. any other file is refused"


def check_official(stock):
    reason = refusal(stock)
    if reason:
        raise PatchError(reason)


def retag(component, tag):
    """The MAIN component with another version tag in its header, and its checksum redone."""
    header = component[:32].replace(STOCK_TAG, b"MAINVer" + tag.encode("ascii") + b"\x00")
    if len(header) != 32 or header == component[:32]:
        raise PatchError("the version tag was not found")
    return header


def assemble_restore(stock):
    """Stock 1.15 under a higher version tag, so the player accepts it over the mod."""
    check_official(stock)
    lengths, offset = split_container(stock)
    main = stock[offset:offset + lengths[0]]
    body = retag(main, PATCH_DATA["restore_tag"]) + main[32:-2]
    return stock[:offset] + body + binascii.crc_hqx(body, 0).to_bytes(2, "little") + stock[offset + lengths[0]:]


def build_restore(stock):
    """The restore file, checked against the one the build made."""
    restore = assemble_restore(stock)
    if sha256(restore) != PATCH_DATA["restore_sha256"]:
        raise PatchError("internal error: the restore file is not the expected one")
    return restore


def assemble_mod(stock, progress=None):
    """The patched update file, made from the official one (build_mod checks it is the tested build)."""
    check_official(stock)
    lengths, offset = split_container(stock)
    main = stock[offset:offset + lengths[0]]
    panel = stock[offset + lengths[0]:]
    lines = main[32:-2].splitlines(keepends=True)
    image = decode_records(main[32:-2])

    packed_bytes = int.from_bytes(image[APPLICATION_OFFSET:APPLICATION_OFFSET + 4], "little")
    packed_end = APPLICATION_OFFSET + 4 + packed_bytes
    if int.from_bytes(image[packed_end:packed_end + 2], "little") != sum(image[APPLICATION_OFFSET:packed_end]) & 0xFFFF:
        raise PatchError("the packed application's checksum is wrong")
    application = lzss_decode(image[APPLICATION_OFFSET + 4:packed_end])
    if sha256(application) != PATCH_DATA["stock_application_sha256"]:
        raise PatchError("the unpacked application is not the expected one")

    # Our bytes at the places the mod hooks in, and our code after the application's end.
    patched = bytearray(application)
    for position, replacement in PATCH_DATA["changes"]:
        new = bytes.fromhex(replacement)
        patched[position:position + len(new)] = new
    patched += zlib.decompress(base64.b64decode(PATCH_DATA["tail"]))
    patched = bytes(patched)
    if sha256(patched) != PATCH_DATA["mod_application_sha256"]:
        raise PatchError("internal error: the patched application is not the expected one")

    packed = lzss_encode(patched, progress)
    if lzss_decode(packed) != patched:
        raise PatchError("internal error: the packed application does not unpack to what went in")
    block = len(packed).to_bytes(4, "little") + packed
    block += (sum(block) & 0xFFFF).to_bytes(2, "little")
    span = application_span(len(block))
    if APPLICATION_OFFSET + span > FLASH_LIMIT:
        raise PatchError("internal error: the packed application does not fit")
    block += b"\xff" * (span - len(block))

    records = [
        encode_record(APPLICATION_OFFSET + index, block[index:index + RECORD_BYTES])
        for index in range(0, span, RECORD_BYTES)
    ]
    body = retag(main, PATCH_DATA["mod_tag"]) + b"".join(lines[:BOOT_REGION_LINES]) + b"".join(records) + lines[-1]
    new_main = body + binascii.crc_hqx(body, 0).to_bytes(2, "little")
    mod = str(len(new_main)).encode("ascii") + b"\r\n" + str(len(panel)).encode("ascii") + b"\r\n" + new_main + panel

    # The two rules every build keeps: the boot region and the panel part are Pioneer's, untouched.
    if decode_records(new_main[32:-2])[:APPLICATION_OFFSET] != image[:APPLICATION_OFFSET]:
        raise PatchError("internal error: the boot region changed")
    if mod[len(mod) - len(panel):] != panel:
        raise PatchError("internal error: the panel part changed")
    return mod


def build_mod(stock, progress=None):
    """The patched update file, byte for byte the build that was tested on the player."""
    mod = assemble_mod(stock, progress)
    if sha256(mod) != PATCH_DATA["mod_sha256"]:
        raise PatchError("internal error: the result is not the tested build")
    return mod


def show_progress(done, total):
    width = 40
    filled = width * done // total if total else width
    sys.stdout.write("\r  packing [" + "#" * filled + "." * (width - filled) + "] %3d%%" % (100 * done // total))
    if done >= total:
        sys.stdout.write("\n")
    sys.stdout.flush()


def write_file(folder, data):
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "XDJ700.UPD")
    with open(path, "wb") as handle:
        handle.write(data)
    with open(path, "rb") as handle:
        if handle.read() != data:
            raise PatchError("the file written to " + path + " does not read back the same")
    return path


# Green when good, red when bad, as on the page; plain where the output is not a terminal.
COLOURS = {"ok": "\033[32m", "bad": "\033[31m"}


def say(text, kind=None):
    if kind and sys.stdout.isatty():
        if os.name == "nt":
            os.system("")  # lets the Windows console show colours
        text = COLOURS[kind] + text + "\033[0m"
    print(text)


def identify(data):
    """What a file is: "mod", "restore", "official" (Pioneer's 1.15), "damaged" (says it is the mod but is not),
    "zip" or None."""
    if data[:2] == b"PK":
        return "zip"
    digest = sha256(data)
    if digest == PATCH_DATA["mod_sha256"]:
        return "mod"
    if digest == PATCH_DATA["restore_sha256"]:
        return "restore"
    if digest == PATCH_DATA["stock_sha256"]:
        return "official"
    header = HEADER.search(data[:160])
    if header and header.group(1).strip() == b"XDJ-700" and \
            (header.group(2) + b"." + header.group(3)).decode("ascii") == PATCH_DATA["mod_tag"]:
        return "damaged"
    return None


def zip_inside(data):
    """What the XDJ700.UPD files in a zip are, as a set of identify() answers."""
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            return {identify(archive.read(name)) for name in archive.namelist() if name.upper().endswith("XDJ700.UPD")}
    except (zipfile.BadZipFile, OSError, RuntimeError, ValueError):
        return set()


AGAIN = ("copy XDJ700.UPD from the zip or folder made here to the top folder of your USB stick again, eject the stick, plug it "
         "back in and try again.")


def stick_check(data, name):
    """The check of the file on the stick: (the answer, True if it is safe to flash). The page gives the same answers."""
    kind = identify(data)
    wrong_name = "" if name.upper() == "XDJ700.UPD" else (
        " but it is called " + name + " on the stick: rename it to XDJ700.UPD, or the player will not see it.")
    if kind == "mod":
        return "confirmed: this is the tested build (" + PATCH_DATA["build"] + "). safe to flash." + wrong_name, not wrong_name
    if kind == "restore":
        return ("confirmed: this is the restore file (the official firmware 1.15). safe to flash to go back."
                + wrong_name, not wrong_name)
    if kind == "official":
        return ("this is the official firmware 1.15 as Pioneer ships it, not a file made here. to install the mod, "
                "copy 1_mod/XDJ700.UPD from the zip or folder made here; to go back to stock, copy 2_stock_restore/XDJ700.UPD.", False)
    if kind == "damaged":
        return ("this says it is the mod (version " + PATCH_DATA["mod_tag"] + "), but it is not byte for byte the "
                "tested build (" + PATCH_DATA["build"] + "): it is damaged, changed or another build. DO NOT flash it. "
                + AGAIN, False)
    if kind == "zip":
        inside = zip_inside(data)
        if "mod" in inside:
            return ("this is the mod, still in its zip. the player cannot flash from a zip: open it, copy the mod's "
                    "XDJ700.UPD to the top folder of your USB stick, then drop that file here.", False)
        if "restore" in inside:
            return ("this is the restore file, still in its zip. the player cannot flash from a zip: open it, copy the "
                    "XDJ700.UPD inside to the top folder of your USB stick, then drop that file here.", False)
        if "official" in inside:
            return ("this is Pioneer's zip with the official firmware 1.15, not a file made here. to install the mod, "
                    "copy 1_mod/XDJ700.UPD from the zip or folder made here.", False)
    return "this file is not right. DO NOT flash it. " + AGAIN, False


def check_file(path):
    """The check of the file on the stick, with the page's answers. True if it is safe to flash."""
    path = path.strip()
    if len(path) > 1 and path[0] == path[-1] and path[0] in "'\"":
        path = path[1:-1]
    elif os.name != "nt":
        path = path.replace("\\ ", " ")  # a file dropped into a terminal comes with its spaces escaped
    name = os.path.basename(path)
    print("checking " + name + "...")
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError:
        say("the file could not be read", "bad")
        return False
    answer, ok = stick_check(data, name)
    say(answer, "ok" if ok else "bad")
    return ok


STEPS = """
what to do now:

  1. the two files:
       the mod:            %s
       the restore file:   %s
  2. copy the mod to the stick. copy the mod's XDJ700.UPD to the top folder
     of a FAT32 USB stick with about 18 MB free. keep the 2_stock_restore
     folder on your computer: it holds the restore file, which puts the
     official firmware 1.15 back.
  3. eject the stick properly, then plug it back in. this matters for the
     next step: without it the computer may answer from its own memory
     instead of reading the stick.
  4. check the file on the stick: drop XDJ700.UPD from the stick into this
     window and press enter. flash only after it is confirmed.
     to check again later:
       python3 %s --check <the XDJ700.UPD on the stick>
  5. flash. with the player switched off, put the stick in. hold IN and
     RELOOP/EXIT, switch the player on, and keep holding until the update
     screen appears. wait until the update has finished and the player has
     restarted. DO NOT SWITCH IT OFF before that.

  *  going back to stock: the same steps with the XDJ700.UPD from the
  2_stock_restore folder. the update screen shows %s for the mod and
  %s for the restore file: those numbers are only there so that the
  player accepts the file.
"""


def ask_checks():
    """Step 4 in the terminal: files dropped into the window are checked until one is right."""
    while True:
        try:
            answer = input("\ndrop the XDJ700.UPD from your USB stick here and press enter (enter alone to stop): ")
        except EOFError:
            return
        if not answer.strip():
            return
        if check_file(answer):
            return


def main(arguments=None):
    parser = argparse.ArgumentParser(
        description="patch your own official XDJ-700 firmware version 1.15 UPD file with the XDJ-700 PLUS mod.",
        epilog="unofficial and experimental. flashing firmware is at your own risk.",
    )
    parser.add_argument("official", nargs="?", help="your own official XDJ-700 firmware version 1.15 UPD file")
    parser.add_argument("--output-dir", default="xdj700_plus", help="where to write the two files (default: %(default)s)")
    parser.add_argument("--yes", action="store_true", help="i have read the notice and accept the risk; do not ask")
    parser.add_argument("--check", metavar="FILE", help="only check a file on the USB stick: is it safe to flash?")
    options = parser.parse_args(arguments)

    print("XDJ-700 PLUS patcher")
    if PATCH_DATA is None:
        print("this is the template, without the mod's code. use the generated xdj700_plus_patcher.py.")
        return 2
    print(PATCH_DATA["build"] + (" (" + PATCH_DATA["date"] + ")" if PATCH_DATA.get("date") else ""))
    if options.check:
        return 0 if check_file(options.check) else 1
    if options.official is None:
        parser.print_usage()
        print("\nyou have to provide your own official XDJ-700 firmware version 1.15 UPD file.")
        print("you will also need a FAT32 USB stick with about 18 MB free.")
        return 2
    print("you will also need a FAT32 USB stick with about 18 MB free.")

    try:
        print("\nreading " + os.path.basename(options.official) + "...")
        stock = read_official_file(options.official)
        if identify(stock) == "mod":
            say(refusal(stock) + ".", "ok")
            print("to check it on the stick: python3 " + (os.path.basename(sys.argv[0]) or "xdj700_plus_patcher.py")
                  + " --check <the XDJ700.UPD on the stick>")
            return 0
        check_official(stock)
        say("official XDJ-700 firmware version 1.15 UPD file detected.", "ok")
        print("\n" + notice())
        if not options.yes:
            if not sys.stdin.isatty():
                raise PatchError("not asked and not confirmed: run it in a terminal, or add --yes after reading the notice")
            print("i have read this and accept the risk.")
            answer = input("type yes to accept and build the mod: ")
            if answer.strip().lower() != "yes":
                print("nothing was written.")
                return 1
        print("\nbuilding the mod (about a minute)...")
        mod = build_mod(stock, show_progress)
        print("checking the result...")
        restore = build_restore(stock)
        mod_path = write_file(os.path.join(options.output_dir, "1_mod"), mod)
    except (PatchError, OSError) as error:
        say(stopped(error), "bad")
        return 1
    try:
        restore_path = write_file(os.path.join(options.output_dir, "2_stock_restore"), restore)
    except (PatchError, OSError) as error:
        say(stopped(error, "the mod was written to " + mod_path + ", the restore file was not: run it again."), "bad")
        return 1

    say("\ndone. both files were checked: the mod is byte for byte the tested build.", "ok")
    print(STEPS % (mod_path, restore_path, os.path.basename(sys.argv[0]) or "xdj700_plus_patcher.py",
                   PATCH_DATA["mod_tag"], PATCH_DATA["restore_tag"]))
    if sys.stdin.isatty() and not options.yes:
        ask_checks()
    return 0


if __name__ == "__main__":
    sys.exit(main())
