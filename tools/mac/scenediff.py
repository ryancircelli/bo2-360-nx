#!/usr/bin/env python3
"""Golden-frame diff for runq scenes: a per-view score and PASS/FAIL, tuned to the two known Metal bugs on Town.

  scenediff.py diff SCENE.json CAPTURES_DIR GOLDEN_DIR [--json OUT]   score a scene run against its golden frames
  scenediff.py score IMAGE [GOLDEN] [--views 4] [--json OUT]         score one frame (golden file or dir optional)
  scenediff.py ready SCENE.json CAPTURES_DIR [GOLDEN_DIR]            exit 0 once "need" frames are in play
  scenediff.py bless SCENE.json CAPTURES_DIR GOLDEN_DIR [--tag T]     add a run's frames to the golden pool

Frames: the scene's "captures" ("22:a") are <scene name>_<label>.png anywhere under CAPTURES_DIR. Frames are compared
at 1280x720, split into the scene's views ("golden": {"views": 4} = 2x2, 2 = top/bottom, 1 = whole frame).

Frame timing: the match fades in from black a variable time after the map loads (19 s in one run, over 26 s in
another), so a scene captures every couple of seconds and sets "golden": {"need": N}: leading frames that are not in
play yet (black, fading in, the loading card; see in_play) are skipped and the first N in-play frames are scored. runq
ends the run as soon as N are in (scene key "end_on_ready").

Goldens are a pool: every view of every PNG under GOLDEN_DIR (verified-clean runs of the same scene). Each test view is
matched to the golden views that look most like it (8x8-block luma distance, best*1.6+2, at most 8), so a shuffled
spawn still finds its pose. Per 16x16 block the group gives a median colour and a spread (max-min); blocks whose
spread is over max_spread (embers, fire, zombies) and the HUD boxes in "ignore" (points, ammo, round title) are not
scored.

Per view:
  ssim          structural similarity of the luma vs the best-matching golden view; catches a wrong pose or a broken
                frame, not small defects (bar 0.30).
  mad           mean absolute difference, 0-255, over RGB, vs the same view.
  gun_green     pixels in the held-weapon box (lower middle-right of the view) whose green beats red and blue by 12 %:
                the M1911 speckle bug paints the gunmetal green and red. Needs no golden. Noise-run 4p Town captures:
                the stock run with the bug (g1, 560-600 s) scores 69-192 per view; 12 clean runs (740 frames) at most 9
                in play; the Vulkan 4-way reference and the owner's reference frames score 0.
  green_blocks  blocks with at least 6 more such green pixels than any golden in the group (speckle anywhere).
  sheet_blocks  stable blocks whose colour moved at least sheet_rgb(30)+spread from the golden median and lost fine
                detail (at most lost_ratio 0.5x the least detailed golden, +0.3): a see-through chain-link fence
                drawn as a solid sheet (dark red with a dot grid; 5-30 % of the detail), or a black blotch. Fire and
                lava flares in front of the scene keep 50-90 % of the detail and do not count.
  sheet_cluster largest 8-connected group of sheet blocks. Clean frames (leave-one-out on four clean frames) reach 7;
                the g1 fence sheet pasted into the same pose scores 20.
  dark_blocks   the sheet_blocks that went darker (reported only).
  outliers      green_blocks + sheet_blocks.
pose          'unmatched' when ssim < min_pose_ssim 0.75 (a spawn spot no golden shows; matching spots score
                0.8-0.95): only gun_green and ssim count for that view; bless the run once it is checked clean.
A view fails when gun_green > 40, green_blocks > 4, sheet_cluster > 10, outliers > 40 or ssim < 0.30 (the scene's
"golden" object overrides any key of DEFAULTS). A scene passes when every view of every frame passes.
Needs numpy and Pillow (runq runs it through `uv run --with numpy --with pillow`).
"""
import argparse, json, os, shutil, sys

import numpy as np
from PIL import Image

