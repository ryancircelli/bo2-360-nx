"""Title detection checks: the title id read from an executable's XEX2 header, the title packs in games/, and the
installer refusing another game's disc that also ships a default_mp.xex (Black Ops does).

Run: python -I tools/tests/test_title_packs.py
Uses small authored executables; the real discs under ~/Games/Xbox360 are checked too when they are present.
"""

from pathlib import Path
import os
import struct
import sys
import tempfile
import unittest

INSTALL = Path(__file__).resolve().parents[1] / "install"
sys.path.insert(0, str(INSTALL))

import game_files  # noqa: E402
import title_packs  # noqa: E402


def xex(title_id, extra_headers=()):
    """The first bytes of an XEX2 file: magic, module flags, header size, reserved, security-info offset, optional
    header count, then the optional headers (execution info last) and the execution-info block itself."""
    headers = list(extra_headers) + [(title_packs.EXECUTION_INFO, None)]
    table_end = 0x18 + 8 * len(headers)
    out = bytearray(b"XEX2" + struct.pack(">IIIII", 0, 0x1000, 0, 0, len(headers)))
    for key, value in headers:
        out += struct.pack(">II", key, table_end if value is None else value)
    # media id, version, base version, title id, platform/type, disc number/count, savegame id
    out += struct.pack(">IIII", 0x11223344, 0x20000000, 0x20000000, title_id) + bytes(8)
    return bytes(out)


class XexTitleId(unittest.TestCase):
    def test_reads_execution_info(self):
        self.assertEqual(title_packs.xex_title_id(xex(0x415608C3)), "415608C3")

    def test_skips_other_headers(self):
        data = xex(0x41560855, extra_headers=[(0x000002FF, 0x40), (0x00010001, 0x82000000)])
        self.assertEqual(title_packs.xex_title_id(data), "41560855")

    def test_rejects_non_executables(self):
        self.assertIsNone(title_packs.xex_title_id(b""))
        self.assertIsNone(title_packs.xex_title_id(b"XEX1" + bytes(60)))
        self.assertIsNone(title_packs.xex_title_id(b"XEX2" + struct.pack(">IIIII", 0, 0, 0, 0, 0)))  # no headers
        self.assertIsNone(title_packs.xex_title_id(xex(0x415608C3)[:-12]))  # cut off before the title id


class Packs(unittest.TestCase):
    def test_every_pack_names_its_title(self):
        packs = title_packs.packs()
        self.assertEqual(packs.get("415608C3"), "bo2")
        self.assertEqual(packs.get("41560855"), "bo1")
        self.assertEqual(packs.get("454107D9"), "nfsmw")
        self.assertEqual(packs.get("4D5309C9"), "fh1")


class Installer(unittest.TestCase):
    def folder(self, root, name, title_id):
        disc = Path(root) / name
        disc.mkdir()
        (disc / "default_mp.xex").write_bytes(xex(title_id) + bytes(64))
        (disc / "default.xex").write_bytes(xex(title_id) + bytes(64))
        return disc

    def test_black_ops_folder_is_refused_by_title(self):
        with tempfile.TemporaryDirectory() as root:
            self.folder(root, "bo1", 0x41560855)
            found = game_files.scan(root)
            self.assertEqual(found.games, [])
            self.assertEqual(len(found.problems), 1)
            self.assertIn("title id is 41560855", found.problems[0])
            self.assertIn("games/bo1", found.problems[0])

    def test_black_ops_ii_folder_is_still_found(self):
        with tempfile.TemporaryDirectory() as root:
            disc = self.folder(root, "bo2", 0x415608C3)
            found = game_files.scan(root)
            self.assertEqual([g.path for g in found.games], [str(disc)])
            self.assertEqual(found.problems, [])

    def test_unknown_title_says_so(self):
        with tempfile.TemporaryDirectory() as root:
            self.folder(root, "other", 0x12345678)
            found = game_files.scan(root)
            self.assertEqual(found.games, [])
            self.assertIn("no title pack", found.problems[0])


REAL = {
    "~/Games/Xbox360/bo1/disc": ("41560855", "bo1"),
    "~/Games/Xbox360/nfsmw/disc": ("454107D9", "nfsmw"),
    "~/Games/Xbox360/fh1/disc": ("4D5309C9", "fh1"),
    "~/Games/Xbox360/bo2-build/data/game": ("415608C3", "bo2"),
}


class RealDiscs(unittest.TestCase):
    def test_real_discs(self):
        checked = 0
        for path, want in REAL.items():
            path = os.path.expanduser(path)
            if os.path.isdir(path):
                self.assertEqual(title_packs.identify(path), want, path)
                checked += 1
        if not checked:
            self.skipTest("no extracted discs on this machine")


if __name__ == "__main__":
    unittest.main()
