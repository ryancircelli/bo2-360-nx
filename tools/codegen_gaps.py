"""Find code in .text that no generated function covers, and classify it.

Indirect calls (vtables, function pointers) into such code only fail at run
time ("[FATAL] Call to invalid or unregistered function"), so gaps are
declared up front. It reads the image bytes to tell padding, data,
continuations and new functions apart.

  end of function = first unconditional exit after the highest address its
  generated body mentions (loc_ labels, call return addresses)

Each gap, after trimming zero padding at both ends, is split at runs of zero
words into segments. For each segment:
  - any word Capstone cannot decode     -> "data?"  (reported, not declared)
  - a branch from the segment back into the function right before the gap
                                        -> continuation: extend that function
  - otherwise                           -> standalone function at segment start

usage: python tools/codegen_gaps.py [--min 8]   (run from the repository root)
Writes games/bo2/gaps.toml and the report work/codegen_gaps.txt (git-ignored, beside the image it reads). For another
project (the campaign's default.xex):
  --gen DIR --prefix NAME --image FILE --text START:END --image-size BYTES --toml FILE --report FILE
"""
import argparse
import bisect
import json
import os
import re
import struct
from collections import defaultdict

import capstone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "games", "bo2", "generated", "default_mp")
IMAGE = os.path.join(ROOT, "work", "dump_tu18", "default_mp.image.bin")
OUT_TOML = os.path.join(ROOT, "games", "bo2", "gaps.toml")
OUT_REPORT = os.path.join(ROOT, "work", "codegen_gaps.txt")
BASE = 0x82000000
TEXT = (0x82160000, 0x82160000 + 0x7DEB24)  # TU18 .text
IMAGE_RANGE = (BASE, BASE + 47054848)        # TU18 image

DEFINE = re.compile(r"^DEFINE_REX_FUNC\(([^)]+)\)")
LABEL = re.compile(r"\bloc_([0-9A-F]{8})\b")
RETADDR = re.compile(r"ctx\.lr = 0x([0-9A-Fa-f]{8})")

data = b""
LABELS = set()   # every loc_ label the generated code mentions (addresses that are mid-function on purpose)
PREFIX = "bo2zm"
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)


def word(ea):
    return struct.unpack(">I", data[ea - BASE: ea - BASE + 4])[0]


def decode(ea):
    ins = next(md.disasm(data[ea - BASE: ea - BASE + 4], ea), None)
    w = struct.unpack_from(">I", data, ea - BASE)[0]
    if ins is None and (w >> 26) in (4, 5, 6):   # Xbox 360 VMX128 / AltiVec: real code that capstone cannot name
        from types import SimpleNamespace
        return SimpleNamespace(mnemonic="vmx128", op_str=f"0x{w:08x}")
    return ins


def branch_target(ea, w):
    op = w >> 26
    if op == 18:  # b / bl / ba
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return li if (w & 2) else ea + li
    if op == 16:  # bc
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return bd if (w & 2) else ea + bd
    return None


def is_exit(w):
    return w in (0x4E800020, 0x4E800420) or ((w >> 26) == 18 and (w & 1) == 0)


def split_at_exits(seg, seg_end):
    """Split code after each unconditional exit (blr, bctr, b) unless earlier
    code in the piece branches past that point. Packed thunk tables such as
    `addi r3,r3,-4; b X` repeated become one function per thunk."""
    pieces, start, reach = [], seg, seg
    for ea in range(seg, seg_end, 4):
        w = word(ea)
        t = branch_target(ea, w)
        if t is not None and ea < t < seg_end:
            reach = max(reach, t)
        if is_exit(w) and ea + 4 < seg_end and reach <= ea:
            pieces.append((start, ea + 4))
            start, reach = ea + 4, ea + 4
    pieces.append((start, seg_end))
    return pieces


