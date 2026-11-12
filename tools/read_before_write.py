"""Registers read before they are written, in the generated C++.

The callers of each finding are listed too.

With cr_as_local / xer_as_local / ctr_as_local / non_volatile_as_local / vector_scratch_as_local
(app/overrides.toml), r14-r31, f14-f31, v0, v14-v127, cr0-cr7, ctr and xer are C++ locals of each generated function, zero at entry. A function that
reads one of them before writing it gets 0 instead of the value PowerPC code would see. That matters for pieces of
split functions: the analyzer cut a function in two, and the first piece jumps into the second (b, a conditional
branch, a jump table or bctr) with registers already set. Such a piece needs share_registers in app/overrides.toml:
it then keeps every register in ctx, and whoever jumps into it passes its locals through ctx
(the SDK's emit_call_sharing_registers). Run this after every codegen.

What is not reported, because it is harmless:
  - prologue saves: a store of a register to the stack before it is written (std/stw/stfd/stvx of r14+ relative to
    r1 or r12); the epilogue reloads it into the local, which dies at the return;
  - the summary-overflow bit copied into compares (cr.so = xer.so, compare<...>(..., xer)): xer.so is only ever set
    by the overflow-enabled instructions.
A read that comes textually before its write can still be fine at run time (a loop, or a path where the write
always comes first): those are the known false alarms, and need a human look.

usage: python tools/read_before_write.py [--gen DIR] [--saves] [--strict]
  --saves   also list the functions whose only early reads are stack saves (normally noise)
  --strict  exit 1 if a function outside KNOWN (below) shows up: it needs a human look, and share_registers in
            app/overrides.toml if it is a piece of a split function
Output: one line per function: name, registers, how it is reached (b / cond / jt / bl / indirect), first read.
"""
import argparse
import glob
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "app", "generated", "default_mp")

FN = re.compile(r"^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)")
# The localized registers, as bare names (ctx.r31 is not a local).
REG = re.compile(r"(?<![.\w])(r(?:1[4-9]|2\d|3[01])|f(?:1[4-9]|2\d|3[01])|"
                 r"v(?:0|1[4-9]|[2-5]\d|6\d|[7-9]\d|1[01]\d|12[0-7])|cr[0-7]|ctr|xer)\b")
ASSIGN = re.compile(r"^([A-Za-z_]\w*)(?:\.[\w\[\]]+)*\s*=(?!=)")
SIMD_STORE_REG = re.compile(r"^simde_mm_store\w*\(\(?(?:simde__m128i\*\)\s*)?(v\d+)\.\w+,(.*)$")
STORE_TO_STACK = re.compile(r"^REX_STORE_U(?:8|16|32|64)\(\s*(?:ctx\.r1\.u32|ctx\.r12\.u32|ea)\b[^,]*,\s*"
                            r"((?:r|f)\d+)\.(?:u8|u16|u32|u64|f64|s32|s64)\s*\);$")
SIMD_STORE_MEM = re.compile(r"^simde_mm_store\w*\(\(simde__m128i\*\)(?:REX_RAW_ADDR\(|\(base \+)")
CALL = re.compile(r"(?<![\w])(sub_[0-9A-F]{8})\(ctx, base\);")
COMPARE = re.compile(r"^(cr[0-7])\.(?:compare|setFromMask)\b")  # methods that write the whole field
SELF_MOVE = re.compile(r"^(\w+)\.u64 = \1\.u64;$")  # mr rN,rN (a no-op)


def strip_so(s):
    s = re.sub(r",\s*xer\)", ")", s) if ".compare" in s else s
    return re.sub(r"cr\d\.so = xer\.so;", "", s)


LABEL = re.compile(r"^(loc_[0-9A-F]+):$")
GOTO = re.compile(r"\bgoto (loc_[0-9A-F]+);")


