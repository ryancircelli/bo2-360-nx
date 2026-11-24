#!/usr/bin/env python3
"""Compare two rexgpu-replay outputs draw by draw.

Each replay directory holds draws.jsonl (one record per draw) and
draws/NNNNNN_<target>.npy readbacks (full images or changed-rectangle deltas
against the previous readback of the same target, see README.md). This tool
rebuilds the full render target after every draw for both replays and reports
the first draw whose output differs beyond a tolerance, with its shader
hashes, render state and side-by-side + diff images.

  replay_diff.py diff A_DIR B_DIR -o REPORT_DIR [--tol 0.004] [--depth-tol 16]
                 [--max-pixels 0] [--delta] [--images 8] [--a-frame F] [--b-frame F]
  replay_diff.py png REPLAY_DIR -o PNG_DIR      # export every readback as PNG

--delta compares what each draw changed instead of the full target, so draws
that only differ because an earlier draw did are not reported; the report
then lists every independently divergent draw.

Requires numpy and Pillow.
"""

import argparse
import html
import json
import os
import sys

import numpy as np
from PIL import Image


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

class Replay:
    def __init__(self, path):
        self.path = path
        self.summary = {}
        summary_path = os.path.join(path, "summary.json")
        if os.path.exists(summary_path):
            with open(summary_path) as f:
                self.summary = json.load(f)
        self.draws = []
        with open(os.path.join(path, "draws.jsonl")) as f:
            for line in f:
                line = line.strip()
                if line:
                    self.draws.append(json.loads(line))

    def label(self):
        return self.summary.get("backend") or os.path.basename(os.path.normpath(self.path))

    def states(self):
        """Yields (draw record, {target: (key, image_after, image_before)})."""
        current = {}  # target label -> (key, image)
        for draw in self.draws:
            out = {}
            for t in draw.get("targets", []):
                label, key = t["target"], t["key"]
                prev = current.get(label)
                if prev is not None and prev[0] != key:
                    prev = None
                before = prev[1] if prev is not None else None
                if t["base"] == "full" or before is None:
                    if t["file"]:
                        image = np.load(os.path.join(self.path, t["file"]))
                    else:
                        image = before
                else:
                    image = before.copy()
                    if t["file"]:
                        x, y, w, h = t["rect"]
                        image[y:y + h, x:x + w] = np.load(os.path.join(self.path, t["file"]))
                if image is None:
                    continue
                current[label] = (key, image)
                out[label] = (key, image, before)
            yield draw, out


# ---------------------------------------------------------------------------
# Image helpers
# ---------------------------------------------------------------------------

def depth_to_float(raw):
    # D24S8 and D24FS8 both keep depth in the upper 24 bits; compare as unorm
    # bits, which is exact for D24S8 and monotonic for 20e4.
    return (raw >> 8).astype(np.float64)