def measure():
    """Return {start: end} for every generated function.

    Counting instruction comments undercounts: jump tables become one C++
    switch with fewer comments. Instead take the highest address the body
    mentions (loc_ labels, call return addresses) and walk the image from
    there to the first unconditional exit (blr, bctr, b without link).
    """
    part = json.load(open(os.path.join(GEN, "codegen.partition.json")))["assignments"]
    by_file = defaultdict(list)
    for addr, idx in part.items():
        by_file[idx].append(int(addr, 16))
    last_seen, mismatches = {}, 0
    LABELS.clear()
    for idx, addrs in by_file.items():
        addrs.sort()
        names, highs = [], []
        with open(os.path.join(GEN, f"{PREFIX}_recomp.{idx}.cpp"), encoding="utf-8", errors="replace") as f:
            for line in f:
                m = DEFINE.match(line)
                if m:
                    names.append(m.group(1))
                    highs.append(0)
                elif highs:
                    for h in LABEL.findall(line):
                        highs[-1] = max(highs[-1], int(h, 16))
                        LABELS.add(int(h, 16))
                    for h in RETADDR.findall(line):
                        highs[-1] = max(highs[-1], int(h, 16) - 4)
        if len(names) != len(addrs):
            mismatches += 1
            continue
        for addr, name, high in zip(addrs, names, highs):
            if name.startswith("sub_") and int(name[4:], 16) != addr:
                mismatches += 1
                break
            last_seen[addr] = max(addr, high)
    starts = sorted(last_seen)
    ends = {}
    for i, s in enumerate(starts):
        limit = starts[i + 1] if i + 1 < len(starts) else TEXT[1]
        ea = last_seen[s]
        end = limit
        while ea < limit:
            w = word(ea)
            if w in (0x4E800020, 0x4E800420) or ((w >> 26) == 18 and (w & 1) == 0):
                end = ea + 4
                break
            ea += 4
        ends[s] = end
    return ends, mismatches