def effect(s):
    """(reads, writes, is_save) of one statement, for the localized registers."""
    if s.startswith(("PPC", "uint", "REX_FUNC_PROLOGUE", "const auto s_")) or SELF_MOVE.match(s):
        return set(), set(), False
    ms = STORE_TO_STACK.match(s)
    if ms and REG.fullmatch(ms.group(1)):
        return {ms.group(1)}, set(), True
    if SIMD_STORE_MEM.match(s):
        regs = set(REG.findall(s))
        if regs and all(r.startswith("v") for r in regs):
            return regs, set(), True
    s2 = strip_so(s)
    mc = COMPARE.match(s2)
    if mc:
        return set(REG.findall(s2[len(mc.group(1)):])), {mc.group(1)}, False
    mv = SIMD_STORE_REG.match(s2)
    if mv:
        return set(REG.findall(mv.group(2))), {mv.group(1)}, False
    ma = ASSIGN.match(s2)
    lhs, rhs = (ma.group(1), s2[ma.end():]) if ma else (None, s2)
    return set(REG.findall(rhs)), ({lhs} if lhs and REG.fullmatch(lhs) else set()), False


def analyze(body):
    """Reads that may see a register no path has written yet. body: the function's raw lines.

    Forward data flow over the gotos: a register is "maybe unwritten" at a point if some path from the entry
    reaches it without writing the register. Nested returns/gotos (inside if/switch blocks) are taken as
    conditional, which can only add paths, so the result errs on the side of reporting.
    Returns ({reg: first line} of real reads, {reg: first line} of stack saves)."""
    blocks = [[]]          # statements per block: (text, reads, writes, is_save)
    succ = [set()]
    labels = {}
    open_end = [True]      # does the block fall through to the next one?
    for raw in body:
        s = raw.strip()
        if not s or s.startswith("//"):
            continue
        ml = LABEL.match(s)
        if ml:
            if blocks[-1] or labels:
                blocks.append([]), succ.append(set()), open_end.append(True)
            labels[ml.group(1)] = len(blocks) - 1
            continue
        if not open_end[-1]:  # unreachable text after a top-level goto/return: start a block nothing enters
            blocks.append([]), succ.append(set()), open_end.append(True)
        r, w, save = effect(s)
        blocks[-1].append((s, r, w, save))
        for target in GOTO.findall(s):
            succ[-1].add(target)
        top = raw.startswith("\t") and not raw.startswith("\t\t")
        if top and (s.startswith("goto ") or s == "return;" or s.startswith("__builtin_trap")):
            open_end[-1] = False
    n = len(blocks)
    edges = [set() for _ in range(n)]
    for i in range(n):
        edges[i] = {labels[t] for t in succ[i] if t in labels}
        if open_end[i] and i + 1 < n:
            edges[i].add(i + 1)
    written_in = [set().union(*[w for _, _, w, _ in b]) if b else set() for b in blocks]
    unwritten_in = [None] * n          # None: not reached yet
    unwritten_in[0] = {"*"}
    work = [0]
    while work:
        i = work.pop()
        if "*" in unwritten_in[i]:  # all registers but the "-x" ones
            out = unwritten_in[i] | {"-" + x for x in written_in[i]}
        else:
            out = unwritten_in[i] - written_in[i]
        for j in edges[i]:
            new = merge(unwritten_in[j], out)
            if new != unwritten_in[j]:
                unwritten_in[j] = new
                work.append(j)
    bad, saved = {}, {}
    for i, b in enumerate(blocks):
        u = unwritten_in[i]
        if u is None:
            continue
        cleared = set()
        for s, r, w, save in b:
            for reg in r:
                if reg not in cleared and maybe(u, reg):
                    (saved if save else bad).setdefault(reg, s[:100])
            cleared |= w
    return bad, saved


def maybe(u, reg):
    if "*" in u:
        return ("-" + reg) not in u
    return reg in u


def merge(a, b):
    """Union of two 'maybe unwritten' sets. Each is either a plain set, or {"*", "-x", ...} = all but x..."""
    if a is None:
        return b
    if "*" in a and "*" in b:
        return {"*"} | ({x for x in a if x.startswith("-")} & {x for x in b if x.startswith("-")})
    if "*" in a or "*" in b:
        full, plain = (a, b) if "*" in a else (b, a)
        return {"*"} | {x for x in full if x.startswith("-") and x[1:] not in plain}
    return a | b


