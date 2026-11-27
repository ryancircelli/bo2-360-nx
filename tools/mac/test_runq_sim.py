"""Simulation harness for the runq daemon: fake clock, fake processes, no real games or signals.

Drives Daemon.tick() against a temp spool and checks liveness:
  - a queued job starts within a bound unless the queue is paused or quiet
  - nothing starts while paused or quiet
  - a finished job is reaped (slot freed) within a bound
  - every pid runq SIGSTOPs is SIGCONTed again
  - a daemon crash/restart adopts running jobs and keeps the queue moving
Run: python3 tools/mac/test_runq_sim.py
"""
import importlib.machinery
import importlib.util
import inspect
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import time as real_time
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
SPOOL_DIR = tempfile.TemporaryDirectory()
os.environ['RUNQ_SPOOL'] = SPOOL_DIR.name
loader = importlib.machinery.SourceFileLoader('runq_sim_under_test', os.environ.get('RUNQ_UNDER_TEST', str(HERE / 'runq')))
spec = importlib.util.spec_from_loader(loader.name, loader)
q = importlib.util.module_from_spec(spec)
loader.exec_module(q)
assert q.SPOOL == SPOOL_DIR.name

FAKE_PID0 = 7_000_000   # above any real pid, so a slipped patch can never signal a real process


class Clock:
    """Stands in for the `time` module inside runq."""
    def __init__(self, t):
        self.t = float(t)

    def time(self):
        return self.t

    def monotonic(self):
        return self.t

    def sleep(self, s):
        self.t += s

    def localtime(self, t=None):
        return real_time.localtime(self.t if t is None else t)

    def strftime(self, fmt, tt=None):
        return real_time.strftime(fmt, self.localtime() if tt is None else tt)

    def __getattr__(self, name):
        return getattr(real_time, name)


class World:
    """Fake process table. A job's command decides its runtime: ['sim', '<seconds>', '<exit code>']."""
    def __init__(self, clock):
        self.clock = clock
        self.next_pid = FAKE_PID0
        self.procs = {}          # pid -> dict(end, code, stopped, alive, pgid, cmd, out)
        self.stopped_by_runq = set()
        self.signals = []

    def spawn(self, cmd, env):
        self.next_pid += 1
        pid = self.next_pid
        args = cmd[cmd.index('sim'):] if 'sim' in cmd else ['sim', '30', '0']
        self.procs[pid] = dict(end=self.clock.t + float(args[1]), code=int(args[2]) if len(args) > 2 else 0,
                               stopped=False, alive=True, cmd=cmd, out=(env or {}).get('RUNQ_OUT'))
        return pid

    def advance(self):
        for pid, p in self.procs.items():
            if p['alive'] and not p['stopped'] and self.clock.t >= p['end']:
                self.exit(pid, p['code'])

    def exit(self, pid, code):
        p = self.procs[pid]
        p['alive'] = False
        p['code'] = code
        if p['out']:   # the job wrapper's exit file
            os.makedirs(p['out'], exist_ok=True)
            Path(p['out'], q.EXIT_FILE).write_text(f'{code}\n')

    def alive(self, pid):
        p = self.procs.get(pid)
        return bool(p and p['alive'])

    def kill(self, pid, sig):
        self.signals.append((pid, sig))
        p = self.procs.get(abs(pid))
        if not p or not p['alive']:
            raise ProcessLookupError(pid)
        if sig == signal.SIGSTOP:
            if not p['stopped']:
                p['stopped'] = True
                p['stop_t'] = self.clock.t
        elif sig == signal.SIGCONT:
            if p['stopped']:
                p['end'] += self.clock.t - p.get('stop_t', self.clock.t)   # time stopped does not count
            p['stopped'] = False
        elif sig in (signal.SIGTERM, signal.SIGKILL, signal.SIGINT):
            if sig == signal.SIGKILL or not p['stopped']:
                self.exit(abs(pid), -sig)
        elif sig == 0:
            pass


class FakePopen:
    def __init__(self, world, cmd, env=None, **kw):
        self.world, self.args = world, cmd
        self.pid = world.spawn(cmd, env)
        self.returncode = None

    def poll(self):
        p = self.world.procs[self.pid]
        if p['alive']:
            return None
        self.returncode = p['code'] if p['code'] >= 0 else p['code']
        return self.returncode

    def wait(self, timeout=None):
        return self.poll()


