# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
import unittest.mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class PatcherFileTests(unittest.TestCase):
    """The patcher's own logic and the generated file; no firmware needed."""

    def setUp(self):
        self.patcher_path = ROOT / "patcher" / "xdj700_plus_patcher.py"
        self.patcher = load("xdj700_plus_patcher", self.patcher_path)
        self.maker = load("make_patcher", ROOT / "tools" / "make_patcher.py")

    def test_generated_file_is_the_template_plus_data(self):
        self.assertEqual(self.maker.render(self.patcher.PATCH_DATA), self.patcher_path.read_text())
        # Our own bytes only: a few short runs inside the application, and the code after its end.
        changed = sum(len(run[1]) // 2 for run in self.patcher.PATCH_DATA["changes"])
        self.assertLessEqual(changed, self.maker.MOST_CHANGED_BYTES)
        # The build it makes is the default one, under its own name and with its own "good to know" list.
        builder = load("build_flac_firmware", ROOT / "tools" / "build_flac_firmware.py")
        name, date, issues = self.maker.RELEASE_BUILD if builder.RELEASE_IS_DEFAULT else self.maker.EVERYDAY_BUILD
        self.assertEqual((self.patcher.PATCH_DATA["build"], self.patcher.PATCH_DATA["known_issues"]), (name, issues))

    def test_the_notice_lists_the_builds_known_issues(self):
        notice = self.patcher.notice()
        self.assertIn("FLASHING FIRMWARE IS AT YOUR OWN RISK", notice)
        for issue in self.patcher.PATCH_DATA["known_issues"]:
            self.assertIn(" ".join(issue.split()[:6]), " ".join(notice.split()))
        self.assertTrue(all(len(line) <= 78 for line in notice.splitlines()))
        self.assertTrue(notice.endswith("=" * 78 + "\n"))
        # The release is named by its own label.
        builder = load("build_flac_firmware", ROOT / "tools" / "build_flac_firmware.py")
        self.assertEqual(self.maker.RELEASE_BUILD[0], builder.RELEASE_VERSION_LABEL)

    def test_a_refusal_of_ours_asks_for_a_report(self):
        # Once the input is the official file, every later refusal is our fault: the line says where to report it.
        ours = self.patcher.stopped(self.patcher.PatchError("internal error: the result is not the tested build"))
        self.assertIn("internal error: the result is not the tested build. nothing was written.", ours)
        self.assertIn(self.patcher.REPORT, ours)
        self.assertIn(self.patcher.REPORT, self.patcher.stopped(self.patcher.PatchError("the version tag was not found")))
        theirs = self.patcher.stopped(self.patcher.PatchError("this is not an XDJ-700 update file"))
        self.assertEqual(theirs, "\nstopped: this is not an XDJ-700 update file. nothing was written.")
        self.assertNotIn("report", self.patcher.stopped(OSError("disk full"), "the mod was written to m, the restore file was not: run it again."))

    def test_the_image_keeps_the_stock_length_or_grows_as_the_build_lets_it(self):
        span = self.patcher.APPLICATION_SPAN_BYTES
        self.assertEqual(self.patcher.application_span(1000), span)
        self.assertEqual(self.patcher.application_span(span), span)
        # Longer than stock: to a whole record, as the build does for the release (its packed bytes and six more).
        for packed in (span, span + 1, span + 26, span + 100000):
            self.assertEqual(self.patcher.application_span(packed + 6), -(-(packed + 6) // 32) * 32)
        # A record up to the updater's limit is read, one past it is refused.
        last = self.patcher.encode_record(self.patcher.FLASH_LIMIT - 32, bytes(range(32)))
        self.assertEqual(self.patcher.decode_records(last)[-32:], bytes(range(32)))
        with self.assertRaises(self.patcher.PatchError):
            self.patcher.decode_records(self.patcher.encode_record(self.patcher.FLASH_LIMIT - 16, bytes(32)))

    def test_no_long_dashes(self):
        for path in (self.patcher_path, ROOT / "tools" / "patcher_template.py", ROOT / "tools" / "make_patcher.py"):
            text = path.read_text()
            self.assertNotIn("\u2013", text, path)
            self.assertNotIn("\u2014", text, path)


class PatcherTests(unittest.TestCase):
    def setUp(self):
        self.firmware = ROOT / "firmware" / "original" / "XDJ700.UPD"
        self.patcher_path = ROOT / "patcher" / "xdj700_plus_patcher.py"
        if not self.firmware.is_file():
            self.skipTest("official XDJ-700 firmware is not available locally")
        self.patcher = load("xdj700_plus_patcher", self.patcher_path)
        self.stock = self.firmware.read_bytes()

    def test_packer_makes_the_c_encoders_choices(self):
        from check_lzss_roundtrip import build_encoder, stock_application

        _, application, _ = stock_application(self.stock)
        with tempfile.TemporaryDirectory(prefix="xdj700-patcher-test-") as folder:
            compress = build_encoder(Path(folder))
            # Code, tables and text from different parts of the image, and the short ends of the input.
            for start, size in ((0, 200000), (0x400000, 150000), (len(application) - 120000, 120000), (5, 2), (7, 0)):
                piece = application[start : start + size]
                packed = self.patcher.lzss_encode(piece)
                self.assertEqual(packed, compress(piece), (start, size))
                self.assertEqual(self.patcher.lzss_decode(packed), piece)

    def test_patcher_gives_the_build_the_builder_gives(self):
        builder = load("build_flac_firmware", ROOT / "tools" / "build_flac_firmware.py")
        tool_prefix = ROOT / "toolchains" / "installed" / "bin" / "sh-elf-"
        if not tool_prefix.with_name(tool_prefix.name + "gcc").is_file():
            self.skipTest("SH-4A cross compiler is not available locally")
        if builder.RELEASE_IS_DEFAULT and not self.c6x_compiler():
            self.skipTest("the audio processor's compiler is not available locally")
        report = builder.build(
            self.firmware, ROOT / "src" / "dr_flac.h", tool_prefix, output_path=None, **builder.build_options()
        )
        data = self.patcher.PATCH_DATA
        # The patcher in the repository belongs to the current source...
        self.assertEqual(data["mod_sha256"], report["container"]["built_sha256"])
        # ...and makes that build, and the restore file, from the official file alone.
        self.assertEqual(self.patcher.sha256(self.patcher.build_mod(self.stock)), data["mod_sha256"])
        restore = load("build_stock_restore", ROOT / "tools" / "build_stock_restore.py")
        self.assertEqual(self.patcher.build_restore(self.stock), restore.build_restore(self.stock, "1.17"))

    @staticmethod
    def c6x_compiler():
        return (ROOT / "toolchains" / "installed" / "c6x" / "bin" / "tic6x-elf-gcc").is_file()

    def test_a_release_patcher_makes_the_release(self):
        # Whatever the default is: a patcher for the release reproduces the release build byte for byte,
        # with the audio processor's block (our code only) in its tail.
        tool_prefix = ROOT / "toolchains" / "installed" / "bin" / "sh-elf-"
        if not tool_prefix.with_name(tool_prefix.name + "gcc").is_file() or not self.c6x_compiler():
            self.skipTest("the cross compilers are not available locally")
        maker = load("make_patcher", ROOT / "tools" / "make_patcher.py")
        name, date, issues = maker.RELEASE_BUILD
        data = maker.patch_data(self.firmware, ROOT / "src" / "dr_flac.h", tool_prefix, name, date,
                                release=True, known_issues=issues)
        with tempfile.TemporaryDirectory(prefix="xdj700-patcher-test-") as folder:
            path = Path(folder) / "release_patcher.py"
            path.write_text(maker.render(data))
            release_patcher = load("release_patcher", path)
        mod = release_patcher.build_mod(self.stock)
        self.assertEqual(release_patcher.sha256(mod), data["mod_sha256"])
        self.assertLessEqual(sum(len(run[1]) // 2 for run in data["changes"]), maker.MOST_CHANGED_BYTES)
        # Longer than stock: the block for the audio processor is appended.
        self.assertGreater(len(mod), len(self.stock))

    def test_anything_but_the_official_file_is_refused(self):
        tampered = bytearray(self.stock)
        tampered[5000000] ^= 1
        for data in (bytes(tampered), self.stock[:-1], b"", b"not a firmware file"):
            with self.assertRaises(self.patcher.PatchError):
                self.patcher.build_mod(data)
            with self.assertRaises(self.patcher.PatchError):
                self.patcher.build_restore(data)

    def test_command_line_writes_nothing_without_consent_or_the_right_file(self):
        with tempfile.TemporaryDirectory(prefix="xdj700-patcher-test-") as folder:
            out = Path(folder) / "out"
            wrong = Path(folder) / "wrong.UPD"
            wrong.write_bytes(self.stock[:1000])
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed):
                self.assertEqual(self.patcher.main([str(wrong), "--output-dir", str(out), "--yes"]), 1)
                self.assertEqual(self.patcher.main(["--output-dir", str(out)]), 2)
            self.assertFalse(out.exists())
            # As on the page, the notice comes once the right file is in, before anything is built:
            # without a terminal to ask in and without --yes, nothing is written.
            self.assertNotIn("FLASHING FIRMWARE IS AT YOUR OWN RISK", printed.getvalue())
            right = Path(folder) / "XDJ700.UPD"
            right.write_bytes(self.stock)
            printed = io.StringIO()
            with contextlib.redirect_stdout(printed), unittest.mock.patch("sys.stdin", io.StringIO("")):
                self.assertEqual(self.patcher.main([str(right), "--output-dir", str(out)]), 1)
            self.assertFalse(out.exists())
            self.assertIn("FLASHING FIRMWARE IS AT YOUR OWN RISK", printed.getvalue())
            self.assertIn("official XDJ-700 firmware version 1.15 UPD file detected.", printed.getvalue())


if __name__ == "__main__":
    unittest.main()
