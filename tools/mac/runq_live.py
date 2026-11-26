"""runq live: the client side of live sessions (see LIVE_HELP below and live_session.py, the job that holds the game).

runq executes this file in its own namespace (`runq live ...`), so D, SPOOL, HERE, rjson,
wjson, find_done, cmd_wait and cmd_cancel come from runq; run directly, it loads the runq next to it first.
"""
# pylint: disable=undefined-variable
import os, re, shutil, subprocess, sys, time, json

if 'cmd_wait' not in globals():   # run directly (runq_live.py ...): load runq's helpers from the runq next to this file
    _runq = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'runq')
    _name, __name__ = __name__, 'runq_live_host'
    exec(compile(open(_runq).read(), _runq, 'exec'), globals())
    __name__, _STANDALONE = _name, True
# ------------------------------------------------------------------ live sessions (runq live ...)
LIVE_DIR = os.path.join(SPOOL, 'live')
LIVE_NAME = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')


def live_record(name):
    if not LIVE_NAME.match(name or ''):
        sys.exit(f'runq live: bad session name {name!r} (letters, digits, . _ -)')
    rec = rjson(os.path.join(LIVE_DIR, name + '.json'))
    if not rec:
        sys.exit(f'runq live: no session {name} (runq live list)')
    return rec


def live_state(rec):
    return rjson(os.path.join(rec['out'], 'live.json'), {}) or {}


def live_job_status(jid):
    # queue/ and running/ file names carry the job id (running/ also has <id>.run.json); match on the id
    for state in ('running', 'queue', 'remote'):
        try:
            if any(jid in n for n in os.listdir(D[state])):
                return 'queued' if state == 'queue' else 'running'
        except OSError:
            pass
    res = rjson(find_done(jid))
    return f'done exit {res.get("exit")}' if res else 'unknown'


def live_request(rec, text, timeout=30.0, alive=None):
    """One request through the session's control folder; returns the reply text ('' = no reply)."""
    control = os.path.join(rec['out'], 'control')
    if not os.path.isdir(control):
        sys.exit(f'runq live: {rec["name"]} has no control folder yet')
    stem = os.urandom(16).hex()
    tmp = os.path.join(control, stem + '.tmp')
    with open(tmp, 'w') as f:
        f.write(text + '\n')
    os.replace(tmp, os.path.join(control, stem + '.req'))
    reply = os.path.join(control, stem + '.reply')
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if os.path.exists(reply):
            with open(reply, errors='replace') as f:
                out = f.read().strip()
            os.unlink(reply)
            return out
        if not (alive() if alive else live_job_status(rec['job']) == 'running'):
            break
        time.sleep(0.02)
    try:
        os.unlink(os.path.join(control, stem + '.req'))
    except OSError:
        pass
    return ''


def live_print(reply, started=None):
    took = f'  ({time.monotonic() - started:.2f} s)' if started is not None else ''
    print((reply or 'error no reply (game busy, hung or gone)') + took, flush=True)
    return 0 if reply.startswith('ok') else 1


def live_capture(rec, alive=None):
    """Raw capture: (ppm path or None, reply)."""
    reply = live_request(rec, 'capture', 60, alive)
    m = re.match(r'ok (\S+\.ppm)', reply)
    return (m.group(1) if m else None), reply
