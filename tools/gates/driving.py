"""Fail-closed gameplay proof, independent of process survival and menu animation."""
import glob
import json
import math
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
MENU = re.compile(r'safe\s*house|resume free roam|select car|career menu|pause menu|leave.*safe|main menu|photo mode|sponsor challenges|resume game|restart level|quit game', re.I)


def recognize(path):
    result = subprocess.run(['osascript', '-l', 'JavaScript', os.path.join(HERE, 'recognize_text.js'), path],
                            capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise ValueError(result.stderr.strip())
    return json.loads(result.stdout)


def hud_speed(words):
    """Only accept a speed reading beside a lower-right HUD unit, never a car specification."""
    if MENU.search(' '.join(w['text'] for w in words)):
        return None
    units = [w for w in words if re.search(r'\b(?:MPH|KM/?H)\b', w['text'], re.I)
             and w['x'] > .55 and w['y'] > .5]
    for unit in units:
        for word in words:
            # Some OCR results combine the number and unit into one observation.
            number = re.fullmatch(r'\s*(\d{1,3})(?:\s*(?:MPH|KM/?H))?\s*', word['text'], re.I)
            if number and abs(word['x'] - unit['x']) < .18 and abs(word['y'] - unit['y']) < .15:
                speed = int(number[1])
                if 5 <= speed <= 400:
                    return speed
    return None


def scene_pixels(path):
    """Read the gate's P6 capture and sample the center world, excluding the HUD edges."""
    with open(path, 'rb') as f:
        magic = f.readline().strip()
        dimensions = f.readline()
        while dimensions.startswith(b'#'):
            dimensions = f.readline()
        width, height = map(int, dimensions.split())
        maximum = int(f.readline())
        data = f.read()
    if magic != b'P6' or maximum != 255 or len(data) != width * height * 3:
        raise ValueError('unsupported or incomplete PPM capture')
    return [data[(int(height * y / 100) * width + int(width * x / 100)) * 3 + c]
            for y in range(25, 76, 2) for x in range(15, 86, 2) for c in range(3)]


def scene_change(before, after):
    a, b = scene_pixels(before), scene_pixels(after)
    if len(a) != len(b):
        return None
    # Both frames must contain scene detail; black loading transitions cannot pass.
    def spread(p):
        mean = sum(p) / len(p)
        return math.sqrt(sum((v - mean) ** 2 for v in p) / len(p))
    if min(spread(a), spread(b)) < 12:
        return None
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def verify(game, out, log):
    proof = {'game': game, 'passed': False, 'samples': [], 'pairs': [], 'errors': []}
    paths = sorted(glob.glob(os.path.join(out, 'shots', 'drive*.ppm')))
    shooter = game in ('bo2', 'bo1')
    # BO1 has no confirmed marker yet: unsupported state must fail until verified.
    in_match = game == 'bo2' and "the game's client is in a match" in log
    for path in paths:
        sample = {'path': path, 'speed': None}
        sample['valid_scene'] = False
        try:
            sample['words'] = recognize(path)
            sample['valid_scene'] = not bool(MENU.search(' '.join(w['text'] for w in sample['words'])))
            sample['speed'] = hud_speed(sample['words'])
        except (ValueError, OSError, subprocess.TimeoutExpired) as error:
            proof['errors'].append(str(error))
        proof['samples'].append(sample)
    for a, b in zip(proof['samples'], proof['samples'][1:]):
        try:
            delta = scene_change(a['path'], b['path'])
        except (ValueError, OSError) as error:
            proof['errors'].append(str(error))
            continue
        moving = delta is not None and delta >= 8
        state = (in_match and a['valid_scene'] and b['valid_scene']) if shooter else a['speed'] is not None and b['speed'] is not None
        proof['pairs'].append({'before': a['path'], 'after': b['path'], 'mean_delta': delta,
                               'gameplay_state': state, 'passed': bool(moving and state)})
    proof['passed'] = any(pair['passed'] for pair in proof['pairs'])
    proof['reason'] = 'gameplay and scene movement observed' if proof['passed'] else 'no verified gameplay movement'
    with open(os.path.join(out, 'driving.json'), 'w') as f:
        json.dump(proof, f, indent=2)
    return proof
