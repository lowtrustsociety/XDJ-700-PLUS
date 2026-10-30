# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""The web page's patcher (tools/web_patcher_core.js) against the Python one.

Without the official firmware, the two are run on a pretend one: an update
file of the same layout, made here, with a small application of its own and
the mod's data made for it the way tools/make_patcher.py makes them. The
page's engine must make the same mod and restore file, byte for byte, and
every piece (the packer, the records, the checksums, the zip files) must
agree with the Python patcher and Python's own libraries. The checks of the
pieces run in Node; they are skipped where Node is not installed.
"""

import base64
import binascii
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
CHECK = ROOT / "tools" / "web_patcher_check.js"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def pretend_firmware(template, rng):
    """An update file of the official layout: two length lines, a MAIN component (32-byte header with
    the version tag, a boot region of 4,081 lines, the packed application, a terminator, CRC-16) and a
    PANEL component. Returns the file and its unpacked application."""
    words = [bytes(rng.getrandbits(8) for _ in range(rng.randint(2, 9))) for _ in range(300)]
    application = b"".join(rng.choice(words) for _ in range(30000)) + bytes(5000) + bytes(range(256)) * 20
    packed = template.lzss_encode(application)
    block = len(packed).to_bytes(4, "little") + packed
    block += (sum(block) & 0xFFFF).to_bytes(2, "little")
    block += b"\xff" * (template.APPLICATION_SPAN_BYTES - len(block))

    def record(kind, address, payload, size):
        body = address.to_bytes(size, "big") + payload
        raw = bytes([len(body) + 1]) + body
        return f"S{kind}".encode() + (raw + bytes([0xFF - (sum(raw) & 0xFF)])).hex().upper().encode() + b"\r\n"

    boot = bytes(rng.getrandbits(8) for _ in range(template.APPLICATION_OFFSET))
    lines = [record("0", 0, b"FAKE XDJ-700", 2)]
    address = 0
    for size in [64] * 4064 + [128] * 16:
        lines.append(record("2", address, boot[address : address + size], 3))
        address += size
    assert address == template.APPLICATION_OFFSET and len(lines) == template.BOOT_REGION_LINES
    for index in range(0, len(block), 32):
        lines.append(template.encode_record(template.APPLICATION_OFFSET + index, block[index : index + 32]))
    lines.append(record("8", 0, b"", 3))
    header = b"PRETEND XDJ-700 " + b"\0" * 4 + b"MAINVer1.15\x00"
    assert len(header) == 32
    body = header + b"".join(lines)
    main = body + binascii.crc_hqx(body, 0).to_bytes(2, "little")
    panel_body = b"PANEL" + bytes(rng.getrandbits(8) for _ in range(5000))
    panel = panel_body + binascii.crc_hqx(panel_body, 0).to_bytes(2, "little")
    update = str(len(main)).encode() + b"\r\n" + str(len(panel)).encode() + b"\r\n" + main + panel
    return update, application


def pretend_data(template, update, application, rng):
    """The mod's data for the pretend firmware, as tools/make_patcher.py makes it; the template's
    PATCH_DATA is set to it, and its two hashes are those of what the Python patcher makes."""
    changes = [[100, "deadbeef"], [4097, "00"], [len(application) - 3, "a5a5a5"]]
    tail = bytes(rng.getrandbits(8) for _ in range(20000)) + bytes(30000) + b"our code" * 500
    patched = bytearray(application)
    for position, replacement in changes:
        patched[position : position + len(replacement) // 2] = bytes.fromhex(replacement)
    patched += tail
    data = {
        "build": "pretend", "date": "2026-10-14", "known_issues": ["None, it is pretend."],
        "mod_tag": "1.16", "restore_tag": "1.17",
        "stock_sha256": sha256(update), "stock_application_sha256": sha256(application),
        "mod_application_sha256": sha256(bytes(patched)), "mod_sha256": "", "restore_sha256": "",
        "changes": changes, "tail": base64.b64encode(zlib.compress(tail, 9)).decode("ascii"),
    }
    template.PATCH_DATA = data
    data["mod_sha256"] = sha256(template.assemble_mod(update))
    data["restore_sha256"] = sha256(template.assemble_restore(update))
    return data


class PageFileTests(unittest.TestCase):
    """The generated page; no Node needed."""

    def setUp(self):
        self.page_path = ROOT / "patcher" / "xdj700_plus_patcher.html"
        self.page = self.page_path.read_text()

    def test_generated_page_is_the_template_core_and_data(self):
        maker = load("make_patcher", ROOT / "tools" / "make_patcher.py")
        patcher = load("xdj700_plus_patcher", ROOT / "patcher" / "xdj700_plus_patcher.py")
        self.assertEqual(maker.render_page(patcher.PATCH_DATA), self.page)
        found = json.loads(re.search(r"var PATCH_DATA = (\{.*?\n\});", self.page, re.S).group(1))
        for key, value in patcher.PATCH_DATA.items():
            self.assertEqual(found[key], value, key)
        notice = found["notice"]
        self.assertEqual(notice["warning"], "FLASHING FIRMWARE IS AT YOUR OWN RISK. It can leave a player unusable. "
                         "There is no warranty of any kind, and no one who made or shared it is responsible for any "
                         "damage or loss.")
        # The points are the README's "Read this first", word for word.
        readme_path = ROOT / "README.md"
        readme = readme_path.read_text()
        section = readme.split("## Read this first\n", 1)[1].split("\n## ", 1)[0]
        self.assertEqual(notice["points"], [line[2:] for line in section.strip().splitlines() if line.startswith("- ")])
        self.assertIn(patcher.PATCH_DATA["restore_sha256"], found["how_to"])
        self.assertIn(patcher.PATCH_DATA["mod_sha256"], found["how_to"])

    def test_the_font_is_inside_as_it_is_with_its_licence(self):
        import base64

        found = re.findall(r'font-weight: (\d+);[^}]*src: url\("data:font/woff2;base64,([A-Za-z0-9+/=]+)"\)', self.page)
        self.assertEqual([int(weight) for weight, _ in found], [400, 500, 700])
        for (weight, data), name in zip(found, ("Regular", "Medium", "Bold")):
            self.assertEqual(base64.b64decode(data), (ROOT / "tools" / "fonts" / f"IoskeleyMono-{name}.woff2").read_bytes())
        header = " ".join(self.page[: self.page.index("<html")].split())
        self.assertIn("Copyright 2015-2026, Renzhi Li", header)
        self.assertIn("SIL Open Font License, Version 1.1", header)
        licence = (ROOT / "tools" / "fonts" / "OFL.txt").read_text()
        self.assertTrue(licence.startswith("Copyright 2015-2026, Renzhi Li"))
        self.assertIn("SIL OPEN FONT LICENSE Version 1.1", licence)

    def test_the_page_loads_and_sends_nothing(self):
        policy = re.search(r'http-equiv="Content-Security-Policy" content="([^"]+)"', self.page).group(1)
        self.assertIn("default-src 'none'", policy)
        self.assertIn("connect-src 'none'", policy)
        self.assertIsNone(re.search(r"""(?:src|href)\s*=\s*["'](?:https?:)?//""", self.page))
        self.assertNotIn("fetch(", self.page)
        self.assertNotIn("XMLHttpRequest", self.page)
        self.assertNotIn("<link", self.page)

    def test_no_long_dashes(self):
        for path in (self.page_path, ROOT / "tools" / "web_patcher_template.html", ROOT / "tools" / "web_patcher_core.js"):
            text = path.read_text()
            self.assertNotIn("\u2013", text, path)
            self.assertNotIn("\u2014", text, path)


