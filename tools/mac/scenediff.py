#!/usr/bin/env python3
"""Golden-frame diff for runq scenes: a per-view score and PASS/FAIL, tuned to the two known Metal bugs on Town.

  scenediff.py diff SCENE.json CAPTURES_DIR GOLDEN_DIR [--json OUT]   score a scene run against its golden frames
  scenediff.py score IMAGE [GOLDEN] [--views 4] [--json OUT]         score one frame (golden optional)
  scenediff.py bless SCENE.json CAPTURES_DIR GOLDEN_DIR               make a run's frames the golden frames

Frames: the scene's "captures" ("6:a") are <scene name>_<label>.png anywhere under CAPTURES_DIR; the golden for a
frame is GOLDEN_DIR/<same name>, else GOLDEN_DIR/default.png. Frames are compared at 1280x720, split into the
scene's views ("golden": {"views": 4} = 2x2, 2 = top/bottom, 1 = whole frame).

Per view:
  ssim         structural similarity of the luma (7x7 box windows); 1 = identical. Animated embers, fire and
               zombies keep it below 1 even on a correct frame, so its bar is low; it catches a wrong pose or a
               broken frame, not small defects.
  mad          mean absolute difference, 0-255, over RGB.
  gun_green    pixels in the held-weapon box (lower middle-right of the view) whose green beats red and blue by
               12 %: the M1911 speckle bug paints the gunmetal green and red. Calibrated on the noise-run 4p Town
               captures: the one stock run with the bug (g1, 560-600 s) scores 69-192 per view; 12 clean runs
               (ga1-ga6, ha1-ha3, qs1, rp1, 740 frames) score at most 9 in play (24 on a loading frame).
  green_blocks 16x16 blocks with at least 6 more such green pixels than the golden's block (speckle anywhere).
  sheet_blocks 16x16 blocks whose mean colour moved at least 30 (sum of |dR|+|dG|+|dB|) from the golden's and lost
               fine detail or gained a flat fill: a see-through chain-link fence drawn as a solid sheet (the bug's
               sheet is dark red with a dot grid, darker or lighter than the background it hides), or a black blotch.
  dark_blocks  the sheet_blocks that went darker (reported, not counted twice).
  outliers     green_blocks + sheet_blocks.
A view fails when gun_green > max_gun_green, outliers > max_outliers or ssim < min_ssim (thresholds in the scene's
"golden" object override the defaults below). A scene passes when every view of every frame passes.
Needs numpy and Pillow (runq runs it through `uv run --with numpy --with pillow`).
"""
import argparse, json, os, shutil, sys

import numpy as np
from PIL import Image

DEFAULTS = {'views': 4, 'max_gun_green': 40, 'max_outliers': 10, 'min_ssim': 0.30,
            'gun_box': [0.55, 0.55, 0.75, 1.00],   # x0 y0 x1 y1 as fractions of a view: the held weapon
            'block': 16, 'sheet_rgb': 30, 'green_ratio': 1.12, 'green_block_px': 6}
W, H = 1280, 720


def load(path):
    im = Image.open(path).convert('RGB')
    if im.size != (W, H):
        im = im.resize((W, H), Image.BILINEAR)
    return np.asarray(im, dtype=np.float32)


