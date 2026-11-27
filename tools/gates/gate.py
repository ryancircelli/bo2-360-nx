#!/usr/bin/env python3
"""gate: one command per gate and per game. Prints ONE line: PASS|FAIL <gate> <game> <facts> | <evidence path>.

  gate.py boot    GAME [--app APP] [--secs 45]            boots, control channel answers, frames drawn, alive, screenshot
  gate.py shot    GAME [--route R] [--at S] [--ff 2]      boots through a route (fast-forwarded) to a scene, screenshots it
  gate.py drive   GAME [--route R]                        full scripted route, alive to its end, no crash, new log kinds
  gate.py guide   GAME                                    Guide opens and closes (log + XN_SYS_UI), screenshots with overlay
  gate.py open    GAME [--secs 90]                        `open -n` Finder-style launch of a scratch clone, fd count < 256
  gate.py set     [GAME ...]                              boot gate for bo2 nfsmw fh1 at once (3 queue slots)
  gate.py speed   GAME --a APP --b APP [--pairs 3]        A/B speed pair(s) through runq perf class + abverdict
  gate.py install GAME --app NEW --repo WORKTREE          base rule, codesign, restore copy, refuses while the owner plays
  gate.py save    GAME --scene NAME [--at S] [--ff 2]     drive route to a moment, write the save point SAVEPOINTS/GAME/NAME.sp
  boot|shot|drive ... --savepoint NAME|FILE               start from a save point instead of booting (seconds, not minutes)

GAME = bo2 | nfsmw | fh1 | nfshp | bo1 | gears.  APP defaults to the installed app in ~/Games/Launchers (it is cloned by runq, never
changed). Every gate but install/set/speed is queued as a runq job (class harness; --class to change, --now runs it
inline, for debugging only), so it is muted, minimized, on its own UDP ports and never opens a real pad. Output goes to
--out (default ~/Games/Xbox360/bo2-build/runs/gates/<game>-<gate>-<time>/). Exit 0 = PASS, 1 = FAIL, 2 = setup error.
Docs: tools/gates/README.md, WORKFLOW.md "Standard gates".
"""
import argparse, glob, hashlib, json, math, os, plistlib, re, shutil, signal, subprocess, sys, time, uuid
import importlib.util
_driving_spec = importlib.util.spec_from_file_location("gate_driving", os.path.join(os.path.dirname(os.path.abspath(__file__)), "driving.py"))
_driving = importlib.util.module_from_spec(_driving_spec)
_driving_spec.loader.exec_module(_driving)
verify_driving = _driving.verify

HOME = os.path.expanduser('~')
HERE = os.path.dirname(os.path.abspath(__file__))
LAUNCHERS = os.path.join(HOME, 'Games', 'Launchers')
SPOOL = os.path.join(HOME, 'Games', 'Xbox360', 'bo2-build', 'runq')
RUNQ = os.path.join(SPOOL, 'bin', 'runq')
RUNQ_ROUTES = os.path.join(SPOOL, 'bin', 'routes')
RUNS = os.path.join(HOME, 'Games', 'Xbox360', 'bo2-build', 'runs', 'gates')
SAVEPOINTS = os.path.join(HOME, 'Games', 'Xbox360', 'bo2-build', 'savepoints')

# app = installed app name; drive/guide routes are in tools/gates/routes (or the runq routes dir for bo2/bo1);
# scene_s = guest seconds the drive route needs to reach a scene worth a screenshot; settle_s = guest seconds
# at the front end before the Guide gate presses the Guide button (no route needed: the Guide is app-level).
GAMES = {
    'gears': dict(app='Gears of War.app', drive='gears-drive', scene_s=180, settle_s=60, seed='gears-360'),
    # save_log: `save` counts --at from this log line (boot time varies with machine load; a save on a loading screen is useless)
    'bo2':   dict(app='BO2 Zombies.app',                  drive='town4', scene_s=140, settle_s=60, seed=None,
                  save_log='[cheats] guest memory in a match', save_after_s=30),
    'nfsmw': dict(app='Need for Speed Most Wanted.app',   drive='nfsmw-drive', scene_s=200, settle_s=75, seed='nfsmw-360', load_pads='pad1 = 0-3600:rt'),
    'fh1':   dict(app='Forza Horizon.app',                drive='fh1-drive', scene_s=150, settle_s=75, seed='fh1-360', load_pads='pad1 = 0-3600:rt'),
    'nfshp': dict(app='Need for Speed Hot Pursuit.app',   drive='nfshp-drive', scene_s=480, settle_s=75, seed='nfshp-360'),
    'bo1':   dict(app='Black Ops.app',                    drive='bo1-drive', scene_s=700, settle_s=60, seed='bo1'),
    # No drive route yet: boot and install only.
    'nfsmw2012': dict(app='Need for Speed Most Wanted 2012.app', drive=None, scene_s=180, settle_s=75, seed='nfsmw2012-360'),
    'ac1':   dict(app="Assassin's Creed.app",             drive=None, scene_s=180, settle_s=75, seed='ac1-360'),
}
SET_GAMES = ['bo2', 'nfsmw', 'fh1']
ERR_KIND = re.compile(r'\] \[(warning|error|critical)\]')
CRASH = re.compile(r'(Guest access violation|unregistered function|MISMATCH|lost wake|Assertion failed|fatal error|Segmentation|EXC_BAD)')


# ----------------------------------------------------------------------------------------------- helpers
LOADED = ''   # set when the run started from a save point: every result line says so


def out_line(verdict, gate, game, facts, evidence, out=None):
    line = f'{verdict} {gate} {game} {facts}{LOADED} | {evidence}'
    if out:
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, 'gate.txt'), 'w') as f:
            f.write(line + '\n')
        with open(os.path.join(out, 'gate.json'), 'w') as f:
            json.dump(dict(verdict=verdict, gate=gate, game=game, facts=facts, evidence=evidence), f)
    print(line, flush=True)
    return 0 if verdict == 'PASS' else 1


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def installed_app(game):
    return os.path.join(LAUNCHERS, GAMES[game]['app'])


def resolve_route(game, route):
    """A route name or path -> a file. Bare names: tools/gates/routes, then the runq routes dir."""
    route = route or GAMES[game]['drive']
    if not route:
        return None
    if os.path.isfile(route):
        return os.path.abspath(route)
    for d in (os.path.join(HERE, 'routes'), RUNQ_ROUTES):
        for name in (route, route + '.route'):
            if os.path.isfile(os.path.join(d, name)):
                return os.path.join(d, name)
    sys.exit(f'gate: no route {route}')


