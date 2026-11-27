"""runq says why a quiet-machine boot_test run has no summary.json. python3 -I test_runq_summary.py"""
import importlib.machinery, importlib.util, os, tempfile
loader = importlib.machinery.SourceFileLoader('runq', os.path.expanduser('~/Games/Xbox360/bo2-build/runq/bin/runq'))
spec = importlib.util.spec_from_loader('runq', loader); rq = importlib.util.module_from_spec(spec); loader.exec_module(rq)
with tempfile.TemporaryDirectory() as t:
    open(os.path.join(t, 'summary.txt'), 'w').write('x\nthe game ended by itself with status 139 (1 = in-game error, 134/139 = crash)\n')
    assert 'status 139' in rq.missing_summary_reason(t, {}), 'crash cause not reported'
    assert 'wall 31' in rq.missing_summary_reason(os.path.join(t, 'none'), {'wall_s': 31})
    # the cause of 11-23: `-- boot_test.sh` without --route is refused at submit now
    assert rq.submit_refusal({'cmd': ['/x/boot_test.sh', '--app', 'A'], 'quiet_machine': {}}), 'route-less boot_test not refused'
print('ok')
