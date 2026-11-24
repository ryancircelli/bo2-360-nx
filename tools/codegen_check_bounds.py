"""Static self-check of recompiler output: function bounds and switch tables.

Reads a generated directory (the *_init.cpp table and the *_recomp.N.cpp
bodies) plus the image the recompiler read, and flags what usually costs a
debugging day later:

  pdata-split   a .pdata function the output splits into several functions
                (the tail is reached by a tail call; only risky when the
                split is wrong, e.g. a loop body cut off from its head)
  pdata-gap     bytes inside a .pdata function that no generated function
                covers (indirect jumps or fall-through there hit nothing)
  pdata-overrun a generated function runs past its .pdata end into code
                .pdata gives to another function
  no-func       a .pdata start that no generated function begins at
  overlap       two generated functions cover the same bytes
  fall-off      a function's last instruction is not an unconditional exit
  switch-count  switch case count differs from the bound the compare
                before the bctr implies
  switch-target a switch case target differs from the table in the image
  bctr-table    a bctr with no switch that loads its target from a table
  emit-error    ERROR / FATAL / STUB lines the emitter wrote

Function extents come from the instruction comments the emitter writes (one
per instruction); every direct branch comment is checked against the image so
a miscount shows up as "alignment" instead of as false findings.

usage: python3 tools/codegen_check_bounds.py --gen DIR --image FILE.image.bin
           [--summary FILE.summary.json] [--max 20] [--json OUT]
"""
import argparse
import bisect
import glob
import json
import os
import re
import struct
import sys
from collections import Counter, defaultdict

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
INSN_RE = re.compile(r"^\t+// ([a-z][a-z0-9.+-]*)(?: (.*))?$")
MAP_RE = re.compile(r"\{ 0x([0-9A-Fa-f]{8}), (\w+) \}")
SWITCH_RE = re.compile(r"^\s*switch \((?:ctx\.)?(r\d+)\.u32\) \{")
CASE_RE = re.compile(r"^\s*case (\d+):")
LABEL_RE = re.compile(r"^loc_([0-9A-F]{8}):")
GOTO_RE = re.compile(r"^\s*goto loc_([0-9A-F]{8});")
ERR_RE = re.compile(r"// (ERROR|FATAL|STUB)\b")
EXITS = {"b", "blr", "bctr", "trap"}


class Image:
    def __init__(self, path, summary):
        self.data = open(path, "rb").read()
        s = json.load(open(summary))
        self.base = int(s["base_address"], 16)
        self.sections = {x["name"]: (int(x["address"], 16), x["size"]) for x in s["sections"]}

    def u32(self, a):
        o = a - self.base
        if 0 <= o <= len(self.data) - 4:
            return struct.unpack_from(">I", self.data, o)[0]
        return None

    def u16(self, a):
        o = a - self.base
        return struct.unpack_from(">H", self.data, o)[0] if 0 <= o <= len(self.data) - 2 else None

    def u8(self, a):
        o = a - self.base
        return self.data[o] if 0 <= o < len(self.data) else None

    def pdata(self):
        if ".pdata" not in self.sections:
            return []
        a, n = self.sections[".pdata"]
        out = []
        for i in range(0, n - 7, 8):
            begin, d = self.u32(a + i), self.u32(a + i + 8 - 4)
            if not begin:
                continue
            out.append((begin, ((d >> 8) & 0x3FFFFF) * 4))
        return out


def branch_target(word, addr):
    op = word >> 26
    if op == 18:
        d = word & 0x03FFFFFC
        d -= (d & 0x02000000) << 1
        return (d if word & 2 else addr + d) & 0xFFFFFFFF
    if op == 16:
        d = word & 0xFFFC
        d -= (d & 0x8000) << 1
        return (d if word & 2 else addr + d) & 0xFFFFFFFF
    return None