def scan(gen):
    """Returns ({fn: {reg: first line}}, {fn: {reg: first line}} for saves only, {callee: [(caller, kind)]})."""
    reads, saves = {}, {}
    callers = defaultdict(list)
    for path in sorted(glob.glob(os.path.join(gen, "*_recomp.*.cpp"))):
        cur = None
        for raw in open(path, encoding="utf-8", errors="ignore"):
            m = FN.match(raw)
            if m:
                cur, body, prev, block = m.group(1), [], "", ""
                continue
            if cur is None:
                continue
            if raw.startswith("}"):
                bad, saved = analyze(body)
                if bad:
                    reads[cur] = bad
                elif saved:
                    saves[cur] = saved
                cur = None
                continue
            body.append(raw.rstrip("\n"))
            s = raw.strip()
            if not s or s.startswith("//"):
                continue
            # how each call is reached
            for callee in CALL.findall(s):
                if prev.startswith("ctx.lr ="):
                    kind = "bl"
                elif raw.startswith("\t\t") and block == "case":
                    kind = "jt"
                elif raw.startswith("\t\t"):
                    kind = "cond"
                else:
                    kind = "b"
                callers[callee].append((cur, kind))
            if s.startswith(("case ", "switch")):
                block = "case"
            elif s.startswith("if ("):
                block = "if"
            if not s.startswith("const auto s_"):
                prev = s
    return reads, saves, callers


def describe(fn, callers):
    kinds = defaultdict(set)
    for caller, kind in callers.get(fn, []):
        kinds[kind].add(caller)
    if not kinds:
        return "indirect only"
    return ", ".join(f"{k} from {' '.join(sorted(v)[:3])}{' ...' if len(v) > 3 else ''}"
                     for k, v in sorted(kinds.items()))


# Reports checked by hand on TU18: harmless, no share_registers needed.
KNOWN = {
    "sub_827DC5C4": "VMX copy kernel: stvlx128 v64/v65 to the stack in its prologue (a save)",
    "sub_827DCA90": "VMX copy kernel: xor r30,r30,r30 (zero whatever r30 held)",
    "sub_827DCE20": "VMX copy kernel: stvlx128 saves of v64/v65 and xor r30,r30,r30",
    "sub_827D7120": "r26 is copied to ctx for split-call ABI; the continuation does not read it and the parent has no later use",
    "sub_827D7D10": "r28 is copied to ctx for split-call ABI; the continuation does not read it and the parent has no later use",
    "sub_8281F610": "software-pipelined loop: the first pass mixes in the caller's v29 on the 360 too, and drops it",
    "sub_8282C2D8": "subfe r29,r29,r29: the result is -1 + CA whatever r29 held",
    "sub_82918CA0": "the game's setjmp itself (saves every register); every call goes through ppc_setjmp",
    "sub_8291B668": "r25-r29 are copied to ctx for a split call; the continuation only reads r24/r31 and the parent has no later use",
    "sub_8292C688": "r29 is copied to ctx for a split call; the continuation has no nonvolatile-register use and the parent has no later use",
    "sub_8292FA64": "not code: the pointer 0x8292FA90 and a zero word before sub_8292FA6C, decoded as one lwz",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", default=GEN)
    ap.add_argument("--saves", action="store_true")
    ap.add_argument("--strict", action="store_true")
    a = ap.parse_args()
    reads, saves, callers = scan(a.gen)
    for fn in sorted(reads):
        regs = " ".join(sorted(reads[fn]))
        note = f" | known: {KNOWN[fn]}" if fn in KNOWN else ""
        print(f"{fn} {regs} | {describe(fn, callers)} | {next(iter(reads[fn].values()))}{note}")
    if a.saves:
        print("-- saves only --")
        for fn in sorted(saves):
            print(f"{fn} {' '.join(sorted(saves[fn]))} | {describe(fn, callers)}")
    new = sorted(set(reads) - set(KNOWN))
    print(f"TOTAL {len(reads)} functions read registers before writing them, {len(new)} not known"
          f" ({len(saves)} more only save them)", file=sys.stderr)
    if a.strict and new:
        sys.exit(f"read_before_write: new findings, look at them by hand: {' '.join(new)}")


if __name__ == "__main__":
    main()
