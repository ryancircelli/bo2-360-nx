#!/usr/bin/env python3
"""replay-gate: picture-regression gate over saved GPU traces (no game boot).

  gate.py check  BUILD_DIR [--only A,B] [--jobs N] [--direct]
  gate.py golden BUILD_DIR [--only A,B]        # (re)make goldens from this build
  gate.py accept RUN_DIR NAME...               # promote a run's output to golden

Per trace: replay with the build's rexgpu-replay + librexgpu-metal.dylib, hash
every draw readback (XXH3 from draws.jsonl) and rebuild the final colour
targets. Compared with the golden stored in $GATE/golden/NAME/.
Verdicts: IDENTICAL (every draw hash equal), PASS (final images within
tolerance, hashes drifted), CHANGED (expected.tsv lists it: not a failure),
FAIL (final image out of tolerance, draw/readback counts changed, crash, timeout).
Exit 0 unless some trace is FAIL.
"""
import shlex, argparse, json, os, shutil, subprocess, sys, time, concurrent.futures as cf
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "replay"))
import replay_diff as rd

GATE = os.environ.get("REPLAY_GATE_DIR", os.path.expanduser("~/Games/Xbox360/bo2-build/replay-gate"))
TIMEOUT = 1500  # bo1-menu-60 (1919 draws, per-draw readback) takes minutes under load; it never deadlocked
PX_TOL = 3 / 255.0      # per-channel error that counts as a differing pixel
PX_FRAC = 2e-4          # fraction of differing pixels allowed (PASS)
MAX_ERR = 24 / 255.0    # any single pixel beyond this is a FAIL

def traces():
    """name -> (trace path, extra replay args, game dir)"""
    out = {}
    for line in open(os.path.join(HERE, "traces.tsv")):
        if line.strip() and not line.startswith("#"):
            f = line.rstrip("\n").split("\t")
            out[f[0]] = (os.path.join(GATE, "traces", f[0] + ".xtr"), f[1] if len(f) > 1 else "", f[2] if len(f) > 2 else "")
    return out

def expected():
    p = os.path.join(HERE, "expected.tsv")
    d = {}
    if os.path.exists(p):
        for line in open(p):
            if line.strip() and not line.startswith("#"):
                k, _, v = line.rstrip("\n").partition("\t")
                d[k] = v
    return d

def bindir(build, game):
    for c in ([os.path.join(build, game)] if game else []) + [build]:
        if os.path.exists(os.path.join(c, "rexgpu-replay")):
            return c
    sys.exit(f"no rexgpu-replay in {build}")

RUNQ = os.path.expanduser("~/Games/Xbox360/bo2-build/runq/bin/runq")

def replay(build, name, trace, extra, game, out, direct):
    b = bindir(build, game)
    shutil.rmtree(out, ignore_errors=True)
    cmd = [os.path.join(b, "rexgpu-replay"), trace, "-o", out, "--plugin", os.path.join(b, "librexgpu-metal.dylib"),
           "--backend", "any", "--label", "metal", "--frames", "1"] + extra.split()
    env = dict(os.environ, REX_AUDIO_MUTE="true", SDL_HIDAPI_LIBUSB="0", SDL_JOYSTICK_HIDAPI="0")
    t = time.time()
    if not direct:
        # runq ordinary harness job (not solo): the queue's solo/speed windows freeze it, so it can never
        # spoil a timed run. Queue wait is not counted against TIMEOUT (runq's --timeout covers the run only).
        sh = "cd %s && REX_AUDIO_MUTE=true SDL_HIDAPI_LIBUSB=0 SDL_JOYSTICK_HIDAPI=0 %s" % (
            shlex.quote(b), " ".join(shlex.quote(c) for c in cmd))
        job = out + ".job"
        shutil.rmtree(job, ignore_errors=True)
        for attempt in (1, 2):
            shutil.rmtree(out, ignore_errors=True)
            r = subprocess.run([RUNQ, "submit", "--class", "harness", "--no-solo", "--name", "replay-gate-" + name,
                                "--owner", "replay-regression", "--out", job, "--timeout", str(TIMEOUT), "--wait",
                                "--", "sh", "-c", sh], capture_output=True, stdin=subprocess.DEVNULL)
            if os.path.exists(os.path.join(out, "summary.json")):
                return 0, "", time.time() - t
        return -1, "runq job produced no summary.json (timeout %ds or crash): %s %s" % (
            TIMEOUT, r.stdout.decode(errors="replace")[-200:], r.stderr.decode(errors="replace")[-200:]), time.time() - t
    for attempt in (1, 2):   # one retry: a stalled replay (seen under heavy load) is not a verdict
        shutil.rmtree(out, ignore_errors=True)
        try:
            r = subprocess.run(["nice", "-n", "15"] + cmd, cwd=b, env=env, capture_output=True,
                               stdin=subprocess.DEVNULL, start_new_session=True, timeout=TIMEOUT)
            return r.returncode, r.stderr.decode(errors="replace")[-400:], time.time() - t
        except subprocess.TimeoutExpired:
            rc, err = -1, f"timeout {TIMEOUT}s (after retry)"
    return rc, err, time.time() - t