def parse_gen(gen):
    names = {}
    for f in glob.glob(os.path.join(gen, "*_init.cpp")):
        for m in MAP_RE.finditer(open(f, errors="replace").read()):
            names[m.group(2)] = int(m.group(1), 16)
    funcs = {}  # start -> dict(insns=[(mn, ops)], switches=[(insn_index, reg, [targets])], errors=[])
    errors = Counter()
    for f in sorted(glob.glob(os.path.join(gen, "*_recomp.*.cpp"))):
        cur = sw = None
        for line in open(f, errors="replace"):
            m = FUNC_RE.match(line)
            if m:
                name = m.group(1).replace("__imp__", "")
                addr = names.get(name)
                if addr is None and name.startswith("sub_"):
                    addr = int(name[4:], 16)
                cur = {"insns": [], "addrs": [], "switches": [], "name": name} if addr is not None else None
                pc = addr
                if cur is not None:
                    funcs[addr] = cur
                sw = None
                continue
            if cur is None:
                continue
            e = ERR_RE.search(line)
            if e:
                errors[e.group(1)] += 1
                cur.setdefault("errors", []).append(line.strip())
                continue
            m = LABEL_RE.match(line)
            if m:
                pc = int(m.group(1), 16)  # labels re-sync past inline tables
                continue
            m = INSN_RE.match(line)
            if m:
                cur["insns"].append((m.group(1), m.group(2) or ""))
                cur["addrs"].append(pc)
                pc += 4
                sw = None
                continue
            m = SWITCH_RE.match(line)
            if m:
                sw = (len(cur["insns"]) - 1, m.group(1), [])
                cur["switches"].append(sw)
                continue
            if sw is not None:
                m = GOTO_RE.match(line)
                if m:
                    sw[2].append(int(m.group(1), 16))
    return funcs, errors


def regs(ops):
    return [o.strip() for o in ops.split(",")]


def imm(s):
    return int(s, 0)


def trace_table(insns, i):
    """Walk back from the bctr at index i. Returns (kind, bound, table, shift, base, index_reg) or None."""
    ins = insns[max(0, i - 64):i]
    j = len(ins) - 1
    want = None
    while j >= 0 and want is None:
        mn, ops = ins[j]
        if mn == "mtctr":
            want = ops.strip()
        j -= 1
    if want is None:
        return None
    # resolve registers backward with a tiny value tracker
    load = None   # (mn, ra, rb)
    add_base = None
    shift_after = 0
    consts = {}
    k = j
    target = want
    chain = []
    while k >= 0:
        mn, ops = ins[k]
        r = regs(ops)
        if r and r[0] == target:
            if mn in ("lwzx", "lbzx", "lhzx") and load is None:
                load = (mn, r[1], r[2])
                chain.append(k)
                break
            if mn == "add" and load is None:
                add_base = (r[1], r[2], k)
                # follow whichever operand is later produced by a load
                target = None
                for kk in range(k - 1, -1, -1):
                    m2, o2 = ins[kk]
                    rr = regs(o2)
                    if rr and rr[0] in add_base[:2] and m2 in ("lwzx", "lbzx", "lhzx", "rlwinm", "slwi"):
                        target = rr[0]
                        break
                if target is None:
                    return None
                k -= 1
                continue
            if mn in ("rlwinm", "slwi") and load is None:
                shift_after = imm(r[2])
                target = r[1]
                k -= 1
                continue
            if mn in ("mr",):
                target = r[1]
                k -= 1
                continue
            return None
        k -= 1
    if load is None:
        return None

    def value(reg, before):
        """lis/addi/ori constant held by reg just before index `before`."""
        v = None
        for kk in range(before - 1, -1, -1):
            mn, ops = ins[kk]
            r = regs(ops)
            if not r or r[0] != reg:
                continue
            if mn == "lis":
                return ((imm(r[1]) << 16) & 0xFFFFFFFF) if v is None else v
            if mn in ("addi", "ori") and r[1] == reg:
                lo = imm(r[2])
                inner = value(reg, kk)
                if inner is None:
                    return None
                return (inner + lo) & 0xFFFFFFFF if mn == "addi" else inner | lo
            if mn == "addi" and r[1] != reg:
                inner = value(r[1], kk)
                return None if inner is None else (inner + imm(r[2])) & 0xFFFFFFFF
            return None
        return None

    mn, ra, rb = load
    table = value(ra, k)
    idx = rb
    if table is None:
        table, idx = value(rb, k), ra
    if table is None:
        return None
    base = None
    if add_base:
        other = [x for x in add_base[:2] if x != target]
        if other:
            base = value(other[0], add_base[2])
    # find the index source: undo a scaling shift on idx
    kk = k - 1
    while kk >= 0:
        m2, o2 = ins[kk]
        rr = regs(o2)
        if rr and rr[0] == idx:
            if m2 in ("rlwinm", "slwi", "add"):
                idx = rr[1]
            elif m2 == "mr":
                idx = rr[1]
            else:
                break
        if m2 in ("cmplwi", "cmpwi") and rr[-2] == idx:
            break
        kk -= 1
    # bound: cmplwi crN,idx,N followed by the branch that skips the switch
    bound = None
    for c in range(len(ins) - 1, -1, -1):
        m2, o2 = ins[c]
        rr = regs(o2)
        if m2 in ("cmplwi", "cmpwi") and len(rr) >= 2 and rr[-2] == idx:
            n = imm(rr[-1])
            br = next((ins[d][0] for d in range(c + 1, len(ins)) if ins[d][0].startswith("b")), "")
            br = br.rstrip("+-")
            bound = {"bgt": n + 1, "bge": n, "bgtlr": n + 1, "bgelr": n}.get(br, ("?", br, n))
            break
    return {"load": mn, "table": table, "base": base, "shift": shift_after, "bound": bound, "idx": idx}


