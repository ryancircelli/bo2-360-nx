"""An unchanged seed or an empty file must never become a successful save proof."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('gate_savefiles', Path(__file__).with_name('savefiles.py'))
saves = importlib.util.module_from_spec(spec)
spec.loader.exec_module(saves)


class Saves(unittest.TestCase):
    def test_seed_unchanged_empty_and_settings_are_not_writes(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root,'default_checkpoint.sav')
            path.write_bytes(b'seed')
            before = saves.inventory(root,'gears')
            self.assertEqual(saves.changed(before,saves.inventory(root,'gears')), {})
            path.write_bytes(b'')
            self.assertEqual(saves.changed(before,saves.inventory(root,'gears')), {})
            Path(root,'settings.toml').write_text('changed=true')
            self.assertEqual(saves.changed(before,saves.inventory(root,'gears')), {})
            path.write_bytes(b'new checkpoint')
            self.assertEqual(list(saves.changed(before,saves.inventory(root,'gears'))), ['default_checkpoint.sav'])

    def test_payload_in_content_directory_and_no_external_symlinks(self):
        with tempfile.TemporaryDirectory() as root:
            content=Path(root,'Hawaii.sav'); content.mkdir()
            (content/'MUD.29').write_bytes(b'actual save')
            (content/'outside').symlink_to('/etc/hosts')
            self.assertEqual(list(saves.inventory(root,'nfsmw2012')), ['Hawaii.sav/MUD.29'])

    def test_reopen_needs_successful_existing_content_open(self):
        writes={'Hawaii.sav/MUD.29':dict(size=11,sha256='hash')}
        for log in ["XamContentCreate Hawaii.sav mode 3 result 3", "XamContentCreate Hawaii.sav mode 1 result 0", 'Hawaii.sav exists']:
            self.assertFalse(saves.reopened(log,writes))
        self.assertTrue(saves.reopened('XamContentCreate Hawaii.sav mode 3 result 0',writes))
        self.assertTrue(saves.reopened("XamContentCreate root 'savegame' content 'Hawaii.sav' type 00000001 mode 3 -> open (result 00000000)",writes))
        self.assertFalse(saves.reopened('XamContentCreate Hawaii.sav mode 3 result 0\nXamContentCreate Hawaii.sav mode 1 -> create result 0\nXamContentCreate Hawaii.sav mode 3 result 0',writes))


if __name__ == '__main__':
    unittest.main()
