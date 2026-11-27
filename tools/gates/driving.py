"""Fail-closed gameplay proof, independent of process survival and menu animation."""
import glob
import json
import math
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
# Every gate game needs a rule here; a game without one can never PASS drive (fail closed).
RACERS = ('nfsmw', 'nfsmw2012')          # speed unit in the lower-right HUD
SHOOTERS = ('bo2', 'bo1')
# Third-person games with no speed HUD: a log line only the running campaign writes, plus movement between two clean frames.
LOG_MARKER = {'gears': re.compile(r'default_checkpoint\.sav')}
# ac1: on-screen prompt text of the Animus-lab/first-level play state, both frames.
TEXT_MARKER = {'ac1': re.compile(r'press any button to interact|hidden blade|notoriety|synchroni[sz]', re.I)}
GAMES = RACERS + SHOOTERS + ('fh1', 'nfshp') + tuple(LOG_MARKER) + tuple(TEXT_MARKER)
MENU = re.compile(r'safe\s*house|resume free roam|select car|career menu|pause menu|leave.*safe|main menu|photo mode|sponsor challenges|resume game|restart level|quit game', re.I)
NEW_GAMES = ('gears', 'ac1', 'nfsmw2012', 'halo3', 'fallout3', 'me2')
GAMES += ('halo3', 'fallout3', 'me2')
FRONTEND = re.compile(r'press.*start|main menu|new game|new campaign|\bgene\b|choose sex|S\.?P\.?E\.?C\.?I\.?A\.?L\.?|strength.*perception|select difficulty|achievements|resume game|memory paused|options', re.I)
REQUIRED_MILESTONES = {
    'gears': {'prison-controls': r'to follow dominic'},
    'me2': {'dialogue-first': r'no time to argue|gave you an order',
            'dialogue-second': r'they.*be here|they damn well'},
    'fallout3': {'nursery': r'one year later|1 year later'},
}
CONVERSATION = re.compile(r'no time to argue|gave you an order|they.*be here|they damn well', re.I)
CAMPAIGN = {
    'gears': re.compile(r'default_checkpoint\.sav', re.I),
    'ac1': re.compile(r'assassin\.(?:sav|dat)', re.I),
    'halo3': re.compile(r'010_jungle', re.I),
    'fallout3': re.compile(r'XamShowKeyboardUI.*answered', re.I),
    'me2': re.compile(r'BIO[ADP]_[^\r\n]*Pro(?:Cer|Nor|L)|BIOGame[^\r\n]*Movies[^\r\n]*Opening_Seq', re.I),
}


def play_state(game, words, log):
    """State evidence is necessary but never sufficient: drive also needs interventions."""
    text = ' '.join(w['text'] for w in words)
    if FRONTEND.search(text) or MENU.search(text):
        return False
    if game == 'nfsmw2012':
        unit = any(re.fullmatch(r'MPH[I1|]?', w['text'].translate(str.maketrans('МРН','MPH')), re.I)
                   and w['x'] > .85 and w['y'] > .8 for w in words)
        map_hud = any(re.search(r'AIRHAV',w['text'],re.I) and w['x'] < .2 and w['y'] > .6 for w in words)
        distance_hud = any(re.fullmatch(r'DISTANCE',w['text'],re.I) and w['x'] < .2 and w['y'] < .2 for w in words)
        return unit or (map_hud and distance_hud)
    if game == 'ac1':
        return bool(re.search(r'first person|vision|high profile|synchroni[sz]|interact|camera|tackle|gentle push|blend|sprint|jump|grab and throw|throw|walk|push|\brun\b', text, re.I))
    if game == 'me2':
        choices = [w for w in words if w['x'] > .5 and w['y'] > .78 and re.search(r'[A-Za-z]{3}',w['text'])]
        if CONVERSATION.search(text) or (len(choices) >= 2 and max(w['y'] for w in choices)-min(w['y'] for w in choices) > .025):
            return False
    if game == 'halo3' and re.search(r'look up|look down', text, re.I):
        return False
    if game == 'halo3' and any(w['x'] > .7 and w['y'] < .2 and re.search(r'\d{2,3}', w['text']) for w in words):
        return True
    return bool(game in CAMPAIGN and CAMPAIGN[game].search(log))


