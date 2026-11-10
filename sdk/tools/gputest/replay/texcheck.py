#!/usr/bin/env python3
"""Stale texture report for rexgpu-replay --texcheck / --race output.

rexgpu-replay writes <out>/texcheck.jsonl: one "stale" record per draw fetch
slot whose texture the cache considers loaded while the guest memory it was
loaded from has changed since (the draw sampled old data), and one "race"
record per guest write that race mode applied inside a texture load window.

  texcheck.py <replay_out> [--baseline <replay_out>] [--limit N] [--json]

Prints one line per distinct stale texture (first occurrence, with the number
of draws that sampled it) and exits 1 if any are left after removing those
also stale in the baseline replay, 0 if clean, 2 on bad input.
"""

import argparse
import collections
import json
import os
import sys


def load(out_dir):
    path = os.path.join(out_dir, "texcheck.jsonl")
    if not os.path.exists(path):
        print(f"texcheck: {path} missing (replay without --texcheck/--race?)", file=sys.stderr)
        return None, None
    stale = collections.OrderedDict()
    races = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("kind") == "race":
                races.append(rec)
            elif rec.get("kind") == "stale":
                key = (rec["texture"], rec["mips"], rec["fmt"], rec["w"], rec["h"], rec["part"],
                       rec["got"], rec["expected"])
                entry = stale.get(key)
                if entry is None:
                    stale[key] = {"first": rec, "draws": 1, "frames": {rec["frame"]},
                                  "slots": {rec["fetch"]}}
                else:
                    entry["draws"] += 1
                    entry["frames"].add(rec["frame"])
                    entry["slots"].add(rec["fetch"])
    summary = {}
    spath = os.path.join(out_dir, "summary.json")
    if os.path.exists(spath):
        try:
            summary = json.load(open(spath)) or {}
        except json.JSONDecodeError:
            summary = {}
    return stale, (races, summary)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("replay_out")
    ap.add_argument("--baseline", help="replay output whose stale textures are expected")
    ap.add_argument("--limit", type=int, default=40, help="lines to print (0 = all)")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    args = ap.parse_args()

    stale, extra = load(args.replay_out)
    if stale is None:
        return 2
    races, summary = extra
    baseline_keys = set()
    if args.baseline:
        base, _ = load(args.baseline)
        if base is None:
            return 2
        # Same texture and part stale in both: not a backend difference.
        baseline_keys = {k[:6] for k in base}
    findings = [(k, v) for k, v in stale.items() if k[:6] not in baseline_keys]
    tc = (summary or {}).get("texcheck") or {}

    if args.json:
        print(json.dumps({
            "replay": args.replay_out,
            "summary": tc,
            "race_injections": len(races),
            "stale_textures": len(findings),
            "stale": [dict(v["first"], draws=v["draws"], frames=len(v["frames"]))
                      for _, v in findings],
        }, indent=1))
    else:
        label = summary.get("backend") or os.path.basename(os.path.normpath(args.replay_out))
        print(f"texcheck {label}: {tc.get('checked', '?')} bound textures checked over "
              f"{summary.get('draws', '?')} draws, {tc.get('loads', '?')} loads, "
              f"{len(races)} race writes, {len(findings)} stale texture(s)"
              + (f" ({len(stale) - len(findings)} also stale in baseline)" if args.baseline else ""))
        shown = findings if not args.limit else findings[:args.limit]
        for _, v in shown:
            first = v["first"]
            more = ""
            if v["draws"] > 1:
                more = f" [sampled stale by {v['draws']} draws in {len(v['frames'])} frame(s)]"
            print(first["text"] + more)
        if args.limit and len(findings) > args.limit:
            print(f"... {len(findings) - args.limit} more")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