def decode_targets(img, t, count, func_start):
    out = []
    for n in range(count):
        if t["load"] == "lwzx":
            v = img.u32(t["table"] + 4 * n)
            if t["base"] is not None:
                v = (t["base"] + (v << t["shift"])) & 0xFFFFFFFF
        elif t["load"] == "lhzx":
            v = img.u16(t["table"] + 2 * n)
            v = None if v is None else ((t["base"] or func_start) + (v << t["shift"])) & 0xFFFFFFFF
        else:
            v = img.u8(t["table"] + n)
            v = None if v is None else ((t["base"] or func_start) + (v << t["shift"])) & 0xFFFFFFFF
        out.append(v)
    return out


def check(gen, image, summary, max_show):
    img = Image(image, summary)
    funcs, errors = parse_gen(gen)
    starts = sorted(funcs)
    ext = {s: (max(funcs[s]["addrs"]) + 4 if funcs[s]["addrs"] else s) for s in starts}
    find = defaultdict(list)
    stats = Counter()

    # alignment: every direct branch comment must match the image word
    bad_align = set()
    for s in starts:
        for n, (mn, ops) in enumerate(funcs[s]["insns"]):
            if mn.startswith("b") and "0x" in ops:
                a = funcs[s]["addrs"][n]
                w = img.u32(a)
                want = int(ops.rsplit(",", 1)[-1].strip(), 16)
                stats["branches"] += 1
                if w is None or branch_target(w, a) != want:
                    bad_align.add(s)
                    break
    for s in bad_align:
        find["alignment"].append(f"{s:08X} {funcs[s]['name']}: instruction comments do not line up with the image")

    pd = img.pdata()
    pranges = sorted(pd)
    pbegins = [p[0] for p in pranges]

    def pdata_of(a):
        i = bisect.bisect_right(pbegins, a) - 1
        return pranges[i] if i >= 0 and pranges[i][0] <= a < pranges[i][0] + pranges[i][1] else None

    # overlaps and fall-off
    for a, b in zip(starts, starts[1:]):
        if ext[a] > b and a not in bad_align:
            if ext[b] > ext[a]:
                find["overlap"].append(f"{a:08X}-{ext[a]:08X} runs into {b:08X}-{ext[b]:08X}")
            else:
                stats["nested-entries"] += 1
    for s in starts:
        ins = funcs[s]["insns"]
        if not ins or s in bad_align:
            continue
        mn = ins[-1][0].rstrip("+-")
        pr = pdata_of(ext[s] - 4)
        if pr and pr[0] + pr[1] == ext[s]:
            continue
        if mn not in EXITS and not (mn == "tw" and ins[-1][1].startswith("31,")):
            find["fall-off"].append(f"{s:08X}-{ext[s]:08X} ends with '{ins[-1][0]} {ins[-1][1]}'")

    # .pdata vs generated extents
    covered = []  # merged intervals of generated code
    for s in starts:
        if covered and s <= covered[-1][1]:
            covered[-1][1] = max(covered[-1][1], ext[s])
        else:
            covered.append([s, ext[s]])
    cstarts = [c[0] for c in covered]

    def uncovered(a, b):
        miss, x = 0, a
        i = bisect.bisect_right(cstarts, x) - 1
        while x < b:
            if i >= 0 and covered[i][0] <= x < covered[i][1]:
                x = covered[i][1]
                i += 1
                continue
            nxt = covered[i + 1][0] if i + 1 < len(covered) else b
            if nxt <= x:
                i += 1
                continue
            stop = min(b, nxt)
            miss += sum(1 for y in range(x, stop, 4) if img.u32(y))
            x = stop
        return miss

    stats["pdata"] = len(pd)
    pstarts = sorted(set(p[0] for p in pd))
    for begin, size in pd:
        end = begin + size
        if begin not in funcs:
            inside = bisect.bisect_right(starts, begin) - 1
            host = starts[inside] if inside >= 0 and ext[starts[inside]] > begin else None
            find["no-func"].append(f"{begin:08X} (+{size:#x})" + (f" inside {host:08X}-{ext[host]:08X}" if host else " uncovered"))
        else:
            e = ext[begin]
            if e < end:
                parts = [s for s in starts[bisect.bisect_left(starts, begin):bisect.bisect_left(starts, end)]]
                miss = uncovered(begin, end)
                if miss:
                    find["pdata-gap"].append(f"{begin:08X}-{end:08X}: {miss} non-zero words not in any function; generated ends {e:08X}")
                elif len(parts) > 1:
                    find["pdata-split"].append(f"{begin:08X}-{end:08X} split at " + ",".join(f"{p:08X}" for p in parts[1:6]))
            elif e > end:
                nxt = pstarts[bisect.bisect_right(pstarts, begin)] if bisect.bisect_right(pstarts, begin) < len(pstarts) else None
                if nxt is not None and nxt < e:
                    find["pdata-overrun"].append(f"{begin:08X}: generated ends {e:08X}, pdata ends {end:08X}, next pdata {nxt:08X}")

    # switch tables
    for s in starts:
        if s in bad_align:
            continue
        f = funcs[s]
        for i, reg, cases in f["switches"]:
            stats["switches"] += 1
            t = trace_table(f["insns"], i)
            where = f"{s:08X} bctr@{f['addrs'][i]:08X}"
            if t is None:
                stats["switch-untraced"] += 1
                continue
            b = t["bound"]
            if isinstance(b, int) and b != len(cases):
                find["switch-count"].append(f"{where}: {len(cases)} cases, compare implies {b} ({t['load']} table {t['table']:08X})")
            elif not isinstance(b, int):
                stats["switch-nobound"] += 1
                find["info-switch-nobound"].append(f"{where}: {len(cases)} cases, bound {b}")
            want = decode_targets(img, t, len(cases), s)
            diff = [n for n, (a, c) in enumerate(zip(want, cases)) if a != c]
            if diff:
                n = diff[0]
                w = want[n]
                find["switch-target"].append(f"{where}: case {n} goes to {cases[n]:08X}, table says {w:08X}" if w is not None else f"{where}: case {n} table unreadable")
            pr = pdata_of(f["addrs"][i])
            if pr and not isinstance(b, int):
                nxt = decode_targets(img, t, len(cases) + 1, s)[-1]
                if nxt is not None and pr[0] <= nxt < pr[0] + pr[1] and nxt % 4 == 0:
                    find["switch-short"].append(f"{where}: {len(cases)} cases but entry {len(cases)} -> {nxt:08X} is in the same function")
            if pr:
                far = [c for c in cases if not (pr[0] <= c < pr[0] + pr[1])]
                if far:
                    find["switch-pdata"].append(f"{where}: {len(far)}/{len(cases)} cases outside its .pdata function {pr[0]:08X}-{pr[0] + pr[1]:08X}, e.g. {far[0]:08X} (bound {b})")
            out = [c for c in cases if not (s <= c < ext[s])]
            if out:
                find["switch-target"].append(f"{where}: {len(out)} cases leave the function, e.g. {out[0]:08X}")
        for i, (mn, ops) in enumerate(f["insns"]):
            if mn == "bctr" and not any(x[0] == i for x in f["switches"]):
                back = [m for m, _ in f["insns"][max(0, i - 8):i]]
                t = trace_table(f["insns"], i)
                if t is not None:
                    n = t["bound"] if isinstance(t["bound"], int) else 1
                    tg = decode_targets(img, t, max(1, min(n, 512)), s)
                    pr = pdata_of(f["addrs"][i]) or (s, ext[s] - s)
                    if not any(x is not None and pr[0] <= x < pr[0] + pr[1] for x in tg):
                        stats["table-tail-calls"] += 1  # function-pointer table, not a switch
                        continue
                    find["bctr-table"].append(f"{s:08X} bctr@{f['addrs'][i]:08X}: {t['load']} table {t['table']:08X} bound {t['bound']}, targets " + ",".join("?" if x is None else f"{x:08X}" for x in tg[:4]))

    for k, v in errors.items():
        find["emit-error"].append(f"{v} x {k}")
    stats["functions"] = len(starts)
    return find, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--summary")
    ap.add_argument("--max", type=int, default=20)
    ap.add_argument("--json")
    a = ap.parse_args()
    summary = a.summary or a.image.replace(".image.bin", ".summary.json")
    find, stats = check(a.gen, a.image, summary, a.max)
    print("stats:", dict(stats))
    for k in sorted(find):
        print(f"\n[{k}] {len(find[k])}")
        for line in find[k][:a.max]:
            print("  " + line)
    if a.json:
        json.dump({"stats": stats, "findings": find}, open(a.json, "w"), indent=1)
    sys.exit(1 if any(k != "pdata-split" for k in find) else 0)


if __name__ == "__main__":
    main()
