import os, plistlib, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import install_guard as ig


def app(root, name, files, plist=None):
    p = os.path.join(root, name + '.app')
    for rel, data in files.items():
        f = os.path.join(p, 'Contents', rel)
        os.makedirs(os.path.dirname(f), exist_ok=True)
        open(f, 'wb').write(data)
    with open(os.path.join(p, 'Contents', 'Info.plist'), 'wb') as f:
        plistlib.dump(plist or {'CFBundleIdentifier': 'x.bo1', 'NSMicrophoneUsageDescription': 'mic'}, f)
    return p


OLD = {'MacOS/bo1': b'1', 'MacOS/bo1_mp': b'2', 'MacOS/librexgpu-metal.dylib': b'm', 'Resources/AppIcon.icns': b'i', 'Resources/runtime/a': b'r'}


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.r = self.d.name
        self.old = app(self.r, 'old', OLD)

    def tearDown(self):
        self.d.cleanup()

    def test_identical_has_no_losses(self):
        self.assertEqual(ig.losses(app(self.r, 'n', OLD), self.old), {})

    def test_the_incident_lost_bo1_mp(self):
        n = app(self.r, 'n', {k: v for k, v in OLD.items() if k != 'MacOS/bo1_mp'})
        self.assertIn('file:MacOS/bo1_mp', ig.losses(n, self.old))

    def test_manifest_mode_binary_and_dylib_and_dir_icon_plist_id(self):
        n = app(self.r, 'n', {'MacOS/bo1': b'1'}, {'CFBundleIdentifier': 'other'})
        L = ig.losses(n, self.old, {'modes': {'multiplayer': 'bo1_mp'}, 'required_paths': ['Resources/data']})
        for k in ['file:MacOS/bo1_mp', 'file:MacOS/librexgpu-metal.dylib', 'file:Resources/AppIcon.icns', 'dir:Resources/runtime',
                  'bundle-id', 'plist-key:NSMicrophoneUsageDescription', 'file:Resources/data']:
            self.assertIn(k, L)

    def test_first_install_only_checks_manifest(self):
        n = app(self.r, 'n', {'MacOS/bo1': b'1'})
        self.assertEqual(list(ig.losses(n, None, {'binaries': ['bo1_mp']})), ['file:MacOS/bo1_mp'])

    def test_report_marks_allowed(self):
        n = app(self.r, 'n', {'MacOS/bo1': b'1'})
        L = ig.losses(n, self.old)
        self.assertIn('ALLOWED file:MacOS/bo1_mp', ig.report(n, self.old, L, {'file:MacOS/bo1_mp'}))

    def test_gates_must_pass_on_same_build(self):
        a = app(self.r, 'n', OLD)
        b = ig.fingerprint(a)
        m = {'gates': ['boot', 'open']}
        rec = lambda g, v, build: dict(gate=g, game='bo1', verdict=v, build=build)
        self.assertEqual(ig.missing_gates('bo1', b, m, [rec('boot', 'PASS', b), rec('open', 'FAIL', b)]), ['open'])
        self.assertEqual(ig.missing_gates('bo1', b, m, [rec('boot', 'PASS', b), rec('open', 'PASS', 'other')]), ['open'])
        self.assertEqual(ig.missing_gates('bo1', b, m, [rec('boot', 'PASS', b), rec('open', 'PASS', b)]), [])

    def test_fingerprint_follows_content_not_path(self):
        self.assertEqual(ig.fingerprint(app(self.r, 'a', OLD)), ig.fingerprint(app(self.r, 'b', OLD)))
        self.assertNotEqual(ig.fingerprint(app(self.r, 'c', dict(OLD, **{'MacOS/bo1': b'9'}))), ig.fingerprint(self.old))

    def test_every_manifest_parses(self):
        for g in os.listdir(ig.MANIFEST_DIR):
            m = ig.load_manifest(g)
            if m:
                self.assertTrue(m['gates'], g)


if __name__ == '__main__':
    unittest.main()