def route_len(path):
    """Seconds a pad script runs: the largest time in `padN = a-b:button, ...` lines, or duration_s."""
    best = 0.0
    for line in open(path):
        line = line.split('#', 1)[0]
        m = re.match(r'\s*duration_s\s*=\s*(\d+)', line)
        if m:
            best = max(best, float(m.group(1)))
        if re.match(r'\s*pad\d', line):
            for t in re.findall(r'(\d+(?:\.\d+)?)(?:-(\d+(?:\.\d+)?))?:', line):
                best = max(best, float(t[1] or t[0]))
    return best


def route_env(route):
    """The route's environment, like boot_test.sh / live_session.py build it for SDK pads."""
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


def free_port_offset():
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


def image_stats(ppm):
    """(mean, spread) of a P6 file from a sparse sample: a black or flat capture is a failed scene."""
    try:
        data = open(ppm, 'rb').read()
        head = data.split(b'\n', 3)
        body = head[3]
    except (OSError, IndexError):
        return None
    s = body[::997]
    if not s:
        return None
    mean = sum(s) / len(s)
    return mean, math.sqrt(sum((x - mean) ** 2 for x in s) / len(s))


def to_png(ppm):
    png = ppm[:-4] + '.png'
    return png if sh(['sips', '-s', 'format', 'png', ppm, '--out', png]).returncode == 0 else ppm


def kinds_of(log):
    """Normalized warning/error kinds (numbers, paths, quoted text stripped), like the old kinds.sh."""
    out = set()
    try:
        for line in open(log, errors='replace'):
            if ERR_KIND.search(line):
                s = re.sub(r"^\[[^\]]*\] ", '', line.rstrip())
                s = re.sub(r'\[t\d+\] ', '', s)
                s = re.sub(r"'[^']*'", 'Q', s)
                s = re.sub(r'path=\S*', 'path=P', s)
                out.add(re.sub(r'\d+', 'N', s)[:70])
    except OSError:
        pass
    return out


# ----------------------------------------------------------------------------------------------- one live game
class Game:
    """A muted, minimized game with the Metal test control folder: ping, press, capture, state, close."""

    def __init__(self, app, out, game, route=None, ff=0, seed=True, env=None, via_open=False, args=()):
        self.app, self.out, self.game = app.rstrip('/'), os.path.abspath(out), game
        self.control, self.logs, self.support = (os.path.join(self.out, n) for n in ('control', 'logs', 'support'))
        for d in (self.control, self.logs, os.path.join(self.out, 'shots')):
            os.makedirs(d, exist_ok=True)
        plist = plistlib.load(open(os.path.join(self.app, 'Contents', 'Info.plist'), 'rb'))
        self.launcher = os.path.join(self.app, 'Contents', 'MacOS', plist['CFBundleExecutable'])
        prefix = plist.get('RexEnvPrefix', 'BO2')
        folder = plist.get('RexSupportFolder') or GAMES[game]['seed']
        src = os.path.join(HOME, 'Library', 'Application Support', folder) if folder else None
        if seed and src and os.path.isdir(src) and not os.path.exists(self.support):
            subprocess.run(['cp', '-cR', src, self.support], check=True)   # APFS clone of the owner profile; the owner's copy is never written
        os.makedirs(self.support, exist_ok=True)
        e = dict(os.environ)
        e.update({
            'REX_AUDIO_MUTE': 'true', 'REX_APU_MUTE': 'true', 'REX_WINDOW_START_MINIMIZED': 'true',
            'REX_METAL_TEST_CONTROL_DIR': self.control, 'REX_METAL_TEST_BACKGROUND': '1',
            'SDL_HIDAPI_LIBUSB': '0', 'SDL_JOYSTICK_HIDAPI': '0',   # never open the owner's real pads
            'SDL_WINDOW_ACTIVATE_WHEN_SHOWN': '0', 'SDL_WINDOW_ACTIVATE_WHEN_RAISED': '0', 'SDL_HINT_MAC_BACKGROUND_APP': '1',
            f'{prefix}_SUPPORT_DIR': self.support, f'{prefix}_LOG_DIR': self.logs, f'{prefix}_NO_DIALOG': '1', 'BO2_NO_DIALOG': '1',
            'METAL360_SUPPORT_DIR': self.support, 'METAL360_LOG_DIR': self.logs, 'METAL360_NO_DIALOG': '1',
            'REX_LOG_LEVEL': 'info', 'REX_INPUT_LOG_STATE': 'true', 'MVK_CONFIG_FAST_MATH_ENABLED': '0',
            'REX_NET_SYSTEM_PORT_OFFSET': os.environ.get('REX_NET_SYSTEM_PORT_OFFSET') or str(free_port_offset()),
        })
        if route:
            e.update(route_env(route))
        if ff and ff != 1:
            e['REX_GUEST_TIME_SCALE'] = str(ff)
            e['REX_HID_VIRTUAL_PAD_GUEST_TIME'] = 'true'   # the route keeps its pace in guest time
        # off-screen engine mode; `open` tests the real window; a job's own REX_HEADLESS=false (runq --window) wins
        e['REX_HEADLESS'] = 'false' if via_open else os.environ.get('REX_HEADLESS', 'true')
        e.update(env or {})
        self.env, self.t0 = e, time.monotonic()
        self.proc = None
        self.via_open = via_open
        if via_open:   # Finder-style launch: Launch Services limits (256 open files), env passed with --env
            oargs = ['open', '-n', '-g', '-j', '--stdout', os.path.join(self.out, 'stdout.txt'), '--stderr', os.path.join(self.out, 'stdout.txt')]
            for k, v in e.items():
                if k.startswith(('REX_', 'SDL_', 'BO2', 'MVK_')) or k.endswith(('_SUPPORT_DIR', '_LOG_DIR', '_NO_DIALOG')):
                    oargs += ['--env', f'{k}={v}']
            subprocess.run(oargs + [self.app, '--args'] + list(args), check=True)
            self.pid = self._find_pid(20)
        else:
            self.proc = subprocess.Popen([self.launcher] + list(args), env=e, stdin=subprocess.DEVNULL,
                                         stdout=open(os.path.join(self.out, 'stdout.txt'), 'ab'), stderr=subprocess.STDOUT, cwd=self.out)
            self.pid = self.proc.pid

    def _find_pid(self, wait):
        for _ in range(int(wait * 2)):
            r = sh(['pgrep', '-f', self.app + '/Contents/MacOS/'])
            pids = [int(p) for p in r.stdout.split()]
            if pids:
                return pids[-1]
            time.sleep(0.5)
        return None

    def alive(self):
        if self.proc:
            return self.proc.poll() is None
        return bool(self.pid) and sh(['kill', '-0', str(self.pid)]).returncode == 0

    def exit_code(self):
        return self.proc.returncode if self.proc else None

    def ctl(self, text, timeout=30):
        stem = uuid.uuid4().hex
        tmp = os.path.join(self.control, stem + '.tmp')
        open(tmp, 'w').write(text + '\n')
        os.replace(tmp, os.path.join(self.control, stem + '.req'))
        reply = os.path.join(self.control, stem + '.reply')
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if os.path.exists(reply):
                r = open(reply).read().strip()
                os.unlink(reply)
                return r
            if not self.alive():
                break
            time.sleep(0.05)
        try:
            os.unlink(os.path.join(self.control, stem + '.req'))
        except OSError:
            pass
        return ''

    def wait_ready(self, timeout):
        """True once the control channel answers a ping."""
        while time.monotonic() - self.t0 < timeout and self.alive():
            if os.path.exists(os.path.join(self.control, 'pid.txt')) and self.ctl('ping', 2).startswith('ok'):
                return True
            time.sleep(0.5)
        return False

    def log(self):
        names = [os.path.join(self.logs, n) for n in os.listdir(self.logs) if n.endswith('.log')]
        return max(names, key=os.path.getmtime) if names else None

    def log_text(self):
        p = self.log()
        return open(p, errors='replace').read() if p else ''

    def wait_log(self, needle, timeout, start=0):
        end = time.monotonic() + timeout
        while time.monotonic() < end and self.alive():
            if needle in self.log_text()[start:]:
                return True
            time.sleep(0.5)
        return needle in self.log_text()[start:]

    def shot(self, name, screen=False):
        """Window-less capture -> shots/NAME.png; returns (png path, (mean, spread)) or (None, None). screen = with the overlay."""
        r = self.ctl('capture screen' if screen else 'capture', 30)
        if not r.startswith('ok '):
            return None, None
        ppm = r[3:].strip()
        dst = os.path.join(self.out, 'shots', name + '.ppm')
        shutil.copy(ppm, dst)
        return to_png(dst), image_stats(dst)

    def frames(self):
        """Frames presented so far: present_submit rows in the game's frame csv (flushed every 500 ms), or None without a csv."""
        self.ctl('flush 1500', 5)
        csvs = glob.glob(os.path.join(self.logs, 'frames-*.csv'))
        if not csvs:
            return None
        return sum(1 for line in open(max(csvs, key=os.path.getmtime)) if line.startswith('present_submit'))

    def stop(self):
        if self.alive():
            self.ctl('close', 5)
            for _ in range(60):
                if not self.alive():
                    break
                time.sleep(0.2)
        for sig in (signal.SIGTERM, signal.SIGKILL):
            if self.alive() and self.pid:
                try:
                    os.kill(self.pid, sig)
                except ProcessLookupError:
                    pass
                for _ in range(40):
                    if not self.alive():
                        break
                    time.sleep(0.25)
        if self.proc:
            self.proc.poll()


