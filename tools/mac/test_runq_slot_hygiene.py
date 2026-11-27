"""Slot hygiene regression tests. No game, daemon or shared spool mutations."""
import importlib.util
import importlib.machinery
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
loader = importlib.machinery.SourceFileLoader('runq_under_test', str(HERE / 'runq'))
spec = importlib.util.spec_from_loader(loader.name, loader)
q = importlib.util.module_from_spec(spec)
loader.exec_module(q)


class SlotHygiene(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = Path(self.tmp.name)
        self.job = dict(id='test', name='live-test', owner='test-owner', out=str(self.out), cmd=['python3', '-I', '/bin/live_session.py'])
        (self.out / 'live.json').write_text(json.dumps(dict(state='ready', ready_at=1000)))

    def test_default_expiry_and_activity(self):
        self.assertEqual(q.live_idle_info(self.job, 1599, {})['live_idle_s'], 599)
        d = object.__new__(q.Daemon)
        d.c = {}
        messages = []
        d.log = messages.append
        j = dict(job=self.job)
        with patch.object(q, 'stream_agent', return_value='owner-agent'), patch.object(q.subprocess, 'Popen') as send:
            d.live_idle_guard(j, 1599)
            self.assertFalse((self.out / 'stop').exists())
            d.live_idle_guard(j, 1600)
            self.assertTrue((self.out / 'stop').exists())
            self.assertIn('no live control command', messages[0])
            self.assertEqual(send.call_args.args[0][:3], ['paseo', 'send', 'owner-agent'])
            d.live_idle_guard(j, 1700)
            self.assertEqual(send.call_count, 1)
        self.assertEqual(json.loads((self.out / 'IDLE-STOP.json').read_text())['live_idle_s'], 600)

    def test_driven_session_and_status_dont_extend(self):
        activity = self.out / 'last-control'
        activity.touch()
        os.utime(activity, (1590, 1590))
        self.assertEqual(q.live_idle_info(self.job, 1600, {})['live_idle_s'], 10)
        self.assertEqual(q.live_idle_info(self.job, 2200, {})['live_idle_s'], 610)
        self.assertEqual(activity.stat().st_mtime, 1590)

    def test_startup_not_expired_and_config_keep_capped(self):
        self.assertEqual(q.live_idle_info(self.job, 1600, {'live_idle_stop_s': 60})['live_idle_stop_s'], 60)
        self.job['cmd'] += ['--keep', '1200']
        self.assertEqual(q.live_idle_info(self.job, 1600, {})['live_idle_stop_s'], 1200)
        self.job['cmd'][-1] = '9999'
        self.assertEqual(q.live_idle_info(self.job, 1600, {'live_idle_stop_s': 9999})['live_idle_stop_s'], 1800)
        (self.out / 'live.json').write_text(json.dumps(dict(state='starting', started=0)))
        self.assertEqual(q.live_idle_info(self.job, 99999, {}), {})
        self.job['cmd'] = ['echo', 'x']
        self.assertEqual(q.live_idle_info(self.job, 99999, {}), {})

    def test_laptop_ssh_refused(self):
        for host in ('drawer-wsl', '100.73.92.76', '10.0.0.47', 'ryanr@drawer-wsl'):
            for cmd in (['ssh', host, 'true'], ['/usr/bin/ssh', '-o', 'BatchMode=yes', '-p', '22', host, 'true'],
                        ['env', 'X=1', 'ssh', host, 'true'], ['bash', '-lc', f'ssh {host} true']):
                with self.subTest(cmd=cmd):
                    self.assertIn('--where laptop', q.submit_refusal(dict(cmd=cmd)))
                    self.assertIn('--where laptop', q.submit_refusal(dict(cmd=cmd, where='any')))
        self.assertIsNone(q.submit_refusal(dict(cmd=['ssh', 'elsewhere', 'true'])))
        self.assertFalse(q.laptop_ssh_command(['echo', 'drawer-wsl']))
        self.assertFalse(q.laptop_ssh_command(['ssh', '-o', 'HostName=drawer-wsl', 'elsewhere', 'true']))
        self.assertIsNone(q.submit_refusal(dict(cmd=['true'], where='laptop')))

    def test_notification_uses_submitter_and_never_live_prefix(self):
        with patch.object(q, 'stream_agent', return_value='unrelated-live-stream') as lookup:
            self.assertEqual(q.job_agent(dict(agent='real-owner', name='live-slot-hygiene')), 'real-owner')
            self.assertEqual(q.job_agent(dict(name='live-slot-hygiene')), '')
            lookup.assert_not_called()
            self.assertEqual(q.job_agent(dict(owner='runq-slot-hygiene', name='live-test')), 'unrelated-live-stream')
            lookup.assert_called_once_with('runq-slot-hygiene')

    def test_client_control_touches_activity(self):
        # Load client into the real queue namespace, as runq dispatch does.
        ns = q.__dict__.copy()
        exec(compile((HERE / 'runq_live.py').read_text(), str(HERE / 'runq_live.py'), 'exec'), ns)
        (self.out / 'control').mkdir()
        self.assertEqual(ns['live_request'](dict(out=str(self.out), name='test'), 'ping', 0, lambda: False), '')
        self.assertTrue((self.out / 'last-control').exists())


if __name__ == '__main__':
    unittest.main()
