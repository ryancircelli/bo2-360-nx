#!/usr/bin/env python3
"""Frame-time statistics from a bo2perf log (docs/mac/perf-log-format.md), or from the game log's [benchmark] line.

  python3 -I tools/mac/perf_summary.py run.tsv [--skip-s 5] [--baseline other.tsv] [--tolerance 5] [--json out.json]

Exit status: 0 normally; 1 with --baseline when this run is worse by more than --tolerance percent in average FPS,
1 % low or p99, or has synchronous compiles the baseline did not; 2 for an unreadable file.
Standard library only.
"""
import argparse, json, math, re, sys


def fail(msg):
    print(f'perf_summary: {msg}', file=sys.stderr)
    sys.exit(2)


def parse_log(path):
    """-> dict(meta, columns, rows (list of dict col -> float|None), events (t_ms, name, fields), end)"""
    meta, events, rows, cols, end, version = {}, [], [], None, None, None
    with open(path, encoding='utf-8', errors='replace') as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\r\n')
            if not line:
                continue
            if n == 1:
                m = re.match(r'# bo2perf (\d+)$', line)
                if not m:
                    return None
                version = int(m.group(1))
                if version != 1:
                    fail(f'{path}: format version {version} is not supported (this tool reads 1)')
                continue
            if line.startswith('#event\t'):
                p = line.split('\t')
                try:
                    t = float(p[1])
                except (IndexError, ValueError):
                    continue
                events.append((t, p[2] if len(p) > 2 else '', dict(kv.split('=', 1) for kv in p[3:] if '=' in kv)))
            elif line.startswith('# end'):
                end = dict(kv.split('=', 1) for kv in line.split('\t')[1:] if '=' in kv)
            elif line.startswith('#'):
                p = line[1:].strip().split('\t', 1)
                if len(p) == 2:
                    meta[p[0].strip()] = p[1].strip()
            elif cols is None:
                cols = line.split('\t')
            else:
                v = line.split('\t')
                row = {}
                for c, x in zip(cols, v):
                    try:
                        row[c] = None if x == '-' else float(x)
                    except ValueError:
                        row[c] = None
                rows.append(row)
    if version is None:
        return None
    for need in ('frame', 't_ms', 'frame_ms'):
        if cols is None or need not in cols:
            fail(f'{path}: no "{need}" column')
    return {'meta': meta, 'columns': cols, 'rows': rows, 'events': events, 'end': end}


BENCH = re.compile(r'\[benchmark\] (\d+) frames in (\d+) s: ([\d.]+) FPS average, median ([\d.]+) ms \(([\d.]+) FPS\), '
                   r'p90 ([\d.]+) ms, p99 ([\d.]+) ms, worst ([\d.]+) ms; over 33\.3 ms ([\d.]+) %, over 50 ms ([\d.]+) %')


def parse_benchmark_line(path):
    last = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = BENCH.search(line)
            if m:
                last = m
    if not last:
        return None
    g = last.groups()
    return {'frames': int(g[0]), 'duration_s': float(g[1]), 'fps_avg': float(g[2]), 'ft_median': float(g[3]),
            'ft_p90': float(g[5]), 'ft_p99': float(g[6]), 'ft_worst': float(g[7]), 'over33_pct': float(g[8]),
            'over50_pct': float(g[9]), 'source': 'benchmark-line'}


def pct(sorted_vals, p):
    if not sorted_vals:
        return None
    return sorted_vals[min(len(sorted_vals) - 1, int(p * len(sorted_vals)))]


def mean(v):
    return sum(v) / len(v) if v else None


def low_fps(ft, share):
    """1000 / mean frame time of the slowest `share` of frames (at least one frame)."""
    k = max(1, int(math.ceil(len(ft) * share)))
    worst = sorted(ft, reverse=True)[:k]
    return 1000.0 / mean(worst) if worst and mean(worst) else None