def fd_count(pid):
    r = sh(['lsof', '-p', str(pid)])
    return len(r.stdout.splitlines()) if r.returncode in (0, 1) else 0


def baseline_diff(game_obj, baseline, out):
    """New warning/error kinds vs a baseline kinds.txt (from an earlier gate run); writes kinds.txt."""
    kinds = kinds_of(game_obj.log() or '')
    open(os.path.join(out, 'kinds.txt'), 'w').write('\n'.join(sorted(kinds)) + '\n')
    if baseline and os.path.exists(baseline):
        old = set(open(baseline).read().splitlines())
        return sorted(kinds - old)
    return None


# ----------------------------------------------------------------------------------------------- inner gates (run inside a runq job)
def g_boot(a, G):
    secs = a.secs
    if not G.wait_ready(a.ready_timeout):
        return fail_start(a, G, 'control channel never answered')
    t_ready = time.monotonic() - G.t0
    end = time.monotonic() + secs
    while time.monotonic() < end and G.alive():
        time.sleep(1)
    alive = G.alive()
    frames = G.frames() if alive else None
    png, st = G.shot('boot') if alive else (None, None)
    text = G.log_text()
    crash = CRASH.search(text)
    G.stop()
    new = baseline_diff(G, a.kinds_from, a.out)
    ok = alive and not crash and (frames is None or frames > 0) and (st is None or st[0] > 3 or st[1] > 3)
    facts = f'ready {t_ready:.0f}s alive {secs}s frames {frames} image {"mean %.0f sd %.0f" % st if st else "none"}'
    if crash:
        facts += f' LOG {crash.group(0)}'
    if new:
        facts += f' new-kinds {len(new)}'
    return out_line('PASS' if ok else 'FAIL', 'boot', a.game, facts, a.out, a.out)


def g_shot(a, G):
    if not G.wait_ready(a.ready_timeout):
        return fail_start(a, G, 'control channel never answered')
    at = a.at or GAMES[a.game]['scene_s']
    ff = a.ff or 1
    shots, ok_any = [], False
    t_end = time.monotonic() + at / ff
    while time.monotonic() < t_end and G.alive():
        time.sleep(1)
    if ff != 1:
        G.ctl('timescale 1', 10)
        time.sleep(3)
    for i in range(3):   # three looks 2 s apart: a single frame can be a fade or a loading flash
        png, st = G.shot(f'scene{i}') if G.alive() else (None, None)
        if png:
            shots.append((png, st))
            ok_any = ok_any or (st and (st[0] > 8 or st[1] > 8))
        time.sleep(2)
    alive = G.alive()
    crash = CRASH.search(G.log_text())
    G.stop()
    ok = alive and ok_any and not crash
    facts = f'scene at {at}s (ff {ff}) shots {len(shots)} ' + (f'mean {shots[-1][1][0]:.0f} sd {shots[-1][1][1]:.0f}' if shots and shots[-1][1] else 'none')
    if crash:
        facts += f' LOG {crash.group(0)}'
    return out_line('PASS' if ok else 'FAIL', 'shot', a.game, facts, os.path.join(a.out, 'shots'), a.out)


