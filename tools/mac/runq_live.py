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
            with open(reply) as f:
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


def live_shot(rec, dest=None, alive=None):
    """Capture of the guest's output (no window, no screen recording) as PNG; returns (path or None, reply)."""
    reply = live_request(rec, 'capture', 60, alive)
    m = re.match(r'ok (\S+\.ppm)', reply)
    if not m:
        return None, reply
    shots = os.path.join(rec['out'], 'shots')
    os.makedirs(shots, exist_ok=True)
    if not dest:
        n = len([x for x in os.listdir(shots) if x.endswith('.png')])
        dest = os.path.join(shots, f'{n:03d}-{time.strftime("%H%M%S")}.png')
    r = subprocess.run(['sips', '-s', 'format', 'png', m.group(1), '--out', dest], capture_output=True, text=True)
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
    if verb == 'shot':
        path, reply = live_shot(rec, os.path.abspath(rest[0]) if rest else None)
        print(path if path else reply or 'error no reply', flush=True)
        return 0 if path else 1
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


LIVE_HELP = """runq live: a test game that stays up while you change it (muted, minimized, counted as a running game)
  runq live start NAME --app APP [--route R] [--ff 2] [--ready-log TEXT] [--support-from DIR] [--patch LIB]
                  [--lifetime 2700] [--class proof] [--arg=--cvar=value]... [--env K=V]...   waits until ready
  runq live exec NAME COMMAND...     any control command: cvar, cmd, patch, slots, timescale, savepoint, press, stick,
                                     capture, state, ping ... (rex/system/live_control.h, metal_test_control.mm)
  runq live cvar NAME CVAR [VALUE]   read or set a hot-reload cvar
  runq live shot NAME [OUT.png]      window-less capture of the game's output (default <out>/shots/NNN-time.png)
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
