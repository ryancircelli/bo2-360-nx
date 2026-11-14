"""Register locals across BO2's setjmp/longjmp (Com_Error recovery): check, and fix the generated code.

The codegen turns `bl setjmp` (setjmp_address in games/bo2/overrides.toml, 0x82918CA0) into
    env = ctx; temp.s64 = ppc_setjmp(ctx.r3.u32); if (temp.s64 != 0) ctx = env;
so a longjmp restores ctx as it was at the setjmp. The game's setjmp also saves CR (mfcr), f14-f31, r13-r31 and
v64-v127 in the buffer (disassembly at 0x82918CB8-0x82918E..), and its longjmp (0x82919E50) puts them back. With
non_volatile_as_local and cr_as_local those registers are C++ locals instead, and after the host longjmp a local
changed since the setjmp has an indeterminate value (C11 7.13.2.1); GCC in particular gives it the value it had at
the longjmp. The 360 code relies on getting the setjmp-time value: sub_8253ECC8 (a text parser) sets r15 before its
setjmp, reuses r15 for something else on the normal path, and its error path reads r15 again expecting the old value.

--fix rewrites every site so the locals the game's setjmp saves are copied before the setjmp, into constants that
nothing changes afterwards, and copied back when the setjmp returns from a longjmp:
    env = ctx;
    {  // setjmp_locals.py ...
    const auto sj_r15 = r15; ...
    temp.s64 = ppc_setjmp(ctx.r3.u32);
    if (temp.s64 != 0) { ctx = env; r15 = sj_r15; ... }
    }
It is idempotent, and has to run after every codegen (tools/codegen.sh does).

Without --fix it reports, per site, the locals that matter: those both written after the setjmp at a point where a
call (a possible longjmp) can still follow, and read after the setjmp returns, before being written. A site with
none of them behaves as on the 360 even unfixed.

usage: python tools/setjmp_locals.py [--gen DIR] [--fix | --verify]
  --verify  change nothing; exit 1 if a setjmp site is not protected (the build scripts run this)
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from read_before_write import FN, GEN, GOTO, LABEL, effect  # noqa: E402

SITE = re.compile(r"ppc_setjmp\(")
CALL = re.compile(r"\(ctx, base\);|REX_CALL_INDIRECT_FUNC\(")
MARK = "// setjmp_locals.py: the locals the game's setjmp saves, as they were at the setjmp"
UNFIXED = re.compile(r"^(\t+)env = ctx;\n"
                     r"\1temp\.s64 = ppc_setjmp\((ctx\.r3\.u32)\);\n"
                     r"\1if \(temp\.s64 != 0\) ctx = env;\n", re.M)
DECL = re.compile(r"^\tPPC(?:CR|V)?Register (r(?:1[4-9]|2\d|3[01])|f(?:1[4-9]|2\d|3[01])|"
                  r"v(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7])|cr[0-7])\{\};$", re.M)


def statements(body):
    """[(text, reads, writes, succs)] with statement-level successors."""
    out, labels = [], {}
    for raw in body:
        s = raw.strip()
        if not s or s.startswith("//"):
            continue
        ml = LABEL.match(s)
        if ml:
            labels[ml.group(1)] = len(out)
            continue
        r, w, _ = effect(s)
        top = raw.startswith("\t") and not raw.startswith("\t\t")
        ends = top and (s.startswith("goto ") or s == "return;" or s.startswith("__builtin_trap"))
        out.append([s, r, w, GOTO.findall(s), ends])
    n = len(out)
    res = []
    for i, (s, r, w, gotos, ends) in enumerate(out):
        succ = {labels[g] for g in gotos if g in labels}
        if not ends and i + 1 < n:
            succ.add(i + 1)
        res.append((s, r, w, succ))
    return res


def check(st, site):
    n = len(st)
    # can a call still follow statement i?
    call_after = [False] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            v = any(CALL.search(st[j][0]) or call_after[j] for j in st[i][3])
            if v != call_after[i]:
                call_after[i] = v
                changed = True
    # registers written on some path after the site, where a call (a possible longjmp) can still follow
    seen, work, written = set(), [site], set()
    while work:
        i = work.pop()
        if i in seen:
            continue
        seen.add(i)
        if call_after[i]:
            written |= st[i][2]
        work.extend(st[i][3])
    # registers live right after the site (read before written on some path from it): backward liveness
    live = [set() for _ in range(n)]
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = set().union(*[live[j] for j in st[i][3]]) if st[i][3] else set()
            new = st[i][1] | (out - st[i][2])
            if new != live[i]:
                live[i] = new
                changed = True
    after = set().union(*[live[j] for j in st[site][3]]) if st[site][3] else set()
    return sorted(written & after), sorted(after)


def fix_function(text):
    """Rewrites the unfixed setjmp sites of one function's text. Returns (text, sites rewritten)."""
    regs = DECL.findall(text)
    if not regs:
        return text, 0

    def rewrite(m):
        tab, r3 = m.group(1), m.group(2)
        save = "".join(f"{tab}const auto sj_{r} = {r};\n" for r in regs)
        back = " ".join(f"{r} = sj_{r};" for r in regs)
        return (f"{tab}env = ctx;\n{tab}{{  {MARK}\n{save}"
                f"{tab}temp.s64 = ppc_setjmp({r3});\n"
                f"{tab}if (temp.s64 != 0) {{ ctx = env; {back} }}\n{tab}}}\n")

    return UNFIXED.subn(rewrite, text)