def g_drive(a, G):
    route = a.route_file
    length = route_len(route) if route else a.secs
    if not G.wait_ready(a.ready_timeout):
        return fail_start(a, G, 'control channel never answered')
    t_end = G.t0 + (length + 20) / (a.ff or 1) + 30
    nxt, n = time.monotonic() + 60, 0
    died = None
    burst_until = None
    while time.monotonic() < t_end:
        if not G.alive():
            died = G.exit_code()
            break
        now = time.monotonic()
        if burst_until is None and ((a.game == 'nfsmw' and 'INGAMEB.LZC' in G.log_text())
                                   or (a.game == 'bo1' and 'zombie_theater.ff' in G.log_text())):
            burst_until, nxt = now + 60, now
        if now >= nxt:
            G.shot(f'drive{n:02d}')
            n, nxt = n + 1, now + (2 if burst_until and now < burst_until else 60)
        time.sleep(1)
    alive = G.alive()
    if alive:
        G.shot('drive-end')
    text = G.log_text()
    crash = CRASH.search(text)
    G.stop()
    new = baseline_diff(G, a.kinds_from, a.out)
    proof = verify_driving(a.game, a.out, text)
    sd = os.path.join(G.out, 'shots')
    first, last = (os.path.join(sd, f) for f in ('drive00.png', 'drive-end.png'))
    # a drive that keeps a picture identical start to end has stopped (a fresh load can freeze a few frames in)
    frozen = alive and os.path.exists(first) and os.path.exists(last) and open(first, 'rb').read() == open(last, 'rb').read()
    # from a save point the scene was proven when it was saved; what the load must show is a live picture
    # (the driving proof needs input-driven movement, which a car parked at a wall or idle players lack)
    ok = alive and not crash and not frozen and (proof["passed"] or bool(a.savepoint))
    facts = f'gameplay {proof["passed"]} route {os.path.basename(route) if route else "-"} {length:.0f}s alive {alive} shots {n + 1}'
    if frozen:
        facts += ' FROZEN (first and last shots identical)'
    if died is not None:
        facts += f' exited {died}'
    if crash:
        facts += f' LOG {crash.group(0)}'
    if new is not None:
        facts += f' new-kinds {len(new)}'
    return out_line('PASS' if ok else 'FAIL', 'drive', a.game, facts, a.out, a.out)


def g_guide(a, G):
    if not G.wait_ready(a.ready_timeout):
        return fail_start(a, G, 'control channel never answered')
    G.shot('before', screen=True)
    mark = 0
    opened = G.wait_log('[guide] opened', a.ready_timeout / 4 + 60)
    time.sleep(1)
    G.shot('open', screen=True)
    time.sleep(3)
    G.shot('open-rb', screen=True)
    closed = G.wait_log('[guide] closed', 30)
    time.sleep(2)
    G.shot('after', screen=True)
    r1 = r2 = ''
    alive = G.alive()
    text = G.log_text()
    sysui = re.findall(r'XN_SYS_UI\s*=\s*(\d)', text[mark:])
    crash = CRASH.search(text)
    G.stop()
    ok = alive and opened and closed and not crash and (not sysui or (sysui[0] == '1' and sysui[-1] == '0'))
    facts = f'opened {opened} closed {closed} XN_SYS_UI {"".join(sysui) or "n/a"} alive {alive} press-replies {r1!r}/{r2!r}'
    if crash:
        facts += f' LOG {crash.group(0)}'
    return out_line('PASS' if ok else 'FAIL', 'guide', a.game, facts, os.path.join(a.out, 'shots'), a.out)


def g_open(a, G):
    # G was started with via_open: a Finder-style launch of the scratch clone (Launch Services open-file limit 256)
    if not G.pid:
        return out_line('FAIL', 'open', a.game, '`open -n` started no process', a.out, a.out)
    mx, end, ready = 0, time.monotonic() + a.secs, False
    while time.monotonic() < end and G.alive():
        mx = max(mx, fd_count(G.pid))
        ready = ready or (os.path.exists(os.path.join(G.control, 'pid.txt')) and G.ctl('ping', 2).startswith('ok'))
        time.sleep(3)
    alive = G.alive()
    frames = G.frames() if alive and ready else None
    G.stop()
    crash = CRASH.search(G.log_text())
    ok = alive and ready and mx < 256 and not crash and (frames is None or frames > 0)
    open(os.path.join(a.out, 'fd.txt'), 'w').write(f'pid={G.pid}\nmaxfd={mx}\n')
    facts = f'ready {ready} alive {alive} max-fds {mx} (<256) frames {frames}'
    if crash:
        facts += f' LOG {crash.group(0)}'
    return out_line('PASS' if ok else 'FAIL', 'open', a.game, facts, a.out, a.out)


def g_speedrun(a, G):
    """One speed run of a non-BO2 game: drive route, then OUT/run/perf.tsv from the game's frame csv so abverdict can read it."""
    route = a.route_file
    length = route_len(route)
    if not G.wait_ready(a.ready_timeout):
        return fail_start(a, G, 'control channel never answered')
    end = time.monotonic() + length + 20
    while time.monotonic() < end and G.alive():
        time.sleep(2)
    alive = G.alive()
    G.ctl('flush 3000', 10)
    G.stop()
    csv = max(glob.glob(os.path.join(a.out, 'frames-*.csv')) + glob.glob(os.path.join(G.out, 'logs', 'frames-*.csv')), key=os.path.getmtime, default=None)
    n = 0
    if csv:
        os.makedirs(os.path.join(a.out, 'run'), exist_ok=True)
        with open(os.path.join(a.out, 'run', 'perf.tsv'), 'w') as f:
            f.write(f'# bo2perf 1\n# route\t{os.path.basename(route)}\n# players\t1\nframe\tt_ms\tframe_ms\tcpu_ms\tgpu_ms\tpresent_wait_ms\tcompile_ms\tdraws\n')
            for line in open(csv):
                p = line.split(',')
                if p[0] == 'present_submit':
                    f.write(f'{p[1]}\t{int(p[2]) / 1000:.3f}\t-\t-\t-\t-\t-\t1\n')
                    n += 1
    ok = alive and n > 200
    return out_line('PASS' if ok else 'FAIL', 'speedrun', a.game, f'alive {alive} frames {n} csv {csv}', a.out, a.out)


def savepoint_path(game, name):
    return name if os.path.isfile(name) else os.path.join(SAVEPOINTS, game, name + '.sp')


def savepoint_check(a, G):
    """None once the game says it restored the save point, else the FAIL line (stale file -> how to remake it)."""
    global LOADED
    m, end = None, time.monotonic() + 90
    while time.monotonic() < end and G.alive():
        text = G.log_text()
        m = re.search(r'\[savepoint\] restored .*', text)
        if m:
            LOADED = f' loaded {os.path.basename(a.savepoint)} {time.monotonic() - G.t0:.1f}s after start'
            return None
        m = re.search(r'\[savepoint\] load: .*', text)
        if m:
            break
        time.sleep(0.5)
    why = m.group(0) if m else 'no "[savepoint] restored" line'
    meta = os.path.splitext(a.savepoint)[0] + '.json'
    if os.path.exists(meta):
        why += f' (remake: {json.load(open(meta)).get("remake", "")})'
    return fail_start(a, G, 'save point did not load: ' + why)