def verify_play(game, out, log):
    """Reject historical alive/cinematic-only runs, even if a save was opened earlier."""
    proof = dict(game=game, passed=False, errors=[], reason='missing condition-driven gameplay evidence')
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location('gate_condition_routes', os.path.join(HERE, 'condition_routes.py'))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with open(os.path.join(out, 'play.json')) as f:
            run = json.load(f)
        if run.get('version') != 1 or run.get('game') != game or not run.get('complete'):
            raise ValueError('conditional route did not complete')
        start, end = run['play_started'], run['play_ended']
        if end - start < 120:
            raise ValueError('less than 120 seconds of verified gameplay')
        observations = {s['path']: s for s in run['observations']}
        for name,pattern in REQUIRED_MILESTONES.get(game, {}).items():
            # Gears' movement tutorial is itself positive control-state evidence;
            # it need not disappear before the player can move.
            valid = game == 'gears' and any(s['at'] <= start and re.search(pattern,' '.join(w['text'] for w in s['words']),re.I)
                                            for s in run['observations'])
            for mark in run.get('milestones', []):
                if mark.get('name') != name:
                    continue
                before, after = observations[mark['before']], observations[mark['after']]
                before_text = ' '.join(w['text'] for w in before['words'])
                after_text = ' '.join(w['text'] for w in after['words'])
                if (before['at'] <= after['at'] <= start and re.search(pattern,before_text,re.I)
                        and (game == 'gears' or (before['at'] < after['at'] and not re.search(pattern,after_text,re.I)))):
                    valid = True
            if not valid:
                raise ValueError('missing completed gameplay prerequisite: '+name)
        active = [s for s in run['observations'] if start <= s['at'] <= end]
        ok = [bool(s['state'] and play_state(game, s['words'], log)) for s in active]
        # A lone OCR dropout between two gameplay frames (HUD digits flicker) is tolerated; two in a row or at an edge is not.
        ok = [o or (0 < i < len(ok)-1 and ok[i-1] and ok[i+1]) for i, o in enumerate(ok)]
        if len(active) < 3 or not all(ok):
            raise ValueError('menu/cutscene/unknown state in the gameplay interval')
        stamps = [s['at'] for s in active]
        if stamps != sorted(stamps) or any(b-a > 30 for a,b in zip(stamps, stamps[1:])):
            raise ValueError('gameplay observations have a gap greater than 30 seconds')
        commands = [e['command'] for e in run['events'] if 'command' in e and e.get('reply', '').startswith('ok')]
        if not any(c.startswith('record ') for c in commands) or run.get('record_error'):
            raise ValueError('live input recording failed')
        counts = dict(movement=0, action=0, **{'camera-left': 0, 'camera-right': 0})
        proof['trials'] = []
        for trial in run['trials']:
            a, b = observations[trial['before']], observations[trial['after']]
            if not (start <= a['at'] < b['at'] <= end) or trial['command'] not in commands:
                continue
            delta = scene_change(a['path'], b['path'])
            passed = delta is not None and delta >= 4
            kind = trial['kind']
            result = dict(kind=kind, delta=delta)
            if kind.startswith('camera-'):
                shift, confidence = module.horizontal_motion(a['path'], b['path'], game)
                result.update(shift=shift, confidence=confidence)
                passed = passed and confidence >= 1.08 and (shift >= 1 if kind == 'camera-left' else shift <= -1)
            if passed and kind in counts:
                counts[kind] += 1
            result['passed'] = bool(passed)
            proof['trials'].append(result)
        proof['counts'] = counts
        proof['duration_s'] = end-start
        proof['passed'] = (counts['movement'] >= 2 and counts['action'] >= 2
                           and counts['camera-left'] >= 2 and counts['camera-right'] >= 2)
        proof['reason'] = ('120s gameplay, movement/action and repeated camera reversals verified' if proof['passed']
                           else 'missing movement/action or input-correlated camera reversals')
    except (OSError, ValueError, KeyError, TypeError) as error:
        proof['errors'].append(str(error))
    with open(os.path.join(out, 'driving.json'), 'w') as f:
        json.dump(proof, f, indent=2)
    return proof


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
    if game in NEW_GAMES:
        return verify_play(game, out, log)
    proof = {'game': game, 'passed': False, 'samples': [], 'pairs': [], 'errors': []}
    if game not in GAMES:
        proof['reason'] = f'no gameplay rule for {game}: add one to tools/gates/driving.py'
        with open(os.path.join(out, 'driving.json'), 'w') as f:
            json.dump(proof, f, indent=2)
        return proof
    log_ok = game not in LOG_MARKER or bool(LOG_MARKER[game].search(log))
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
            if game in TEXT_MARKER:
                sample['hud_present'] = bool(TEXT_MARKER[game].search(' '.join(w['text'] for w in sample['words'])))
            if game in LOG_MARKER:
                sample['hud_present'] = sample['valid_scene'] and log_ok   # no HUD text to read: the log marker is the state
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
        state = bool(state) and (a['valid_scene'] and b['valid_scene'] if game in LOG_MARKER else True)
        if game == 'bo1':
            state = (bo1_loaded and a['valid_scene'] and b['valid_scene']
                     and a.get('bo1_hud', False) and b.get('bo1_hud', False))
        if game == 'nfshp' and a['race_progress'] and b['race_progress']:
            state = (a['race_progress']['miles'] - b['race_progress']['miles'] >= .05
                     and b['race_progress']['elapsed_s'] > a['race_progress']['elapsed_s'])
        proof['pairs'].append({'before': a['path'], 'after': b['path'], 'mean_delta': delta,
                               'gameplay_state': state, 'passed': bool(moving and state)})
    proof['passed'] = any(pair['passed'] for pair in proof['pairs'])
    proof['reason'] = ('gameplay and scene movement observed' if proof['passed'] else
                       'no verified gameplay movement' + ('' if log_ok else f' (log marker {LOG_MARKER[game].pattern} never written)'))
    with open(os.path.join(out, 'driving.json'), 'w') as f:
        json.dump(proof, f, indent=2)
    return proof
