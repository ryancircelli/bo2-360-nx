"""Screen/log-driven pad routes. The adapter only needs ctl, shot, alive and log_text.

Recipes are JSON, recordings remain ordinary SDK .route/input.txt files. Navigation
never schedules an input against boot time. All delays are bounded wait-for conditions.
"""
import json
import os
import re
import shutil
import time


FORMAT = 'gameplay-route-v1'


def load(path):
    with open(path) as f:
        prefix = f.read(1)
        if prefix != '{':
            return None
        f.seek(0)
        recipe = json.load(f)
    if recipe.get('format') != FORMAT:
        raise ValueError('unsupported conditional route format')
    return recipe


def horizontal_motion(a, b):
    """Estimate horizontal scene displacement; return shift and improvement over zero.

    Use the central world, excluding HUD and subtitles. Camera interventions in
    opposite directions must move scene content in opposite directions twice.
    Animated menus/cinematics plus an old checkpoint log cannot satisfy drive.
    """
    def gray(path):
        with open(path, 'rb') as f:
            if f.readline().strip() != b'P6':
                raise ValueError('expected P6 capture')
            line = f.readline()
            while line.startswith(b'#'):
                line = f.readline()
            w, h = map(int, line.split())
            if int(f.readline()) != 255:
                raise ValueError('expected 8-bit capture')
            pixels = f.read()
        if len(pixels) != w * h * 3:
            raise ValueError('truncated capture')
        return [[sum(pixels[(int(y*h/72)*w + int(x*w/128))*3:(int(y*h/72)*w + int(x*w/128))*3+3])/3
                 for x in range(128)] for y in range(72)]
    aa, bb = gray(a), gray(b)
    def error(dx):
        return sum(abs(aa[y][x] - bb[y][x+dx])
                   for y in range(20, 53, 2) for x in range(27, 101, 2))
    errors = {dx: error(dx) for dx in range(-10, 11)}
    shift = min(errors, key=errors.get)
    return shift, errors[0] / max(errors[shift], 1)


