"""No-game regressions for cinematic false positives and input-response evidence."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


HERE = Path(__file__).parent


def load(name):
    spec = importlib.util.spec_from_file_location('test_' + name, HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


routes, driving, gate = load('condition_routes'), load('driving'), load('gate')


class ConditionRoutes(unittest.TestCase):
    def image(self, path, shift=0):
        # A detailed world translated across a fixed viewport.
        pixels = bytearray()
        for y in range(72):
            for x in range(128):
                v = ((x-shift)*73 + y*47 + ((x-shift)*y)%43) % 256
                pixels.extend([v, v, v])
        path.write_bytes(b'P6\n128 72\n255\n' + pixels)

    def test_camera_direction_and_frozen_scene(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory, name+'.ppm') for name in ('base','left','right')]
            for path, shift in zip(paths, (0,4,-4)):
                self.image(path, shift)
            self.assertEqual(routes.horizontal_motion(paths[0],paths[1])[0], 4)
            self.assertEqual(routes.horizontal_motion(paths[0],paths[2])[0], -4)
            self.assertLess(routes.horizontal_motion(paths[0],paths[0])[1], 1.08)

    def test_chase_car_does_not_mask_world_camera_motion(self):
        with tempfile.TemporaryDirectory() as directory:
            paths=[Path(directory,'before.ppm'),Path(directory,'after.ppm')]
            for path,shift in zip(paths,(0,4)):
                pixels=bytearray()
                for y in range(72):
                    for x in range(128):
                        xx=x-shift if 16 <= y < 34 else x
                        value=(xx*73+y*47+(xx*y)%43)%256
                        pixels.extend([value]*3)
                path.write_bytes(b'P6\n128 72\n255\n'+pixels)
            self.assertEqual(routes.horizontal_motion(*paths,game='nfsmw2012')[0],4)

    def test_first_capture_waits_for_renderer_readiness(self):
        with tempfile.TemporaryDirectory() as directory:
            ppm=Path(directory,'frame.ppm'); self.image(ppm)
            class Adapter:
                calls=0
                flushed=False
                def shot(self,name):
                    self.calls+=1
                    return (None,None) if self.calls==1 else (str(ppm.with_suffix('.png')),None)
                def alive(self): return True
                def log_text(self): return ''
                def ctl(self,command,*args):
                    self.flushed=command.startswith('flush')
                    return 'ok'
            adapter=Adapter()
            runner=routes.Runner(adapter,dict(game='gears'),directory,driving)
            with patch.object(driving,'recognize',return_value=[]):
                runner.snapshot()
            self.assertTrue(adapter.flushed)
            self.assertEqual(adapter.calls,2)

    def test_partial_mph_ocr_is_accepted_only_in_driving_hud(self):
        words=[dict(text='MPHI',x=.92,y=.87)]
        self.assertTrue(driving.play_state('nfsmw2012',words,''))
        self.assertFalse(driving.play_state('nfsmw2012',[dict(text='MPHI',x=.2,y=.2)],''))
        self.assertFalse(driving.play_state('nfsmw2012',words+[dict(text='Main Menu',x=.1,y=.1)],''))

    def fixture(self, directory, *, elapsed=160, cinematic=False, missing_state=False, recording=True):
        root = Path(directory)
        observations, trials, events = [], [], []
        kinds = ['movement','camera-left','camera-right','action'] * 2
        for i, kind in enumerate(kinds):
            command = 'press a 500' if kind == 'action' else 'stick '+kind
            pair = []
            for after in (False,True):
                path = root / f'{i}-{int(after)}.ppm'
                shift = (4 if kind != 'camera-right' or cinematic else -4) if after else 0
                self.image(path,shift)
                sample = dict(path=str(path),at=(i*2+int(after))*elapsed/15,
                              words=[],state=not missing_state)
                observations.append(sample)
                pair.append(sample)
            trials.append(dict(kind=kind,command=command,before=pair[0]['path'],after=pair[1]['path']))
            events.append(dict(command=command,reply='ok'))
        if recording:
            events.append(dict(command='record '+str(root/'recorded.input.txt'),reply='ok recorded 16 samples'))
        run = dict(version=1,game='gears',complete=True,play_started=0,play_ended=elapsed,
                   observations=observations,trials=trials,events=events)
        (root/'play.json').write_text(json.dumps(run))

    def test_complete_gameplay_and_repeated_camera_reversal(self):
        with tempfile.TemporaryDirectory() as root:
            self.fixture(root)
            proof=driving.verify('gears',root,'default_checkpoint.sav')
            self.assertTrue(proof['passed'], proof)

    def test_cinematic_cannot_pass_with_checkpoint_and_frame_change(self):
        with tempfile.TemporaryDirectory() as root:
            self.fixture(root,cinematic=True)
            self.assertFalse(driving.verify('gears',root,'default_checkpoint.sav')['passed'])

    def test_short_run_unknown_state_or_no_recording_cannot_pass(self):
        for changes in [dict(elapsed=60),dict(missing_state=True),dict(recording=False)]:
            with self.subTest(changes=changes), tempfile.TemporaryDirectory() as root:
                self.fixture(root,**changes)
                self.assertFalse(driving.verify('gears',root,'default_checkpoint.sav')['passed'])

    def test_all_six_recipes_parse_and_create_virtual_pad_without_timed_script(self):
        for game in driving.NEW_GAMES:
            path=HERE/'routes'/(game+'-play.route')
            self.assertEqual(routes.load(path)['game'],game)
            self.assertEqual(gate.route_env(path),{'REX_HID_VIRTUAL_PADS':'1'})
            self.assertGreaterEqual(gate.route_len(path), 120)


if __name__ == '__main__':
    unittest.main()