def digest(out):
    """-> (per-draw records, final colour images {label: float array}, summary)"""
    rep = rd.Replay(out)
    recs, final = [], {}
    for draw, targets in rep.states():
        for label, (key, image, _) in targets.items():
            if image.ndim == 3:
                final[label] = image
    for d in rep.draws:
        recs.append({"d": d["draw"], "vs": d.get("vs"), "ps": d.get("ps"),
                     "h": [[t["target"], t["hash"]] for t in d.get("targets", [])]})
    return recs, final, rep.summary

def top_targets(final, n=2):
    # largest area first, then label: stable pick of the main colour targets
    return sorted(final, key=lambda k: (-final[k].shape[0] * final[k].shape[1], k))[:n]

def save_golden(gdir, recs, final, summary, build):
    shutil.rmtree(gdir, ignore_errors=True)
    os.makedirs(gdir)
    keep = top_targets(final)
    for k in keep:
        np.save(os.path.join(gdir, f"final_{k}.npy"), np.nan_to_num(final[k]).astype(np.float16))
        Image.fromarray(rd.to_rgb8(final[k])).save(os.path.join(gdir, f"final_{k}.png"))
    json.dump({"draws": recs, "targets": keep, "summary": {k: summary.get(k) for k in ("draws", "readbacks", "failed_readbacks", "ok")},
               "build": build, "date": time.strftime("%F %T")}, open(os.path.join(gdir, "golden.json"), "w"), separators=(",", ":"))

def compare(gdir, recs, final, summary, rundir):
    g = json.load(open(os.path.join(gdir, "golden.json")))
    gs, why = g["summary"], []
    for k in ("draws", "failed_readbacks"):
        if gs.get(k) != summary.get(k):
            why.append(f"{k} {gs.get(k)} -> {summary.get(k)}")
    first = None
    ndiff = 0
    for a, b in zip(g["draws"], recs):
        if a["h"] != b["h"] or a["vs"] != b["vs"] or a["ps"] != b["ps"]:
            ndiff += 1
            first = first or (a, b)
    worst, frac, heat = 0.0, 0.0, []
    for k in g["targets"]:
        if k not in final:
            why.append(f"target {k} missing"); continue
        a = np.load(os.path.join(gdir, f"final_{k}.npy")).astype(np.float32)
        b = np.nan_to_num(final[k]).astype(np.float16).astype(np.float32)
        if a.shape != b.shape:
            why.append(f"{k} shape {a.shape} -> {b.shape}"); continue
        err = np.abs(a[..., :3] - b[..., :3]).max(axis=-1)
        mask = err > PX_TOL
        worst, frac = max(worst, float(err.max())), max(frac, float(mask.mean()))
        if err.max() > 0 and (mask.any()):
            os.makedirs(rundir, exist_ok=True)
            rd.side_by_side(a, b, mask, err, os.path.join(rundir, f"diff_{k}.png"))
            heat.append(f"diff_{k}.png")
    if worst > MAX_ERR: why.append(f"max pixel error {worst*255:.0f}/255")
    if frac > PX_FRAC: why.append(f"{frac*100:.3f}% pixels differ")
    info = {"hash_diff_draws": ndiff, "max_err": round(worst * 255, 1), "px_frac": frac, "images": heat}
    if first:
        a, b = first
        info["first_draw"] = a["d"]; info["vs"] = b["vs"]; info["ps"] = b["ps"]; info["golden_vs_ps"] = [a["vs"], a["ps"]]
    if why: return "FAIL", why, info
    return ("IDENTICAL" if ndiff == 0 else "PASS"), [], info