class Runner:
    def __init__(self, adapter, recipe, out, driving):
        self.game, self.recipe, self.out, self.driving = adapter, recipe, out, driving
        self.t0 = time.monotonic()
        self.capture_index = 0
        self.observations, self.events, self.trials = [], [], []
        self.proof = dict(version=1, game=recipe['game'], complete=False,
                          observations=self.observations, events=self.events, trials=self.trials)

    def save(self):
        with open(os.path.join(self.out, 'play.json'), 'w') as f:
            json.dump(self.proof, f, indent=2)

    def command(self, command):
        reply = self.game.ctl(command)
        self.events.append(dict(at=time.monotonic()-self.t0, command=command, reply=reply))
        if not reply.startswith('ok'):
            raise RuntimeError(f'{command}: {reply or "no reply"}')
        return reply

    def snapshot(self):
        index = self.capture_index
        self.capture_index += 1
        png, _ = self.game.shot(f'observe-{index:04d}')
        if not png:
            raise RuntimeError('capture failed')
        path = os.path.splitext(png)[0] + '.ppm'
        words = self.driving.recognize(path)
        log = self.game.log_text()
        state = self.driving.play_state(self.recipe['game'], words, log)
        sample = dict(at=time.monotonic()-self.t0, path=path, words=words, state=state, log_size=len(log))
        self.observations.append(sample)
        self.save()
        return sample

    def discard(self, sample):
        """Intermediate unchanged wait captures are not evidence; bound disk use."""
        self.observations.remove(sample)
        for path in (sample['path'], os.path.splitext(sample['path'])[0] + '.png'):
            os.unlink(path)

    def wait_for(self, before, *, timeout=12, change=6, text=None, log=None):
        """Bounded event wait, always against the capture/log offset before input."""
        end = time.monotonic() + timeout
        offset = before['log_size']
        last = before
        while self.game.alive() and time.monotonic() < end:
            if last is not before:
                self.discard(last)
            last = self.snapshot()
            joined = ' '.join(w['text'] for w in last['words'])
            delta = self.driving.scene_change(before['path'], last['path'])
            if ((text and re.search(text, joined, re.I))
                    or (log and re.search(log, self.game.log_text()[offset:], re.I))
                    or (not text and not log and delta is not None and delta >= change)):
                self.events.append(dict(at=last['at'], wait_for=dict(text=text, log=log, change=change),
                                        matched=True, before=before['path'], after=last['path']))
                return last
        self.events.append(dict(at=last['at'], wait_for=dict(text=text, log=log, change=change), matched=False))
        return last

    def navigate(self):
        deadline = time.monotonic() + self.recipe.get('navigation_timeout_s', 900)
        current = self.snapshot()
        while self.game.alive() and time.monotonic() < deadline:
            if current['state']:
                return current
            joined = ' '.join(w['text'] for w in current['words'])
            rule = next((r for r in self.recipe['navigation'] if re.search(r['text'], joined, re.I)), None)
            if rule:
                for cmd in rule['commands']:
                    self.command(cmd)
            current = self.wait_for(current, timeout=12, change=6)
        raise RuntimeError('navigation timed out without a verified gameplay state')

    def trial(self, kind, command):
        before = self.snapshot()
        self.command(command)
        after = self.wait_for(before, timeout=8, change=4)
        self.command('stick l off')
        self.command('stick r off')
        self.command('trigger r off')
        self.command('release all')
        self.trials.append(dict(kind=kind, command=command, before=before['path'], after=after['path'],
                                started=before['at'], ended=after['at']))
        self.save()
        if not after['state']:
            raise RuntimeError(f'lost gameplay state after {kind}')
        return after

    def named(self, sample, name):
        stem = os.path.join(self.out, 'shots', 'drive-' + name)
        shutil.copyfile(sample['path'], stem + '.ppm')
        png = os.path.splitext(sample['path'])[0] + '.png'
        shutil.copyfile(png, stem + '.png')

    def run(self):
        try:
            self.command('timescale 1')
            start = self.navigate()
            self.named(start, '01-gameplay-start')
            self.proof['play_started'] = start['at']
            deadline = time.monotonic() + self.recipe.get('play_timeout_s', 480)
            last = start
            cycles = 0
            while last['at'] - start['at'] < 125 and time.monotonic() < deadline:
                last = self.trial('movement', self.recipe['controls']['movement'])
                for direction in ('camera-left', 'camera-right'):
                    last = self.trial(direction, self.recipe['controls'][direction])
                if cycles == 0:
                    self.named(last, '02-movement-camera')
                last = self.trial('action', self.recipe['controls']['action'])
                cycles += 1
            self.named(last, '03-action-end')
            self.proof['play_ended'] = last['at']
            self.proof['complete'] = last['at'] - start['at'] >= 120
        except (OSError, ValueError, RuntimeError) as error:
            self.proof['error'] = str(error)
        finally:
            try:
                self.command('stick l off')
                self.command('stick r off')
                self.command('trigger r off')
                self.command('release all')
                samples = os.path.join(self.out, 'recorded.input.txt')
                reply = self.command('record ' + samples)
                first = re.search(r'first_ms (\d+)', reply)
                count = re.search(r'ok recorded (\d+) samples', reply)
                if not first or not count or int(count[1]) == 0:
                    raise RuntimeError('record returned no pad samples')
                if first:
                    with open(os.path.join(self.out, 'recorded.route'), 'w') as f:
                        f.write(f'REX_HID_VIRTUAL_PADS = 1\nREX_HID_REPLAY_INPUT = {samples}\n'
                                f'REX_HID_REPLAY_DELAY = {int(first[1])/1000:.3f}\n')
            except (OSError, RuntimeError) as error:
                self.proof['record_error'] = str(error)
                self.proof['complete'] = False
            self.save()
        return self.proof
