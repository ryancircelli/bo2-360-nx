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
            with patch.object(driving, 'recognize', return_value=words):
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
        self.assertFalse(self.exercise(hud(0)))
        self.assertFalse(self.exercise(hud(60), moving=False))

    def test_driving_hud_and_moving_world_pass(self):
        self.assertTrue(self.exercise(hud(60)))

    def test_shooter_menu_after_match_cannot_pass(self):
        self.assertFalse(self.exercise([{'text': 'Resume Game', 'x': .3, 'y': .3}],
                                       game='bo2', log="the game's client is in a match"))

    def test_shooter_requires_match_and_moving_world(self):
        self.assertFalse(self.exercise([], game='bo2'))
        self.assertTrue(self.exercise([], game='bo2', log="the game's client is in a match"))
        self.assertFalse(self.exercise([], moving=False, game='bo2', log="the game's client is in a match"))


if __name__ == '__main__':
    unittest.main()
