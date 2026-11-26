"""Synthetic NUP and NSP orchestration checks. No game data is used."""

from contextlib import redirect_stdout
import hashlib
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import package_import as importer
from package_nup import format as nup


def fixture(path, packages, *, compressed=False, bad_hash=False):
    """Write a NUP with one section and one file per package."""
    strings, nodes, children, files, records = bytearray(), [], [], [], []

    def name(value):
        offset = len(strings)
        encoded = value.encode()
        strings.extend(encoded)
        return offset, len(encoded)

    dir_count = sum(2 + ("/" in entry[3]) for entry in packages)
    for i, (title, kind, version, filename, content) in enumerate(packages):
        root, section = len(nodes), len(nodes) + 1
        child = len(children)
        flags = (nup.PKG_FLAG_STANDALONE_UPDATE if kind == nup.KIND_UPDATE and
                 not any(p[1] == nup.KIND_BASE for p in packages) else 0)
        records.append(nup.pack_pkg(nup.Package(title, root, nup.NO_DIR, section, version, kind, flags)))
        nodes.extend([nup._dir_s.pack(*name(""), child, 1, 0),
                      nup._dir_s.pack(*name("romfs"), child + 1, 1, 0)])
        children.append(section)
        if "/" in filename:
            directory, filename = filename.split("/", 1)
            nodes.append(nup._dir_s.pack(*name(directory), child + 2, 1, 0))
            children.append(section + 1)
        children.append(dir_count + i)
        files.append((name(filename), content))

    child_data = struct.pack(f"<{len(children)}I", *children)
    pool_offset = (nup._toc_head_s.size + len(records) * nup.PKG_SIZE + len(nodes) * nup.DIR_SIZE
                   + len(files) * nup.FILE_SIZE + len(child_data))
    toc_size = pool_offset + len(strings)
    blob_at = (nup.HEADER_SIZE + toc_size + 15) & ~15
    blobs, file_data = bytearray(), []
    for (offset, length), content in files:
        codec = nup.CODEC_RAW
        stored = content
        if compressed:
            import zstandard
            stored = zstandard.ZstdCompressor().compress(content)
            codec = nup.CODEC_ZSTD
        blobs.extend(b"\0" * (-len(blobs) % 16))
        digest = hashlib.sha256(b"bad" if bad_hash else content).digest()
        file_data.append(nup._file_s.pack(offset, length, len(content), blob_at + len(blobs),
                                          len(stored), digest, codec, 0, 0))
        blobs.extend(stored)
    toc = (nup._toc_head_s.pack(nup.TOC_MAGIC, len(records), len(nodes), len(files),
                               pool_offset, len(strings), 0)
           + b"".join(records + nodes + file_data) + child_data + bytes(strings))
    header = nup._super_s.pack(nup.MAGIC, nup.HEADER_SIZE, 1, nup.HEADER_SIZE, len(toc),
                              hashlib.sha256(toc).digest(), blob_at, blob_at + len(blobs),
                              len(records), 0, b"\0" * 16, b"\0" * 32)
    path.write_bytes((header + toc).ljust(blob_at, b"\0") + blobs)


class PackageImportTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.out = self.root / "out"
        self.out.mkdir()
        self.cancel = self.root / "cancel"
        self.archive = self.root / "game.nup"

    def run_import(self, *args):
        log = io.StringIO()
        with redirect_stdout(log):
            result = importer.main(["--out", str(self.out), "--cancel", str(self.cancel),
                                    "--game-version", "1.9.0", *args])
        return result, log.getvalue()

    def test_bundle_uses_newest_update_and_keeps_dlc_separate(self):
        fixture(self.archive, [
            (importer.BASE, nup.KIND_BASE, 0, "game", b"base"),
            (importer.UPDATE, nup.KIND_UPDATE, 2, "System/Version.txt", b"1.9.0"),
            (importer.UPDATE, nup.KIND_UPDATE, 1, "System/Version.txt", b"1.8.0"),
            (importer.BASE + 0x1002, nup.KIND_ADDON, 0, "addon", b"dlc"),
        ], compressed=True)
        self.assertEqual(self.run_import(str(self.archive))[0], 0)
        self.assertEqual((self.out / "romfs/System/Version.txt").read_bytes(), b"1.9.0")
        self.assertEqual((self.out / "aoc2/addon").read_bytes(), b"dlc")

    def test_rejects_unsafe_names(self):
        fixture(self.archive, [(importer.BASE + 0x1001, nup.KIND_ADDON, 0, "../outside", b"data")])
        self.assertEqual(self.run_import(str(self.archive)), (1, "failure\tinvalid\n"))
        self.assertFalse((self.out / "outside").exists())

    def test_standalone_nup_update(self):
        fixture(self.archive, [(importer.UPDATE, nup.KIND_UPDATE, 2, "System/Version.txt", b"1.9.0")])
        self.assertEqual(self.run_import(str(self.archive))[0], 0)
        self.assertEqual((self.out / "romfs/System/Version.txt").read_bytes(), b"1.9.0")

    def test_unsupported_nup_is_rejected_before_review(self):
        fixture(self.archive, [(importer.UPDATE, nup.KIND_UPDATE, 1114112, "System/Version.txt", b"2.4.7")])
        self.assertEqual(self.run_import("--confirm", str(self.root / "confirm"), str(self.archive)),
                         (1, "failure\tversion\n"))
        self.assertEqual(list(self.out.iterdir()), [])

    def test_dlc_only_nup(self):
        fixture(self.archive, [(importer.BASE + 0x1002, nup.KIND_ADDON, 2, "addon", b"dlc")])
        self.assertEqual(self.run_import(str(self.archive))[0], 0)
        self.assertEqual((self.out / "aoc2/addon").read_bytes(), b"dlc")
        self.assertFalse((self.out / "romfs").exists())

    def test_verifies_payload_hashes(self):
        fixture(self.archive, [(importer.BASE + 0x1001, nup.KIND_ADDON, 0, "data", b"data")], bad_hash=True)
        self.assertEqual(self.run_import(str(self.archive))[0], 1)

    def test_cancellation(self):
        fixture(self.archive, [(importer.BASE, nup.KIND_BASE, 0, "data", b"data")])
        self.cancel.touch()
        self.assertEqual(self.run_import(str(self.archive))[0], 1)

    def test_preview_reports_metadata_before_writing_payload(self):
        fixture(self.archive, [
            (importer.BASE, nup.KIND_BASE, 0, "game", b"base"),
            (importer.UPDATE, nup.KIND_UPDATE, 1114112, "System/Version.txt", b"\xef\xbb\xbf1.9.0\r\n"),
            (importer.BASE + 0x1001, nup.KIND_ADDON, 0, "System/AocVersion.txt", b"3.5\n"),
            (importer.BASE + 0x1002, nup.KIND_ADDON, 0, "System/AocVersion.txt", b"6.1\n"),
        ], compressed=True)
        confirm = self.root / "confirm"

        def approve(_):
            self.assertEqual(list(self.out.iterdir()), [])
            confirm.touch()

        with patch.object(importer.time, "sleep", side_effect=approve) as wait:
            result, log = self.run_import("--confirm", str(confirm), str(self.archive))
        wait.assert_called_once()
        self.assertEqual(result, 0)
        self.assertTrue(log.startswith("item\tbase\t\tgame.nup\nitem\tupdate\t1.9.0\tgame.nup\n"
                                       "item\taoc1\t3.5\tgame.nup\nitem\taoc2\t6.1\tgame.nup\nreview\n"))
        self.assertEqual((self.out / "romfs/System/Version.txt").read_bytes(), b"\xef\xbb\xbf1.9.0\r\n")

    def test_cancel_from_preview_leaves_destination_empty(self):
        fixture(self.archive, [(importer.UPDATE, nup.KIND_UPDATE, 0, "System/Version.txt", b"1.9.0")])
        with patch.object(importer.time, "sleep", side_effect=lambda _: self.cancel.touch()):
            result, log = self.run_import("--confirm", str(self.root / "confirm"), str(self.archive))
        self.assertEqual(result, 1)
        self.assertTrue(log.endswith("review\nfailure\tcancelled\n"))
        self.assertEqual(list(self.out.iterdir()), [])

    def test_standalone_nsp_patch_requires_base(self):
        with patch.object(importer, "inventory", return_value=[
            importer.Content(self.root / "update.nsp", "update", importer.UPDATE, 10),
        ]):
            self.assertEqual(self.run_import(str(self.archive)), (1, "failure\tbase\n"))

    def test_nsp_base_requires_matching_update(self):
        with patch.object(importer, "inventory", return_value=[
            importer.Content(self.root / "base.nsp", "base", importer.BASE, 0),
            importer.Content(self.root / "other-update.nsp", "update", importer.UPDATE + 0x2000, 10),
        ]):
            self.assertEqual(self.run_import(str(self.archive)), (1, "failure\tupdate\n"))
        self.assertEqual(list(self.out.iterdir()), [])

    def test_nsp_merge_must_report_supported_romfs_version(self):
        with patch.object(importer, "inventory", return_value=[
            importer.Content(self.root / "base.nsp", "base", importer.BASE, 0),
            importer.Content(self.root / "update.nsp", "update", importer.UPDATE, 1114112),
        ]), patch.object(importer, "run_nxpatch", return_value=[("update", "2.4.7")]) as helper:
            self.assertEqual(self.run_import(str(self.archive)), (1, "failure\tversion\n"))
            helper.assert_called_once()
            self.assertEqual(helper.call_args.args[-1][-2:], ["--versions", "1"])
        self.assertEqual(list(self.out.iterdir()), [])

    def test_combined_nsp_protocol(self):
        nsp = self.root / "combined.nsp"
        records = [
            importer.Content(nsp, "dlc", importer.BASE + 0x1001, 2),
            importer.Content(nsp, "update", importer.UPDATE, 10),
            importer.Content(nsp, "base", importer.BASE, 0),
        ]
        confirm = self.root / "confirm"
        versions = [("base", "1.0.0"), ("update", "1.9.0"), ("aoc1", "3.5")]
        with patch.object(importer, "inventory", return_value=records), patch.object(
                importer, "run_nxpatch", side_effect=[versions, []]) as helper:
            def approve(_):
                helper.assert_called_once()
                self.assertEqual(helper.call_args.args[-1][-2:], ["--versions", "1"])
                self.assertEqual(list(self.out.iterdir()), [])
                confirm.touch()

            with patch.object(importer.time, "sleep", side_effect=approve):
                result, log = self.run_import("--confirm", str(confirm), str(nsp))
                self.assertEqual(result, 0)
                self.assertIn("item\tupdate\t1.9.0\tcombined.nsp\n", log)
                self.assertIn("item\taoc1\t3.5\tcombined.nsp\n", log)
        self.assertEqual(helper.call_args.args[1:], ("extract", [
            "--base", str(nsp), "--update", str(nsp), "--version", "10",
            "--dlc", f"{importer.BASE + 0x1001:X}|2|{nsp}", "--require-version", "1.9.0",
            "--out", str(self.out),
        ]))


if __name__ == "__main__":
    unittest.main()
