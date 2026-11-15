#!/usr/bin/env -S python3 -I
"""Builds a runnable, signed Mac app from the player's own copy of a game, in one command:

  tools/mac/build_app.py (--iso FILE | --folder DIR) [--dlc PATH]... [--title-update PATH]... [--out DIR] [--install]

  --iso FILE           an Xbox 360 disc image (XGD2 or XGD3 .iso)
  --folder DIR         a folder extracted from one (the folder with default.xex / default_mp.xex)
  --dlc PATH           DLC: an STFS package (LIVE/PIRS/CON), a folder of them, or an installed content folder (the
                       one holding 0000000000000000/<title id>/). Repeatable.
  --title-update PATH  a title update: its STFS package, a folder holding it, or the folder extracted from it.
                       Repeatable; the one the port is built against is picked.
  --out DIR            where the app is written (default: the current folder)
  --install            write it to ~/Games/Launchers instead (--launchers-dir to change), replacing an older copy
  --work DIR           the work folder for the extracted data, generated code and build
                       (default ~/Games/Xbox360/app-builder/<game>); kept between runs, so a rebuild is incremental

The game is recognised from the title id in its executable: Black Ops II (415608C3, with Title Update 18), Black Ops
(41560855), Forza Horizon (4D5309C9) and Need for Speed Most Wanted (454107D9). Anything else, or an incomplete copy,
is refused with one sentence saying what is missing.

The recompiled code is made from one exact executable, so the builder checks it: each game's executables must have
the SHA-256 recorded in games/<game>/app.toml [source] (Black Ops II: tools/install/game_files.py, the disc's
default_mp.xex plus Title Update 18's default_mp.xexp). Another version of the executable (another region, another
title update) would not match the generated code and is refused rather than built into an app that cannot run.

What it does, in order: copies the game's files into <work>/data (game/, update/ for Black Ops II's title update,
content/ for DLC), never changing the originals (files are APFS clones or copies, never hard links); generates the
code from the executable (rexglue codegen plus the game's fix-up passes, skipped when nothing changed); builds it
(through buildq when it is there); and packs the app with tools/mac/pack_app.py, the game data cloned inside it.
The generated code and the build live in <work>/tree, a mirror of this checkout, so the checkout stays clean and two
builds never share generated code. Needs Xcode, cmake, ninja and, for Black Ops II, the LunarG Vulkan SDK (dxc).
"""
import argparse
import ctypes
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "tools")
for _p in (os.path.join(TOOLS, "inspect"), os.path.join(TOOLS, "install"), TOOLS):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import game_files  # noqa: E402
import install_dlc  # noqa: E402
import title_packs  # noqa: E402
import xiso  # noqa: E402
from stfs_extract import Stfs  # noqa: E402

SUPPORTED = {"bo2": "Black Ops II", "bo1": "Black Ops", "fh1": "Forza Horizon", "nfsmw": "Need for Speed Most Wanted"}
STFS_MAGICS = (b"CON ", b"LIVE", b"PIRS")
CONTENT_TITLE_UPDATE = 0x000B0000
BUILDQ = os.environ.get("BUILDQ", os.path.expanduser("~/Games/Xbox360/bo2-build/runq/bin/buildq"))
# Tree entries the mirror makes itself (the rest of the checkout is linked in).
OWN_ENTRIES = {"games", "app", "work", "out", "bo2_data.txt", ".git"}
# The generated code depends on these tools as well as the game's own files.
CODEGEN_TOOLS = ("read_before_write.py", "setjmp_locals.py", "direct_calls.py", "localize.py", "guest_barriers.py",
                 "codegen.sh")


class Refused(Exception):
    """The input cannot be built; the message is one plain sentence for the player."""


def say(text=""):
    print(text, flush=True)


def sha256_file(path):
    return game_files.sha256(path)


# --- Copies that never touch the originals ---------------------------------------------------------------------------

_libc = ctypes.CDLL(None, use_errno=True)
_clonefile = getattr(_libc, "clonefile", None)


