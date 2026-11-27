"""Synthetic content only: no game files or signing keys."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("content_disc", Path(__file__).parents[1] / "mac/install_content_disc.py")
disc = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(disc)


def package(path, title=0x12345678, name=b"addon.esm", payload=b"synthetic DLC", parent=0xFFFF):
    data = bytearray(0xD000)
    data[:4] = b"PIRS"
    struct.pack_into(">II", data, 0x340, 0x971A, 2)
    struct.pack_into(">I", data, 0x360, title)
    struct.pack_into(">QII", data, 0x22C, 0xFFFFFFFFFFFFFFFF, 0x1234, 1)
    data[0x411:0x411 + 8] = "Pack".encode("utf-16-be")
    data[0x379] = 0x24
    data[0x37B] = 1  # read-only STFS
    struct.pack_into("<H", data, 0x37C, 1)
    struct.pack_into(">I", data, 0x395, 2)
    struct.pack_into(">I", data, 0xA000 + 20, 0xFFFFFF)
    struct.pack_into(">I", data, 0xA000 + 44, 0xFFFFFF)
    entry = 0xB000
    data[entry:entry + len(name)] = name
    data[entry + 0x28] = len(name)
    data[entry + 0x2C] = 1
    data[entry + 0x2F] = 1
    struct.pack_into(">HI", data, entry + 0x32, parent, len(payload))
    data[0xC000:0xC000 + len(payload)] = payload
    path.write_bytes(data)


class InstallContentDisc(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.pkg = self.source / ("A" * 42)
        package(self.pkg)
        self.out = self.root / "content"

    def run_install(self, *args):
        return disc.main([str(self.source), "--title-id", "12345678", "--out", str(self.out), *args])

    def test_embedded_title_payload_header_and_licence(self):
        misleading = self.source / "WRONGTITLE" / "FFFFFFFF"
        misleading.mkdir(parents=True)
        self.pkg.rename(misleading / self.pkg.name)
        self.run_install()
        root = self.out / "0000000000000000/12345678"
        self.assertEqual((root / "00000002" / self.pkg.name / "addon.esm").read_bytes(), b"synthetic DLC")
        header = (root / "Headers/00000002" / (self.pkg.name + ".header")).read_bytes()
        self.assertEqual(len(header), 0x14C)
        self.assertEqual(struct.unpack_from(">II", header), (1, 2))
        self.assertEqual(struct.unpack_from(">QI", header, 0x138), (0, 0x12345678))
        self.assertEqual(struct.unpack_from("=I", header, 0x148)[0], 0x1234)
        with self.assertRaises(FileExistsError):
            self.run_install()

    def test_list_writes_nothing_and_wrong_title_fails(self):
        self.run_install("--list")
        self.assertFalse(self.out.exists())
        package(self.pkg, title=0x87654321)
        with self.assertRaises(ValueError):
            self.run_install()
        self.assertFalse(self.out.exists())

    def test_traversal_and_bad_parent_leave_no_package(self):
        for name, parent in [(b"../escape", 0xFFFF), (b"addon.esm", 0)]:
            package(self.pkg, name=name, parent=parent)
            with self.assertRaises(ValueError):
                self.run_install()
            self.assertFalse(self.out.exists())

    def test_short_chain_never_publishes(self):
        package(self.pkg, payload=b"X" * 8192)
        with self.assertRaises(ValueError):
            self.run_install()
        self.assertFalse(list(self.out.rglob("*.header")))
        self.assertFalse(list(self.out.rglob("addon.esm")))

    def test_symlink_cannot_redirect_install(self):
        self.out.mkdir()
        (self.out / "0000000000000000").symlink_to(self.source, target_is_directory=True)
        with self.assertRaises(ValueError):
            self.run_install()
        self.assertFalse((self.source / "12345678").exists())


if __name__ == "__main__":
    unittest.main()