def unprotected(gen):
    """Sites in functions with register locals that --fix has not rewritten yet (code generated without
    non_volatile_as_local has no such functions, and nothing to protect)."""
    fn_start = re.compile(r"^DEFINE_REX_FUNC\(", re.M)
    n = total = 0
    for path in glob.glob(os.path.join(gen, "*_recomp.*.cpp")):
        text = open(path, encoding="utf-8").read()
        if "ppc_setjmp(" not in text:
            continue
        total += text.count("ppc_setjmp(")
        starts = [m.start() for m in fn_start.finditer(text)] + [len(text)]
        for a, b in zip(starts, starts[1:]):
            body = text[a:b]
            if DECL.search(body):  # a function with register locals: every site needs the fix
                n += body.count("ppc_setjmp(") - body.count(MARK)
    return n, total


def fix(gen):
    files = sites = 0
    fn_start = re.compile(r"^DEFINE_REX_FUNC\(", re.M)
    for path in sorted(glob.glob(os.path.join(gen, "*_recomp.*.cpp"))):
        text = open(path, encoding="utf-8").read()
        if "ppc_setjmp(" not in text:
            continue
        starts = [m.start() for m in fn_start.finditer(text)] + [len(text)]
        parts, n_file = [text[:starts[0]]], 0
        for a, b in zip(starts, starts[1:]):
            new, n = fix_function(text[a:b])
            parts.append(new)
            n_file += n
        if n_file:
            open(path, "w", encoding="utf-8", newline="\n").write("".join(parts))
            files += 1
            sites += n_file
    left, total = unprotected(gen)
    fixed = total - left
    print(f"setjmp_locals: {sites} sites rewritten in {files} files; {fixed} of {total} sites protected")
    if fixed != total:
        sys.exit("setjmp_locals: some setjmp sites do not have the expected shape; look at them by hand")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", default=GEN)
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--verify", action="store_true")
    a = ap.parse_args()
    if a.verify:
        n, total = unprotected(a.gen)
        if n:
            sys.exit(f"setjmp_locals --verify: {n} of {total} setjmp sites use register locals without the longjmp fix: "
                     f"the generated code was not made by tools/codegen.sh (run it)")
        print(f"setjmp_locals --verify: ok ({total} sites, none unprotected)")
        return
    if a.fix:
        fix(a.gen)
        return
    risky_total = sites = 0
    for path in sorted(glob.glob(os.path.join(a.gen, "*_recomp.*.cpp"))):
        cur = None
        for raw in open(path, encoding="utf-8", errors="ignore"):
            m = FN.match(raw)
            if m:
                cur, body = m.group(1), []
                continue
            if cur is None:
                continue
            if raw.startswith("}"):
                if any(SITE.search(x) for x in body):
                    protected = any(MARK in x for x in body)
                    st = statements(body)
                    for i, (s, *_rest) in enumerate(st):
                        if SITE.search(s):
                            sites += 1
                            risky, live = check(st, i)
                            risky_total += bool(risky) and not protected
                            print(f"{cur} site {sites}: live after setjmp {' '.join(live) or '-'}; "
                                  f"also written after it: {' '.join(risky) or 'none'}"
                                  f"{' (protected)' if protected else ''}")
                cur = None
                continue
            body.append(raw.rstrip("\n"))
    print(f"TOTAL {sites} setjmp sites, {risky_total} unprotected with locals that a longjmp may not restore",
          file=sys.stderr)


if __name__ == "__main__":
    main()