def clone_or_copy(src, dst):
    """An APFS clone of src at dst (no extra disk space, and writing one never changes the other), else a copy."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.remove(dst)
    if _clonefile is not None and _clonefile(os.fsencode(src), os.fsencode(dst), 0) == 0:
        return
    shutil.copy2(src, dst + ".part")
    os.replace(dst + ".part", dst)


# game_files lays files out with hard links, which share the original's data: a later write through either name
# would change the player's file. The builder's copies are clones instead.
game_files.link_or_copy = clone_or_copy


def clone_tree(src, dst, skip=lambda rel: False):
    """Makes dst hold the files of src (clones), keeping files already there with the same size. Returns the count."""
    count = 0
    for dp, dn, fn in os.walk(src):
        rel_dir = os.path.relpath(dp, src)
        dn[:] = [d for d in dn if not skip(os.path.normpath(os.path.join(rel_dir, d)))]
        for n in fn:
            rel = os.path.normpath(os.path.join(rel_dir, n))
            if skip(rel):
                continue
            s, d = os.path.join(dp, n), os.path.join(dst, rel)
            if not (os.path.isfile(d) and os.path.getsize(d) == os.path.getsize(s)):
                clone_or_copy(s, d)
            count += 1
    return count


def disc_skip(rel):
    top = rel.split(os.sep)[0].lower()
    return top == "$systemupdate"


# --- Inputs ----------------------------------------------------------------------------------------------------------

def packages_in(path):
    """[(path, Stfs header facts)] of the STFS packages at path (a file or a folder, searched recursively)."""
    files = [path] if os.path.isfile(path) else [os.path.join(dp, n) for dp, _, fn in os.walk(path) for n in sorted(fn)]
    found = []
    for f in files:
        try:
            with open(f, "rb") as h:
                if h.read(4) not in STFS_MAGICS:
                    continue
            s = Stfs(f)
        except (OSError, ValueError, struct.error):
            continue
        try:
            found.append((f, {"title_id": s.title_id, "content_type": s.content_type,
                              "display_name": s.display_name}))
        finally:
            s.f.close()
    return found


def identify(source):
    title, pack = title_packs.identify(source)
    if title is None:
        raise Refused(f"{source} has no Xbox 360 game executable (default.xex) that names a title: "
                      "is it the game's disc image or the folder extracted from it?")
    if pack not in SUPPORTED:
        raise Refused(f"{source} is a game this builder does not support (title id {title}); it builds "
                      + ", ".join(SUPPORTED.values()) + ".")
    return title, pack


def app_facts(game):
    if game == "bo2":
        return {"name": "Black Ops II", "source": {}}
    with open(os.path.join(ROOT, "games", game, "app.toml"), "rb") as f:
        return tomllib.load(f)


def check_executables(game, facts, game_dir):
    """Refuses a copy whose executables are not the ones the code is generated from."""
    if game == "bo2":
        wanted = {"default_mp.xex": game_files.DISC_XEX_SHA256}
    else:
        wanted = facts.get("source", {}).get("executables", {})
    for name, digest in wanted.items():
        path = os.path.join(game_dir, name)
        if not os.path.isfile(path):
            raise Refused(f"the copy of {SUPPORTED[game]} has no {name}: is it complete?")
        if sha256_file(path) != digest:
            raise Refused(f"the copy's {name} is not the version this port is generated from (another region or "
                          f"revision of {SUPPORTED[game]}), so the recompiled code would not match it.")


# --- Game data -------------------------------------------------------------------------------------------------------

def extract_disc(source, kind, game_dir, modes=None):
    """The disc's files into game_dir. modes: Black Ops II's mode filter (game_files), None for every file."""
    keep = (lambda rel: True) if modes is None else (lambda rel: game_files.wanted(rel, modes))
    if kind == "iso":
        say(f"  extracting {os.path.basename(source)} (several minutes the first time)")
        last = [-10]

        def progress(done, total, rel):
            pct = done * 100 // max(total, 1)
            if pct >= last[0] + 10:
                last[0] = pct - pct % 10
                say(f"    {last[0]} %")

        try:
            with xiso.Xiso(source) as x:
                x.extract(game_dir, skip=lambda rel: disc_skip(rel) or not keep(rel), progress=progress)
        except xiso.XisoError as e:
            raise Refused(str(e)) from None
    else:
        say(f"  copying {source}")
        n = clone_tree(source, game_dir,
                       skip=lambda rel: disc_skip(rel) or rel.split(os.sep)[0].lower() == "content"
                       or (os.path.isfile(os.path.join(source, rel)) and not keep(rel)))
        say(f"    {n} files")


def title_update_dir(paths, work):
    """Black Ops II: the folder with Title Update 18's files (default_mp.xexp and the patch zones), from the
    --title-update inputs. None when none of them is TU18."""
    others = []
    for path in paths:
        if os.path.isdir(path):
            for dp, _, fn in os.walk(path):
                if "default_mp.xexp" in fn:
                    if sha256_file(os.path.join(dp, "default_mp.xexp")) == game_files.TU18_XEXP_SHA256:
                        return dp
                    others.append(dp)
        for pkg, facts in packages_in(path):
            if facts["title_id"] != game_files.TITLE_ID or facts["content_type"] != CONTENT_TITLE_UPDATE:
                continue
            s = Stfs(pkg)
            try:
                tu18 = game_files.is_tu18(s)
            finally:
                s.f.close()
            if tu18:
                out = os.path.join(work, "title_update")
                game_files.extract_package(pkg, out)
                return out
            others.append(pkg)
    if others:
        raise Refused("the title update given is not Title Update 18, the one Black Ops II's port is built against "
                      f"({others[0]}).")
    return None


def plan_content(paths, title):
    """([(package, facts)], [installed content folders]) of the --dlc inputs; refuses any that is not this game's DLC
    before anything is copied."""
    title_id = int(title, 16)
    packages, installed_dirs = [], []
    for path in paths:
        found = packages_in(path)
        mine = [(p, f) for p, f in found if f["title_id"] == title_id]
        if mine:
            packages += mine
            continue
        tree = os.path.join(path, "0000000000000000", title.upper())
        if os.path.isdir(path) and os.path.isdir(tree):
            installed_dirs.append(path)
            continue
        if found:
            raise Refused(f"the DLC at {path} is for another game (title id {found[0][1]['title_id']:08X}).")
        raise Refused(f"{path} holds no DLC package (STFS LIVE/PIRS/CON file) or installed content folder.")
    return packages, installed_dirs


def install_content(plan, title, content, modes=None):
    """DLC into content/0000000000000000/<title>/<type>/<package>/ with its Headers/ entry, the layout the runtime's
    content manager reads. Black Ops II keeps only the Zombies and Multiplayer files (game_files)."""
    title_id = int(title, 16)
    packages, installed_dirs = plan
    names = []
    for path in installed_dirs:
        say(f"  DLC folder {path}")
        clone_tree(path, content)
        names.append(path)
    if game_files.TITLE_ID == title_id:
        pk = []
        for p, _ in packages:
            s = Stfs(p)
            try:
                if s.content_type == game_files.CONTENT_DLC:
                    pk.append(game_files.Package(p, s))
            finally:
                s.f.close()
        names += [p.display_name for p in game_files.install_packages(pk, modes, content)]
        return names
    root = os.path.join(content, "0000000000000000", title.upper())
    for p, facts in packages:
        kind = f"{facts['content_type']:08X}"
        name = os.path.basename(p)
        display = facts["display_name"].replace("�", "").strip() or name
        say(f"  {display}")
        game_files.extract_package(p, os.path.join(root, kind, name))
        os.makedirs(os.path.join(root, "Headers", kind), exist_ok=True)
        mask = install_dlc.license_mask(p)
        with open(os.path.join(root, "Headers", kind, name + ".header"), "wb") as f:
            f.write(install_dlc.header_bytes(name, display, title_id, facts["content_type"]))
            if mask:
                f.write(struct.pack("<I", mask))
        names.append(display)
    return names


def lay_out_data(game, title, facts, args, data, work):
    game_dir = os.path.join(data, "game")
    modes = {"zm", "mp"} if game == "bo2" else None
    kind = "iso" if args.iso else "folder"
    source = args.iso or args.folder
    if game != "bo2" and args.title_update:
        raise Refused(f"{SUPPORTED[game]} is built from the disc's executable alone; leave out --title-update.")
    tu = None
    if game == "bo2":
        tu = title_update_dir(args.title_update, work)
        if tu is None:
            # A folder copy may already carry it (the layout this builder and tools/build_mac.sh write).
            for cand in (os.path.join(source, "..", "update"), source) if kind == "folder" else ():
                xexp = os.path.join(cand, "default_mp.xexp")
                if os.path.isfile(xexp) and sha256_file(xexp) == game_files.TU18_XEXP_SHA256:
                    tu = os.path.normpath(cand)
                    break
        if tu is None:
            raise Refused("Black Ops II needs Title Update 18: pass its package (or the folder extracted from it) "
                          "with --title-update.")
    dlc = plan_content(args.dlc, title)
    say("Game files")
    extract_disc(source, kind, game_dir, modes)
    check_executables(game, facts, game_dir)
    if game == "bo2":
        say("Title Update 18")
        patches = {n: os.path.join(tu, n) for n in os.listdir(tu)
                   if os.path.isfile(os.path.join(tu, n)) and game_files.wanted(n, modes)}
        game_files.sync_folder(patches, os.path.join(data, "update"))
        # The game loads the patch from beside the executable, as on the console's hard drive.
        for n in ("default_mp.xexp", "default.xexp"):
            if n in patches:
                clone_or_copy(patches[n], os.path.join(game_dir, n))
    say("DLC")
    content = os.path.join(data, "content")
    if os.path.isdir(content):
        shutil.rmtree(content)  # exactly the DLC of this run
    os.makedirs(content)
    names = install_content(dlc, title, content, modes)
    say("  " + (", ".join(names) if names else "none"))
    return names


# --- The mirror tree, codegen and build ------------------------------------------------------------------------------

def git_files(prefix):
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z", "--", prefix], capture_output=True, check=True)
    return [f for f in out.stdout.decode().split("\0") if f]


def make_tree(tree, game, host):
    """<work>/tree: the checkout's entries as links, games/<game> as a copy of its tracked files (codegen writes beside
    them), the other games as links, out/host-mac as the host tools."""
    os.makedirs(os.path.join(tree, "games"), exist_ok=True)
    for name in os.listdir(ROOT):
        if name in OWN_ENTRIES:
            continue
        link = os.path.join(tree, name)
        if not os.path.islink(link) or os.readlink(link) != os.path.join(ROOT, name):
            if os.path.lexists(link):
                os.remove(link)
            os.symlink(os.path.join(ROOT, name), link)
    for name in os.listdir(os.path.join(ROOT, "games")):
        if name != game and not os.path.lexists(os.path.join(tree, "games", name)):
            os.symlink(os.path.join(ROOT, "games", name), os.path.join(tree, "games", name))
    gdir = os.path.join(tree, "games", game)
    tracked = git_files(f"games/{game}")
    keep = {os.path.normpath(os.path.join(tree, f)) for f in tracked}
    for f in tracked:
        s, d = os.path.join(ROOT, f), os.path.join(tree, f)
        if not (os.path.isfile(d) and open(s, "rb").read() == open(d, "rb").read()):
            os.makedirs(os.path.dirname(d), exist_ok=True)
            shutil.copy2(s, d)
    # Files dropped from the checkout since the last run (generated code and executables stay).
    for dp, dn, fn in os.walk(gdir):
        dn[:] = [d for d in dn if d not in ("generated", "work", "out")]
        for n in fn:
            p = os.path.normpath(os.path.join(dp, n))
            if p not in keep and not n.endswith((".xex", ".xexp", ".log")) and n != ".codegen-stamp":
                os.remove(p)
    if game == "bo2" and not os.path.lexists(os.path.join(tree, "app")):
        os.symlink("games/bo2", os.path.join(tree, "app"))
    os.makedirs(os.path.join(tree, "out"), exist_ok=True)
    hm = os.path.join(tree, "out", "host-mac")
    if os.path.realpath(hm) != os.path.realpath(host):
        if os.path.lexists(hm):
            os.remove(hm)
        os.symlink(host, hm)
    return gdir


def run(cmd, cwd=None, env=None, log=None, quiet=False):
    say("  $ " + " ".join(cmd) if not quiet else "")
    out = open(log, "a") if log else None
    try:
        r = subprocess.run(cmd, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT if out else None)
    finally:
        if out:
            out.close()
    if r.returncode:
        tail = ""
        if log and os.path.isfile(log):
            tail = "\n" + "".join(open(log, errors="replace").readlines()[-25:])
        raise SystemExit(f"failed ({r.returncode}): {' '.join(cmd)}{tail}")


def build_host(host, jobs, log):
    """rexglue (the code generator) and xexdump, built once from this checkout's SDK."""
    rexglue = os.path.join(host, "bin", "rexglue")
    if os.path.isfile(rexglue) and os.path.isfile(os.path.join(host, "switch_bo2", "xexdump")):
        return rexglue
    say("Host tools (rexglue, xexdump): first build, several minutes")
    if not os.path.isdir(os.path.join(ROOT, "sdk", "thirdparty", "cli11", "include")):
        run([sys.executable, "-I", os.path.join(TOOLS, "fetch_thirdparty.py")], log=log)
    for d in os.listdir(os.path.join(ROOT, "sdk", "thirdparty")):
        p = os.path.join(ROOT, "sdk", "thirdparty", d)
        if os.path.isdir(p) and not os.path.exists(os.path.join(p, ".git")):
            os.makedirs(os.path.join(p, ".git"))
    run(["cmake", "-S", os.path.join(ROOT, "sdk"), "-B", host, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_OSX_ARCHITECTURES=arm64",
         "-DCMAKE_C_FLAGS=-march=armv8-a", "-DCMAKE_CXX_FLAGS=-march=armv8-a", f"-DREXGLUE_OUTPUT_DIR={host}/bin",
         f"-DCMAKE_PROJECT_ReXGlue_INCLUDE={ROOT}/cmake/host_tools_inject.cmake"], log=log)
    run(buildq() + ["cmake", "--build", host, "-j", str(jobs), "--target", "rexglue", "xexdump"], log=log)
    return rexglue