def one(mode, build, name, spec, run, direct):
    trace, extra, game = spec
    out = os.path.join(run, name, "raw")
    rc, err, secs = replay(build, name, trace, extra, game, out, direct)
    res = {"name": name, "secs": round(secs, 1)}
    gdir = os.path.join(GATE, "golden", name)
    try:
        if rc != 0 or not os.path.exists(os.path.join(out, "summary.json")):
            res.update(verdict="FAIL", why=[f"replay rc={rc} {err.strip()[-200:]}"], info={})
        else:
            recs, final, summ = digest(out)
            if mode == "golden":
                save_golden(gdir, recs, final, summ, build)
                res.update(verdict="GOLDEN", why=[], info={"draws": len(recs)})
            elif not os.path.exists(os.path.join(gdir, "golden.json")):
                res.update(verdict="FAIL", why=["no golden (run golden first)"], info={})
            else:
                v, why, info = compare(gdir, recs, final, summ, os.path.join(run, name))
                res.update(verdict=v, why=why, info=info)
                if v == "FAIL" or mode == "keep":
                    save_golden(os.path.join(run, name, "new"), recs, final, summ, build)
    except Exception as e:
        res.update(verdict="FAIL", why=[f"gate error {e!r}"], info={})
    if res["verdict"] in ("IDENTICAL", "PASS", "GOLDEN"):
        shutil.rmtree(os.path.join(run, name), ignore_errors=True)
    else:
        shutil.rmtree(out, ignore_errors=True)   # raw is huge; failures keep diff PNGs + new/ golden
    return res

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["check", "golden", "accept"])
    ap.add_argument("a"); ap.add_argument("rest", nargs="*")
    ap.add_argument("--only"); ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--direct", action="store_true", help="run niced here, OUTSIDE runq (default submits runq harness jobs; direct runs can spoil solo speed runs - avoid)")
    ap.add_argument("--run-dir")
    ap.add_argument("--extra", default="", help="extra rexgpu-replay args for every trace, e.g. '--cvar k=v'")
    args = ap.parse_args()
    if args.cmd == "accept":
        for n in args.rest:
            src = os.path.join(args.a, n, "new")
            dst = os.path.join(GATE, "golden", n)
            shutil.rmtree(dst, ignore_errors=True); shutil.copytree(src, dst); print("accepted", n)
        return 0
    allt = {k: (a, (b + " " + args.extra).strip(), c) for k, (a, b, c) in traces().items()}
    names = args.only.split(",") if args.only else list(allt)
    run = args.run_dir or os.path.join(GATE, "runs", time.strftime("%m%d-%H%M%S"))
    os.makedirs(run, exist_ok=True)
    t0 = time.time()
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        res = list(ex.map(lambda n: one(args.cmd, os.path.abspath(args.a), n, allt[n], run, args.direct), names))
    exp = expected()
    fails = 0
    for r in res:
        if r["verdict"] == "FAIL":
            if r["name"] in exp: r["verdict"] = "CHANGED"; r["why"].append("expected: " + exp[r["name"]])
            else: fails += 1
    print(f"\nREPLAY GATE  {args.cmd}  {args.a}   {time.time()-t0:.0f}s   run: {run}")
    for r in res:
        extra = ""
        if r["why"]: extra = "  " + "; ".join(r["why"])
        i = r["info"]
        if r["verdict"] == "PASS": extra = f"  {i['hash_diff_draws']} draws drift, max err {i['max_err']}/255"
        print(f"  {r['verdict']:9} {r['name']:26} {r['secs']:6.1f}s{extra}")
        if r["verdict"] in ("FAIL", "CHANGED") and i.get("first_draw") is not None:
            print(f"            first diverging draw {i['first_draw']}  vs {i['vs']} ps {i['ps']}  ({i['hash_diff_draws']} draws differ)")
            for im in i["images"]: print(f"            {os.path.join(run, r['name'], im)}")
    print("\nRESULT:", "FAIL" if fails else "PASS", f"({fails} regressions)" if fails else "")
    json.dump(res, open(os.path.join(run, "result.json"), "w"), indent=1)
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