def stats(rows):
    ft = [r['frame_ms'] for r in rows if r.get('frame_ms') is not None]
    if not ft:
        return None
    s = sorted(ft)
    total_ms = sum(ft)
    out = {
        'frames': len(ft), 'duration_s': total_ms / 1000.0,
        'fps_avg': 1000.0 * len(ft) / total_ms if total_ms else None,
        'ft_avg': mean(ft), 'ft_median': pct(s, 0.5), 'ft_p90': pct(s, 0.9), 'ft_p99': pct(s, 0.99),
        'ft_p999': pct(s, 0.999), 'ft_worst': s[-1],
        'fps_1pct_low': low_fps(ft, 0.01), 'fps_01pct_low': low_fps(ft, 0.001),
        'over16_pct': 100.0 * sum(x > 16.8 for x in ft) / len(ft),
        'over33_pct': 100.0 * sum(x > 33.4 for x in ft) / len(ft),
        'over50_pct': 100.0 * sum(x > 50.0 for x in ft) / len(ft),
        'source': 'bo2perf',
    }
    for col in ('cpu_ms', 'gpu_ms', 'present_wait_ms'):
        v = sorted(r[col] for r in rows if r.get(col) is not None)
        if v:
            out[col] = {'avg': mean(v), 'p99': pct(v, 0.99), 'worst': v[-1]}
    comp = [r['compile_ms'] for r in rows if r.get('compile_ms')]
    if any('compile_ms' in r for r in rows):
        out['compile_frames'] = len(comp)
        out['compile_total_ms'] = sum(comp)
    if 'cpu_ms' in out and 'gpu_ms' in out:
        a, b = out['cpu_ms']['avg'], out['gpu_ms']['avg']
        out['bound'] = 'gpu' if b > a * 1.1 else 'cpu' if a > b * 1.1 else 'balanced'
        if out.get('present_wait_ms') and out['present_wait_ms']['avg'] > 0.5 * out['ft_avg']:
            out['bound'] = 'vsync/present-limited'
    return out


def window(log, skip_s):
    t0 = None
    for t, name, _ in log['events']:
        if name == 'loading_done':
            t0 = t
            break
    if skip_s is not None:
        first = next((r['t_ms'] for r in log['rows'] if r.get('t_ms') is not None), 0)
        t0 = first + skip_s * 1000.0
    t1 = next((t for t, name, _ in log['events'] if name == 'route_end'), None)
    rows = [r for r in log['rows'] if r.get('t_ms') is not None and (t0 is None or r['t_ms'] >= t0) and (t1 is None or r['t_ms'] <= t1)]
    segs, cur = [], None
    for t, name, f in log['events']:
        if name == 'segment':
            if cur:
                cur[2] = t
            cur = [f.get('name', f'segment{len(segs) + 1}'), t, None]
            segs.append(cur)
    out = {}
    for name, a, b in segs:
        part = [r for r in rows if r['t_ms'] >= a and (b is None or r['t_ms'] < b)]
        st = stats(part)
        if st:
            out[name] = st
    return rows, out


def fmt(st, indent=''):
    L = []
    def line(s): L.append(indent + s)
    if st['source'] == 'benchmark-line':
        line(f"{st['frames']} frames in {st['duration_s']:.0f} s (from the [benchmark] line)")
        line(f"fps avg {st['fps_avg']:.1f}   ft median {st['ft_median']:.1f}  p90 {st['ft_p90']:.1f}  p99 {st['ft_p99']:.1f}  worst {st['ft_worst']:.1f} ms")
        line(f"over 33.3 ms {st['over33_pct']:.1f} %   over 50 ms {st['over50_pct']:.1f} %")
        return '\n'.join(L)
    line(f"{st['frames']} frames in {st['duration_s']:.1f} s")
    line(f"fps avg {st['fps_avg']:.1f}   1% low {st['fps_1pct_low']:.1f}   0.1% low {st['fps_01pct_low']:.1f}")
    line(f"frame ms: avg {st['ft_avg']:.2f}  median {st['ft_median']:.2f}  p90 {st['ft_p90']:.2f}  p99 {st['ft_p99']:.2f}  "
         f"p99.9 {st['ft_p999']:.2f}  worst {st['ft_worst']:.2f}")
    line(f"over 16.7 ms {st['over16_pct']:.1f} %   over 33.3 ms {st['over33_pct']:.1f} %   over 50 ms {st['over50_pct']:.1f} %")
    for col, label in (('cpu_ms', 'cpu'), ('gpu_ms', 'gpu'), ('present_wait_ms', 'present wait')):
        if col in st:
            c = st[col]
            line(f"{label:13s} ms: avg {c['avg']:.2f}  p99 {c['p99']:.2f}  worst {c['worst']:.2f}")
    if 'bound' in st:
        line(f"limited by: {st['bound']}")
    if 'compile_frames' in st:
        line(f"synchronous compiles: {st['compile_frames']} frames, {st['compile_total_ms']:.1f} ms total")
    return '\n'.join(L)


