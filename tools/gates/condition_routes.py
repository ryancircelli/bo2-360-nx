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


def horizontal_motion(a, b, game=None):
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
    # The chase car is stationary in screen space; use the world above it.
    rows = range(16, 34, 2) if game in ('nfsmw2012','ac1') else range(20, 53, 2)
    def error(dx):
        return sum(abs(aa[y][x] - bb[y][x+dx])
                   for y in rows for x in range(27, 101, 2))
    errors = {dx: error(dx) for dx in range(-10, 11)}
    shift = min(errors, key=errors.get)
    return shift, errors[0] / max(errors[shift], 1)


class Runner:
    def __init__(self, adapter, recipe, out, driving):
        self.game, self.recipe, self.out, self.driving = adapter, recipe, out, driving
        self.t0 = time.monotonic()
        self.capture_index = 0
        self.observations, self.events, self.trials = [], [], []
        self.milestones = []
        self.proof = dict(version=1, game=recipe['game'], complete=False,
                          observations=self.observations, events=self.events, trials=self.trials, milestones=self.milestones)

    def save(self):
        with open(os.path.join(self.out, 'play.json'), 'w') as f:
            json.dump(self.proof, f, indent=2)

    def command(self, command):
        reply = self.game.ctl(command)
        self.events.append(dict(at=time.monotonic()-self.t0, command=command, reply=reply))
        if not reply.startswith('ok'):
            raise RuntimeError(f'{command}: {reply or "no reply"}')
        if command.startswith('press '):
            end = time.monotonic() + 15
            while time.monotonic() < end and self.game.alive():
                pad = self.game.ctl('pad')
                if re.search(r'\btaps 0\b', pad):
                    break
            else:
                raise RuntimeError('wait-for queued pad tap completion timed out')
        return reply

    def snapshot(self):
        index = self.capture_index
        self.capture_index += 1
        png, _ = self.game.shot(f'observe-{index:04d}')
        deadline = time.monotonic() + 30
        while not png and self.game.alive() and time.monotonic() < deadline:
            # Control ping can answer before the first rendered frame. This is a
            # capture-ready condition, not a fixed delay before navigating.
            self.game.ctl('flush 1500', 5)
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
        for path in (sample['path'], os.path.splitext(sample['path'])[0] + '.png',
                     os.path.splitext(sample['path'])[0] + '.capture.json'):
            if os.path.exists(path):
                os.unlink(path)

    def wait_for(self, before, *, timeout=12, change=6, text=None, log=None, motion_sign=None, not_text=None, navigation=False):
        """Bounded event wait, always against the capture/log offset before input."""
        end = time.monotonic() + timeout
        offset = before['log_size']
        last = before
        while self.game.alive() and time.monotonic() < end:
            if last is not before:
                self.discard(last)
            last = self.snapshot()
            joined = ' '.join(w['text'] for w in last['words'])
            before_text = ' '.join(w['text'] for w in before['words'])
            navigation_text = navigation and not not_text and joined != before_text and any(
                re.search(rule['text'],joined,re.I) for rule in self.recipe['navigation'])
            navigation_state = navigation and not not_text and last['state'] and not before['state']
            delta = self.driving.scene_change(before['path'], last['path'])
            motion_ok = True
            if motion_sign is not None:
                shift, confidence = horizontal_motion(before['path'], last['path'], self.recipe['game'])
                motion_ok = shift * motion_sign >= 1 and confidence >= 1.08
            if ((text and re.search(text, joined, re.I))
                    or navigation_text or navigation_state
                    or (not_text and not re.search(not_text,joined,re.I))
                    or (log and re.search(log, self.game.log_text()[offset:], re.I))
                    or (not text and not log and not not_text and delta is not None and delta >= change and motion_ok)):
                self.events.append(dict(at=last['at'], wait_for=dict(text=text, log=log, change=change),
                                        matched=True, before=before['path'], after=last['path']))
                return last
        self.events.append(dict(at=last['at'], wait_for=dict(text=text, log=log, change=change), matched=False))
        return last

    def navigate(self):
        deadline = time.monotonic() + self.recipe.get('navigation_timeout_s', 900)
        current = self.snapshot()
        unhandled, previous_text = 0, None
        camera_failures = 0
        while self.game.alive() and time.monotonic() < deadline:
            completed = {m['name'] for m in self.milestones}
            required = set(self.driving.REQUIRED_MILESTONES.get(self.recipe['game'], {}))
            if current['state'] and required <= completed:
                # Loading a campaign save is not the end of a cinematic. Do not
                # start the two-minute interval until the camera answers both ways.
                ready = True
                for kind, sign in (('camera-left', 1), ('camera-right', -1)):
                    try:
                        after = self.trial(kind, self.recipe['controls'][kind])
                    except RuntimeError:
                        current = self.snapshot()
                        ready = False
                        break
                    trial = self.trials[-1]
                    shift, confidence = horizontal_motion(trial['before'], trial['after'], self.recipe['game'])
                    if shift * sign < 1 or confidence < 1.08:
                        ready = False
                    current = after
                if ready:
                    return current
                camera_failures += 1
                if camera_failures >= 10:
                    raise RuntimeError('gameplay prerequisites seen but camera did not respond to ten paired probes')
            joined = ' '.join(w['text'] for w in current['words'])
            rule = next((r for r in self.recipe['navigation'] if re.search(r['text'], joined, re.I)), None)
            if rule:
                for cmd in rule['commands']:
                    self.command(cmd)
                if rule.get('mark_on_seen'):
                    self.milestones.append(dict(name=rule['mark'],before=current['path'],after=current['path'],commands=rule['commands']))
                    self.save()
                unhandled = 0
            elif joined and not current['state']:
                unhandled = unhandled + 1 if joined == previous_text else 0
                if unhandled >= 12:
                    raise RuntimeError('unhandled stable navigation text: ' + joined[:240])
            previous_text = joined
            before = current
            current = self.wait_for(current, timeout=12, change=6, navigation=True, not_text=rule['text'] if rule and rule.get('mark') and not rule.get('mark_on_seen') else None)
            if rule and rule.get('mark') and not rule.get('mark_on_seen') and not re.search(rule['text'],' '.join(w['text'] for w in current['words']),re.I):
                self.milestones.append(dict(name=rule['mark'],before=before['path'],after=current['path'],commands=rule['commands']))
                self.save()
            if not rule and before in self.observations[2:]:
                needed = {p for t in self.trials for p in (t['before'],t['after'])}
                needed.update(p for m in self.milestones for p in (m['before'],m['after']))
                if before['path'] not in needed:
                    self.discard(before)
                    self.save()
        raise RuntimeError('navigation timed out without a verified gameplay state')

    def trial(self, kind, command):
        before = self.snapshot()
        self.command(command)
        sign = (1 if kind == 'camera-left' else -1) if kind.startswith('camera-') else None
        after = self.wait_for(before, timeout=2 if sign is not None else 8, change=4, motion_sign=sign)
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
                movement = self.recipe['controls'].get('movement-alternate',self.recipe['controls']['movement']) if cycles % 2 else self.recipe['controls']['movement']
                last = self.trial('movement', movement)
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
