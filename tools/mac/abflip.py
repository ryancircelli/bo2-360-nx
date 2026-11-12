#!/usr/bin/env python3
"""Analyze benchmark abarm events and frame_ms (column 3) in perf.tsv.

Frames must lie wholly after the arm's settle interval. 1% low is the
reciprocal of the mean frame time of the slowest ceil(1% * n) frames.
"""
import argparse
import bisect
from collections import defaultdict
from dataclasses import dataclass, field
import math
from pathlib import Path


@dataclass
class Segment:
    start: float
    arm: int
    factors: dict
    settle: float
    end: float = math.inf
    frames: list = field(default_factory=list)


def load(path, settle_override=None):
    events, rows = [], []
    with open(path) as source:
        for line in source:
            c = line.rstrip('\n').split('\t')
            if c[0] == '#event' and len(c) >= 3 and c[2] in ('abarm', 'route_end'):
                fields = dict(v.split('=', 1) for v in c[3:] if '=' in v)
                events.append((float(c[1]), c[2], fields))
            elif line[:1].isdigit() and len(c) >= 3:
                try:
                    t, ms = float(c[1]), float(c[2])
                    if math.isfinite(t) and math.isfinite(ms) and ms > 0:
                        rows.append((t, ms))
                except ValueError:
                    pass
    segments = []
    for t, name, fields in sorted(events, key=lambda e: e[0]):
        if segments:
            segments[-1].end = min(segments[-1].end, t)
        if name == 'abarm' and int(fields['arm']) >= 0:
            factors = dict((k, int(v)) for k, v in
                           (part.split('=', 1) for part in fields.get('factors', '').split(',') if part))
            settle = settle_override if settle_override is not None else float(fields.get('settle_s', 3))
            segments.append(Segment(t, int(fields['arm']), factors, settle * 1000))
    if not segments:
        raise ValueError('no benchmark abarm events found')
    starts = [s.start for s in segments]
    skipped = defaultdict(int)
    for t, ms in rows:
        i = bisect.bisect_right(starts, t) - 1
        if i < 0 or t >= segments[i].end:
            skipped['outside'] += 1
        elif t - ms < segments[i].start + segments[i].settle:
            skipped['settle_or_boundary'] += 1
        else:
            segments[i].frames.append(ms)
    return segments, skipped


def percentile(values, fraction):
    return values[min(len(values) - 1, int(fraction * len(values)))]


def describe(frames):
    if not frames:
        return 'frames=0'
    v = sorted(frames)
    slow = v[-max(1, math.ceil(len(v) * .01)):]
    return ('frames=%d avg_fps=%.2f p50_ms=%.2f p90_ms=%.2f p99_ms=%.2f '
            '1%%_low_fps=%.2f >25ms=%.2f%% >33.4ms=%.2f%%' %
            (len(v), 1000 * len(v) / sum(v), percentile(v, .5), percentile(v, .9),
             percentile(v, .99), 1000 * len(slow) / sum(slow),
             100 * sum(x > 25 for x in v) / len(v), 100 * sum(x > 33.4 for x in v) / len(v)))


def sign_p(wins, losses):
    n = wins + losses
    return min(1., 2 * sum(math.comb(n, k) for k in range(min(wins, losses) + 1)) / 2 ** n) if n else 1.


def report(segments, skipped):
    arms = defaultdict(list)
    factors = defaultdict(lambda: {0: [], 1: []})
    for s in segments:
        arms[s.arm].extend(s.frames)
        for name, on in s.factors.items():
            factors[name][on].extend(s.frames)
    print('Frames excluded:', dict(skipped))
    for arm, frames in sorted(arms.items()):
        print('arm', arm, describe(frames))
    for name, states in sorted(factors.items()):
        for on in (0, 1):
            print('factor', name, 'on' if on else 'off', describe(states[on]))
    print('Adjacent segment p90 comparisons (overlapping pairs; descriptive, not independent):')
    signs = defaultdict(lambda: [0, 0, 0])
    for i, (a, b) in enumerate(zip(segments, segments[1:])):
        if a.end != b.start or not a.frames or not b.frames:
            continue
        pa, pb = percentile(sorted(a.frames), .9), percentile(sorted(b.frames), .9)
        winner = 'tie' if pa == pb else ('A' if pa < pb else 'B')
        print(' pair=%d A:arm=%d p90=%.2f B:arm=%d p90=%.2f lower=%s' % (i, a.arm, pa, b.arm, pb, winner))
        changed = [name for name in a.factors.keys() & b.factors.keys() if a.factors[name] != b.factors[name]]
        # A factor's sign test uses only adjacent pairs holding the other factor fixed.
        if len(changed) == 1:
            name = changed[0]
            on_wins = (pa < pb and a.factors[name]) or (pb < pa and b.factors[name])
            signs[name][2 if pa == pb else (0 if on_wins else 1)] += 1
    for name in sorted(factors):
        on, off, ties = signs[name]
        print('sign_test', name, 'on_wins=%d off_wins=%d ties=%d two_sided_p=%.4g (other factor fixed)' %
              (on, off, ties, sign_p(on, off)))
    print('Sign-test p values assume independent pairs; adjacent overlapping pairs and route drift limit inference.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('perf_tsv', type=Path)
    parser.add_argument('--settle-s', type=float, help='override settle_s recorded in each event (default 3 if absent)')
    args = parser.parse_args()
    if args.settle_s is not None and args.settle_s < 0:
        parser.error('--settle-s must be nonnegative')
    try:
        report(*load(args.perf_tsv, args.settle_s))
    except (ValueError, OSError) as e:
        parser.exit(1, str(e) + '\n')


if __name__ == '__main__':
    main()
