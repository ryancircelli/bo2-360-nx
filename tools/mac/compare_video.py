#!/usr/bin/env python3
"""Frame-compare a test recording against a reference recording.

  compare_video.py ours.mp4 reference.mp4 [--size 320x180] [--max-mad 12] [--quadrants] [--json out.json]

Both videos are decoded with ffmpeg to small grayscale frames, resampled to the same number of frames (by time, so a
different frame rate does not matter), and compared frame by frame: mean absolute difference (MAD, 0..255) and PSNR.
--quadrants also reports the four screen quarters separately (split screen: one bad player view shows up).
Exit 0 when the median MAD is under --max-mad and under 10 % of frames exceed twice it; 1 otherwise; 2 on errors.
Needs numpy (python3 -m venv /tmp/app-agent/venv && pip install numpy) and ffmpeg (FFMPEG=... to override).
Adapted in spirit from peggle1-nx tools/compare_video.py (thumbnail matching by MAD).
"""
import argparse, json, os, shutil, subprocess, sys
try:
    import numpy as np
except ImportError:
    sys.exit('compare_video: numpy is required')


def decode(path, w, h, ff):
    p = subprocess.run([ff, '-v', 'error', '-i', path, '-vf', f'scale={w}:{h}', '-pix_fmt', 'gray', '-f', 'rawvideo', '-'],
                       capture_output=True)
    if p.returncode or not p.stdout:
        sys.exit(f'compare_video: cannot decode {path}: {p.stderr.decode()[-300:]}')
    a = np.frombuffer(p.stdout, np.uint8)
    return a[: len(a) // (w * h) * w * h].reshape(-1, h, w).astype(np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ours'); ap.add_argument('ref')
    ap.add_argument('--size', default='320x180'); ap.add_argument('--max-mad', type=float, default=12.0)
    ap.add_argument('--quadrants', action='store_true'); ap.add_argument('--json')
    a = ap.parse_args()
    ff = os.environ.get('FFMPEG') or shutil.which('ffmpeg')
    if not ff:
        sys.exit('compare_video: ffmpeg not found')
    w, h = map(int, a.size.split('x'))
    x, r = decode(a.ours, w, h, ff), decode(a.ref, w, h, ff)
    n = min(len(x), len(r))
    xi = np.linspace(0, len(x) - 1, n).round().astype(int); ri = np.linspace(0, len(r) - 1, n).round().astype(int)
    x, r = x[xi], r[ri]
    d = np.abs(x - r)
    mad = d.mean(axis=(1, 2)); mse = ((x - r) ** 2).mean(axis=(1, 2))
    psnr = 10 * np.log10(255.0 ** 2 / np.maximum(mse, 1e-6))
    res = {'frames_compared': int(n), 'frames_ours': len(x), 'frames_ref': len(r), 'mad_median': float(np.median(mad)),
           'mad_p95': float(np.percentile(mad, 95)), 'mad_max': float(mad.max()), 'psnr_median': float(np.median(psnr)),
           'worst_frame_fraction': float(np.argmax(mad)) / max(n - 1, 1)}
    if a.quadrants:
        hh, hw = h // 2, w // 2
        q = {'tl': d[:, :hh, :hw], 'tr': d[:, :hh, hw:], 'bl': d[:, hh:, :hw], 'br': d[:, hh:, hw:]}
        res['quadrant_mad_median'] = {k: float(np.median(v.mean(axis=(1, 2)))) for k, v in q.items()}
    bad = float((mad > 2 * a.max_mad).mean())
    ok = res['mad_median'] < a.max_mad and bad < 0.10
    res['frames_over_2x_threshold'] = bad; res['pass'] = bool(ok)
    print(json.dumps(res, indent=1))
    if a.json:
        json.dump(res, open(a.json, 'w'), indent=1)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
