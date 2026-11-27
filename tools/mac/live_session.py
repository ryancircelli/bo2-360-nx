#!/usr/bin/env -S python3 -I
"""live_session.py: one long-lived test game that stays up while you change it (runs as a runq job).

Started by `runq live start`, never by hand: runq counts it as a running game, clones the app into the job folder,
mutes it and kills its process group at the job timeout. The game starts minimized and in Metal background mode (no
focus, synthetic input only, window-less captures), optionally drives a route to the scene (optionally fast-forwarded),
then waits for commands through the Metal test control folder (<out>/control, see
sdk/src/graphics/metal/port/rex/ui/metal/metal_test_control.mm and rex/system/live_control.h).

  live_session.py --app APP --out OUT [--route R] [--ready-log TEXT] [--ready-timeout S] [--ff X]
                  [--support-from DIR] [--patch LIB] [--lifetime S] [--arg --name=value]... [--env K=V]...

State goes to <out>/live.json (state: starting, ready, failed, stopped). The session ends on <out>/stop, when the game
exits, or after --lifetime seconds (default 2700 = 45 min); the game is closed through the control channel, then
SIGTERM/SIGKILL of its own pid only. Exit: 0 ended normally, 3 the game died before the stop, 2 setup error.
"""
import argparse, json, os, plistlib, re, shutil, signal, subprocess, sys, time, uuid

HERE = os.path.dirname(os.path.abspath(__file__))


def wjson(path, obj):
    tmp = f'{path}.{os.getpid()}.tmp'
    with open(tmp, 'w') as f:
        json.dump(obj, f, indent=1)
    os.replace(tmp, path)


def control_request(control, text, timeout=30.0, alive=None):
    """Writes a request into the control folder and returns the reply ('' on timeout)."""
    stem = uuid.uuid4().hex
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
        if alive is not None and not alive():
            break
        time.sleep(0.02)
    try:
        os.unlink(os.path.join(control, stem + '.req'))
    except OSError:
        pass
    return ''


def route_env(route):
    """The route's environment, the way boot_test.sh builds it for SDK pads."""
    env = {}
    for line in open(route):
        line = line.split('#', 1)[0]
        if '=' not in line:
            continue
        k, v = (x.strip() for x in line.split('=', 1))
        k = k.replace(' ', '')
        if not v or k.startswith('BO2ZM_TEST_BUTTONS'):
            continue
        if k.startswith(('BO2ZM_', 'AUDIO_', 'REX_')):
            env['REX_' + k[4:] if k.startswith('REX_') else 'REX_' + k] = v
    env['REX_HID_VIRTUAL_PAD_SCRIPT'] = os.path.abspath(route)
    return env


def resolve_route(route):
    if not route or os.path.isfile(route):
        return route
    for cand in (os.path.join(HERE, 'routes', route + '.route'), os.path.join(HERE, 'routes', route)):
        if os.path.isfile(cand):
            return cand
    sys.exit(f'live_session: no route file {route}')


def free_port_offset():
    """An offset whose game UDP ports (offset + 1000, + 1001) are free, the way boot_test.sh picks one."""
    import socket
    for offset in range(50000, 64000, 1000):
        ok = True
        for port in (offset + 1000, offset + 1001):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                try:
                    s.bind(('0.0.0.0', port))
                except OSError:
                    ok = False
        if ok:
            return offset
    return 50000