def g_save(a, G):
    """Drive the route to --at guest seconds, write SAVEPOINTS/GAME/SCENE.sp (+ .json, .png)."""
    if not G.wait_ready(a.ready_timeout):
        return fail_start(a, G, 'control channel never answered')
    cfg = GAMES[a.game]
    log = cfg.get('save_log')
    at, ff = a.at or cfg.get('save_after_s') or cfg['scene_s'], a.ff or 1
    t_from = G.t0
    if log:
        if not G.wait_log(log, 900):
            G.stop()
            return out_line('FAIL', 'save', a.game, f'never logged "{log}"', a.out, a.out)
        t_from = time.monotonic()
    while time.monotonic() < t_from + at / ff and G.alive():
        time.sleep(1)
    if ff != 1:
        G.ctl('timescale 1', 10)
        time.sleep(3)
    dest = savepoint_path(a.game, a.scene)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    t = time.monotonic()
    r = G.ctl(f'savepoint file {dest}.tmp', 120) if G.alive() else 'game died'
    took = time.monotonic() - t
    png, st = G.shot('save') if G.alive() else (None, None)
    alive = G.alive()
    G.stop()
    ok = alive and r.startswith('ok') and os.path.exists(dest + '.tmp')
    if ok:
        os.replace(dest + '.tmp', dest)
        if png:
            shutil.copy(png, os.path.splitext(dest)[0] + '.png')
        hdr = open(dest, 'rb').read(32)
        json.dump(dict(made=time.strftime('%Y-%m-%d %H:%M'), at=at, after_log=log, ff=ff, route=os.path.basename(a.route_file or ''),
                       fingerprint=hdr[12:28].hex(),
                       remake=f'gate.py save {a.game} --scene {a.scene} --at {at:g} --ff {ff:g} --app APP'),
                  open(os.path.splitext(dest)[0] + '.json', 'w'), indent=1)
    facts = f'scene {a.scene} at {at:g}s' + (' after the in-a-match line' if log else '') + f' (ff {ff:g}) {os.path.getsize(dest) >> 20 if ok else 0} MB in {took:.1f}s reply {r[:120]}'
    return out_line('PASS' if ok else 'FAIL', 'save', a.game, facts, dest if ok else a.out, a.out)


def fail_start(a, G, why):
    died = G.exit_code()
    tail = (G.log_text()[-300:] or '').replace('\n', ' ')
    G.stop()
    return out_line('FAIL', a.gate, a.game, f'{why} (exit {died}) log tail: {tail}', a.out, a.out)


INNER = dict(boot=g_boot, shot=g_shot, drive=g_drive, guide=g_guide, open=g_open, speedrun=g_speedrun, save=g_save)


def run_inner(a):
    """Runs inside the runq job (or --now): start the game the way the gate needs, then the gate body."""
    os.makedirs(a.out, exist_ok=True)
    a.route_file = None
    env = dict(kv.split('=', 1) for kv in a.env)
    if a.savepoint:   # the game starts at the saved moment: no route, no fast-forward
        a.savepoint = savepoint_path(a.game, a.savepoint)
        if not os.path.isfile(a.savepoint):
            return out_line('FAIL', a.gate, a.game, f'no save point {a.savepoint} (make one: gate.py save {a.game} --scene NAME)', a.out, a.out)
        env['REX_LOAD_SAVEPOINT'] = a.savepoint
        a.route, a.ff, a.at = None, 0, a.at or 3
        # Same pads and settings (god mode...) as the run that saved, minus its inputs and one-shot starts.
        meta = os.path.splitext(a.savepoint)[0] + '.json'
        route = json.load(open(meta)).get('route') if os.path.isfile(meta) else None
        route = resolve_route(a.game, route) if route else None
        if route:
            os.makedirs(a.out, exist_ok=True)
            for k, v in route_env(route).items():
                if not k.startswith(('REX_BO2ZM_TEST_START', 'REX_BO2ZM_BENCHMARK', 'REX_HID_VIRTUAL_PAD_SCRIPT')):
                    env.setdefault(k, v)
            players = [l for l in open(route) if l.replace(' ', '').startswith('players=')]
            pads = os.path.join(a.out, 'pads.route')
            # racing games: hold the throttle so the drive proof sees movement (the saved route's inputs count from boot)
            open(pads, 'w').write(''.join(players) + (GAMES[a.game].get('load_pads', '') + '\n' if GAMES[a.game].get('load_pads') else ''))
            env.setdefault('REX_HID_VIRTUAL_PAD_SCRIPT', pads)
    need_route = a.gate in ('shot', 'drive', 'speedrun', 'save') and not a.savepoint
    if need_route or a.route:
        a.route_file = resolve_route(a.game, a.route) if (a.route or GAMES[a.game]['drive']) else None
    if need_route and not a.route_file and a.gate != 'shot':
        return out_line('FAIL', a.gate, a.game, 'no drive route for this game yet (pass --route)', a.out, a.out)
    if a.gate == 'guide':   # the Guide button comes from the pad script: open, RB, LB, close (times in guest seconds)
        t = a.at or GAMES[a.game]['settle_s']
        a.route_file = os.path.join(a.out, 'guide.route')
        open(a.route_file, 'w').write(f'players = 1\npad1 = {t}-{t + 0.4}:guide, {t + 3}-{t + 3.4}:rb, {t + 6}-{t + 6.4}:lb, {t + 9}-{t + 9.4}:guide\n')
    seed = a.gate in ('shot', 'drive', 'guide', 'speedrun', 'save') and not a.no_seed
    try:
        G = Game(a.app, a.out, a.game, route=a.route_file, ff=a.ff,
                 seed=seed, via_open=(a.gate == 'open'), env=env)
    except Exception as e:   # noqa: BLE001 - setup problem, report it
        return out_line('FAIL', a.gate, a.game, f'could not start: {e}', a.out, a.out)
    signal.signal(signal.SIGTERM, lambda *_: (G.stop(), sys.exit(3)))
    if a.savepoint:
        if not G.wait_ready(a.ready_timeout):
            return fail_start(a, G, 'control channel never answered')
        bad = savepoint_check(a, G)
        if bad is not None:
            return bad
    return INNER[a.gate](a, G)


# ----------------------------------------------------------------------------------------------- outer: queue the inner as a runq job
def runq_submit(args, name, app, out, klass, timeout, extra=(), wait=True, solo=False):
    cmd = [RUNQ, 'submit', '--class', klass, '--name', name, '--out', out, '--timeout', str(timeout)]
    if app:
        cmd += ['--app', app]
    if solo:
        cmd += ['--solo', '--timing']
    cmd += list(extra) + (['--wait'] if wait else [])
    cmd += list(args)
    return sh(cmd)


def default_out(game, gate):
    return os.path.join(RUNS, f'{game}-{gate}-{time.strftime("%m%d-%H%M%S")}')


