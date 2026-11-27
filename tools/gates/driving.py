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


def hud_present(words):
    if MENU.search(' '.join(w['text'] for w in words)):
        return False
    return any(re.search(r'\b(?:MPH|KM/?H)\b', w['text'], re.I)
               and w['x'] > .55 and w['y'] > .5 for w in words)


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


def forza_speed(words):
    """FH1's large three-digit speed, including leading zeroes, in the driving HUD."""
    if MENU.search(' '.join(w['text'] for w in words)):
        return None
    for word in words:
        if (.80 <= word['x'] <= .91 and .75 <= word['y'] <= .84
                and .04 <= word.get('w', 0) <= .1 and .03 <= word.get('h', 0) <= .08
                and re.fullmatch(r'\d{3}', word['text'])):
            speed = int(word['text'])
            if 5 <= speed <= 400:
                return speed
    return None


def race_progress(words):
    """Hot Pursuit's HUD has no speed unit: use its timer and distance remaining."""
    if MENU.search(' '.join(w['text'] for w in words)):
        return None
    distance = elapsed = None
    for word in words:
        if word['x'] < .7 or word['y'] > .3:
            continue
        m = re.fullmatch(r'(\d+(?:\.\d+)?)\s*Miles', word['text'], re.I)
        if m:
            distance = float(m[1])
        m = re.fullmatch(r'(\d+):(\d{2})[.:](\d{2})', word['text'])
        if m and int(m[2]) < 60:
            elapsed = int(m[1]) * 60 + int(m[2]) + int(m[3]) / 100
    return {'miles': distance, 'elapsed_s': elapsed} if distance is not None and elapsed is not None else None


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
    paths = sorted(glob.glob(os.path.join(out, 'shots', 'drive*.ppm')), key=os.path.getmtime)
    shooter = game in ('bo2', 'bo1')
    # a BO2 save point is only made in a match (gate save waits for it), so a load counts as in a match
    in_match = game == 'bo2' and ('[savepoint] restored' in log or any("the game's client is in a match" in line
                  and not re.search(r"waiting|timed out|wait_log", line, re.I) for line in log.splitlines()))
    bo1_loaded = bool(re.search(r'\[zone\] open .*zombie_theater\.ff', log, re.I))
    for path in paths:
        sample = {'path': path, 'speed': None, 'hud_present': False, 'race_progress': None}
        sample['valid_scene'] = False
        try:
            sample['words'] = recognize(path)
            sample['valid_scene'] = not bool(MENU.search(' '.join(w['text'] for w in sample['words'])))
            sample['bo1_hud'] = any(re.fullmatch(r'\d{1,3}\s*/\s*\d{1,3}', w['text'])
                                     and w['x'] > .65 and w['y'] > .7 for w in sample['words'])
            sample['speed'] = hud_speed(sample['words'])
            sample['hud_present'] = hud_present(sample['words'])
            if game == 'fh1':
                sample['speed'] = forza_speed(sample['words'])
                sample['hud_present'] = sample['speed'] is not None
            if game == 'nfshp':
                sample['race_progress'] = race_progress(sample['words'])
        except (ValueError, OSError, subprocess.TimeoutExpired) as error:
            proof['errors'].append(str(error))
        proof['samples'].append(sample)
    # OCR may miss the unit during a single frame. Compare consecutive positively
    # identified gameplay captures as well as immediate neighbors; never substitute
    # a menu or a loading capture for either endpoint.
    eligible = [s for s in proof['samples'] if s['hud_present'] or s.get('bo1_hud')
                or (game == 'nfshp' and s['race_progress'])]
    candidates = list(zip(proof['samples'], proof['samples'][1:]))
    candidates += list(zip(eligible, eligible[1:]))
    seen = set()
    for a, b in candidates:
        key = (a['path'], b['path'])
        if key in seen:
            continue
        seen.add(key)
        try:
            delta = scene_change(a['path'], b['path'])
        except (ValueError, OSError) as error:
            proof['errors'].append(str(error))
            continue
        moving = delta is not None and delta >= 8
        state = (in_match and a['valid_scene'] and b['valid_scene']) if shooter else a['hud_present'] and b['hud_present']
        if game == 'bo1':
            state = (bo1_loaded and a['valid_scene'] and b['valid_scene']
                     and a.get('bo1_hud', False) and b.get('bo1_hud', False))
        if game == 'nfshp' and a['race_progress'] and b['race_progress']:
            state = (a['race_progress']['miles'] - b['race_progress']['miles'] >= .05
                     and b['race_progress']['elapsed_s'] > a['race_progress']['elapsed_s'])
        proof['pairs'].append({'before': a['path'], 'after': b['path'], 'mean_delta': delta,
                               'gameplay_state': state, 'passed': bool(moving and state)})
    proof['passed'] = any(pair['passed'] for pair in proof['pairs'])
    proof['reason'] = 'gameplay and scene movement observed' if proof['passed'] else 'no verified gameplay movement'
    with open(os.path.join(out, 'driving.json'), 'w') as f:
        json.dump(proof, f, indent=2)
    return proof