def latest_log(logs):
    try:
        names = [os.path.join(logs, n) for n in os.listdir(logs) if n.endswith('.log')]
    except OSError:
        return None
    return max(names, key=os.path.getmtime) if names else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--app', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--route')
    ap.add_argument('--ready-log', help='ready once the game log has this text (else: first frame/ping)')
    ap.add_argument('--ready-timeout', type=float, default=900)
    ap.add_argument('--ff', type=float, default=0, help='guest time scale until ready (2 is safe for routes)')
    ap.add_argument('--support-from', help='seed the support folder (saves, settings) from this folder')
    ap.add_argument('--patch', help='patch library loaded at start-up (patch_library cvar)')
    ap.add_argument('--lifetime', type=float, default=2700)
    ap.add_argument('--pads', type=int, default=0, help='virtual pads for live input (default: the route\'s, else 1)')
    ap.add_argument('--arg', action='append', default=[], help='extra game argument, e.g. --arg=--foo=1')
    ap.add_argument('--env', action='append', default=[])
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    control, logs, support = (os.path.join(out, n) for n in ('control', 'logs', 'support'))
    state_path = os.path.join(out, 'live.json')
    for d in (control, logs, os.path.join(out, 'shots'), os.path.join(out, 'patches'), os.path.join(out, 'savepoints')):
        os.makedirs(d, exist_ok=True)
    if a.support_from and not os.path.exists(os.path.join(support, '.seeded')):
        if os.path.exists(support):
            shutil.rmtree(support)
        subprocess.run(['cp', '-cR', a.support_from, support], check=True)   # APFS clone
        open(os.path.join(support, '.seeded'), 'w').close()
    os.makedirs(support, exist_ok=True)
    try:
        os.unlink(os.path.join(out, 'stop'))
    except OSError:
        pass

    app = a.app.rstrip('/')
    with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f:
        info = plistlib.load(f)
    launcher = os.path.join(app, 'Contents', 'MacOS', info['CFBundleExecutable'])
    prefix = info.get('RexEnvPrefix', 'BO2')
    route = resolve_route(a.route)

    env = dict(os.environ)
    env.update({
        'REX_AUDIO_MUTE': 'true', 'REX_WINDOW_START_MINIMIZED': 'true',   # owner rules: silent, never in the way
        'REX_HEADLESS': 'true',   # off-screen engine mode: no window, Dock icon or audio device (headless-test-mode)
        'REX_METAL_TEST_CONTROL_DIR': control, 'REX_METAL_TEST_BACKGROUND': '1',
        'SDL_HIDAPI_LIBUSB': '0', 'SDL_JOYSTICK_HIDAPI': '0',               # never open the owner's real devices
        'SDL_WINDOW_ACTIVATE_WHEN_SHOWN': '0', 'SDL_WINDOW_ACTIVATE_WHEN_RAISED': '0', 'SDL_HINT_MAC_BACKGROUND_APP': '1',
        f'{prefix}_SUPPORT_DIR': support, f'{prefix}_LOG_DIR': logs, f'{prefix}_NO_DIALOG': '1', 'BO2_NO_DIALOG': '1',
        # Legacy and metal360 launchers need the same private save/log roots.
        'METAL360_SUPPORT_DIR': support, 'METAL360_LOG_DIR': logs, 'METAL360_NO_DIALOG': '1',
        'REX_LOG_LEVEL': env.get('REX_LOG_LEVEL', 'info'),
    })
    env['MVK_CONFIG_FAST_MATH_ENABLED'] = '0'
    if not env.get('REX_NET_SYSTEM_PORT_OFFSET'):
        env['REX_NET_SYSTEM_PORT_OFFSET'] = str(free_port_offset())   # like boot_test.sh: never share UDP ports
    if route:
        env.update(route_env(route))
    if a.pads or not route:   # live press/stick need a virtual pad on every player they drive
        env['REX_HID_VIRTUAL_PADS'] = str(min(max(a.pads, 1), 4))
    if a.ff and a.ff != 1:
        env['REX_GUEST_TIME_SCALE'] = str(a.ff)
        env['REX_HID_VIRTUAL_PAD_GUEST_TIME'] = 'true'   # the route keeps its pace in guest time
    if a.patch:
        env['REX_PATCH_LIBRARY'] = os.path.abspath(a.patch)
    for kv in a.env:
        k, v = kv.split('=', 1)
        env[k] = v

    state = {'state': 'starting', 'app': app, 'out': out, 'control': control, 'route': route, 'ff': a.ff, 'pads': a.pads,
             'pid': None, 'started': time.time(), 'lifetime_s': a.lifetime, 'runq_job': env.get('RUNQ_JOB')}
    wjson(state_path, state)
    t0 = time.monotonic()
    stdout = open(os.path.join(out, 'stdout.txt'), 'ab')
    # live shader constants: `runq live consts` writes this file, the Metal backend re-reads it within a second
    os.makedirs(os.path.join(out, 'shader'), exist_ok=True)
    args = [f'--metal_shader_constant_overrides={os.path.join(out, "shader", "constants.txt")}'] + a.arg
    proc = subprocess.Popen([launcher] + args, env=env, stdin=subprocess.DEVNULL, stdout=stdout,
                            stderr=subprocess.STDOUT, cwd=out)
    state['pid'] = proc.pid
    wjson(state_path, state)
    stopping = False

    def on_term(_sig, _frm):
        nonlocal stopping
        stopping = True
    signal.signal(signal.SIGTERM, on_term)
    alive = lambda: proc.poll() is None

    def fail(why, code=3):
        state.update(state='failed', reason=why, ended=time.time())
        wjson(state_path, state)
        print(f'live_session: {why}', flush=True)
        stop_game()
        sys.exit(code)

    def stop_game():
        if proc.poll() is None:
            control_request(control, 'close', 5, alive)
            for _ in range(100):
                if proc.poll() is not None:
                    break
                time.sleep(0.1)
        for sig in (signal.SIGTERM, signal.SIGKILL):
            if proc.poll() is None:
                try:
                    proc.send_signal(sig)   # the launcher forwards it to the game it started
                except ProcessLookupError:
                    pass
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    pass

    # ready: the control channel answers, then the log marker (if any)
    while not (os.path.exists(os.path.join(control, 'pid.txt')) and control_request(control, 'ping', 2, alive)):
        if not alive():
            fail(f'game exited with {proc.returncode} before its control channel came up')
        if time.monotonic() - t0 > a.ready_timeout or stopping:
            fail('control channel never answered')
        time.sleep(0.5)
    if a.ready_log:
        while True:
            log = latest_log(logs)
            # skip the pad script's own "waiting for a log line with ..." echo of the same text
            if log and any(a.ready_log in line and 'waiting for a log line' not in line
                           for line in open(log, errors='replace')):
                break
            if not alive():
                fail(f'game exited with {proc.returncode} before "{a.ready_log}"')
            if time.monotonic() - t0 > a.ready_timeout or stopping:
                fail(f'"{a.ready_log}" not in the log after {a.ready_timeout:.0f} s')
            time.sleep(0.5)
    if a.ff and a.ff != 1:
        state['timescale_reply'] = control_request(control, 'timescale 1', 10, alive)   # iterate at real time
    state.update(state='ready', ready_s=round(time.monotonic() - t0, 1), log=latest_log(logs))
    wjson(state_path, state)
    print(f'live_session: ready after {state["ready_s"]} s (pid {proc.pid}, control {control})', flush=True)

    while alive() and not stopping and not os.path.exists(os.path.join(out, 'stop')):
        if time.monotonic() - t0 > a.lifetime:
            state['reason'] = f'lifetime {a.lifetime:.0f} s reached'
            break
        time.sleep(0.5)
    died = not alive() and not stopping and not os.path.exists(os.path.join(out, 'stop'))
    stop_game()
    state.update(state='stopped' if not died else 'failed', ended=time.time(), exit=proc.returncode,
                 reason=state.get('reason') or ('game exited on its own' if died else 'stop requested'))
    wjson(state_path, state)
    print(f'live_session: {state["reason"]}, game exit {proc.returncode}', flush=True)
    sys.exit(3 if died else 0)


if __name__ == '__main__':
    main()