def inner_args(a, out):
    x = ['python3', '-I', os.path.abspath(__file__), '--inner', a.gate, a.game, '--out', '{OUT}', '--app', '{APP}']
    for k in ('route', 'at', 'ff', 'secs', 'kinds_from', 'scene', 'savepoint'):
        if getattr(a, k, None) not in (None, ''):
            x += [f'--{k.replace("_", "-")}', str(getattr(a, k))]
    for e in a.env:
        x += ['--env', e]
    if a.no_seed:
        x.append('--no-seed')
    return x


def queue_gate(a, gate=None, game=None):
    """Submit one gate as a runq job, wait, print and return the result line."""
    gate, game = gate or a.gate, game or a.game
    b = argparse.Namespace(**vars(a)); b.gate, b.game = gate, game
    app = a.app if (a.app and game == a.game) else installed_app(game)
    out = a.out if (a.out and game == a.game and gate == a.gate) else default_out(game, gate)
    os.makedirs(os.path.dirname(out.rstrip('/')), exist_ok=True)
    if a.now:
        b.app, b.out = app, out
        return run_inner(b)
    timeout = {'boot': 300, 'shot': 900, 'drive': 1500, 'guide': 600, 'open': 400, 'save': 900}.get(gate, 900)
    args = ['--'] + inner_args(b, out)
    r = runq_submit(args, f'gate-{game}-{gate}', app, out, a.klass, timeout,
                    extra=['--window'] if gate == 'open' else [])   # open tests the real window: runq keeps it on-screen
    txt = os.path.join(out, 'gate.txt')
    if os.path.exists(txt):
        line = open(txt).read().strip()
        print(line)
        return 0 if line.startswith('PASS') else 1
    return out_line('FAIL', gate, game, f'job produced no result (runq exit {r.returncode}): {(r.stdout + r.stderr)[-200:].strip()}', out)


def g_set(a):
    games = a.games or SET_GAMES
    import concurrent.futures as cf
    with cf.ThreadPoolExecutor(len(games)) as ex:
        res = list(ex.map(lambda g: queue_gate(a, 'boot', g), games))
    return 0 if not any(res) else 1


CONTROL_CACHE = os.path.join(SPOOL, 'control-cache')
CONTROL_CACHE_VER = 1        # bump when runq's speed-run rules change: old cached controls are then ignored
CONTROL_MAX_AGE_S = 6 * 3600  # a cached control is reused for pairs 2+ of a verdict only while this fresh
CONTROL_KEEP = 3


def control_key(game, app, route):
    """Same control binary + game + route + code version. Hashes every file under Contents/MacOS (a rebuilt or
    re-signed control gets a new key)."""
    h = hashlib.sha1(f'{CONTROL_CACHE_VER}|{game}|{route}'.encode())
    for dp, _, fs in sorted(os.walk(os.path.join(app, 'Contents', 'MacOS'))):
        for f in sorted(fs):
            h.update(f.encode())
            with open(os.path.join(dp, f), 'rb') as fh:
                for blk in iter(lambda: fh.read(1 << 20), b''):
                    h.update(blk)
    return h.hexdigest()[:16]


def control_cached(key):
    """Valid, fresh control run dirs for this key, newest first. Validity is re-checked by abverdict (runq status)."""
    d = os.path.join(CONTROL_CACHE, key)
    out = []
    for f in sorted(glob.glob(os.path.join(d, '*.json')), reverse=True):
        try:
            e = json.load(open(f))
        except (OSError, ValueError):
            continue
        if time.time() - e['t'] <= CONTROL_MAX_AGE_S and os.path.isfile(os.path.join(e['out'], 'run', 'perf.tsv')):
            out.append(e['out'])
    return out[:CONTROL_KEEP]


def control_store(key, out):
    d = os.path.join(CONTROL_CACHE, key)
    os.makedirs(d, exist_ok=True)
    json.dump({'t': time.time(), 'out': out, 'machine': os.uname().nodename}, open(os.path.join(d, f'{time.time():.0f}-{os.path.basename(out)}.json'), 'w'))


def g_speed_bench(a):
    """bo2 deterministic bench (class bench, solo): N runs per side interleaved, abverdict --bench on CPU work and GPU ms.
    Needs apps built with branch deterministic-bench (SDK bench cvars); older apps are refused."""
    abv = os.path.join(SPOOL, 'bin', 'abverdict.py')
    base = a.out or default_out('bo2', 'speed-bench')
    for app in (a.a, a.b):
        if sh(['grep', '-rlaqF', '[bench] done', os.path.join(app, 'Contents', 'MacOS')]).returncode:
            return out_line('FAIL', 'speed', 'bo2', f'{app} has no deterministic bench (built before origin/deterministic-bench); use the fps-pair gate', base, base)
    route = resolve_route('bo2', a.route or 'town4_det')
    outs = {'A': [], 'B': []}
    for n in range(1, a.pairs + 1):    # a.pairs = max runs per side here; the verdict needs >= 2 valid per side
        jobs = []
        for side, app in (('A', a.a), ('B', a.b)) if n % 2 else (('B', a.b), ('A', a.a)):
            out = os.path.join(base, f'{side}{n}')
            outs[side].append(out)
            r = sh([RUNQ, 'submit', '--class', 'bench', '--solo', '--timing', '--app', app, '--route', route, '--timeout', '420',
                    '--env', 'END_ON_LOG=[bench] done', '--name', f'gate-bench-{side}{n}', '--out', out])
            m = re.search(r'job (\S+)', r.stdout)
            if not m:
                return out_line('FAIL', 'speed', 'bo2', f'submit failed: {(r.stdout + r.stderr)[-200:]}', base)
            jobs.append(m.group(1))
        sh([RUNQ, 'wait'] + jobs)
        if n < 2:
            continue
        v = sh(['python3', abv, '--bench', '--a'] + outs['A'] + ['--b'] + outs['B'])
        print(v.stdout.strip())
        if v.returncode == 0:
            words = re.findall(r'-> (FASTER|SLOWER|SAME)', v.stdout)
            return out_line('FAIL' if 'SLOWER' in words else 'PASS', 'speed', 'bo2', f'bench {"/".join(words)} (CPU work/GPU) after {n} runs per side: ' + ' | '.join(l for l in v.stdout.splitlines() if '->' in l), base, base)
        # exit 2: a run was invalid -> drop nothing, abverdict skips invalid ones; exit 3: MORE -> another run per side
    return out_line('FAIL', 'speed', 'bo2', f'no bench verdict after {a.pairs} runs per side (MORE = same within noise)', base, base)