def buildq():
    return [BUILDQ, "--"] if os.access(BUILDQ, os.X_OK) else []


def stamp_of(parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(p if isinstance(p, bytes) else str(p).encode())
        h.update(b"\0")
    return h.hexdigest()


def codegen(game, facts, gdir, data, rexglue, log):
    """Generates games/<game>/generated in the tree from the game's executables, then runs the game's fix-up passes
    (app.toml [source] codegen_passes). Skipped when the executables, the game's files, the passes and rexglue are
    the same as last time."""
    src = facts.get("source", {})
    exes = src.get("executables", {})
    for name in exes:
        clone_or_copy(os.path.join(data, "game", name), os.path.join(gdir, name))
    passes = src.get("codegen_passes", [])
    parts = [rexglue_digest(rexglue), " ".join(passes)] + [f"{n}={d}" for n, d in sorted(exes.items())]
    for f in sorted(git_files(f"games/{game}")):
        parts.append(f)
        parts.append(open(os.path.join(ROOT, f), "rb").read())
    for t in CODEGEN_TOOLS:
        p = os.path.join(TOOLS, t)
        parts.append(open(p, "rb").read() if os.path.isfile(p) else b"")
    stamp = stamp_of(parts)
    stamp_file = os.path.join(gdir, ".codegen-stamp")
    if (os.path.isfile(stamp_file) and open(stamp_file).read() == stamp
            and os.path.isfile(os.path.join(gdir, "generated", "rexglue.cmake"))):
        say("Generated code: up to date")
        return
    say("Generated code (a few minutes)")
    if os.path.isfile(stamp_file):
        os.remove(stamp_file)
    shutil.rmtree(os.path.join(gdir, "generated"), ignore_errors=True)
    manifest = f"{game}_manifest.toml"
    run(["nice", "-n", "10", rexglue, "codegen", "--ignore-stamp", manifest], cwd=gdir, log=log)
    with open(os.path.join(gdir, manifest), "rb") as f:
        man = tomllib.load(f)
    gens = [os.path.normpath(m["out_directory_path"]) for m in modules_of(man)]
    py = [sys.executable, "-I"]
    for g in gens:
        for p in passes:
            args = {"read_before_write": ["--strict"], "setjmp_locals": ["--fix"]}.get(p, [])
            run(py + [os.path.join(TOOLS, p + ".py"), "--gen", g] + args, cwd=gdir, log=log)
    with open(stamp_file, "w") as f:
        f.write(stamp)


def modules_of(manifest):
    """The output folders the manifest generates (its entry point and any modules)."""
    found = []

    def walk(v):
        if isinstance(v, dict):
            if "out_directory_path" in v:
                found.append(v)
            for x in v.values():
                walk(x)
        elif isinstance(v, list):
            for x in v:
                walk(x)
    walk(manifest)
    return found


_digests = {}


def rexglue_digest(path):
    if path not in _digests:
        _digests[path] = sha256_file(path)
    return _digests[path]


def build_bo2(tree, work, jobs, log):
    """tools/build_mac.sh's dump, codegen, shaders and app steps in the tree, with everything under work."""
    env = dict(os.environ, BO2_BUILD=work, BO2_OUT=os.path.join(work, "build"),
               BO2_SHADERS=os.path.join(work, "shaders"), BO2_RUN=os.path.join(work, "run"),
               PYTHON=sys.executable, JOBS=str(jobs))
    script = os.path.join(tree, "tools", "build_mac.sh")
    say("Executable dump, generated code and shaders (several minutes)")
    run(["bash", script, "--only", "dump,codegen,shaders"], env=env, log=log)
    say("Build (long the first time)")
    run(buildq() + ["bash", script, "--only", "app"], env=env, log=log)
    return os.path.join(work, "build")


def build_other(game, tree, work, log):
    bdir = os.path.join(work, "build")
    say("Build (long the first time)")
    env = dict(os.environ, PYTHON=sys.executable, BUILDQ=BUILDQ)
    run(["bash", os.path.join(tree, "tools", "mac", "make_game_app.sh"), game, "--build-dir", bdir, "--no-pack"],
        env=env, log=log)
    return bdir


def pack(game, facts, tree, bdir, data, work, app):
    say(f"Packing {app}")
    cmd = [sys.executable, "-I", os.path.join(tree, "tools", "mac", "pack_app.py"), "--build", bdir, "--data", data,
           "--data-mode", "clone", "--out", app]
    if game == "bo2":
        cmd += ["--shaders", os.path.join(work, "shaders")]
    else:
        cmd += ["--game", game]
    run(cmd)


# --- Main ------------------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description="Build a Mac app from your own copy of a supported Xbox 360 game.")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--iso", help="an Xbox 360 disc image (.iso)")
    src.add_argument("--folder", help="a folder extracted from the disc (with default.xex)")
    ap.add_argument("--dlc", action="append", default=[], help="DLC package, folder of packages or content folder")
    ap.add_argument("--title-update", action="append", default=[], help="title update package or extracted folder")
    ap.add_argument("--out", default=".", help="folder the app is written to (default: the current folder)")
    ap.add_argument("--install", action="store_true", help="write the app to the launchers folder instead")
    ap.add_argument("--launchers-dir", default=os.path.expanduser("~/Games/Launchers"))
    ap.add_argument("--work", help="work folder (default ~/Games/Xbox360/app-builder/<game>)")
    ap.add_argument("--host", help="host tools build folder (default <checkout>/out/host-mac)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--data-only", action="store_true", help="stop after laying out the game data (for tests)")
    args = ap.parse_args(argv)
    if sys.platform != "darwin" or os.uname().machine != "arm64":
        raise SystemExit("tools/mac/build_app.py is for macOS on Apple silicon")

    source = os.path.abspath(os.path.expanduser(args.iso or args.folder))
    if args.iso and not os.path.isfile(source):
        raise Refused(f"{source} is not a file.")
    if args.folder and not os.path.isdir(source):
        raise Refused(f"{source} is not a folder.")
    if args.iso:
        args.iso = source
    else:
        args.folder = source
    args.dlc = [os.path.abspath(os.path.expanduser(p)) for p in args.dlc]
    args.title_update = [os.path.abspath(os.path.expanduser(p)) for p in args.title_update]
    for p in args.dlc + args.title_update:
        if not os.path.exists(p):
            raise Refused(f"{p} does not exist.")

    if args.iso:
        try:
            with xiso.Xiso(source) as x:
                end = max((x.base + sector * xiso.SECTOR + size for _, sector, size in x.walk()), default=0)
                if end > x.size:
                    raise Refused(f"{source} ends early ({x.size} of at least {end} bytes): is the image complete?")
        except xiso.XisoError as e:
            raise Refused(str(e)) from None
    title, game = identify(source)
    facts = app_facts(game)
    name = facts.get("name", SUPPORTED[game])
    say(f"{name} (title id {title}) from {source}")
    work = os.path.abspath(os.path.expanduser(args.work or f"~/Games/Xbox360/app-builder/{game}"))
    data, tree = os.path.join(work, "data"), os.path.join(work, "tree")
    os.makedirs(work, exist_ok=True)
    log = os.path.join(work, "build_app.log")
    with open(log, "a") as f:
        f.write(f"\n== {time.strftime('%Y-%m-%d %H:%M:%S')} {' '.join(sys.argv)}\n")

    lay_out_data(game, title, facts, args, data, work)
    if args.data_only:
        say(f"data laid out in {data}")
        return 0
    host = os.path.abspath(args.host or os.path.join(ROOT, "out", "host-mac"))
    rexglue = build_host(host, args.jobs, log)
    gdir = make_tree(tree, game, host)
    if game == "bo2":
        bdir = build_bo2(tree, work, args.jobs, log)
    else:
        codegen(game, facts, gdir, data, rexglue, log)
        bdir = build_other(game, tree, work, log)
    out_dir = args.launchers_dir if args.install else os.path.abspath(os.path.expanduser(args.out))
    os.makedirs(out_dir, exist_ok=True)
    app = os.path.join(out_dir, name + ".app")
    pack(game, facts, tree, bdir, data, work, app)
    say(f"ready: {app}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Refused as e:
        print(f"build_app: {e}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)