def read_ppm(path):
    """(width, height, rgb bytes) of a binary P6 file."""
    with open(path, 'rb') as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:   # magic, width, height, maxval; '#' comments skipped
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos)
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    assert parts[0] == b'P6' and parts[3] == b'255', parts
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]
def region_mean_diff(a, b, box):
    """Mean absolute channel difference (0..255) of two read_ppm images inside box (x, y, w, h as fractions)."""
    w, h = a[0], a[1]
    if (w, h) != (b[0], b[1]):
        return 255.0
    x0, y0 = int(box[0] * w), int(box[1] * h)
    x1, y1 = max(x0 + 1, int((box[0] + box[2]) * w)), max(y0 + 1, int((box[1] + box[3]) * h))
    step = max(1, (x1 - x0) * (y1 - y0) // 40000)   # sample at most ~40k pixels: fast in pure Python
    total = n = 0
    for y in range(y0, y1, max(1, int(step ** 0.5))):
        row = y * w * 3
        for x in range(x0, x1, max(1, int(step ** 0.5))):
            i = row + x * 3
            total += abs(a[2][i] - b[2][i]) + abs(a[2][i + 1] - b[2][i + 1]) + abs(a[2][i + 2] - b[2][i + 2])
            n += 3
    return total / max(n, 1)
def live_shot(rec, dest=None, alive=None, size=0, ppm=None, reply=None):
    """Capture of the guest's output (no window, no screen recording) as PNG; returns (path or None, reply).
    size > 0 shrinks the longest side to that many pixels in the same conversion (for reading the shot)."""
    if ppm is None:
        ppm, reply = live_capture(rec, alive)
    m = re.match(r'(\S+\.ppm)', ppm or '')
    if not m:
        return None, reply
    shots = os.path.join(rec['out'], 'shots')
    os.makedirs(shots, exist_ok=True)
    if not dest:
        n = len([x for x in os.listdir(shots) if x.endswith('.png')])
        dest = os.path.join(shots, f'{n:03d}-{time.strftime("%H%M%S")}.png')
    r = subprocess.run(['sips', '-s', 'format', 'png'] + (['-Z', str(size)] if size else []) + [m.group(1), '--out', dest],
                       capture_output=True, text=True)
    if r.returncode:
        return None, f'error sips: {r.stderr.strip()}'
    try:
        os.unlink(m.group(1))
    except OSError:
        pass
    return dest, reply


def cmd_live(argv):
    import argparse
    os.makedirs(LIVE_DIR, exist_ok=True)
    verb = argv[0] if argv else 'help'
    if verb in ('help', '-h', '--help'):
        print(LIVE_HELP)
        return 0
    if verb == 'start':
        ap = argparse.ArgumentParser(prog='runq live start')
        ap.add_argument('name')
        ap.add_argument('--app', required=True)
        ap.add_argument('--route')
        ap.add_argument('--ready-log')
        ap.add_argument('--ready-timeout', type=float, default=900)
        ap.add_argument('--ff', type=float, default=0)
        ap.add_argument('--support-from')
        ap.add_argument('--patch')
        ap.add_argument('--lifetime', type=float, default=2700)
        ap.add_argument('--pads', type=int, default=0, help='virtual pads for live input (default: the route\'s, else 1)')
        ap.add_argument('--class', dest='klass', default='proof')
        ap.add_argument('--arg', action='append', default=[])
        ap.add_argument('--env', action='append', default=[])
        ap.add_argument('--no-wait', action='store_true', help='return once queued, do not wait for ready')
        a = ap.parse_args(argv[1:])
        old = rjson(os.path.join(LIVE_DIR, a.name + '.json')) if LIVE_NAME.match(a.name) else None
        if old and live_job_status(old['job']) in ('running', 'queued'):
            sys.exit(f'runq live: {a.name} is already {live_job_status(old["job"])} (job {old["job"]}); stop it first')
        live_record_name = a.name
        if not LIVE_NAME.match(live_record_name):
            sys.exit(f'runq live: bad session name {a.name!r}')
        cmd = [sys.executable, '-I', os.path.join(HERE, 'live_session.py'), '--app', '{APP}', '--out', '{OUT}',
               '--ready-timeout', str(a.ready_timeout), '--lifetime', str(a.lifetime)]
        for opt in ('route', 'ready_log', 'support_from', 'patch'):
            v = getattr(a, opt)
            if v:
                cmd += ['--' + opt.replace('_', '-'), os.path.abspath(v) if opt in ('support_from', 'patch') or
                        (opt == 'route' and os.path.exists(v)) else v]
        if a.ff:
            cmd += ['--ff', str(a.ff)]
        if a.pads:
            cmd += ['--pads', str(a.pads)]
        cmd += [f'--arg={x}' for x in a.arg] + [f'--env={x}' for x in a.env]
        sub = [sys.executable, '-I', os.path.join(HERE, 'runq'), 'submit', '--app', a.app, '--name', 'live-' + a.name,
               '--class', a.klass, '--timeout', str(int(a.lifetime + a.ready_timeout + 120)), '--'] + cmd
        r = subprocess.run(sub, capture_output=True, text=True)
        m = re.search(r'job (\S+) out (\S+)', r.stdout)
        if r.returncode or not m:
            sys.exit(f'runq live: submit failed: {r.stdout}{r.stderr}')
        rec = {'name': a.name, 'job': m.group(1), 'out': m.group(2), 'app': a.app, 'created': time.time()}
        wjson(os.path.join(LIVE_DIR, a.name + '.json'), rec)
        print(f'live {a.name}: job {rec["job"]} out {rec["out"]}', flush=True)
        if a.no_wait:
            return 0
        t0 = time.monotonic()
        while True:
            st = live_state(rec)
            status = live_job_status(rec['job'])
            if st.get('state') == 'ready':
                print(f'live {a.name}: ready (game up {st.get("ready_s")} s, {time.monotonic() - t0:.0f} s incl. queue)',
                      flush=True)
                return 0
            if st.get('state') == 'failed' or status not in ('running', 'queued'):
                print(f'live {a.name}: failed: {st.get("reason") or status} (see {rec["out"]}/runq.log)', flush=True)
                return 1
            time.sleep(1)
    if verb == 'selftest':
        return live_selftest()
    if verb == 'list':
        for f in sorted(os.listdir(LIVE_DIR)):
            if f.endswith('.json'):
                rec = rjson(os.path.join(LIVE_DIR, f))
                if rec:
                    st = live_state(rec)
                    print(f'{rec["name"]:20} {live_job_status(rec["job"]):12} {st.get("state", "-"):9} job {rec["job"]} '
                          f'out {rec["out"]}')
        return 0
    if len(argv) < 2:
        sys.exit(LIVE_HELP)
    rec = live_record(argv[1])
    rest = argv[2:]
    t0 = time.monotonic()
    if verb == 'status':
        print(json.dumps({**rec, 'job_status': live_job_status(rec['job']), **live_state(rec)}, indent=1))
        return 0
    if verb == 'exec':
        if not rest:
            sys.exit('usage: runq live exec NAME COMMAND...')
        return live_print(live_request(rec, ' '.join(rest), 60), t0)
    if verb == 'cvar':
        return live_print(live_request(rec, 'cvar ' + ' '.join(rest)), t0)
    if verb in ('press', 'hold', 'release', 'stick', 'trigger', 'pad'):   # same as exec NAME press ...
        return live_print(live_request(rec, ' '.join([verb] + rest)), t0)
    if verb == 'shot':
        ap = argparse.ArgumentParser(prog='runq live shot')
        ap.add_argument('out', nargs='?')
        ap.add_argument('--size', type=int, default=0, help='longest side in pixels (e.g. 800 for reading it)')
        a = ap.parse_args(rest)
        path, reply = live_shot(rec, os.path.abspath(a.out) if a.out else None, size=a.size)
        print(f'{path}  ({time.monotonic() - t0:.2f} s)' if path else reply or 'error no reply', flush=True)
        return 0 if path else 1
    if verb == 'wait-for':
        return live_wait_for(rec, rest, t0)
    if verb == 'record':
        return live_record_route(rec, rest, t0)
    if verb == 'swap-patch':
        if not rest:
            sys.exit('usage: runq live swap-patch NAME LIB.dylib [--shot] | off')
        if rest[0] == 'off':
            return live_print(live_request(rec, 'patch off'), t0)
        lib = os.path.abspath(rest[0])
        patches = os.path.join(rec['out'], 'patches')
        os.makedirs(patches, exist_ok=True)
        # A fresh name per swap: never overwrite a library the game has mapped (that kills it).
        dst = os.path.join(patches, f'v{len(os.listdir(patches)) + 1:03d}-{os.path.basename(lib)}')
        shutil.copy2(lib, dst)
        rc = live_print(live_request(rec, 'patch ' + dst), t0)
        if rc == 0 and '--shot' in rest:
            path, reply = live_shot(rec)
            print(path if path else reply, flush=True)
        return rc
    if verb == 'consts':
        if not rest:
            sys.exit('usage: runq live consts NAME FILE|off [--shot]')
        path = os.path.join(rec['out'], 'shader', 'constants.txt')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if rest[0] == 'off':
            try:
                os.unlink(path)
            except OSError:
                pass
        else:
            tmp = path + '.tmp'
            shutil.copyfile(rest[0], tmp)
            os.replace(tmp, path)   # a new mtime: the game re-reads the file within one second
        time.sleep(1.3)
        print(f'ok constants {"cleared" if rest[0] == "off" else "set from " + rest[0]} ({time.monotonic() - t0:.1f} s)',
              flush=True)
        if '--shot' in rest:
            shot, reply = live_shot(rec)
            print(shot if shot else reply, flush=True)
            return 0 if shot else 1
        return 0
    if verb in ('save', 'load'):
        if not rest or not LIVE_NAME.match(rest[0]):
            sys.exit(f'usage: runq live {verb} NAME SAVEPOINT')
        path = os.path.join(rec['out'], 'savepoints', rest[0] + '.rexsp')
        return live_print(live_request(rec, f'savepoint {verb} {path}'), t0)
    if verb == 'stop':
        open(os.path.join(rec['out'], 'stop'), 'w').close()
        if live_job_status(rec['job']) == 'queued':
            cmd_cancel([rec['job']])
        rc = cmd_wait([rec['job']])
        return rc
    sys.exit(LIVE_HELP)


def live_log_path(rec):
    logs = os.path.join(rec['out'], 'logs')
    names = [os.path.join(logs, n) for n in os.listdir(logs) if n.endswith('.log')] if os.path.isdir(logs) else []
    return max(names, key=os.path.getmtime) if names else None
def live_wait_for(rec, argv, t0):
    """wait-for NAME --log TEXT | --change X,Y,W,H [--threshold T] [--timeout S] [--shot OUT] [--size N]:
    returns as soon as the game logs TEXT (a line written after the call), or the region (fractions of the frame)
    differs from how it looked at the call by more than T (mean 0..255 per channel, default 6)."""
    import argparse
    ap = argparse.ArgumentParser(prog='runq live wait-for')
    ap.add_argument('--log')
    ap.add_argument('--change')
    ap.add_argument('--threshold', type=float, default=6.0)
    ap.add_argument('--timeout', type=float, default=30.0)
    ap.add_argument('--shot')
    ap.add_argument('--size', type=int, default=0)
    a = ap.parse_args(argv)
    if bool(a.log) == bool(a.change):
        sys.exit('usage: runq live wait-for NAME --log TEXT | --change X,Y,W,H [--threshold T] [--timeout S] [--shot OUT]')
    end = t0 + a.timeout
    got = None
    if a.log:
        path = live_log_path(rec)
        f = open(path, 'rb') if path else None
        if f:
            f.seek(0, 2)
        needle, tail = a.log.encode(), b''
        while time.monotonic() < end and got is None:
            if f is None:
                path = live_log_path(rec)
                f = open(path, 'rb') if path else None
            chunk = f.read() if f else b''
            for line in (tail + chunk).split(b'\n')[:-1]:
                if needle in line:
                    got = 'log ' + line.decode(errors='replace').strip()[-200:]
                    break
            tail = (tail + chunk).rsplit(b'\n', 1)[-1]
            if got is None:
                if live_job_status(rec['job']) != 'running':
                    break
                time.sleep(0.05)
        ppm = reply = None
    else:
        box = [float(v) for v in a.change.split(',')]
        if len(box) != 4:
            sys.exit('wait-for --change wants X,Y,W,H as fractions of the frame, e.g. 0,0,1,1')
        ppm, reply = live_capture(rec)
        if not ppm:
            return live_print(reply, t0)
        first = read_ppm(ppm)
        os.unlink(ppm)
        ppm = None
        diff = 0.0
        while time.monotonic() < end:
            ppm, reply = live_capture(rec)
            if not ppm:
                return live_print(reply, t0)
            diff = region_mean_diff(first, read_ppm(ppm), box)
            if diff > a.threshold:
                got = f'change {diff:.1f}'
                break
            os.unlink(ppm)
            ppm = None
        if got is None:
            reply = f'last diff {diff:.1f}'
    took = time.monotonic() - t0
    if got is None:
        if ppm:
            os.unlink(ppm)
        print(f'error timeout after {took:.1f} s' + (f' ({reply})' if reply else ''), flush=True)
        return 1
    print(f'ok {got}  ({took:.2f} s)', flush=True)
    if a.shot:
        path, r = live_shot(rec, os.path.abspath(a.shot), size=a.size, ppm=ppm, reply=reply)
        print(path or r, flush=True)
    elif ppm:
        os.unlink(ppm)
    return 0
def live_record_route(rec, argv, t0, alive=None):
    """record NAME OUT.route: every pad input of the session so far (route + live) as a replayable route.
    The route keeps the session route's settings, drops its pad scripts (they are in the recording) and replays
    the samples at the time they happened; start it the same way (same --ff)."""
    if len(argv) != 1 or not argv[0].endswith('.route'):
        sys.exit('usage: runq live record NAME OUT.route')
    route_out = os.path.abspath(argv[0])
    samples = route_out[:-len('.route')] + '.input.txt'
    reply = live_request(rec, 'record ' + samples, 30, alive)
    m = re.match(r'ok recorded (\d+) samples .* first_ms (\d+)', reply)
    if not m:
        return live_print(reply, t0)
    st = live_state(rec)
    lines = []
    if st.get('route'):
        pad_key = re.compile(r'^\s*(BO2ZM_TEST_BUTTONS\w*|pads?\d?|p\d)\s*=', re.I)
        lines = ['# ' + l if pad_key.match(l) else l for l in open(st['route']).read().splitlines()]
    lines += [f'# recorded from live session {rec["name"]} ({m.group(1)} pad samples)',
              f'REX_HID_REPLAY_INPUT = {samples}', f'REX_HID_REPLAY_DELAY = {int(m.group(2)) / 1000:.3f}']
    if st.get('ff') and st['ff'] != 1:
        lines.append('REX_HID_VIRTUAL_PAD_GUEST_TIME = true   # recorded on the guest clock: start with the same --ff')
    with open(route_out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'ok {route_out} ({m.group(1)} samples in {samples})  ({time.monotonic() - t0:.2f} s)', flush=True)
    return 0
LIVE_HELP = """runq live: a test game that stays up while you change it (muted, minimized, counted as a running game)
  runq live start NAME --app APP [--route R] [--ff 2] [--ready-log TEXT] [--support-from DIR] [--patch LIB]
                  [--lifetime 2700] [--class proof] [--pads N] [--arg=--cvar=value]... [--env K=V]...   waits until ready
  runq live exec NAME COMMAND...     any control command: cvar, cmd, patch, slots, timescale, savepoint, press, stick,
                                     capture, state, ping ... (rex/system/live_control.h, metal_test_control.mm)
  runq live cvar NAME CVAR [VALUE]   read or set a hot-reload cvar
  runq live shot NAME [OUT.png] [--size 800]   window-less capture of the game's output (default <out>/shots/)
  runq live press NAME a|b|x|y|start|back|up|down|left|right|lb|rb|lt|rt|ls|rs[+...] [ms] [p2]   queued tap
  runq live hold|release NAME BUTTONS|all [p2]   runq live stick NAME l|r X Y [ms] [p2]   (-1..1, up = +Y; off)
  runq live trigger NAME l|r V [ms] [p2]         runq live pad NAME [p2]   (what live input holds now)
  runq live wait-for NAME --log TEXT | --change X,Y,W,H [--threshold 6] [--timeout 30] [--shot OUT --size 800]
  runq live record NAME OUT.route    the session's pad input so far as a replayable route (start it with --route)
  runq live swap-patch NAME LIB [--shot] | off   load a patch library (bound at the next frame), optional capture
  runq live consts NAME FILE|off     live shader constants (lines `ps|vs HASH REG COMPONENT VALUE`), applied within 1 s
  runq live save NAME SP | load NAME SP          save points (load: not available yet)
  runq live status NAME | list | stop NAME"""


def live_selftest():
    """runq live selftest: the client against a fake control channel (no game, no daemon)."""
    import tempfile, threading, importlib.util
    tmp = tempfile.mkdtemp(prefix='runq-live-test-')
    rec = {'name': 't', 'job': 'none', 'out': tmp}
    control = os.path.join(tmp, 'control')
    os.makedirs(control)
    ppm = os.path.join(tmp, 'frame.ppm')
    with open(ppm, 'wb') as f:
        f.write(b'P6\n2 2\n255\n' + bytes([255, 0, 0] * 4))
    seen, stop = [], threading.Event()

    def fake_game():   # answers like metal_test_control.mm: <stem>.req -> <stem>.reply, request file removed
        while not stop.is_set():
            for n in os.listdir(control):
                if n.endswith('.req'):
                    text = open(os.path.join(control, n)).read().strip()
                    seen.append(text)
                    reply = {'ping': 'ok ready', 'capture': f'ok {ppm}'}.get(text, 'ok ' + text)
                    if text.startswith('record '):
                        open(text[7:], 'w').write('# glitch-input v1\n')
                        reply = f'ok recorded 3 samples to {text[7:]} first_ms 2500'
                    if text.startswith('hang'):
                        continue
                    with open(os.path.join(control, n[:-4] + '.reply'), 'w') as f:
                        f.write(reply)
                    os.unlink(os.path.join(control, n))
            time.sleep(0.005)
    th = threading.Thread(target=fake_game, daemon=True)
    th.start()
    up = lambda: True
    try:
        assert live_request(rec, 'ping', 5, up) == 'ok ready'
        assert live_request(rec, 'cvar   foo 1', 5, up) == 'ok cvar   foo 1'
        assert live_request(rec, 'hang', 0.2, up) == ''           # no reply: empty, request withdrawn
        assert not [n for n in os.listdir(control) if n.endswith(('.req', '.tmp', '.reply'))], os.listdir(control)
        assert live_request(rec, 'ping', 5, lambda: False) in ('', 'ok ready')   # a dead game never blocks
        path, reply = live_shot(rec, None, up)
        assert path and path.endswith('.png') and os.path.getsize(path) > 0, (path, reply)
        assert not os.path.exists(ppm)                            # the raw capture is removed after conversion
        assert live_print('ok x') == 0 and live_print('error y') == 1 and live_print('') == 1
        img = lambda px: (2, 2, bytes(px))
        assert region_mean_diff(img([0] * 12), img([0] * 12), (0, 0, 1, 1)) == 0
        assert region_mean_diff(img([0] * 12), img([0] * 9 + [255] * 3), (0.5, 0.5, 0.5, 0.5)) == 255
        assert region_mean_diff(img([0] * 12), img([0] * 9 + [255] * 3), (0, 0, 0.5, 0.5)) == 0
        with open(ppm, 'wb') as f:
            f.write(b'P6\n# c\n2 2\n255\n' + bytes(range(12)))
        assert read_ppm(ppm) == (2, 2, bytes(range(12)))
        src_route = os.path.join(tmp, 'src.route')
        with open(src_route, 'w') as f:
            f.write('BO2ZM_MAP = town\nBO2ZM_TEST_BUTTONS = 5:a\npad2 = 6:b\nplayers = 2\n')
        with open(os.path.join(tmp, 'live.json'), 'w') as f:
            json.dump({'route': src_route, 'ff': 2}, f)
        out_route = os.path.join(tmp, 'rec.route')
        assert live_record_route(rec, [out_route], time.monotonic(), up) == 0
        text = open(out_route).read()
        assert '# BO2ZM_TEST_BUTTONS' in text and '# pad2' in text and 'players = 2' in text, text
        assert 'REX_HID_REPLAY_DELAY = 2.500' in text and 'REX_HID_VIRTUAL_PAD_GUEST_TIME = true' in text, text
        assert LIVE_NAME.match('bo2-town.1') and not LIVE_NAME.match('../x') and not LIVE_NAME.match('')
        spec = importlib.util.spec_from_file_location('live_session', os.path.join(HERE, 'live_session.py'))
        ls = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(ls)
        route = os.path.join(tmp, 't.route')
        with open(route, 'w') as f:
            f.write('duration_s = 60\nBO2ZM_TEST_BUTTONS = a 1\nBO2ZM_MAP = town # c\nREX_FOO = 2\nAUDIO_X = 1\n')
        env = ls.route_env(route)
        assert env == {'REX_BO2ZM_MAP': 'town', 'REX_FOO': '2', 'REX_AUDIO_X': '1',
                       'REX_HID_VIRTUAL_PAD_SCRIPT': route}, env
    finally:
        stop.set()
        th.join(1)
        shutil.rmtree(tmp, ignore_errors=True)
    print('runq live selftest: ok')
    return 0


if globals().get('_STANDALONE'):
    sys.exit(cmd_live(sys.argv[1:]))