# Measured same-build run-to-run CV for speed routes without a template route in abverdict (see reports/gate-kit-rest.md).
SPEED_SIGMA = {'nfsmw': 0.02}   # 2 valid same-build runs 58.52/58.19 (0.56% apart) -> 2%, rounded up; fh1 unmeasured (its one run was invalid: compiler overlap)


def g_speed(a):
    """A/B speed pairs through the queue (class perf, solo) and abverdict; bo2 uses town4 via runq's route runs."""
    abv = os.path.join(SPOOL, 'bin', 'abverdict.py')
    if not (a.a and a.b):
        sys.exit('speed needs --a APP --b APP (the two builds to compare)')
    if a.bench:
        if a.game != 'bo2':
            sys.exit('speed --bench is bo2 only (the other games have no bench)')
        return g_speed_bench(a)
    outs = {'A': [], 'B': []}
    base = a.out or default_out(a.game, 'speed')
    # Control cache: pair 1 always runs a fresh control A (interleaved with B, same machine state). Pairs 2+ reuse
    # earlier valid runs of the same control binary instead of re-measuring it (statistical caveat: reused controls
    # were not interleaved with this B, so slow machine drift between them is not cancelled; hence the 6 h limit and
    # the fresh first pair). --no-control-cache turns it off.
    ckey = control_key(a.game, a.a, a.route or 'town4') if a.game == 'bo2' and not getattr(a, 'no_control_cache', False) else None
    cached = [c for c in control_cached(ckey)] if ckey else []
    for pair in range(1, a.pairs + 1):
        jobs = []
        for side, app in ((('A', a.a), ('B', a.b)) if pair % 2 else (('B', a.b), ('A', a.a))):   # alternate who runs first: the 2nd run of a pair is hotter
            out = os.path.join(base, f'{side}{pair}')
            if side == 'A' and pair > 1 and cached:
                outs['A'].append(cached.pop(0))
                print(f'control cache: pair {pair} reuses {outs["A"][-1]}')
                continue
            outs[side].append(out)
            if a.game == 'bo2':
                cmd = [RUNQ, 'submit', '--class', 'perf', '--app', app, '--route', resolve_route('bo2', a.route or 'town4'), '--name', f'gate-speed-{side}{pair}', '--out', out]
            else:
                b = argparse.Namespace(**vars(a)); b.gate, b.out = 'speedrun', out
                cmd = [RUNQ, 'submit', '--class', 'perf', '--solo', '--timing', '--app', app, '--name', f'gate-speed-{side}{pair}', '--out', out, '--timeout', '1800', '--'] + inner_args(b, out)
            r = sh(cmd)
            m = re.search(r'job (\S+)', r.stdout)
            if not m:
                return out_line('FAIL', 'speed', a.game, f'submit failed: {(r.stdout + r.stderr)[-200:]}', base)
            jobs.append(m.group(1))
        sh([RUNQ, 'wait'] + jobs)
        if ckey and jobs:
            control_store(ckey, outs['A'][-1]) if outs['A'][-1].startswith(base) and os.path.isfile(os.path.join(outs['A'][-1], 'run', 'perf.tsv')) else None
        sig = ['--sigma', str(SPEED_SIGMA[a.game])] if a.game in SPEED_SIGMA else []
        v = sh(['python3', abv] + sig + ['--a'] + outs['A'] + ['--b'] + outs['B'])
        print(v.stdout.strip().splitlines()[-1] if v.stdout.strip() else v.stderr.strip()[-200:])
        word = re.search(r'\b(FASTER|SLOWER|SAME|MORE|INVALID)\b', v.stdout or '')
        if v.returncode == 0:
            word = re.search(r'\b(FASTER|SLOWER|SAME)\b', v.stdout)
            return out_line('FAIL' if word and word.group(1) == 'SLOWER' else 'PASS', 'speed', a.game,
                            f'{word.group(1) if word else "?"} after {pair} pair(s): ' + ' '.join(v.stdout.split()[-12:]), base, base)
        if v.returncode == 2:   # a run collapsed or was invalid: redo this pair
            outs['A'].pop(); outs['B'].pop()
            continue
    return out_line('FAIL', 'speed', a.game, f'no verdict after {a.pairs} pairs (MORE = same within noise; treat as SAME if a speed change was not claimed)', base, base)


def g_install(a):
    """Install-with-checks. Order: owner not playing, base rule, new app codesign + identity, restore copy, swap in place, verify."""
    game = a.game
    name = GAMES[game]['app']
    dest_dir = os.path.expanduser(a.dest or LAUNCHERS)
    dest = os.path.join(dest_dir, name)
    new = os.path.abspath(a.app or '')
    out = a.out or default_out(game, 'install')
    os.makedirs(out, exist_ok=True)
    fails = []
    notes = []
    if not os.path.isdir(os.path.join(new, 'Contents')):
        return out_line('FAIL', 'install', game, f'--app {new} is not an app bundle', out, out)
    # 1. owner is playing? any process running out of the installed app (runq clones live elsewhere)
    r = sh(['pgrep', '-f', dest + '/Contents/MacOS/'])
    if r.stdout.split():
        return out_line('FAIL', 'install', game, f'REFUSED: {name} is running (pids {" ".join(r.stdout.split())}); the owner may be playing', out, out)
    # 2. install base rule
    if a.repo:
        sh(['git', '-C', a.repo, 'fetch', '-q', 'origin'])
        ref = a.integration if '/' in a.integration else 'origin/' + a.integration
        ok = sh(['git', '-C', a.repo, 'merge-base', '--is-ancestor', ref, a.head]).returncode == 0
        notes.append(f'base {"ok" if ok else "BEHIND"} ({ref} vs {a.head})')
        if not ok:
            fails.append(f'{a.head} does not contain {ref}: rebase first (install base rule)')
    else:
        fails.append('--repo WORKTREE required for the install base rule (the tree the app was built from)')
    # 3. new app: codesign and same identity as the installed one
    cs = sh(['codesign', '--verify', '--deep', '--strict', new])
    notes.append(f'codesign rc {cs.returncode}')
    if cs.returncode:
        fails.append('codesign --verify --deep --strict failed on the new app: ' + cs.stderr.strip()[:120])
    if os.path.isdir(dest):
        try:
            pn = plistlib.load(open(os.path.join(new, 'Contents', 'Info.plist'), 'rb'))
            po = plistlib.load(open(os.path.join(dest, 'Contents', 'Info.plist'), 'rb'))
            if pn.get('CFBundleIdentifier') != po.get('CFBundleIdentifier'):
                fails.append(f'bundle id {pn.get("CFBundleIdentifier")} != installed {po.get("CFBundleIdentifier")}: wrong game?')
        except (OSError, plistlib.InvalidFileException) as e:
            fails.append(f'Info.plist unreadable: {e}')
    if fails:
        return out_line('FAIL', 'install', game, '; '.join(fails) + ' | ' + '; '.join(notes), out, out)
    if a.dry_run:
        return out_line('PASS', 'install', game, 'dry run, all checks passed, nothing copied | ' + '; '.join(notes), out, out)
    # 4. restore copy of what is installed now (APFS clone; kept outside /tmp so the cleaner leaves it)
    restore = os.path.join(a.restore_dir or os.path.join(out, 'restore'), name)
    if os.path.isdir(dest):
        os.makedirs(os.path.dirname(restore), exist_ok=True)
        shutil.rmtree(restore, ignore_errors=True)
        subprocess.run(['cp', '-cR', dest, restore], check=True)
        rc = sh(['codesign', '--verify', '--deep', '--strict', restore]).returncode
        notes.append(f'restore copy codesign rc {rc}')
    # 5. swap in place: same folder, no "(previous)" copy left behind
    os.makedirs(dest_dir, exist_ok=True)
    rs = sh(['rsync', '-a', '--delete', new + '/', dest + '/'])
    if rs.returncode:
        return out_line('FAIL', 'install', game, f'rsync failed: {rs.stderr.strip()[:150]} (restore copy {restore})', out, out)
    # 6. verify the installed app
    cs2 = sh(['codesign', '--verify', '--deep', '--strict', dest]).returncode
    exe = os.path.join(dest, 'Contents', 'MacOS', GAMES[game]['app'][:-4])
    same = sh(['diff', '-rq', new, dest]).returncode == 0
    leftovers = [n for n in os.listdir(dest_dir) if 'previous' in n.lower() and game_word(game) in n.lower()]
    ok = cs2 == 0 and same and not leftovers
    return out_line('PASS' if ok else 'FAIL', 'install', game,
                    f'{dest} codesign rc {cs2} identical-to-new {same} leftovers {leftovers or "none"} | ' + '; '.join(notes) + f' | restore: {restore}', out, out)