def referenced_entries(starts, ends):
    """Referenced entry addresses that fall inside a generated function body.

    The body walk in measure() counts a call's return address as part of the function, so a function that follows a
    call to a never-returning function (exit, ExTerminateThread) is swallowed: nothing else marks where it starts. A
    pointer to it (a lis+addi/ori pair, or a data word) shows it is an entry. Only addresses that no label of the
    generated code uses and whose previous word ends a function or is a call are taken."""
    import bisect
    text_lo, text_hi = TEXT
    found = {}
    def consider(t, how, table=False):
        if t % 4 or not (text_lo < t < text_hi) or t in LABELS or t in ends:
            return
        i = bisect.bisect_right(starts, t) - 1
        if i < 0 or not (starts[i] < t < ends[starts[i]]):
            return
        prev = word(t - 4)
        ends_fn = is_exit(prev) or (prev >> 26) == 18 or prev in (0x4E800020, 0x4E800420)   # b, blr, bctr (vtable slots only)
        if not (is_exit(prev) or ((prev >> 26) == 18 and (prev & 1) == 1) or (table and ends_fn)) or decode(t) is None or IMAGE_RANGE[0] <= word(t) < IMAGE_RANGE[1]:
            return   # (a word that is itself an image address is table data, not code)
        found[t] = how
    # A branch-only thunk is also an explicit reference to an entry point. The
    # scanner may have swallowed its target after a non-returning call in another
    # function; ordinary branches inside that function are not evidence of an entry.
    for source in starts:
        if ends[source] != source + 4 or not (text_lo <= source < text_hi):
            continue
        instruction = word(source)
        if instruction >> 26 == 18 and not instruction & 1:
            target = branch_target(source, instruction)
            if target is not None and not (source <= target < ends[source]):
                consider(target, "tail thunk")
    n = len(data) // 4
    def is_code(i):
        if not 0 <= i < n:
            return False
        v = struct.unpack_from(">I", data, 4 * i)[0]
        return text_lo < v < text_hi and v % 4 == 0 and not text_lo <= BASE + 4 * i < text_hi
    for k in range(n):
        w = struct.unpack_from(">I", data, 4 * k)[0]
        ea = BASE + 4 * k
        # Data words count only inside a vtable-like run (3+ consecutive words that all are code addresses): SEH/EH scope
        # tables point at handler labels in the middle of functions, but are not runs of bare code pointers.
        if not (text_lo <= ea < text_hi) and is_code(k):
            if (is_code(k - 1) and is_code(k + 1)) or (is_code(k + 1) and is_code(k + 2)) or (is_code(k - 1) and is_code(k - 2)):
                consider(w, "vtable word", True)
        if (w >> 26) == 15 and (w & 0xFFFF) >= 0x8000 and text_lo <= ea < text_hi:
            hi = (w & 0xFFFF) << 16
            for j in range(1, 7):
                if 4 * (k + j) + 4 > len(data):
                    break
                v = struct.unpack_from(">I", data, 4 * (k + j))[0]
                if (v >> 26) == 14 and ((v >> 16) & 31) == ((w >> 21) & 31):   # addi rX, rD, lo on the lis's register
                    lo = v & 0xFFFF
                    consider((hi + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF, "lis+addi")
                elif (v >> 26) == 24 and ((v >> 21) & 31) == ((w >> 21) & 31):   # ori rX, rD, lo
                    consider(hi | (v & 0xFFFF), "lis+ori")
        elif not (text_lo <= ea < text_hi) and not w % 4:
            # A word in a data section that points into .text (vtable, function pointer list). Only code outside
            # .text is scanned, so SEH/EH tables inside it are not read; consider() keeps just targets that follow
            # an exit or a call.
            consider(w, "data pointer")
    return found


def native_functions(path):
    """The addresses in the [rexcrt] section of an overrides.toml: functions the codegen drops for native code, so their
    bytes are a gap on purpose."""
    found, section = set(), None
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if line.startswith("["):
                section = line
            elif section == "[rexcrt]" and "=" in line:
                found.add(int(line.split("=", 1)[1].strip(), 16))
    return found


def main():
    ap = argparse.ArgumentParser()
    global BASE, GEN, IMAGE, OUT_TOML, OUT_REPORT, TEXT, IMAGE_RANGE, PREFIX, data
    ap.add_argument("--min", type=int, default=8)
    ap.add_argument("--gen", default=GEN, help="the generated code's folder")
    ap.add_argument("--prefix", default=PREFIX, help="the project name in the generated file names")
    ap.add_argument("--image", default=IMAGE, help="the decrypted image")
    ap.add_argument("--text", help=".text as START:END in hex (default: default_mp's)")
    ap.add_argument("--image-size", type=lambda v: int(v, 0), help="the image's size in bytes")
    ap.add_argument("--base", type=lambda v: int(v, 16), help="the image's load address in hex (default 82000000; a DLL module differs)")
    ap.add_argument("--toml", default=OUT_TOML)
    ap.add_argument("--report", default=OUT_REPORT)
    ap.add_argument("--overrides", default=os.path.join(ROOT, "games", "bo2", "overrides.toml"),
                    help="the project's overrides.toml: its [rexcrt] functions are dropped by the codegen on purpose")
    a = ap.parse_args()
    GEN, IMAGE, OUT_TOML, OUT_REPORT, PREFIX = a.gen, a.image, a.toml, a.report, a.prefix
    if a.base:
        BASE = a.base
    if a.text:
        TEXT = tuple(int(v, 16) for v in a.text.split(":"))
    if a.image_size:
        IMAGE_RANGE = (BASE, BASE + a.image_size)
    data = open(IMAGE, "rb").read()
    native = native_functions(a.overrides)
    # plus the ones the codegen swapped for rexcrt_ itself (XDK library signatures), as its register file lists them
    for f in os.listdir(GEN):
        if f.endswith("_register.cpp"):
            native |= {int(m, 16) for m in re.findall(r"SetFunction\((0x[0-9A-Fa-f]+), rexcrt_", open(os.path.join(GEN, f)).read())}

    ends, mismatches = measure()
    starts = sorted(ends)
    toml, report = [], []
    counts = defaultdict(int)

    for t, how in sorted(referenced_entries(starts, ends).items()):
        if t in native:
            continue
        counts["referenced"] += 1
        first = decode(t)
        toml.append(f'"0x{t:08X}" = {{ }}   # referenced as an entry ({how}), inside the body walk of an earlier function: {first.mnemonic} {first.op_str}'.rstrip())
        report.append(f"referenced  0x{t:08X}  {how}")

    for i, s in enumerate(starts[:-1]):
        end, nxt = ends[s], starts[i + 1]
        if end >= nxt or not (TEXT[0] <= end < TEXT[1]):
            continue
        # Split the gap into non-zero segments.
        segs, ea = [], end
        while ea < nxt:
            while ea < nxt and word(ea) == 0:
                ea += 4
            seg = ea
            while ea < nxt and word(ea) != 0:
                ea += 4
            if ea > seg:
                segs.append((seg, ea))
        if not segs:
            counts["padding"] += 1
            continue
        for seg, seg_end in segs:
            if seg_end - seg < a.min:
                counts["tiny"] += 1
                report.append(f"tiny        0x{seg:08X} {seg_end - seg:5d} B")
                continue
            bad = [x for x in range(seg, seg_end, 4) if decode(x) is None]
            if bad:
                counts["data?"] += 1
                report.append(f"data?       0x{seg:08X} {seg_end - seg:5d} B  undecodable at 0x{bad[0]:08X}")
                continue
            back = [x for x in range(seg, seg_end, 4)
                    if (t := branch_target(x, word(x))) is not None and s <= t < end]
            if back and seg == segs[0][0]:
                counts["continuation"] += 1
                toml.append(f'"0x{s:08X}" = {{ end = 0x{seg_end:08X} }}   # continuation: 0x{back[0]:08X} branches back into it')
                report.append(f"continuation 0x{seg:08X} {seg_end - seg:5d} B  parent 0x{s:08X} -> end 0x{seg_end:08X}")
            elif back:
                counts["needs-review"] += 1
                report.append(f"needs-review 0x{seg:08X} {seg_end - seg:5d} B  branches into 0x{s:08X} but not adjacent")
            elif all(IMAGE_RANGE[0] <= word(x) < IMAGE_RANGE[1] for x in range(seg, seg_end, 4)):
                # Every word is an address inside the image: a pointer table
                # (e.g. SEH scope data before a function), not code.
                counts["pointers"] += 1
                report.append(f"pointers    0x{seg:08X} {seg_end - seg:5d} B  all words are image addresses")
            else:
                for piece, piece_end in split_at_exits(seg, seg_end):
                    if piece in native:
                        counts["native"] += 1
                        report.append(f"native      0x{piece:08X} {piece_end - piece:5d} B  [rexcrt]: replaced by native code")
                        continue
                    counts["standalone"] += 1
                    first = decode(piece)
                    toml.append(f'"0x{piece:08X}" = {{ }}   # {piece_end - piece} B: '
                                f'{first.mnemonic} {first.op_str}'.rstrip())
                    report.append(f"standalone  0x{piece:08X} {piece_end - piece:5d} B  after 0x{s:08X}")

    header = [
        "# ============================================================================",
        f"#  Code gaps in the TU18 image {os.path.basename(IMAGE)} - GENERATED by tools/codegen_gaps.py",
        "#  Do not edit by hand: fix the tool or move an entry into overrides.toml.",
        f"#  {len(starts)} functions measured, {mismatches} files skipped (name/address mismatch)",
        "#  " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())),
        "# ============================================================================",
        "[functions]",
    ]
    # Merge with what is already declared: once a gap is declared, the next
    # codegen covers it and this run no longer sees it as a gap.
    entries = {}
    if os.path.exists(OUT_TOML):
        for line in open(OUT_TOML, encoding="utf-8"):
            if line.startswith('"0x') and int(line.split('"')[1], 16) not in native:
                entries[line.split('"')[1]] = line.rstrip("\n")
    for line in toml:
        entries[line.split('"')[1]] = line
    toml = [entries[k] for k in sorted(entries)]
    header[4] = f"#  {len(toml)} entries; this run: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
    with open(OUT_TOML, "w", encoding="utf-8") as f:
        f.write("\n".join(header + toml) + "\n")
    os.makedirs(os.path.dirname(OUT_REPORT), exist_ok=True)
    with open(OUT_REPORT, "w", encoding="utf-8") as f:
        f.write("\n".join(header[3:5] + report) + "\n")
    print(f"measured {len(starts)} functions, {mismatches} file mismatches")
    print(", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    print(f"wrote {OUT_TOML} ({len(toml)} entries) and {OUT_REPORT}")


if __name__ == "__main__":
    main()
