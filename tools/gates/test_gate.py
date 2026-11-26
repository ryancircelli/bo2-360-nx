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
        self.assertEqual(sorted(set(gate.GAMES) - set(driving.GAMES)), [], 'drive would fail closed for these: add a rule')

    def test_drive_routes_exist(self):
        for game, g in gate.GAMES.items():
            if g['drive']:
                self.assertTrue(any((HERE / 'routes' / (g['drive'] + '.route')).exists() for _ in [0])
                                or game in ('bo2',), f'{game}: no routes/{g["drive"]}.route')

    def test_eight_games(self):
        self.assertEqual(len(gate.GAMES), 8)


if __name__ == '__main__':
    unittest.main()