def views(img, n):
    if n == 4:
        h, w = H // 2, W // 2
        return [img[:h, :w], img[:h, w:], img[h:, :w], img[h:, w:]]
    if n == 2:
        return [img[:H // 2], img[H // 2:]]
    return [img]


def luma(v):
    return v[..., 0] * 0.299 + v[..., 1] * 0.587 + v[..., 2] * 0.114


def box(a, r):
    """Mean over a (2r+1)^2 window, edges clamped."""
    p = np.pad(a, r, mode='edge')
    c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    k = 2 * r + 1
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def ssim(a, b):
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ma, mb = box(a, 3), box(b, 3)
    va, vb = box(a * a, 3) - ma * ma, box(b * b, 3) - mb * mb
    cov = box(a * b, 3) - ma * mb
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
    return float(s.mean())


def green_mask(v, ratio):
    """Truly green pixels. Town is lit orange and red, and fire is yellow (green close to red), so a pixel whose
    green beats both red and blue by the ratio is the speckle (it is dim: green 15-60 on dark gunmetal)."""
    r, g, b = v[..., 0], v[..., 1], v[..., 2]
    return (g > r * ratio + 2) & (g > b * ratio + 2) & (g > 12)


def blocks(a, n, fn):
    h, w = a.shape[0] // n * n, a.shape[1] // n * n
    x = a[:h, :w].reshape(h // n, n, w // n, n)
    return fn(x, axis=(1, 3))


def detail(l):
    """Fine detail: absolute luma minus its 3x3 mean (wire mesh, gun edges)."""
    return np.abs(l - box(l, 1))


def score_view(v, g, o):
    x0, y0, x1, y1 = o['gun_box']
    h, w = v.shape[:2]
    gm = green_mask(v, o['green_ratio'])
    gun = gm[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]
    res = {'gun_green': int(gun.sum())}
    if g is None:
        res['verdict'] = 'FAIL' if res['gun_green'] > o['max_gun_green'] else 'PASS'
        return res
    n = o['block']
    lv, lg = luma(v), luma(g)
    gg = green_mask(g, o['green_ratio'])
    green_b = blocks(gm.astype(np.float32), n, np.sum) - blocks(gg.astype(np.float32), n, np.sum)
    mv = np.stack([blocks(v[..., c], n, np.mean) for c in range(3)], -1)
    mg = np.stack([blocks(g[..., c], n, np.mean) for c in range(3)], -1)
    moved = np.abs(mv - mg).sum(-1) >= o['sheet_rgb']
    det_v, det_g = blocks(detail(lv), n, np.mean), blocks(detail(lg), n, np.mean)
    sd_v, sd_g = blocks(lv, n, np.std), blocks(lg, n, np.std)
    sheet_b = moved & ((det_v <= det_g * 1.1 + 0.5) | (sd_v <= sd_g * 0.8 + 0.5))
    dark_b = sheet_b & (mv.sum(-1) < mg.sum(-1))
    res.update(ssim=round(ssim(lv, lg), 3), mad=round(float(np.abs(v - g).mean()), 1),
               green_blocks=int((green_b >= o['green_block_px']).sum()), sheet_blocks=int(sheet_b.sum()),
               dark_blocks=int(dark_b.sum()))
    res['outliers'] = res['green_blocks'] + res['sheet_blocks']
    why = []
    if res['gun_green'] > o['max_gun_green']:
        why.append('gun speckle')
    if res['outliers'] > o['max_outliers']:
        why.append('block outliers')
    if res['ssim'] < o['min_ssim']:
        why.append('low ssim')
    res['verdict'] = 'FAIL' if why else 'PASS'
    if why:
        res['why'] = why
    return res


def score(img_path, golden_path, o):
    img = load(img_path)
    gold = load(golden_path) if golden_path else None
    out = []
    for i, v in enumerate(views(img, o['views'])):
        g = views(gold, o['views'])[i] if gold is not None else None
        out.append(score_view(v, g, o))
    return out


def opts(scene):
    o = dict(DEFAULTS)
    gold = scene.get('golden')
    if isinstance(gold, dict):
        o.update({k: v for k, v in gold.items() if k in DEFAULTS})
    return o


def frames(scene):
    return [(c.split(':', 1)[1], f'{scene["name"]}_{c.split(":", 1)[1]}.png') for c in scene.get('captures', [])]


def find(root, name):
    for d, _, files in os.walk(root):
        if name in files:
            return os.path.join(d, name)
    return None


def fmt_view(i, r):
    s = f'  view {i + 1}: {r["verdict"]}  gun_green {r["gun_green"]}'
    if 'ssim' in r:
        s += f'  ssim {r["ssim"]}  mad {r["mad"]}  outliers {r["outliers"]} (green {r["green_blocks"]}, sheet {r["sheet_blocks"]}, dark {r["dark_blocks"]})'
    return s + (f'  <- {", ".join(r["why"])}' if r.get('why') else '')


def cmd_diff(a):
    scene = json.load(open(a.scene))
    o = opts(scene)
    report = {'scene': scene['name'], 'golden': a.golden, 'options': o, 'frames': {}}
    ok = True
    for label, name in frames(scene):
        cap = find(a.captures, name)
        gold = os.path.join(a.golden, name)
        if not os.path.exists(gold):
            gold = os.path.join(a.golden, 'default.png')
        if not cap:
            report['frames'][label] = {'error': f'no capture {name}'}
            print(f'{label}: FAIL  no capture {name} under {a.captures}')
            ok = False
            continue
        if not os.path.exists(gold):
            gold = None
        vs = score(cap, gold, o)
        fv = 'PASS' if all(v['verdict'] == 'PASS' for v in vs) else 'FAIL'
        ok &= fv == 'PASS'
        report['frames'][label] = {'capture': cap, 'golden': gold, 'verdict': fv, 'views': vs}
        print(f'{label}: {fv}  ({os.path.basename(cap)} vs {os.path.basename(gold) if gold else "no golden"})')
        for i, v in enumerate(vs):
            print(fmt_view(i, v))
    report['verdict'] = 'PASS' if ok else 'FAIL'
    # one line per view over all frames: the worst value of each score
    worst = []
    for i in range(o['views']):
        vv = [f['views'][i] for f in report['frames'].values() if 'views' in f]
        if not vv:
            continue
        w = {'gun_green': max(v['gun_green'] for v in vv)}
        if all('ssim' in v for v in vv):
            w.update(ssim=min(v['ssim'] for v in vv), outliers=max(v['outliers'] for v in vv))
        worst.append(w)
    report['worst_by_view'] = worst
    if a.json:
        json.dump(report, open(a.json, 'w'), indent=1)
    print(f'scenediff {scene["name"]}: {report["verdict"]}  worst by view: '
          + '  '.join(f'v{i + 1} ' + ' '.join(f'{k} {v}' for k, v in w.items()) for i, w in enumerate(worst)))
    return 0 if ok else 1


def cmd_score(a):
    o = dict(DEFAULTS, views=a.views)
    vs = score(a.image, a.golden, o)
    for i, v in enumerate(vs):
        print(fmt_view(i, v))
    if a.json:
        json.dump(vs, open(a.json, 'w'), indent=1)
    return 0 if all(v['verdict'] == 'PASS' for v in vs) else 1


def cmd_bless(a):
    scene = json.load(open(a.scene))
    os.makedirs(a.golden, exist_ok=True)
    for _, name in frames(scene):
        cap = find(a.captures, name)
        if not cap:
            sys.exit(f'no capture {name} under {a.captures}')
        shutil.copy2(cap, os.path.join(a.golden, name))
        print(f'golden: {os.path.join(a.golden, name)}')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    d = sp.add_parser('diff')
    d.add_argument('scene'); d.add_argument('captures'); d.add_argument('golden'); d.add_argument('--json')
    s = sp.add_parser('score')
    s.add_argument('image'); s.add_argument('golden', nargs='?'); s.add_argument('--views', type=int, default=4)
    s.add_argument('--json')
    b = sp.add_parser('bless')
    b.add_argument('scene'); b.add_argument('captures'); b.add_argument('golden')
    a = ap.parse_args()
    sys.exit({'diff': cmd_diff, 'score': cmd_score, 'bless': cmd_bless}[a.cmd](a))


if __name__ == '__main__':
    main()
