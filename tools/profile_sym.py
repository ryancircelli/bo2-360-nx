# Symbolizes the console profiler's log: replaces each "image+0xNNN" (the SDK profiler's own format) in
# logs/rex/rex_profile.log with the function (and file) from the ELF of the same build (games/bo2/out/switch/bo2zm, the
# unstripped executable the build leaves beside the NRO; an ELF of a different build gives wrong names).
# usage: python tools/profile_sym.py rex_profile.log bo2zm_<commit>.elf out.txt
import re, subprocess, sys

ADDR_RE = r"image\+0x([0-9a-f]+)"
ADDR2LINE = r"C:\devkitPro\devkitA64\bin\aarch64-none-elf-addr2line.exe"
src, elf, dst = sys.argv[1:4]
text = open(src, encoding="utf-8", errors="replace").read()
addrs = sorted(set(re.findall(ADDR_RE, text)))
names = {}
CHUNK = 2000
# 1:1 mapping (no inlining chains), plus file:line
for i in range(0, len(addrs), CHUNK):
    part = addrs[i:i + CHUNK]
    out = subprocess.run([ADDR2LINE, "-f", "-C", "-e", elf] + ["0x" + a for a in part],
                         capture_output=True, text=True).stdout.splitlines()
    for k, a in enumerate(part):
        func = out[2 * k] if 2 * k < len(out) else "?"
        loc = out[2 * k + 1] if 2 * k + 1 < len(out) else "?"
        loc = re.sub(r".*[/\\]", "", loc).split(" ")[0]
        func = re.sub(r"\(anonymous namespace\)::", "", func)
        func = re.sub(r"bo2zm::native::", "", func)
        func = re.sub(r"\(.*", "", func)  # drop the parameter list
        names[a] = "%s@%s" % (func[-70:], loc)

def sub(m):
    return names.get(m.group(1), m.group(0))

open(dst, "w", encoding="utf-8").write(re.sub(ADDR_RE, sub, text))
print("symbolized", len(addrs), "addresses")