class Sim(unittest.TestCase):
    START = real_time.mktime((2026, 11, 26, 22, 0, 0, 0, 0, -1))

    def setUp(self):
        for k in ('queue', 'running', 'done', 'jobs', 'builds'):
            d = q.D[k]
            if os.path.isdir(d):
                for f in os.listdir(d):
                    fp = os.path.join(d, f)
                    if os.path.isfile(fp):
                        os.unlink(fp)
        for f in (q.STATE, q.CONFIG, q.QUIET):
            if os.path.exists(f):
                os.unlink(f)
        q.mkdirs()
        self.clock = Clock(self.START)
        self.world = World(self.clock)
        self.n = 0
        w = self.world
        real_kill, real_killpg = os.kill, os.killpg

        def fake_kill(pid, sig):
            if abs(pid) < FAKE_PID0:
                if sig == 0 and abs(pid) == os.getpid():
                    return real_kill(pid, 0)
                raise ProcessLookupError(pid)   # never signal a real process
            w.kill(pid, sig)

        def fake_popen(cmd, *a, **kw):
            if cmd and cmd[0] == 'paseo':
                return FakePopen(w, ['sim', '0', '0'])
            if '_freeze-watchdog' in cmd:
                return FakePopen(w, ['sim', '999999', '0'])
            return FakePopen(w, cmd, env=kw.get('env'))

        def no_run(*a, **kw):
            if a and a[0] and str(a[0][0]).endswith('/winguard'):
                return q.subprocess.CompletedProcess(a[0], 0, '', '')   # no visible window
            raise AssertionError(f'runq ran a real command in the simulation: {a[0] if a else kw}')

        patches = [
            patch.object(q, 'time', self.clock),
            patch.object(os, 'kill', fake_kill),
            patch.object(os, 'killpg', fake_kill),
            patch.object(os, 'getloadavg', lambda: (1.0, 1.0, 1.0)),
            patch.object(q, 'alive', lambda pid: bool(pid) and w.alive(int(pid))),
            patch.object(q.subprocess, 'Popen', fake_popen),
            patch.object(q.subprocess, 'run', no_run),
            patch.object(q.subprocess, 'check_output', no_run),
            patch.object(q.subprocess, 'call', no_run),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self.stub_system()
        self.write_config({})
        self.log, self.seen_running, self.dead_since = [], set(), {}

    def stub_system(self):
        """Machine probes: an otherwise idle Mac on mains power."""
        self.power = dict(connected=True, ac=True, watts=140, charge=100, charging=False)
        self.cache = tempfile.mkdtemp(dir=SPOOL_DIR.name)
        stubs = dict(
            compiler_processes=lambda *a, **k: self._snapshot([]),
            process_ancestors=lambda pid=None: {os.getpid()},
            darwin_user_cache_dir=lambda: self.cache,
            games_outside=lambda own: [],
            power_source=lambda: dict(self.power),
            process_cwds=lambda pids: {},
            process_tree=lambda pid: [pid],
            set_background=lambda pids, on: None,
            set_power_mode=lambda mode: True,
            read_gauges=lambda: {},
        )
        for name, fn in stubs.items():
            p = patch.object(q, name, fn)   # AttributeError if runq renamed it: fix the stub list
            p.start()
            self.addCleanup(p.stop)

    def _snapshot(self, v):
        q.PROCESS_SNAPSHOT = []
        return v

    # ---------------------------------------------------------------- helpers
    def write_config(self, extra):
        c = dict(slots=3)
        c.update(extra)
        Path(q.CONFIG).write_text(json.dumps(c))
        os.utime(q.CONFIG, (self.clock.t, self.clock.t))

    def submit(self, secs=30, code=0, **job):
        self.n += 1
        jid = f'{self.n:04d}'
        out = os.path.join(q.D['jobs'], jid)
        os.makedirs(out, exist_ok=True)
        j = dict(id=jid, name=f'sim{jid}', owner=job.pop('owner', 'agent-a'), out=out, cmd=['sim', str(secs), str(code)],
                 submitted=q.now_s() if hasattr(q, 'now_s') else '', submitted_t=self.clock.t, priority=0,
                 **{'class': job.pop('klass', q.CLASSES['sweep'])}, solo=False, app=None, clone_app=False, env={})
        j.update(job)
        Path(q.D['queue'], f'{jid}.json').write_text(json.dumps(j))
        return jid

    def daemon(self):
        d = q.Daemon()
        d.log = lambda msg: self.log.append((self.clock.t, msg))
        d.adopt()
        d.pool_open()   # a real FIFO in the temp spool: build tokens are part of what the harness checks
        self.addCleanup(os.close, d.fifo)
        return d

    def run_for(self, d, seconds, check=None, step=1):
        """Tick the daemon like its main loop (time.sleep(1) between ticks). A tick that raises is a daemon crash."""
        end = self.clock.t + seconds
        while self.clock.t < end:
            self.world.advance()
            if d.tick():
                return True
            self.check_invariants(d)
            if check:
                check(d)
            self.clock.sleep(step)
        return False

    def run_until(self, d, cond, limit, step=1, what=''):
        end = self.clock.t + limit
        while self.clock.t < end:
            self.world.advance()
            d.tick()
            self.check_invariants(d)
            if cond():
                return self.clock.t
            self.clock.sleep(step)
        self.fail(f'not within {limit} s: {what}\nlast log: {self.log[-15:]}')

    def check_invariants(self, d):
        c = q.cfg()
        if q.paused(c) or int(c.get('slots', 3)) == 0:
            new = [jid for jid in d.jobs if jid not in self.seen_running]
            self.assertEqual(new, [], 'a job started while the queue was paused or quiet')
        self.seen_running |= set(d.jobs)
        # a finished job's slot is freed by the next tick: no job may sit in d.jobs dead for more than 2 ticks
        for jid, j in d.jobs.items():
            pid = j['proc'].pid
            if not self.world.alive(pid):
                self.dead_since.setdefault(jid, self.clock.t)
                self.assertLess(self.clock.t - self.dead_since[jid], 5, f'{jid} dead but still holds slot {j["slot"]}')

    def crash(self, d):
        """The daemon process dies hard (SIGKILL / exception): no finally block runs, its FIFO closes."""
        os.close(d.fifo)
        d.fifo = os.open(os.devnull, os.O_RDONLY)   # keep the cleanup close() valid

    def add_build(self, name='b1', waited=0):
        pid = self.world.spawn(['sim', '999999'], None)
        child = self.world.spawn(['sim', '999999'], None)
        reg = real_time.strftime('%Y-%m-%d %H:%M:%S', real_time.localtime(self.clock.t - waited))
        Path(q.D['builds'], f'{name}.json').write_text(json.dumps(dict(pid=pid, child_pid=child, registered=reg)))
        return pid, child

    def stopped_pids(self):
        return sorted(pid for pid, p in self.world.procs.items() if p['alive'] and p['stopped'])

    def state_of(self, jid):
        if os.path.exists(os.path.join(q.D['queue'], f'{jid}.json')):
            return 'queued'
        if os.path.exists(os.path.join(q.D['running'], f'{jid}.json')):
            return 'running'
        if os.path.exists(os.path.join(q.D['done'], f'{jid}.json')):
            return 'done'
        return 'missing'

    # ---------------------------------------------------------------- scenarios
    def test_smoke_three_jobs_run_and_finish(self):
        d = self.daemon()
        ids = [self.submit(secs=20) for _ in range(5)]
        self.run_for(d, 200)
        self.assertEqual([self.state_of(i) for i in ids], ['done'] * 5, self.log[-20:])


    # (b) midnight pause: "08:00" set at 23:02 must hold until 08:00 next morning, across a restart and a config rewrite
    def test_midnight_pause_holds_overnight(self):
        self.clock.t = real_time.mktime((2026, 11, 26, 23, 2, 0, 0, 0, -1))
        self.write_config({'paused_until': '08:00'})
        d = self.daemon()
        jid = self.submit(secs=20)
        self.run_for(d, 3600, step=10)                         # to 00:02
        self.assertEqual(self.state_of(jid), 'queued')
        self.crash(d)
        raw = json.loads(Path(q.CONFIG).read_text())
        raw['note'] = 'rewritten while paused'                 # e.g. quiet on/off rewrites config.json
        Path(q.CONFIG).write_text(json.dumps(raw))
        os.utime(q.CONFIG, (self.clock.t, self.clock.t))
        d = self.daemon()
        wake = real_time.mktime((2026, 11, 27, 8, 0, 0, 0, 0, -1))
        self.run_for(d, wake - self.clock.t - 30, step=10)
        self.assertEqual(self.state_of(jid), 'queued', 'started before 08:00')
        t = self.run_until(d, lambda: self.state_of(jid) != 'queued', 120, what='job starts after the pause ends')
        self.assertGreaterEqual(t, wake)

    def test_unreadable_pause_holds_not_runs(self):
        # "24:00" / "8am" are typos for a pause: holding is the safe reading (the owner is at the machine)
        for bad in ('2026-11-27 24:00', '8am'):
            self.write_config({'paused_until': bad})
            self.assertTrue(q.paused(q.cfg()), bad)

    # (a) idle queue with work waiting: a live session must not keep a solo window from opening, ordinary jobs flow
    def test_live_session_does_not_idle_queue(self):
        d = self.daemon()
        live = self.submit(secs=20000, cmd=['python3', '-I', '/x/live_session.py', 'sim', '20000', '0'])
        self.run_for(d, 5)
        self.assertEqual(self.state_of(live), 'running')
        perf = self.submit(secs=60, klass=q.CLASSES['perf'], solo=True, quiet_machine=True, measured=True)
        ords = [self.submit(secs=100) for _ in range(6)]
        self.run_until(d, lambda: self.state_of(perf) == 'done', 1800, what='perf job runs beside a live session')
        self.run_until(d, lambda: all(self.state_of(i) == 'done' for i in ords), 3600, what='ordinary jobs drain')

    # runq-aging (11-27: bench jobs waited 6 h behind a steady stream of higher-class jobs)
    def test_aged_job_outranks_higher_class(self):
        self.write_config(dict(slots=1))
        d = self.daemon()
        first = self.submit(secs=10, klass=q.CLASSES['ship'])
        self.run_for(d, 3)
        old = self.submit(secs=20, klass=q.CLASSES['bench'], submitted_t=self.clock.t - 6000)
        fresh = [self.submit(secs=20, klass=q.CLASSES['harness']) for _ in range(4)]
        self.run_until(d, lambda: self.state_of(first) == 'done', 60)
        self.run_until(d, lambda: self.state_of(old) != 'queued', 60, what='aged bench job takes the next slot')
        self.assertTrue(all(self.state_of(i) == 'queued' for i in fresh), 'a fresh harness job jumped the aged job')

    def test_aged_solo_job_forces_its_window(self):
        self.write_config(dict(slots=3, ord_min_s=3600, ord_window_s=3600))
        d = self.daemon()
        ords = [self.submit(secs=120) for _ in range(60)]
        self.run_for(d, 30)
        solo = self.submit(secs=30, klass=q.CLASSES['bench'], solo=True, quiet_machine=True, measured=True,
                           submitted_t=self.clock.t - 6000)
        self.run_until(d, lambda: self.state_of(solo) == 'done', 600, what='aged solo job runs inside 10 min, not after ord_min_s')

    def test_one_owner_cannot_hold_most_slots(self):
        d = self.daemon()
        a = [self.submit(secs=100, klass=q.CLASSES['harness'], owner='ownerA') for _ in range(4)]
        b = self.submit(secs=100, klass=q.CLASSES['sweep'], owner='ownerB')
        self.run_for(d, 5)
        self.assertEqual(self.state_of(b), 'running', 'owner B waited behind owner A\'s higher class jobs')
        self.assertLessEqual(sum(self.state_of(i) == 'running' for i in a), 2)

    # laptop-only jobs must not hold the Mac's solo window or build cap
    def test_laptop_jobs_do_not_hold_mac(self):
        d = self.daemon()
        self.submit(secs=60, where='laptop', solo=True)
        ords = [self.submit(secs=60) for _ in range(4)]
        self.run_until(d, lambda: all(self.state_of(i) == 'done' for i in ords), 900, what='Mac jobs run past a laptop job')
        self.assertGreater(d.cap_now()[0], 0, d.cap_now())

    # (d) build cap 0 with nothing running: a perf job that cannot start must not hold the builds at 0
    def test_held_perf_job_does_not_zero_build_cap(self):
        self.power.update(watts=30)                            # adapter too weak: the perf job is held
        self.add_build(waited=60)
        d = self.daemon()
        self.submit(secs=60, klass=q.CLASSES['perf'], solo=True, quiet_machine=True, measured=True)
        self.run_until(d, lambda: not d.jobs and d.cap_now()[0] > 0 and self.clock.t - self.START > 120, 1200,
                       what='build cap above 0 while the only queued job is held')

    # (c) a lane job frozen through a solo window must still die on cancel
    def test_cancel_frozen_lane_job(self):
        d = self.daemon()
        live = self.submit(secs=20000, cmd=['python3', '-I', '/x/live_session.py', 'sim', '20000', '0'])
        self.run_for(d, 5)
        perf = self.submit(secs=600, klass=q.CLASSES['perf'], solo=True, quiet_machine=True, measured=True)
        self.run_until(d, lambda: self.state_of(perf) == 'running', 1800, what='perf job starts')
        pid = d.jobs[live]['proc'].pid
        Path(q.D['running'], live + '.cancel').touch()
        self.run_until(d, lambda: not self.world.alive(pid) and self.state_of(live) == 'done', 60,
                       what='cancelled frozen lane job dies')

    # (e) builds frozen for a quiet job are resumed even when the daemon dies during the job
    def test_frozen_builds_resume_after_crash_job_ends_while_down(self):
        b = self.add_build()
        d = self.daemon()
        perf = self.submit(secs=120, klass=q.CLASSES['perf'], solo=True, quiet_machine=True, measured=True)
        self.run_until(d, lambda: self.state_of(perf) == 'running', 1200, what='perf job starts')
        self.run_until(d, lambda: set(b) <= set(self.stopped_pids()), 60, what='builds frozen')
        self.crash(d)
        self.clock.sleep(300)
        self.world.advance()                                    # job ends while no daemon runs
        d = self.daemon()
        self.run_for(d, 10)
        self.assertEqual(self.stopped_pids(), [], 'builds left stopped after restart')
        self.assertEqual(self.state_of(perf), 'done')

    def test_frozen_builds_resume_after_crash_job_ends_later(self):
        b = self.add_build()
        d = self.daemon()
        perf = self.submit(secs=300, klass=q.CLASSES['perf'], solo=True, quiet_machine=True, measured=True)
        self.run_until(d, lambda: self.state_of(perf) == 'running', 1200, what='perf job starts')
        self.run_until(d, lambda: set(b) <= set(self.stopped_pids()), 60, what='builds frozen')
        self.crash(d)
        d = self.daemon()
        self.run_until(d, lambda: self.state_of(perf) == 'done', 900, what='adopted perf job finishes')
        self.run_for(d, 5)
        self.assertEqual(self.stopped_pids(), [], 'builds left stopped after the adopted job ended')

    # crash sources seen in daemon.log: a job that deletes its own out dir, a broken queue file
    def test_job_deleting_out_dir_does_not_kill_daemon(self):
        d = self.daemon()
        jid = self.submit(secs=20)
        self.run_until(d, lambda: self.state_of(jid) == 'running', 60, what='starts')
        import shutil
        out = d.jobs[jid]['job']['out']
        self.clock.sleep(30)
        self.world.advance()
        shutil.rmtree(out)
        nxt = self.submit(secs=20)
        self.run_until(d, lambda: self.state_of(nxt) == 'done', 300, what='queue keeps going')
        self.assertEqual(self.state_of(jid), 'done')

    def test_bad_queue_file_does_not_kill_daemon(self):
        d = self.daemon()
        bad = self.submit(secs=20)
        j = json.loads(Path(q.D['queue'], bad + '.json').read_text())
        del j['cmd']
        Path(q.D['queue'], bad + '.json').write_text(json.dumps(j))
        good = self.submit(secs=20)
        self.run_until(d, lambda: self.state_of(good) == 'done', 300, what='good job runs past a bad one')
        self.assertEqual(self.state_of(bad), 'done')
        self.assertEqual(json.loads(Path(q.D['done'], bad + '.json').read_text())['status'], 'error')

    def test_launch_failure_after_freeze_releases_everything(self):
        b = self.add_build()
        d = self.daemon()
        perf = self.submit(secs=60, klass=q.CLASSES['perf'], solo=True, quiet_machine=True, measured=True)
        real = q.buildq_owned_processes

        def boom(procs):   # fails only inside launch(), after the build roots were frozen
            if any(f.function == 'launch' for f in inspect.stack()):
                raise RuntimeError('probe failed')
            return real(procs)
        p = patch.object(q, 'buildq_owned_processes', boom)
        p.start()
        self.addCleanup(p.stop)
        self.run_until(d, lambda: self.state_of(perf) == 'done', 1200, what='failed launch is recorded')
        p.stop()
        self.assertEqual(self.stopped_pids(), [], 'builds left frozen by a failed launch')
        self.assertFalse(d.quiet_starting)
        fd = os.open(q.BUILD_EXEC_LOCK, os.O_RDWR)
        q.fcntl.flock(fd, q.fcntl.LOCK_SH | q.fcntl.LOCK_NB)   # raises if the daemon still holds it exclusively
        os.close(fd)
        nxt = self.submit(secs=20)
        self.run_until(d, lambda: self.state_of(nxt) == 'done', 600, what='queue keeps going')

    def test_slots_lowered_while_running(self):
        d = self.daemon()
        ids = [self.submit(secs=100) for _ in range(3)]
        self.run_until(d, lambda: len(d.jobs) == 3, 60, what='3 running')
        self.write_config({'slots': 1})
        more = [self.submit(secs=10) for _ in range(2)]
        self.run_until(d, lambda: all(self.state_of(i) == 'done' for i in ids + more), 600, what='all done')

    def test_quiet_slots_zero_holds(self):
        self.write_config({'slots': 0, 'game_cap': 0, 'idle_cap': 0})
        d = self.daemon()
        jid = self.submit(secs=20)
        self.run_for(d, 600, step=5)
        self.assertEqual(self.state_of(jid), 'queued')
        self.write_config({})
        self.run_until(d, lambda: self.state_of(jid) == 'done', 300, what='runs after quiet off')

    def test_result_record_failure_frees_slot(self):
        d = self.daemon()
        jid = self.submit(secs=20)
        with patch.object(q.Daemon, 'finish_record', side_effect=OSError(28, 'No space left on device')):
            self.run_until(d, lambda: self.state_of(jid) == 'done', 120, what='bare done record written')
        self.assertEqual(d.jobs, {})
        nxt = self.submit(secs=20)
        self.run_until(d, lambda: self.state_of(nxt) == 'done', 120, what='queue keeps going')

    def test_dead_lane_job_at_restart_resumes_its_processes(self):
        d = self.daemon()
        live = self.submit(secs=20000, cmd=['python3', '-I', '/x/live_session.py', 'sim', '20000', '0'])
        self.run_for(d, 5)
        perf = self.submit(secs=600, klass=q.CLASSES['perf'], solo=True, quiet_machine=True, measured=True)
        self.run_until(d, lambda: self.state_of(perf) == 'running', 1800, what='perf job starts')
        pid = d.jobs[live]['proc'].pid
        self.assertIn(pid, self.stopped_pids())
        straggler = self.world.spawn(['sim', '999999'], None)   # a child of the lane job, stopped with it
        Path(q.D['running'], live + '.lanestop').write_text(json.dumps([pid, straggler]))
        self.world.kill(straggler, signal.SIGSTOP)
        self.crash(d)
        self.world.kill(pid, signal.SIGKILL)                    # the lane job's root dies while the daemon is down
        d = self.daemon()
        self.assertNotIn(straggler, self.stopped_pids())
        self.assertFalse(Path(q.D['running'], live + '.lanestop').exists())

    def test_tick_exception_does_not_end_daemon(self):
        d = q.Daemon()
        d.log = lambda msg: self.log.append((self.clock.t, msg))
        calls = []

        def tick():
            calls.append(1)
            if len(calls) == 1:
                raise RuntimeError('fifo gone')
            return True   # second tick: a handoff
        old = signal.getsignal(signal.SIGTERM), signal.getsignal(signal.SIGUSR1)
        self.addCleanup(signal.signal, signal.SIGTERM, old[0])
        self.addCleanup(signal.signal, signal.SIGUSR1, old[1])
        with patch.object(d, 'tick', tick):
            d.run()
        os.close(d.fifo)
        self.assertEqual(len(calls), 2)
        self.assertTrue(any('tick failed' in m for _, m in self.log))

if __name__ == '__main__':
    unittest.main(verbosity=2)