@unittest.skipIf(shutil.which("node") is None, "Node is needed to run the page's engine")
class WebPatcherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.template = load("patcher_template_for_web", ROOT / "tools" / "patcher_template.py")
        cls.folder = tempfile.TemporaryDirectory(prefix="xdj700-web-patcher-")
        cls.work = Path(cls.folder.name)
        rng = random.Random(700)
        cls.update, cls.application = pretend_firmware(cls.template, rng)
        cls.data = pretend_data(cls.template, cls.update, cls.application, rng)
        (cls.work / "stock.UPD").write_bytes(cls.update)
        (cls.work / "data.json").write_text(json.dumps(cls.data))

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def node(self, *args, expect_error=False):
        outcome = subprocess.run(["node", str(CHECK), *map(str, args)], capture_output=True, text=True)
        if expect_error:
            self.assertEqual(outcome.returncode, 3, outcome.stderr)
        else:
            self.assertEqual(outcome.returncode, 0, outcome.stdout + outcome.stderr)
        return json.loads(outcome.stdout)

    def file(self, name, data):
        path = self.work / name
        path.write_bytes(data)
        return path

    def test_the_page_makes_the_python_patchers_files(self):
        mod, restore = self.work / "mod.UPD", self.work / "restore.UPD"
        self.node("build", self.work / "data.json", self.work / "stock.UPD", mod, restore)
        self.template.PATCH_DATA = self.data
        self.assertEqual(mod.read_bytes(), self.template.build_mod(self.update))
        self.assertEqual(restore.read_bytes(), self.template.build_restore(self.update))
        # The same from the zip the firmware comes in.
        official = io.BytesIO()
        with zipfile.ZipFile(official, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("XDJ700_115/readme.txt", "hello")
            archive.writestr("XDJ700_115/XDJ700.UPD", self.update)
        mod_zip = self.work / "mod_zip.UPD"
        self.node("build", self.work / "data.json", self.file("official.zip", official.getvalue()), mod_zip, self.work / "r.UPD")
        self.assertEqual(mod_zip.read_bytes(), mod.read_bytes())

    def test_anything_but_the_official_file_is_refused(self):
        changed = bytearray(self.update)
        changed[len(changed) // 2] ^= 1
        for name, data in (("changed.UPD", bytes(changed)), ("short.UPD", self.update[:-1]), ("empty.UPD", b""),
                           ("text.UPD", b"not a firmware file")):
            result = self.node("build", self.work / "data.json", self.file(name, data), self.work / "x", self.work / "y",
                               expect_error=True)
            self.assertIn("not the official", result["error"])
            self.assertFalse((self.work / "x").exists())
        # A mod whose data does not give the tested build is not handed out.
        wrong = dict(self.data, mod_sha256="0" * 64)
        (self.work / "wrong.json").write_text(json.dumps(wrong))
        result = self.node("build", self.work / "wrong.json", self.work / "stock.UPD", self.work / "x", self.work / "y",
                           expect_error=True)
        self.assertIn("not the tested build", result["error"])

    def test_zip_files_in_and_out(self):
        for method in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
            official = io.BytesIO()
            with zipfile.ZipFile(official, "w", method) as archive:
                archive.writestr("a/XDJ700.upd", b"the file" * 1000)
            out = self.work / "read.bin"
            self.node("read-official", self.file("in.zip", official.getvalue()), out)
            self.assertEqual(out.read_bytes(), b"the file" * 1000)
        damaged = bytearray(official.getvalue())
        damaged[50] ^= 0xFF  # in the packed data of the one file
        self.node("read-official", self.file("damaged.zip", bytes(damaged)), out, expect_error=True)
        for names in ((), ("XDJ700.UPD", "copy/XDJ700.UPD")):
            archive_bytes = io.BytesIO()
            with zipfile.ZipFile(archive_bytes, "w") as archive:
                archive.writestr("other.txt", b"x")
                for name in names:
                    archive.writestr(name, b"y")
            result = self.node("read-official", self.file("none.zip", archive_bytes.getvalue()), out, expect_error=True)
            self.assertIn("no XDJ700.UPD", result["error"])
        # The zip the page hands out: Python reads it, every CRC-32 right, every file as it went in.
        one, two = bytes(range(256)) * 300, b"second\r\n" * 99
        made = self.work / "made.zip"
        self.node("zip-write", made, "1_mod/XDJ700.UPD", self.file("one", one), "HOW TO INSTALL.txt", self.file("two", two))
        with zipfile.ZipFile(made) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(archive.namelist(), ["1_mod/XDJ700.UPD", "HOW TO INSTALL.txt"])
            self.assertEqual(archive.read("1_mod/XDJ700.UPD"), one)
            self.assertEqual(archive.read("HOW TO INSTALL.txt"), two)
        damaged = bytearray(made.read_bytes())
        damaged[100] ^= 1
        with zipfile.ZipFile(io.BytesIO(bytes(damaged))) as archive:
            self.assertEqual(archive.testzip(), "1_mod/XDJ700.UPD")

    def test_the_pieces_agree_with_python(self):
        rng = random.Random(1)
        for length in (0, 1, 55, 56, 63, 64, 65, 119, 1000, 300000):
            data = bytes(rng.getrandbits(8) for _ in range(length))
            path = self.file("piece", data)
            self.assertEqual(self.node("sha256", path), {"subtle": sha256(data), "plain": sha256(data)}, length)
            self.assertEqual(self.node("crc", path), {"hqx": binascii.crc_hqx(data, 0), "crc32": zlib.crc32(data)})
        block = bytes(rng.getrandbits(8) for _ in range(32 * 50))
        out = self.work / "records"
        self.node("records", "0x40000", self.file("block", block), out)
        expected = b"".join(self.template.encode_record(0x40000 + i, block[i : i + 32]) for i in range(0, len(block), 32))
        self.assertEqual(out.read_bytes(), expected)
        self.node("decode-records", out, self.work / "decoded")
        self.assertEqual((self.work / "decoded").read_bytes(), self.template.decode_records(expected))

    def test_the_packer_makes_the_python_packers_choices(self):
        rng = random.Random(2)
        cases = {
            "application": self.application,
            "random": bytes(rng.getrandbits(8) for _ in range(20000)),
            "zeros and runs": bytes(9000) + b"ab" * 3000 + b"abc" * 3000 + bytes(range(256)) * 30,
            "short ends": b"abcabcab",
            "one byte": b"x",
            "nothing": b"",
        }
        for name, data in cases.items():
            packed = self.work / "packed"
            self.node("lzss-encode", self.file("plain", data), packed)
            self.assertEqual(packed.read_bytes(), self.template.lzss_encode(data), name)
            self.node("lzss-decode", packed, self.work / "unpacked")
            self.assertEqual((self.work / "unpacked").read_bytes(), data, name)

    def test_the_check_of_the_file_on_the_stick(self):
        mod, restore = self.work / "mod2.UPD", self.work / "restore2.UPD"
        self.node("build", self.work / "data.json", self.work / "stock.UPD", mod, restore)
        data = self.work / "data.json"
        self.assertEqual(self.node("identify", data, mod)["kind"], "mod")
        self.assertEqual(self.node("identify", data, restore)["kind"], "restore")
        damaged = bytearray(mod.read_bytes())
        damaged[123456] ^= 0x10
        self.assertIsNone(self.node("identify", data, self.file("damaged.UPD", bytes(damaged)))["kind"])
        # A file whose header says it is the mod (XDJ-700, 1.16) but is not the tested build.
        damaged = self.file("damaged116.UPD", b"40\r\n4\r\n" + b"XDJ-700".ljust(12) + b"MAINVer1.16\x00" + bytes(40) + b"PANL")
        self.assertEqual(self.node("identify", data, damaged)["kind"], "damaged")
        self.assertEqual(self.node("identify", data, self.work / "stock.UPD")["kind"], "official")
        self.assertIsNone(self.node("identify", data, self.file("text.UPD", b"not a firmware file"))["kind"])
        made = self.work / "handed_out.zip"
        self.node("zip-write", made, "1_mod/XDJ700.UPD", mod, "2_stock_restore/XDJ700.UPD", restore)
        self.assertEqual(self.node("identify", data, made)["kind"], "zip")
        restore_zip = self.work / "restore_only.zip"
        self.node("zip-write", restore_zip, "XDJ700.UPD", restore)
        pioneer_zip = self.work / "pioneer.zip"
        self.node("zip-write", pioneer_zip, "XDJ700.UPD", self.work / "stock.UPD")
        other_zip = self.work / "other.zip"
        self.node("zip-write", other_zip, "notes.txt", self.file("notes.txt", b"hello"))

        # The page and the Python patcher give the same answer for every kind of file.
        cases = [
            (mod, "XDJ700.UPD", "confirmed: this is the tested build", True),
            (mod, "xdj700_plus.UPD", "rename it to XDJ700.UPD", False),
            (restore, "XDJ700.UPD", "confirmed: this is the restore file", True),
            (self.work / "stock.UPD", "XDJ700.UPD", "the official firmware 1.15 as Pioneer ships it", False),
            (damaged, "XDJ700.UPD", "this says it is the mod (version 1.16)", False),
            (made, "xdj700_plus.zip", "this is the mod, still in its zip", False),
            (restore_zip, "restore.zip", "this is the restore file, still in its zip", False),
            (pioneer_zip, "XDJ700_115.zip", "Pioneer's zip with the official firmware 1.15", False),
            (other_zip, "other.zip", "this file is not right", False),
            (self.work / "text.UPD", "XDJ700.UPD", "this file is not right", False),
        ]
        for path, name, expected, safe in cases:
            page = self.node("stick-check", data, path, name)
            python = self.template.stick_check(path.read_bytes(), name)
            self.assertEqual((page["text"], page["ok"]), python, name)
            self.assertIn(expected, python[0], name)
            self.assertEqual(python[1], safe, name)

    def test_a_refused_file_is_named(self):
        # Given in place of the official file, a file is named when it can be: the mod or the restore file
        # this patcher makes, another player's file, a modified or older XDJ-700 file. Python and the page agree.
        mod, restore = self.work / "mod3.UPD", self.work / "restore3.UPD"
        self.node("build", self.work / "data.json", self.work / "stock.UPD", mod, restore)

        def headed(player, version):
            return b"40\r\n4\r\n" + player.ljust(12).encode() + b"MAINVer" + version.encode() + b"\x00" + bytes(40) + b"PANL"

        tampered = bytearray(headed("XDJ-700", "1.15"))
        cases = {
            "stock.UPD": (None, None),
            "mod3.UPD": ("already the mod (pretend). nothing to build", None),
            "restore3.UPD": ("the restore file this patcher makes", None),
            "other.UPD": ("another player (XDJ-1000MK2)", headed("XDJ-1000MK2", "1.45")),
            "modded.UPD": ("says it is the mod (version 1.16), but it is not byte for byte", headed("XDJ-700", "1.16")),
            "restored.UPD": ("says version 1.17: it has already been modified", headed("XDJ-700", "1.17")),
            "newer.UPD": ("says version 1.20: it has already been modified", headed("XDJ-700", "1.20")),
            "older.UPD": ("older XDJ-700 firmware (version 1.10)", headed("XDJ-700", "1.10")),
            "changed.UPD": ("says XDJ-700 version 1.15, but it is not the official file unchanged", bytes(tampered)),
            "text.UPD": ("this is not the official XDJ-700 firmware version 1.15 UPD file", b"not a firmware file"),
            "empty.UPD": ("this is not the official XDJ-700 firmware version 1.15 UPD file", b""),
        }
        for name, (expected, content) in cases.items():
            path = self.file(name, content) if content is not None else self.work / name
            python = self.template.refusal(path.read_bytes())
            page = self.node("refusal", self.work / "data.json", path)["refusal"]
            self.assertEqual(page, python, name)
            if expected is None:
                self.assertIsNone(python, name)
            else:
                self.assertIn(expected, python, name)
                self.assertNotIn("\u2014", python)
                self.assertNotIn("\u2013", python)


if __name__ == "__main__":
    unittest.main()
