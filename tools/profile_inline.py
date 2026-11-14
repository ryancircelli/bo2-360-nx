# Sums one thread's samples in the console profiler's log (logs/rex/rex_profile.log, with bo2zm_profile_stacks = 1)
# by the outermost function of each sample's inline chain (addr2line -i), and with --lines by its file:line, so the
# samples inside one big function with everything inlined (the native renderer's Draw) split by the line that called
# the inlined code. Needs an ELF with line tables: a BO2_DEBUG_LINES=1 build of the same commit (games/bo2/build_switch.sh;
# its .text is the same as the normal build's). Kernel waits (svc) are left out of the busy total.
# usage: python tools/profile_inline.py rex_profile.log bo2zm_<commit>_lines.elf [--thread NAME] [--top N] [--lines]
#        [--only SUBSTRING]
import argparse, re, subprocess
from collections import Counter

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("elf")
ap.add_argument("--thread", default="GPU native ring")
ap.add_argument("--top", type=int, default=30)
ap.add_argument("--lines", action="store_true", help="group by the outermost frame's file:line")
ap.add_argument("--only", default=None, help="only outermost functions whose name contains this")
ap.add_argument("--inner", action="store_true", help="group by the innermost frame (the inlined function) instead")
a = ap.parse_args()
ADDR2LINE = r"C:\devkitPro\devkitA64\bin\aarch64-none-elf-addr2line.exe"

counts = Counter()
inside, samples = False, 0
for line in open(a.log, encoding="utf-8", errors="replace"):
    if line.startswith("-- thread "):
        inside = ('"%s"' % a.thread) in line
        samples = 0
        continue
    if not inside:
        continue
    m = re.match(r"\s+(\d+) samples", line)
    if m:
        samples = int(m.group(1))
        continue
    m = re.match(r"\s+([\d.]+)%\s+pc image\+0x([0-9a-f]+)", line)
    if m and samples:
        counts[m.group(2)] += float(m.group(1)) * samples / 100.0

chains = {}
addrs = list(counts)
for i in range(0, len(addrs), 500):
    part = addrs[i:i + 500]
    out = subprocess.run([ADDR2LINE, "-i", "-f", "-C", "-a", "-e", a.elf] + ["0x" + x for x in part],
                         capture_output=True, text=True).stdout.splitlines()
    cur, frames = None, []
    for l in out + ["0x0"]:
        if re.match(r"^0x[0-9a-f]+$", l):
            if cur is not None:
                chains[cur] = frames
            cur, frames = l[2:].lstrip("0"), []
            continue
        frames.append(l)


def clean(name):
    name = name.replace("(anonymous namespace)::", "").replace("bo2zm::native::", "")
    return re.sub(r"\(.*", "", name)[-60:]


groups = Counter()
busy = 0.0
for addr, n in counts.items():
    frames = chains.get(addr.lstrip("0"), [])
    if len(frames) < 2 or any(".s:" in loc for loc in frames[1::2]):
        continue  # unknown, or a kernel call (svc.s): waiting, not working
    busy += n
    name = clean(frames[-2])
    if a.only and a.only not in name:
        continue
    loc = frames[-1].replace("\\", "/").split("/")[-1].split(" ")[0]
    if a.inner:
        name = clean(frames[0])
        loc = frames[1].replace("\\", "/").split("/")[-1].split(" ")[0]
    groups[name + " @" + (loc if a.lines else loc.split(":")[0])] += n
print("thread %s: %.0f busy samples" % (a.thread, busy))
for key, n in groups.most_common(a.top):
    print("%6.0f %5.1f%%  %s" % (n, 100 * n / busy if busy else 0, key))
