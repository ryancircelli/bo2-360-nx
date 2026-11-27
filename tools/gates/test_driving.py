"""Regressions for the alive-only false PASS (no game or OCR service needed)."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('driving', Path(__file__).with_name('driving.py'))
driving = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driving)


def hud(speed):
    return [{'text': 'MPH', 'x': .85, 'y': .85}, {'text': str(speed), 'x': .8, 'y': .8}]


class GameplayProof(unittest.TestCase):
    def exercise(self, words, moving=True, game='nfsmw', log=''):
        with tempfile.TemporaryDirectory() as root:
            shots = Path(root, 'shots')
            shots.mkdir()
            for i in range(2):
                # Detailed frames with a large change, like an animated menu background.
                data = bytes((x + i * 50 * moving) % 256 for x in range(120 * 100 * 3))
                (shots / f'drive{i:02d}.ppm').write_bytes(b'P6\n120 100\n255\n' + data)
            ocr = {'side_effect': words} if isinstance(words, tuple) else {'return_value': words}
            with patch.object(driving, 'recognize', **ocr):
                proof = driving.verify(game, root, log)
            self.assertEqual(proof, json.loads(Path(root, 'driving.json').read_text()))
            return proof['passed']

    def test_animated_menu_cannot_pass(self):
        self.assertFalse(self.exercise([{'text': 'Resume Free Roam', 'x': .6, 'y': .8}]))

    def test_safe_house_with_speed_text_cannot_pass(self):
        self.assertFalse(self.exercise(hud(60) + [{'text': 'Safe House', 'x': .2, 'y': .4}]))

    def test_car_specification_speed_cannot_pass(self):
        self.assertFalse(self.exercise([{'text': 'MPH', 'x': .2, 'y': .3},
                                       {'text': '180', 'x': .2, 'y': .25}]))

    def test_stationary_or_frozen_game_cannot_pass(self):
        self.assertFalse(self.exercise(hud(0), moving=False))
        self.assertFalse(self.exercise(hud(60), moving=False))

    def test_driving_hud_and_moving_world_pass(self):
        self.assertTrue(self.exercise(hud(60)))
        self.assertTrue(self.exercise([{'text': 'MPH', 'x': .85, 'y': .85}]))

    def test_hot_pursuit_requires_decreasing_race_distance(self):
        def progress(distance, clock):
            return [{'text': distance + ' Miles', 'x': .85, 'y': .2},
                    {'text': clock, 'x': .8, 'y': .15}]
        a = progress('8.4', '1:00.00')
        b = progress('7.9', '2:00.00')
        self.assertTrue(self.exercise((a, b), game='nfshp'))
        self.assertFalse(self.exercise((a, a), game='nfshp'))
        self.assertFalse(self.exercise((b, a), game='nfshp'))
        self.assertFalse(self.exercise((a, b), moving=False, game='nfshp'))

    def test_forza_speed_hud_is_distinct_from_menu_or_zero_speed(self):
        def speed(value):
            return [{'text': value, 'x': .845, 'y': .79, 'w': .065, 'h': .045}]
        self.assertTrue(self.exercise((speed('059'), speed('053')), game='fh1'))
        self.assertFalse(self.exercise(speed('000'), game='fh1'))
        self.assertFalse(self.exercise(speed('059'), game='fh1', moving=False))
        self.assertIsNone(driving.forza_speed([{'text': '059', 'x': .1, 'y': .2, 'w': .06, 'h': .04}]))

    def test_bo1_requires_zombies_zone_and_ammo_hud(self):
        words = [{'text': '8/32', 'x': .78, 'y': .85}]
        log = r'[zone] open D:\zombie_theater.ff'
        self.assertTrue(self.exercise(words, game='bo1', log=log))
        self.assertFalse(self.exercise(words, game='bo1'))
        self.assertFalse(self.exercise([], game='bo1', log=log))
        self.assertFalse(self.exercise(words, moving=False, game='bo1', log=log))

    def test_shooter_menu_after_match_cannot_pass(self):
        self.assertFalse(self.exercise([{'text': 'Resume Game', 'x': .3, 'y': .3}],
                                       game='bo2', log="the game's client is in a match"))

    def test_shooter_requires_match_and_moving_world(self):
        self.assertFalse(self.exercise([], game='bo2'))
        self.assertFalse(self.exercise([], game='bo2', log="waiting for a log line with the game's client is in a match"))
        self.assertTrue(self.exercise([], game='bo2', log="the game's client is in a match"))
        self.assertFalse(self.exercise([], moving=False, game='bo2', log="the game's client is in a match"))

    def test_gears_needs_campaign_log_marker_and_movement(self):
        log = "XamContentCreate root 'save' content 'default_checkpoint.sav'"
        self.assertTrue(self.exercise([], game='gears', log=log))
        self.assertFalse(self.exercise([], game='gears'))                       # alive in a menu: no marker
        self.assertFalse(self.exercise([], game='gears', log=log, moving=False))
        self.assertFalse(self.exercise([{'text': 'Main Menu', 'x': .5, 'y': .5}], game='gears', log=log))

    def test_ac1_needs_play_state_text_and_movement(self):
        words = [{'text': 'PRESS ANY BUTTON TO INTERACT', 'x': .5, 'y': .8}]
        self.assertTrue(self.exercise(words, game='ac1'))
        self.assertFalse(self.exercise([], game='ac1'))
        self.assertFalse(self.exercise(words, game='ac1', moving=False))

    def test_game_without_a_rule_cannot_pass(self):
        self.assertFalse(self.exercise(hud(60), game='newgame-without-rule'))

    def test_nfsmw2012_uses_the_speed_hud(self):
        self.assertTrue(self.exercise(hud(60), game='nfsmw2012'))
        self.assertFalse(self.exercise([], game='nfsmw2012'))


if __name__ == '__main__':
    unittest.main()