DEFAULTS = {'views': 4, 'max_gun_green': 40, 'max_green_blocks': 4, 'max_sheet_cluster': 10, 'max_outliers': 40, 'min_ssim': 0.30,
            'gun_box': [0.55, 0.55, 0.75, 1.00],   # x0 y0 x1 y1 as fractions of a view: the held weapon
            # HUD boxes left out of the block scores (fractions of a view): points, ammo and its blood icon, which come
            # and go; the round title in the middle
            'ignore': [[0.76, 0.55, 1.00, 0.92], [0.40, 0.25, 0.60, 0.40]],
            # a frame is in play when every view is at least ready_luma of its best golden's brightness and its ssim
            # against it is at least ready_ssim (the loading card scores 0.12-0.24, the fade from black is half as bright)
            'ready_luma': 0.8, 'ready_ssim': 0.35, 'need': 0,  # need -1: every frame from the first in play
            # in-play frames to drop before scoring: the first one can still show low-res textures streaming in
            'settle': 0,
            'block': 16, 'sheet_rgb': 30, 'max_spread': 60, 'lost_ratio': 0.5, 'min_pose_ssim': 0.75, 'green_ratio': 1.12, 'green_block_px': 6,
            'fence_regions': None}
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


def small(v):
    return blocks(luma(v), 8, np.mean)


class Golden:
    """A pool of golden views: every view of every PNG in the golden dir (clean runs, any frame label). A test view
    is matched to the golden views that show the same pose (players can spawn on different spots), and the spread
    of that group says which blocks animate on their own (lava, embers, text fades, gun sway) and how much."""

    def __init__(self, paths, o):
        self.o, self.views, self.names = o, [], []
        for p in paths:
            for i, v in enumerate(views(load(p), o['views'])):
                self.views.append(v)
                self.names.append(f'{os.path.basename(p)}#{i + 1}')
        self.small = [small(v) for v in self.views]

    def group(self, v):
        d = np.array([float(np.abs(small(v) - s).mean()) for s in self.small])
        best = float(d.min())
        idx = [i for i in np.argsort(d) if d[i] <= best * 1.6 + 2.0][:8]
        return idx, best