def g_replay(a):
    """gate replay BUILD_DIR [--repo R] [--only a,b]: picture-regression replay of the saved GPU traces (sdk/tools/gputest/replay-gate).
    Run it first for any change under sdk/src/graphics. Its runs go through runq (harness, non-solo); BO1 traces take minutes."""
    if len(a.game) != 1:
        sys.exit('gate replay: BUILD_DIR (dir with rexgpu-replay + librexgpu-metal.dylib, flat or per-game subdirs)')
    tool = os.path.join(os.path.expanduser(a.repo or '.'), 'sdk/tools/gputest/replay-gate/replay-gate')
    if not os.path.exists(tool):
        sys.exit(f'gate replay: no {tool} (pass --repo of a tree with the replay-gate tool)')
    out = a.out or default_out('all', 'replay')
    os.makedirs(out, exist_ok=True)
    r = subprocess.run([tool, 'check', os.path.expanduser(a.game[0])] + (['--only', a.only] if a.only else []), capture_output=True, text=True)
    open(os.path.join(out, 'replay-gate.txt'), 'w').write(r.stdout + r.stderr)
    res = re.search(r'^RESULT: (.*)$', r.stdout, re.M)
    bad = [l.split()[1] for l in r.stdout.splitlines() if re.match(r'\s+(FAIL|ERROR)\b', l)]
    ok = r.returncode == 0
    return out_line('PASS' if ok else 'FAIL', 'replay', 'all', (res.group(1).strip() if res else 'no RESULT line') + (' failing ' + ','.join(bad) if bad else ''), out, out)


def game_word(game):
    return {'bo2': 'bo2', 'nfsmw': 'most wanted', 'fh1': 'forza', 'nfshp': 'hot pursuit', 'bo1': 'black ops', 'nfsmw2012': 'most wanted 2012', 'gears': 'gears of war', 'ac1': 'assassin'}[game]


# ----------------------------------------------------------------------------------------------- cli
def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('gate', choices=['boot', 'shot', 'drive', 'guide', 'open', 'set', 'speed', 'install', 'speedrun', 'replay', 'save'])
    p.add_argument('game', nargs='*')
    p.add_argument('--app', help='app to test (default: the installed one); for install: the NEW app')
    p.add_argument('--out')
    p.add_argument('--route')
    p.add_argument('--at', type=float, help='shot/guide: guest seconds to run before looking')
    p.add_argument('--ff', type=float, default=0, help='guest time scale until the scene (2 is safe for routes); 0 = off')
    p.add_argument('--secs', type=float, default=45)
    p.add_argument('--scene', help='save: save point name (SAVEPOINTS/GAME/NAME.sp)')
    p.add_argument('--savepoint', help='boot/shot/drive: start from this save point (name in SAVEPOINTS/GAME, or a file)')
    p.add_argument('--ready-timeout', type=float, default=240, dest='ready_timeout')
    p.add_argument('--kinds-from', dest='kinds_from', help='kinds.txt of an earlier run: report new warning/error kinds')
    p.add_argument('--env', action='append', default=[])
    p.add_argument('--no-seed', action='store_true', help='do not clone the owner profile into the run')
    p.add_argument('--class', dest='klass', default='harness')
    p.add_argument('--now', action='store_true', help='run inline instead of through runq (debugging only)')
    p.add_argument('--inner', action='store_true', help=argparse.SUPPRESS)
    p.add_argument('--a'), p.add_argument('--b'), p.add_argument('--pairs', type=int, default=3), p.add_argument('--bench', action='store_true', help='speed bo2: deterministic bench instead of fps pairs'), p.add_argument('--no-control-cache', action='store_true')
    p.add_argument('--only'), p.add_argument('--repo'), p.add_argument('--head', default='HEAD'), p.add_argument('--integration', default='origin/main')
    p.add_argument('--dest'), p.add_argument('--restore-dir', dest='restore_dir'), p.add_argument('--dry-run', action='store_true')
    argv = sys.argv[1:]
    inner = '--inner' in argv
    if inner:
        argv.remove('--inner')
    a = p.parse_args(argv)
    if a.gate == 'open' and a.secs == 45:
        a.secs = 90
    if a.gate == 'set':
        a.games, a.game = a.game, None
        return g_set(a)
    if a.gate == 'replay':
        return g_replay(a)
    if len(a.game) != 1 or a.game[0] not in GAMES:
        sys.exit(f'gate: GAME must be one of {" ".join(GAMES)}')
    a.game = a.game[0]
    if a.gate == 'speed':
        return g_speed(a)
    if a.gate == 'install':
        return g_install(a)
    if a.gate == 'save' and not a.scene:
        sys.exit('gate: save needs --scene NAME')
    if inner:
        return run_inner(a)
    return queue_gate(a)


if __name__ == '__main__':
    sys.exit(main())