def compare(cur, base, tol):
    """-> (lines, regressed)"""
    L, bad = [], False
    def row(label, a, b, higher_better):
        nonlocal bad
        if a is None or b is None or b == 0:
            return
        d = 100.0 * (a - b) / b
        worse = (d < -tol) if higher_better else (d > tol)
        bad |= worse
        L.append(f"  {label:14s} {a:9.2f}  vs {b:9.2f}  {d:+6.1f} %{'  WORSE' if worse else ''}")
    row('fps avg', cur['fps_avg'], base['fps_avg'], True)
    row('1% low fps', cur.get('fps_1pct_low'), base.get('fps_1pct_low'), True)
    row('frame p99 ms', cur['ft_p99'], base['ft_p99'], False)
    for col in ('cpu_ms', 'gpu_ms', 'present_wait_ms'):
        if col in cur and col in base:
            row(col, cur[col]['avg'], base[col]['avg'], False)
    if cur.get('compile_frames', 0) > base.get('compile_frames', 0):
        L.append(f"  compiles: {cur['compile_frames']} frames vs {base.get('compile_frames', 0)}  WORSE")
        bad = True
    return L, bad


def load(path, skip_s):
    log = parse_log(path)
    if log is None:
        b = parse_benchmark_line(path)
        if b is None:
            fail(f'{path}: neither a bo2perf log (first line "# bo2perf 1") nor a game log with a [benchmark] summary line')
        return {'meta': {}}, b, {}
    rows, segs = window(log, skip_s)
    st = stats(rows)
    if st is None:
        fail(f'{path}: no frames in the measured window')
    if log['end'] is None:
        st['incomplete'] = True
    return log, st, segs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--skip-s', type=float, default=None, help='skip this many seconds from the first frame (default: from the loading_done event)')
    ap.add_argument('--baseline')
    ap.add_argument('--tolerance', type=float, default=5.0)
    ap.add_argument('--json')
    a = ap.parse_args()
    log, st, segs = load(a.log, a.skip_s)
    meta = log['meta']
    print(f'== {a.log}' + (''.join(f'  {k}={meta[k]}' for k in ('build', 'renderer', 'gpu', 'resolution', 'players', 'route') if k in meta)))
    if st.get('incomplete'):
        print('(no "# end" line: the run was stopped before the game closed the file)')
    print(fmt(st))
    for name, s in segs.items():
        print(f'-- segment {name}')
        print(fmt(s, '   '))
    result = {'summary': st, 'segments': segs, 'meta': meta}
    rc = 0
    if a.baseline:
        blog, bst, _ = load(a.baseline, a.skip_s)
        print(f'== baseline {a.baseline}')
        print(fmt(bst))
        lines, bad = compare(st, bst, a.tolerance) if bst['source'] == 'bo2perf' and st['source'] == 'bo2perf' else \
            (['  (baseline is a [benchmark] line: comparing average FPS and p99 only)'] +
             compare({'fps_avg': st['fps_avg'], 'ft_p99': st['ft_p99']}, {'fps_avg': bst['fps_avg'], 'ft_p99': bst['ft_p99']}, a.tolerance)[0],
             compare({'fps_avg': st['fps_avg'], 'ft_p99': st['ft_p99']}, {'fps_avg': bst['fps_avg'], 'ft_p99': bst['ft_p99']}, a.tolerance)[1])
        print(f'== change (tolerance {a.tolerance:g} %)')
        print('\n'.join(lines) or '  nothing comparable')
        result['baseline'] = bst
        result['regressed'] = bad
        rc = 1 if bad else 0
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(result, f, indent=1)
    sys.exit(rc)


if __name__ == '__main__':
    main()