def largest_cluster(m):
    """Size of the largest 8-connected group of True cells: a filled-in fence is one solid patch of blocks, while
    clean frame-to-frame noise (embers, lava, gun sway) is scattered."""
    seen = np.zeros_like(m, dtype=bool)
    best = 0
    for y, x in zip(*np.nonzero(m)):
        if seen[y, x]:
            continue
        seen[y, x] = True
        stack, size = [(y, x)], 0
        while stack:
            cy, cx = stack.pop()
            size += 1
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    ny, nx = cy + dy, cx + dx
                    if 0 <= ny < m.shape[0] and 0 <= nx < m.shape[1] and m[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
        best = max(best, size)
    return best


def block_stats(v, n):
    mean = np.stack([blocks(v[..., c], n, np.mean) for c in range(3)], -1)
    return mean, blocks(detail(luma(v)), n, np.mean)


def score_view(v, gold, o, view_idx=0):
    x0, y0, x1, y1 = o['gun_box']
    h, w = v.shape[:2]
    gm = green_mask(v, o['green_ratio'])
    gun = gm[int(y0 * h):int(y1 * h), int(x0 * w):int(x1 * w)]
    res = {'gun_green': int(gun.sum())}
    if gold is None or not gold.views:
        res['verdict'] = 'FAIL' if res['gun_green'] > o['max_gun_green'] else 'PASS'
        return res
    n = o['block']
    idx, best = gold.group(v)
    gs = [gold.views[i] for i in idx]
    g = gs[0]
    lv = luma(v)
    mv, dv = block_stats(v, n)
    st = [block_stats(x, n) for x in gs]
    gmeans = np.stack([m for m, _ in st])                      # k x by x bx x 3
    gdet = np.stack([d for _, d in st])
    med = np.median(gmeans, 0)
    spread = (gmeans.max(0) - gmeans.min(0)).sum(-1)           # how much each block moves between clean frames
    if len(gs) < 2:
        spread = spread + o['sheet_rgb']                       # one golden: no measure of motion, be lenient
    moved = np.abs(mv - med).sum(-1)
    stable = spread <= o['max_spread']
    by, bx = stable.shape
    hud = np.ones_like(stable)                                 # False on the HUD boxes in o['ignore']
    for x0_, y0_, x1_, y1_ in o['ignore']:
        hud[int(y0_ * by):int(np.ceil(y1_ * by)), int(x0_ * bx):int(np.ceil(x1_ * bx))] = False
    stable &= hud
    # fine detail gone: a filled-in mesh is flat (detail 5-30 % of the golden's); fire, lava flares and embers in
    # front of the scene brighten a block but keep 50-90 % of its detail
    lost = dv <= gdet.min(0) * o['lost_ratio'] + 0.3
    sheet_b = stable & lost & (moved >= o['sheet_rgb'] + spread)
    regions = o.get('fence_regions')
    if regions and view_idx < len(regions):
        x0, y0, x1, y1 = regions[view_idx]
        region = np.zeros_like(sheet_b)
        region[int(y0 * by):int(np.ceil(y1 * by)), int(x0 * bx):int(np.ceil(x1 * bx))] = True
        sheet_b &= region
    dark_b = sheet_b & (mv.sum(-1) < med.sum(-1))
    green_g = np.stack([blocks(green_mask(x, o['green_ratio']).astype(np.float32), n, np.sum) for x in gs]).max(0)
    green_b = blocks(gm.astype(np.float32), n, np.sum) - green_g
    green_b[~hud] = 0                                          # the HUD points are drawn in each player's colour
    res.update(match=gold.names[idx[0]], group=len(gs), ssim=round(ssim(lv, luma(g)), 3),
               mad=round(float(np.abs(v - g).mean()), 1), stable_pct=round(float(stable.mean()) * 100),
               green_blocks=int((green_b >= o['green_block_px']).sum()), sheet_blocks=int(sheet_b.sum()),
               dark_blocks=int(dark_b.sum()), sheet_cluster=largest_cluster(sheet_b))
    res['outliers'] = res['green_blocks'] + res['sheet_blocks']
    res['_sheet'] = sheet_b
    return judge(res, o)


def persist(frames_views, o):
    """A broken fence texture stays broken from frame to frame; a fire flare, an explosion glow or a passing zombie
    does not. With two or more scored frames, a sheet block only counts when a sheet block sits on it or next to it in
    another frame of the same view (the players stand still), and sheet_cluster is recomputed on those blocks."""
    if len(frames_views) < 2:
        return
    for i in range(len(frames_views[0])):
        ms = [vs[i].get('_sheet') for vs in frames_views]
        if any(m is None for m in ms):
            continue
        grown = []
        for m in ms:                                           # 3x3 dilation: one block of slack
            g = m.copy()
            g[1:] |= m[:-1]; g[:-1] |= m[1:]
            g2 = g.copy()
            g2[:, 1:] |= g[:, :-1]; g2[:, :-1] |= g[:, 1:]
            grown.append(g2)
        for k, vs in enumerate(frames_views):
            r = vs[i]
            other = np.zeros_like(ms[k])
            for j, g in enumerate(grown):
                if j != k:
                    other |= g
            keep = ms[k] & other
            r['sheet_cluster_frame'] = r['sheet_cluster']
            r['sheet_cluster'] = largest_cluster(keep)
            judge(r, o)


def judge(res, o):
    res.pop('verdict', None); res.pop('why', None)
    # Block scores compare like with like: a player who spawned on a spot no golden shows (ssim 0.5-0.65, against
    # 0.8-0.95 for a matching spot) moves every block, so only the pistol score counts and the view asks for a bless.
    res['pose'] = 'matched' if res['ssim'] >= o['min_pose_ssim'] else 'unmatched'
    why = []
    if res['gun_green'] > o['max_gun_green']:
        why.append('gun speckle')
    if res['pose'] == 'matched':
        if res['green_blocks'] > o['max_green_blocks']:
            why.append('green speckle blocks')
        if res['sheet_cluster'] > o['max_sheet_cluster']:
            why.append('solid sheet')
        if res['outliers'] > o['max_outliers']:
            why.append('block outliers')
    if res['ssim'] < o['min_ssim']:
        why.append('low ssim')
    res['verdict'] = 'FAIL' if why else 'PASS'
    if why:
        res['why'] = why
    return res


def in_play(img, gold, o):
    """True when the frame shows the match: not black, not fading in, not a loading card."""
    for v in views(img, o['views']):
        lv = luma(v)
        if gold is None or not gold.views:
            if lv.mean() < 25:
                return False
            continue
        g = luma(gold.views[gold.group(v)[0][0]])
        if lv.mean() < o['ready_luma'] * g.mean() or ssim(lv, g) < o['ready_ssim']:
            return False
    return True


def pick_frames(scene, caps, gold, o):
    """(label, name, path or None, scored) for the scene's frames. With "need" N, leading frames that are not yet in
    play (black, fading in, loading) are skipped and the first N from the first in-play frame are scored; without it,
    every frame is scored."""
    fs = [(label, name, find(caps, name)) for label, name in frames(scene)]
    need = o.get('need') or 0
    if not need:
        return [(l, n, p, True) for l, n, p in fs]
    out, started, settle = [], False, o.get('settle') or 0
    for l, n, p in fs:
        if not started and p and in_play(load(p), gold, o):
            started = True
        if started and p and settle:
            settle -= 1
            out.append((l, n, p, False))
            continue
        take = bool(started and p) and (need < 0 or sum(1 for x in out if x[3]) < need)
        out.append((l, n, p, take))
    return out


def score(img_path, gold, o, keep_masks=False):
    vs = [score_view(v, gold, o, i) for i, v in enumerate(views(load(img_path), o['views']))]
    return vs if keep_masks else strip(vs)


def strip(vs):
    for v in vs:
        v.pop('_sheet', None)
    return vs


def golden_pool(path, o):
    if not path:
        return None
    if os.path.isfile(path):
        return Golden([path], o)
    ps = sorted(os.path.join(d, f) for d, _, fs in os.walk(path) for f in fs if f.endswith('.png'))
    return Golden(ps, o) if ps else None


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
        s += f'  ssim {r["ssim"]}  mad {r["mad"]}  [{r["match"]} +{r["group"] - 1}, stable {r["stable_pct"]}%]  outliers {r["outliers"]} (green {r["green_blocks"]}, sheet {r["sheet_blocks"]} cluster {r["sheet_cluster"]}'
        if 'sheet_cluster_frame' in r and r['sheet_cluster_frame'] != r['sheet_cluster']:
            s += f' persistent, {r["sheet_cluster_frame"]} in this frame alone'
        s += f', dark {r["dark_blocks"]})'
    if r.get('pose') == 'unmatched':
        s += '  (spawn spot not in the golden pool: block scores not counted; bless this run if it is clean)'
    return s + (f'  <- {", ".join(r["why"])}' if r.get('why') else '')


def cmd_diff(a):
    scene = json.load(open(a.scene))
    o = opts(scene)
    report = {'scene': scene['name'], 'golden': a.golden, 'options': o, 'frames': {}}
    ok = True
    gold = golden_pool(a.golden, o)
    report['golden_views'] = len(gold.views) if gold else 0
    picked = pick_frames(scene, a.captures, gold, o)
    report['skipped'] = [label for label, _, p, take in picked if p and not take]
    if o.get('need') and sum(1 for x in picked if x[3]) < o['need']:
        ok = False
        report['error'] = f'only {sum(1 for x in picked if x[3])} of {o["need"]} frames in play'
        print(f'FAIL  {report["error"]} (skipped as not in play: {" ".join(report["skipped"]) or "none"})')
    scored = []
    for label, name, cap, take in picked:
        if cap and not take:
            continue
        if not cap and o.get('need'):
            continue
        scored.append((label, name, cap, score(cap, gold, o, keep_masks=True) if cap else None))
    persist([vs for _, _, cap, vs in scored if cap], o)
    for label, name, cap, vs in scored:
        if not cap:
            report['frames'][label] = {'error': f'no capture {name}'}
            print(f'{label}: FAIL  no capture {name} under {a.captures}')
            ok = False
            continue
        strip(vs)
        fv = 'PASS' if all(v['verdict'] == 'PASS' for v in vs) else 'FAIL'
        ok &= fv == 'PASS'
        report['frames'][label] = {'capture': cap, 'verdict': fv, 'views': vs}
        print(f'{label}: {fv}  ({os.path.basename(cap)} vs {len(gold.views) if gold else 0} golden views)')
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
            w.update(ssim=min(v['ssim'] for v in vv), green_blocks=max(v['green_blocks'] for v in vv),
                     sheet_cluster=max(v['sheet_cluster'] for v in vv), outliers=max(v['outliers'] for v in vv))
        worst.append(w)
    report['worst_by_view'] = worst
    report['unmatched_views'] = sum(1 for f in report['frames'].values() for v in f.get('views', [])
                                    if v.get('pose') == 'unmatched')
    if a.json:
        json.dump(report, open(a.json, 'w'), indent=1)
    print(f'scenediff {scene["name"]}: {report["verdict"]}  worst by view: '
          + '  '.join(f'v{i + 1} ' + ' '.join(f'{k} {v}' for k, v in w.items()) for i, w in enumerate(worst)))
    return 0 if ok else 1


def cmd_score(a):
    o = dict(DEFAULTS, views=a.views)
    vs = score(a.image, golden_pool(a.golden, o), o)
    for i, v in enumerate(vs):
        print(fmt_view(i, v))
    if a.json:
        json.dump(vs, open(a.json, 'w'), indent=1)
    return 0 if all(v['verdict'] == 'PASS' for v in vs) else 1


def cmd_ready(a):
    """Exit 0 once the captures hold the scene's "need" in-play frames (boot_test END_ON_CMD ends the run there)."""
    scene = json.load(open(a.scene))
    o = opts(scene)
    need = o.get('need') or len(frames(scene))
    o['need'] = need
    n = sum(1 for x in pick_frames(scene, a.captures, golden_pool(a.golden, o), o) if x[3])
    print(f'{n}/{need} frames in play')
    return 0 if n >= need else 1


def cmd_bless(a):
    scene = json.load(open(a.scene))
    os.makedirs(a.golden, exist_ok=True)
    for _, name in frames(scene):
        cap = find(a.captures, name)
        if not cap:
            sys.exit(f'no capture {name} under {a.captures}')
        dst = os.path.join(a.golden, f'{a.tag}_{name}' if a.tag else name)
        shutil.copy2(cap, dst)
        print(f'golden: {dst}')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    d = sp.add_parser('diff')
    d.add_argument('scene'); d.add_argument('captures'); d.add_argument('golden'); d.add_argument('--json')
    s = sp.add_parser('score')
    s.add_argument('image'); s.add_argument('golden', nargs='?'); s.add_argument('--views', type=int, default=4)
    s.add_argument('--json')
    r = sp.add_parser('ready')
    r.add_argument('scene'); r.add_argument('captures'); r.add_argument('golden', nargs='?')
    b = sp.add_parser('bless')
    b.add_argument('scene'); b.add_argument('captures'); b.add_argument('golden')
    b.add_argument('--tag', help='prefix for the file names, so frames from several clean runs sit side by side')
    a = ap.parse_args()
    sys.exit({'diff': cmd_diff, 'score': cmd_score, 'ready': cmd_ready, 'bless': cmd_bless}[a.cmd](a))


if __name__ == '__main__':
    main()