def to_rgb8(image):
    if image.ndim == 2:
        d = depth_to_float(image)
        lo, hi = float(d.min()), float(d.max())
        n = (d - lo) / (hi - lo) if hi > lo else np.zeros_like(d)
        g = (n * 255.0 + 0.5).astype(np.uint8)
        return np.stack([g, g, g], axis=-1)
    rgb = np.nan_to_num(image[..., :3].astype(np.float64), nan=1.0, posinf=1.0, neginf=0.0)
    return (np.clip(rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def diff_heat(mask, err):
    h, w = mask.shape
    out = np.zeros((h, w, 3), np.uint8)
    if err is not None and err.size and err.max() > 0:
        n = np.clip(err / err.max(), 0, 1)
        out[..., 0] = (64 + 191 * n).astype(np.uint8) * mask
        out[..., 1] = (32 * (1 - n)).astype(np.uint8) * mask
    else:
        out[..., 0] = 255 * mask
    return out


def side_by_side(a, b, mask, err, path):
    tiles = [to_rgb8(a), to_rgb8(b), diff_heat(mask, err)]
    h = max(t.shape[0] for t in tiles)
    gap = np.full((h, 4, 3), 255, np.uint8)
    row = []
    for i, t in enumerate(tiles):
        if t.shape[0] < h:
            t = np.pad(t, ((0, h - t.shape[0]), (0, 0), (0, 0)))
        row.append(t)
        if i + 1 < len(tiles):
            row.append(gap)
    Image.fromarray(np.concatenate(row, axis=1)).save(path)


def compare(a, b, tol, depth_tol):
    """Returns (mask of differing pixels, per-pixel error) or (None, None)
    when the shapes differ."""
    if a.shape != b.shape:
        return None, None
    if a.ndim == 2:
        err = np.abs(depth_to_float(a) - depth_to_float(b))
        return err > depth_tol, err
    fa = a.astype(np.float64)
    fb = b.astype(np.float64)
    nan_mismatch = np.isnan(fa) != np.isnan(fb)
    err = np.abs(np.nan_to_num(fa, nan=0.0) - np.nan_to_num(fb, nan=0.0))
    err = np.where(nan_mismatch, np.inf, err).max(axis=-1)
    return err > tol, np.where(np.isinf(err), 1e9, err)


def compare_delta(a, before_a, b, before_b, tol, depth_tol):
    """Per-pixel error between the changes two backends made in one draw."""
    if a.ndim == 2:
        da = depth_to_float(a) - depth_to_float(before_a)
        db = depth_to_float(b) - depth_to_float(before_b)
        err = np.abs(da - db)
        return err > depth_tol, err
    fa = np.nan_to_num(a.astype(np.float64)) - np.nan_to_num(before_a.astype(np.float64))
    fb = np.nan_to_num(b.astype(np.float64)) - np.nan_to_num(before_b.astype(np.float64))
    nan_mismatch = np.any(np.isnan(a) != np.isnan(b), axis=-1)
    err = np.abs(fa - fb).max(axis=-1)
    err = np.where(nan_mismatch, 1e9, err)
    return err > tol, err


def changed(before, after):
    if before is None:
        return np.ones(after.shape[:2], bool)
    if after.ndim == 2:
        return before != after
    return np.any(before.view(np.uint32) != after.view(np.uint32), axis=-1)


# ---------------------------------------------------------------------------
# diff
# ---------------------------------------------------------------------------

def state_diff(da, db):
    out = []
    for k in sorted(set(da.get("regs", {})) | set(db.get("regs", {}))):
        va, vb = da.get("regs", {}).get(k), db.get("regs", {}).get(k)
        if va != vb:
            out.append((k, va, vb))
    return out


def draw_summary(d):
    return {
        "draw": d["draw"], "frame": d.get("frame"), "packet": d.get("packet"),
        "prim_type": d.get("prim_type"), "index_count": d.get("index_count"),
        "indexed": d.get("indexed"), "vs": d.get("vs"), "ps": d.get("ps"),
        "is_resolve": d.get("is_resolve"),
    }


def run_diff(args):
    A, B = Replay(args.a), Replay(args.b)
    os.makedirs(args.out, exist_ok=True)
    findings = []  # dicts
    structural = []
    n = min(len(A.draws), len(B.draws))
    if len(A.draws) != len(B.draws):
        structural.append(f"draw count differs: {len(A.draws)} vs {len(B.draws)}")

    def frame_filter(states, frame):
        for d, t in states:
            if frame is None or d.get("frame") == frame:
                yield d, t

    if args.a_frame is not None or args.b_frame is not None:
        n = min(sum(1 for d in A.draws if args.a_frame is None or d.get("frame") == args.a_frame),
                sum(1 for d in B.draws if args.b_frame is None or d.get("frame") == args.b_frame))
        structural = []
    for (da, ta), (db, tb) in zip(frame_filter(A.states(), args.a_frame),
                                  frame_filter(B.states(), args.b_frame)):
        idx = da["draw"]
        for field in ("prim_type", "index_count", "vs", "ps"):
            if da.get(field) != db.get(field):
                structural.append(f"draw {idx}: {field} differs: {da.get(field)} vs {db.get(field)}")
        for label in sorted(set(ta) | set(tb)):
            if label not in ta or label not in tb:
                findings.append({"draw": idx, "target": label, "kind": "missing",
                                 "detail": f"readback only in {'A' if label in ta else 'B'}",
                                 "record": da})
                continue
            key_a, img_a, before_a = ta[label]
            key_b, img_b, before_b = tb[label]
            mask, err = compare(img_a, img_b, args.tol, args.depth_tol)
            if mask is None:
                findings.append({"draw": idx, "target": label, "kind": "shape",
                                 "detail": f"{img_a.shape} vs {img_b.shape}", "record": da})
                continue
            if args.delta and before_a is not None and before_b is not None \
                    and before_a.shape == img_a.shape and before_b.shape == img_b.shape:
                # Compare what the draw changed: (after - before) in A against
                # (after - before) in B, only where either backend wrote. Error
                # inherited from earlier draws cancels out (fully for opaque
                # writes and untouched pixels, scaled by the blend factor when
                # blending), so later draws are not blamed for it.
                written = changed(before_a, img_a) | changed(before_b, img_b)
                mask, err = compare_delta(img_a, before_a, img_b, before_b,
                                          args.tol, args.depth_tol)
                mask = mask & written
            count = int(mask.sum())
            if count > args.max_pixels:
                findings.append({
                    "draw": idx, "target": label, "kind": "pixels", "pixels": count,
                    "total": int(mask.size), "max_err": float(err[mask].max()) if count else 0.0,
                    "record": da, "record_b": db, "a": img_a, "b": img_b, "mask": mask, "err": err,
                })

    first = findings[0] if findings else None
    for stale in os.listdir(args.out):
        if stale.startswith("draw") and stale.endswith(".png"):
            os.remove(os.path.join(args.out, stale))
    shown = findings if args.delta else findings[:1]
    shown = shown[:args.images]
    for i, f in enumerate(shown):
        if f["kind"] == "pixels":
            name = f"draw{f['draw']:06}_{f['target']}.png"
            side_by_side(f["a"], f["b"], f["mask"], f["err"], os.path.join(args.out, name))
            f["image"] = name

    divergent_draws = sorted({f["draw"] for f in findings})
    result = {
        "a": A.path, "b": B.path, "a_label": A.label(), "b_label": B.label(),
        "draws_compared": n, "mode": "delta" if args.delta else "absolute",
        "tolerance": args.tol, "depth_tolerance": args.depth_tol, "max_pixels": args.max_pixels,
        "structural": structural,
        "divergent_draws": divergent_draws,
        "findings": [{k: f.get(k) for k in ("draw", "target", "kind", "pixels", "max_err", "detail")}
                     | {"vs": f["record"].get("vs"), "ps": f["record"].get("ps")}
                     for f in findings],
        "first_divergent_draw": first["draw"] if first else None,
    }
    if first:
        result["first"] = {**draw_summary(first["record"]), "target": first["target"],
                           "kind": first["kind"], "pixels": first.get("pixels"),
                           "max_err": first.get("max_err"), "image": first.get("image")}
    with open(os.path.join(args.out, "result.json"), "w") as f:
        json.dump(result, f, indent=2)
    write_reports(args, A, B, result, findings, shown)

    if first:
        r = first["record"]
        print(f"FIRST DIVERGENT DRAW {first['draw']} (frame {r.get('frame')}, packet {r.get('packet')}) "
              f"target {first['target']}: {first.get('pixels', first['kind'])} px, "
              f"max err {first.get('max_err', 0):.4g}; VS {r.get('vs')} PS {r.get('ps')}")
        if args.delta and len(divergent_draws) > 1:
            print(f"independently divergent draws ({len(divergent_draws)}): "
                  + " ".join(map(str, divergent_draws[:40])))
    else:
        print(f"no divergence in {n} draws")
    for s in structural[:10]:
        print("STRUCTURAL:", s)
    print(f"report: {os.path.join(args.out, 'report.html')}")
    return 1 if (first or structural) else 0


def fmt_regs(record):
    return "\n".join(f"{k} = {v}" for k, v in record.get("regs", {}).items())


def write_reports(args, A, B, result, findings, shown):
    md = [f"# Replay diff: {A.label()} vs {B.label()}", "",
          f"- A: `{A.path}`", f"- B: `{B.path}`",
          f"- mode: {result['mode']}, tolerance {args.tol} (color), {args.depth_tol} (depth, 24-bit units), "
          f"max differing pixels {args.max_pixels}",
          f"- draws compared: {result['draws_compared']}",
          f"- divergent draws: {len(result['divergent_draws'])}", ""]
    if result["structural"]:
        md += ["## Structural differences", ""] + [f"- {s}" for s in result["structural"][:50]] + [""]
    if not findings:
        md += ["No divergent draw.", ""]
    for f in shown:
        r = f["record"]
        md += [f"## Draw {f['draw']} ({f['target']})", "",
               f"- frame {r.get('frame')}, packet {r.get('packet')} at {r.get('packet_base')}",
               f"- prim_type {r.get('prim_type')}, index_count {r.get('index_count')}, "
               f"indexed {r.get('indexed')}, resolve {r.get('is_resolve')}",
               f"- VS `{r.get('vs')}`  PS `{r.get('ps')}`"]
        if f["kind"] == "pixels":
            md += [f"- {f['pixels']} of {f['total']} pixels differ, max error {f['max_err']:.6g}",
                   "", f"![A | B | diff]({f.get('image')})"]
        else:
            md += [f"- {f['kind']}: {f.get('detail')}"]
        textures = r.get("textures", [])
        if textures:
            md += ["", "Texture fetch constants:", ""]
            md += [f"- {t['stage']} fetch {t['fetch']}: format {t['format']} base {t['base']} "
                   f"dwords {' '.join(t['dwords'])}" for t in textures]
        if f.get("record_b"):
            sd = state_diff(r, f["record_b"])
            if sd:
                md += ["", "Register state differs between the two records:", ""]
                md += [f"- {k}: {va} vs {vb}" for k, va, vb in sd]
        md += ["", "<details><summary>Render state</summary>", "", "```", fmt_regs(r), "```", "",
               "</details>", ""]
    with open(os.path.join(args.out, "report.md"), "w") as f:
        f.write("\n".join(md))

    h = ["<!doctype html><meta charset=utf-8><title>replay diff</title>",
         "<style>body{font:14px system-ui;margin:2em}img{image-rendering:pixelated;max-width:100%;"
         "border:1px solid #ccc}pre{background:#f6f6f6;padding:8px;font-size:12px}"
         "td,th{padding:2px 8px;text-align:left}</style>",
         f"<h1>Replay diff: {html.escape(A.label())} vs {html.escape(B.label())}</h1>",
         f"<p>A: <code>{html.escape(A.path)}</code><br>B: <code>{html.escape(B.path)}</code><br>"
         f"mode {result['mode']}, color tolerance {args.tol}, depth tolerance {args.depth_tol}, "
         f"{result['draws_compared']} draws compared, {len(result['divergent_draws'])} divergent.</p>"]
    for s in result["structural"][:50]:
        h.append(f"<p><b>structural:</b> {html.escape(s)}</p>")
    if not findings:
        h.append("<p><b>No divergent draw.</b></p>")
    for f in shown:
        r = f["record"]
        h.append(f"<h2>Draw {f['draw']} ({html.escape(f['target'])})</h2><table>"
                 f"<tr><th>frame / packet</th><td>{r.get('frame')} / {r.get('packet')} at {r.get('packet_base')}</td></tr>"
                 f"<tr><th>primitive</th><td>type {r.get('prim_type')}, {r.get('index_count')} indices, "
                 f"indexed {r.get('indexed')}</td></tr>"
                 f"<tr><th>VS</th><td><code>{r.get('vs')}</code></td></tr>"
                 f"<tr><th>PS</th><td><code>{r.get('ps')}</code></td></tr>")
        if f["kind"] == "pixels":
            h.append(f"<tr><th>pixels</th><td>{f['pixels']} / {f['total']}, max error {f['max_err']:.6g}</td></tr></table>")
            h.append(f"<p>A | B | diff</p><img src='{html.escape(f.get('image', ''))}'>")
        else:
            h.append(f"<tr><th>{f['kind']}</th><td>{html.escape(str(f.get('detail')))}</td></tr></table>")
        if r.get("textures"):
            h.append("<pre>" + html.escape("\n".join(
                f"{t['stage']} fetch {t['fetch']}: format {t['format']} base {t['base']} {' '.join(t['dwords'])}"
                for t in r["textures"])) + "</pre>")
        h.append("<details><summary>render state</summary><pre>" + html.escape(fmt_regs(r)) + "</pre></details>")
    with open(os.path.join(args.out, "report.html"), "w") as f:
        f.write("\n".join(h))


# ---------------------------------------------------------------------------
# png
# ---------------------------------------------------------------------------

def run_png(args):
    rep = Replay(args.replay)
    os.makedirs(args.out, exist_ok=True)
    n = 0
    for draw, targets in rep.states():
        for label, (key, image, before) in targets.items():
            Image.fromarray(to_rgb8(image)).save(
                os.path.join(args.out, f"draw{draw['draw']:06}_{label}.png"))
            n += 1
    print(f"wrote {n} images to {args.out}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("-o", "--out", default="replay_diff")
    d.add_argument("--tol", type=float, default=1.5 / 255.0, help="max abs color error per channel")
    d.add_argument("--depth-tol", type=float, default=16.0, help="max depth error in 24-bit units")
    d.add_argument("--max-pixels", type=int, default=0, help="differing pixels allowed per target")
    d.add_argument("--delta", action="store_true", help="compare per-draw changes, list all independent divergences")
    d.add_argument("--images", type=int, default=8, help="max divergent draws to render")
    d.add_argument("--a-frame", type=int, help="only compare draws of this frame of A (0 based)")
    d.add_argument("--b-frame", type=int, help="only compare draws of this frame of B")
    p = sub.add_parser("png")
    p.add_argument("replay")
    p.add_argument("-o", "--out", default="replay_png")
    args = ap.parse_args()
    sys.exit(run_diff(args) if args.cmd == "diff" else run_png(args))


if __name__ == "__main__":
    main()
