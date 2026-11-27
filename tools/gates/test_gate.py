"""Gate kit self-test: log-check regex and the game table. No game, OCR or runq needed: `gate.py selftest`."""
import importlib.util
import os
from pathlib import Path
import unittest

HERE = Path(__file__).parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gate, driving = load('gate'), load('driving')


class LogCheck(unittest.TestCase):
    def test_zero_counters_are_not_failures(self):
        for line in ('drawconsts=7108/11071 verify_mismatch=0 bindless_miss=0', 'verify mismatches 0', 'MISMATCH=0'):
            self.assertIsNone(gate.CRASH.search(line), line)

    def test_real_failures_are_caught(self):
        for line in ('native: MISMATCH at 82A63B88', 'verify_mismatch=3', 'Guest access violation', 'lost wake'):
            self.assertTrue(gate.CRASH.search(line), line)

    def test_gate_line_names_the_check_that_fired(self):
        text = '[a] ok\n[2026-11-24 23:41:11.040] [warning] [core] [t1] [native_audio] biquad: MISMATCH at 82A6\n[b] ok\n'
        hit = gate.log_hit(gate.CRASH.search(text))
        self.assertIn('native_audio', hit)
        self.assertNotIn('[b] ok', hit)


class RunDir(unittest.TestCase):
    def test_same_second_gates_get_different_dirs(self):
        import tempfile
        from unittest import mock
        with tempfile.TemporaryDirectory() as d, mock.patch.object(gate, 'RUNS', d), \
                mock.patch.object(gate.time, 'strftime', return_value='1127-070645'), mock.patch.object(gate.os, 'getpid', return_value=1):
            a, b = gate.default_out('fh1', 'drive'), gate.default_out('fh1', 'drive')   # a second gate of the same pid/second takes the -1 suffix
            self.assertNotEqual(a, b)
            self.assertTrue(os.path.isdir(a) and os.path.isdir(b))

    def test_two_fake_gates_in_one_second_do_not_share_a_control_channel(self):
        import tempfile, subprocess, sys
        code = ('import sys,importlib.util as u;s=u.spec_from_file_location("gate",sys.argv[1]);m=u.module_from_spec(s);s.loader.exec_module(m);'
                'import os;m.RUNS=sys.argv[2];print(m.default_out("fh1","drive"))')
        with tempfile.TemporaryDirectory() as d:
            ps = [subprocess.Popen([sys.executable, '-I', '-c', code, str(HERE / 'gate.py'), d], stdout=subprocess.PIPE, text=True) for _ in range(2)]
            outs = [p.communicate()[0].strip() for p in ps]
            self.assertEqual(len(set(outs)), 2, outs)

    def test_reused_dir_fails_loud(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'control'))
            open(os.path.join(d, 'control', 'pid.txt'), 'w').write('1')
            with self.assertRaises(SystemExit):
                gate.Game('/x.app', d, 'bo2')


class Launch(unittest.TestCase):
    def test_canonical_minimized_and_mute_names_are_set(self):
        src = (HERE / 'gate.py').read_text()
        for name in ('REX_UI_SDL_START_MINIMIZED', 'REX_UI_FULLSCREEN', 'REX_APU_MUTE'):
            self.assertIn("'" + name + "'", src)


class HeadlessMarker(unittest.TestCase):
    def test_marker_found_only_when_present(self):
        import tempfile
        for body, want in ((b'xx headless_active yy', True), (b'nothing', False)):
            with tempfile.TemporaryDirectory() as d:
                os.makedirs(d + '/Contents/MacOS')
                open(d + '/Contents/MacOS/lib', 'wb').write(body)
                self.assertEqual(gate.has_headless(d), want)


class GameTable(unittest.TestCase):
    def test_every_gate_game_has_a_gameplay_rule(self):
        # me2 has a menu route only: its drive gate fails closed until it gets a gameplay rule.
        self.assertEqual(sorted(set(gate.GAMES) - set(driving.GAMES) - {'me2'}), [], 'drive would fail closed for these: add a rule')

    def test_drive_routes_exist(self):
        for game, g in gate.GAMES.items():
            if g['drive']:
                self.assertTrue(any((HERE / 'routes' / (g['drive'] + '.route')).exists() for _ in [0])
                                or game in ('bo2',), f'{game}: no routes/{g["drive"]}.route')

    def test_nine_games(self):
        self.assertEqual(len(gate.GAMES), 9)


if __name__ == '__main__':
    unittest.main()
