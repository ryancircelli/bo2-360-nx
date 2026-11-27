"""Builds the campaign's shader library, bo2cp_shaders.spvpack, from the game files in the data folder: the same steps
as tools/mac/build_shaders.py does for Zombies, over the campaign's
fastfiles instead.

The campaign's zones are every .ff in game/ and update/ except Zombies' (a "zm" part in the name), multiplayer's
(mp_* and *_mp) and other languages'. Strike Force is campaign: its so_/su_ zones are kept, and so are the four
multiplayer maps it plays on (mp_dockside, mp_drone, mp_overflow, mp_socotra), which it may load alongside them.
One Zombies zone is read as well, patch_ui_zm: the campaign creates its 2D "vertcolorsimple" shaders (a texture
times the vertex color: HUD icons, the button glyphs in prompts) at startup from data that is in none of its own
zones, and that zone carries the same shaders. Without them those HUD draws have no shader and are skipped.

Everything it writes is derived from the game and stays in the data folder (default <data>/shaders/campaign):
  unique/   the shader blobs, one per content hash (kept: a later run only adds new zones)
  hlsl/, spirv/  the translation's intermediate files (removed at the end, as the installer does)
  bo2cp_shaders.spvpack  the library

usage: python -I campaign/build_shaders.py [--out DIR] [--tools DIR] [--dxc DIR] [--jobs N] [--list] [--no-extract]
  --tools  the folder with bo2zm_hlsl.exe and bo2zm_pack.exe (default work/xenosrecomp)
  --dxc    the folder with dxc.exe and spirv-val.exe (default: the newest Vulkan SDK in C:/VulkanSDK)
  --list   only print the zones it would read
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
OTHER_LANGUAGE = re.compile(r"^(bp|fj|fr|ge|it|ja|ms|po|ru|sp)_")  # as tools/mac/build_shaders.py
STRIKE_FORCE_MAPS = {"mp_dockside", "mp_drone", "mp_overflow", "mp_socotra"}
# Zones of the other modes that hold shaders the campaign uses but does not load from its own zones (see above).
SHADERS_FROM_OTHER_ZONES = {"patch_ui_zm"}


def data_dir():
    return subprocess.run(["bash", os.path.join(ROOT, "tools", "data_dir.sh")], capture_output=True, text=True,
                          check=True).stdout.strip()


def windows_path(path):
    """A Git Bash path (/e/...) as Windows writes it (E:/...)."""
    m = re.match(r"^/([a-zA-Z])/(.*)$", path)
    return f"{m.group(1).upper()}:/{m.group(2)}" if m else path


def campaign_zone(name):
    stem = name[:-3].lower()
    if not name.endswith(".ff") or OTHER_LANGUAGE.match(stem) or stem.startswith("en_"):
        return False
    if stem in SHADERS_FROM_OTHER_ZONES:
        return True
    if re.search(r"(^|_)zm(_|$)", stem):
        return False
    if stem.startswith(("so_", "su_")) or stem in STRIKE_FORCE_MAPS:
        return True
    return not (stem.startswith("mp_") or stem.endswith("_mp") or stem == "ui_mp")


def zones(data):
    found = []
    for folder in ("game", "update"):
        path = os.path.join(data, folder)
        found += [os.path.join(path, n) for n in sorted(os.listdir(path)) if campaign_zone(n)]
    return found


def say(text):
    print(f"[{time.strftime('%H:%M:%S')}] {text}", flush=True)


def compile_spirv(dxc_dir, hlsl, spirv, sources, log_path, jobs):
    """dxc and spirv-val over every HLSL file, with the installer's flags. Returns the names that failed."""
    dxc = os.path.join(dxc_dir, "dxc.exe")
    val = os.path.join(dxc_dir, "spirv-val.exe")
    done = [0]
    started = time.time()

    def one(name):
        base = name[:-5]
        kind, extra = ("ps_6_6", []) if base.startswith("p_") else ("vs_6_6", ["-fvk-invert-y"])
        out = os.path.join(spirv, base + ".spv")
        r = subprocess.run([dxc, "-spirv", "-T", kind, "-E", "main", "-HV", "2021", "-fspv-target-env=vulkan1.2",
                            "-fvk-use-dx-layout", *extra, "-Fo", out, os.path.join(hlsl, name)],
                           capture_output=True, text=True, errors="replace")
        if r.returncode == 0:
            r = subprocess.run([val, "--target-env", "vulkan1.2", "--scalar-block-layout", out],
                               capture_output=True, text=True, errors="replace")
        done[0] += 1
        if done[0] % 1000 == 0:
            say(f"  {done[0]} of {len(sources)} compiled ({time.time() - started:.0f} s)")
        return base, r.returncode, (r.stdout + r.stderr).strip()[:300]

    failed = []
    with ThreadPoolExecutor(max_workers=jobs) as pool, open(log_path, "a", encoding="utf-8", errors="replace") as log:
        for base, rc, text in pool.map(one, sources):
            if rc != 0:
                failed.append(base)
                log.write(f"{base}: {text}\n")
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out")
    ap.add_argument("--tools", default=os.path.join(ROOT, "work", "xenosrecomp"))
    ap.add_argument("--dxc")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--no-extract", action="store_true",
                    help="translate and pack the shaders already in unique/ (after a translator change)")
    a = ap.parse_args()

    data = windows_path(data_dir())
    found = zones(data)
    if a.list:
        for z in found:
            print(z)
        print(f"{len(found)} zones")
        return 0
    dxc_dir = a.dxc or (sorted(glob.glob("C:/VulkanSDK/*/Bin")) or [""])[-1]
    for exe in (os.path.join(a.tools, "bo2zm_hlsl.exe"), os.path.join(a.tools, "bo2zm_pack.exe"),
                os.path.join(dxc_dir, "dxc.exe"), os.path.join(dxc_dir, "spirv-val.exe")):
        if not os.path.isfile(exe):
            sys.exit(f"missing {exe} (see --tools and --dxc)")
    out = a.out or os.path.join(data, "shaders", "campaign")
    unique, hlsl, spirv = (os.path.join(out, d) for d in ("unique", "hlsl", "spirv"))
    scratch = os.path.join(out, "zones")
    for d in (scratch, hlsl, spirv):
        shutil.rmtree(d, ignore_errors=True)
    for d in (unique, scratch, hlsl, spirv):
        os.makedirs(d, exist_ok=True)
    log_path = os.path.join(out, "build_shaders.log")

    if not a.no_extract:
        say(f"extracting from {len(found)} zones into {out}")
        image = os.path.join(ROOT, "work", "dump_tu18", "default_mp.image.bin")  # the zones' key (ff_shaders.py)
        subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "inspect", "ff_shaders.py"), *found,
                        "--dump", "--out", scratch, "--xex-image", image], check=True)
    # The blobs are named by content, so one folder of them all is the set without duplicates.
    for d in os.listdir(scratch):
        path = os.path.join(scratch, d)
        if d.endswith("_shaders") and os.path.isdir(path):
            for n in os.listdir(path):
                if n.endswith(".bin") and not os.path.exists(os.path.join(unique, n)):
                    os.replace(os.path.join(path, n), os.path.join(unique, n))
    shutil.rmtree(scratch, ignore_errors=True)  # the decompressed zones: gigabytes
    say(f"{len(os.listdir(unique))} shaders; translating")

    common = os.path.join(ROOT, "tools", "xenosrecomp", "translator", "shader_common.h")
    subprocess.run([os.path.join(a.tools, "bo2zm_hlsl.exe"), unique, hlsl, common], check=True,
                   stdout=subprocess.DEVNULL)
    sources = sorted(n for n in os.listdir(hlsl) if n.endswith(".hlsl"))
    say(f"{len(sources)} translated; compiling to SPIR-V")
    failed = compile_spirv(dxc_dir, hlsl, spirv, sources, log_path, a.jobs)
    if failed:
        say(f"{len(failed)} did not compile and are left out (see {log_path}): {', '.join(failed[:5])}")
    pack = os.path.join(out, "bo2cp_shaders.spvpack")
    if os.path.exists(pack):
        os.remove(pack)
    subprocess.run([os.path.join(a.tools, "bo2zm_pack.exe"), unique, spirv, pack], check=True)
    say(f"shader library: {pack}, {os.path.getsize(pack) / 1e6:.1f} MB")
    for d in (hlsl, spirv):
        shutil.rmtree(d, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
