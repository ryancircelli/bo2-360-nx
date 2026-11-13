"""Builds the prebuilt runtime package that goes into the release kits, from this checkout.

The kits let a player build the game with only the toolchain of their target (devkitPro for the Switch, LLVM and
Visual Studio's build tools for Windows). Everything else a build needs is prebuilt here, from this repository:

  host/              rexglue.exe and xexdump.exe with rexruntime.dll (tools/build_host.sh)
                     bo2zm_hlsl.exe, the shader translator (built as tools/xenosrecomp/run.sh builds it)
                     bo2zm_pack.exe, the shader packer (tools/xenosrecomp/bo2zm_pack.cpp)
                     dxc.exe, dxcompiler.dll and spirv-val.exe, the shader compilers, from the Vulkan SDK
  switch/portlibs/   the Mesa/NVK Vulkan driver for the Switch, built from patches/mesa-bo2zm.patch by
                     tools/build_mesa.sh, with the static zstd tools/build_zstd_switch.sh adds to it
  licenses/          the licenses of the prebuilt parts
  manifest.json      what is inside, with checksums, and the commit it was built from

Nothing in it comes from the game: no generated code, no shader library, no NRO or exe, no PGO profile.

usage: python -I tools/install/make_runtime.py --out <folder> [--target switch|windows|all] [--mesa-dir DIR]
                                               [--skip-host-build] [--skip-mesa-build]

  --mesa-dir DIR       where tools/build_mesa.sh clones and builds Mesa (default mesa-kit/ in the data folder; no spaces), and
                       zstd goes next to it (<dir>-zstd, or ZSTD_DIR). With --skip-mesa-build an existing build there
                       is packed as it is, after checking it carries the current patch.
  --skip-host-build    pack the host tools this checkout already built (tools/build_host.sh)

Run it from Git Bash or a Windows prompt with the tools of docs/building.md installed. The long part is Mesa (about
half an hour the first time).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
NAME = "bo2-360-nx"


def say(text):
    print(text, flush=True)


def inside(path, root):
    """Whether path is root or under it (False across drives)."""
    try:
        return os.path.commonpath([path, root]) == root
    except ValueError:
        return False


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
    return h.hexdigest()


def find_bash():
    for c in (os.environ.get("BO2_BASH", ""), r"C:\Program Files\Git\bin\bash.exe", shutil.which("bash") or ""):
        if c and os.path.isfile(c):
            return c
    sys.exit("bash was not found (Git for Windows)")


def bash(script, *args, env=None):
    r = subprocess.run([find_bash(), "-c", script, "make_runtime", *args], cwd=ROOT, env=env)
    if r.returncode != 0:
        sys.exit(f"failed: {script[:80]}")


def toolchain():
    """What tools/toolchain.sh finds: the same lookup the build scripts use."""
    out = subprocess.run([find_bash(), "-c", '. tools/toolchain.sh; echo "VULKAN_SDK=$(cygpath -m "$VULKAN_SDK")"; '
                          'echo "CLANG=$(cygpath -m "$(command -v clang++)")"'],
                         cwd=ROOT, capture_output=True, text=True).stdout
    return dict(line.split("=", 1) for line in out.splitlines() if "=" in line)


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], capture_output=True, text=True).stdout.strip()


def data_dir():
    """The data folder: $BO2_DATA, else the path in bo2_data.txt at the repository's root, else <root>/data. Git Bash
    exports Windows paths as /e/..., which Windows Python cannot open, so those are turned back into E:/..."""
    root = ROOT
    value = os.environ.get("BO2_DATA", "")
    if not value:
        try:
            with open(os.path.join(root, "bo2_data.txt"), encoding="utf-8") as f:
                value = f.read().strip()
        except OSError:
            value = ""
    value = value or os.path.join(root, "data")
    m = re.match(r"^/([a-zA-Z])/(.*)$", value)
    if m:
        value = f"{m.group(1).upper()}:/{m.group(2)}"
    return os.path.abspath(value)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--out", required=True)
    p.add_argument("--target", choices=("switch", "windows", "all"), default="all")
    p.add_argument("--mesa-dir", default=os.path.join(data_dir(), "mesa-kit"))
    p.add_argument("--skip-host-build", action="store_true")
    p.add_argument("--skip-mesa-build", action="store_true")
    args = p.parse_args()
    out = os.path.abspath(args.out)
    if inside(out, ROOT):
        sys.exit("the package must be written outside the repository")
    host = os.path.join(out, "host")
    licenses = os.path.join(out, "licenses")
    os.makedirs(host, exist_ok=True)
    os.makedirs(licenses, exist_ok=True)
    tc = toolchain()

    # 1. The code generator and xexdump.
    if not args.skip_host_build:
        say("== host tools (tools/build_host.sh)")
        bash("tools/build_host.sh")
    for src in ("sdk/out/win-amd64/rexglue.exe", "sdk/out/win-amd64/rexruntime.dll", "out/host/switch_bo2/xexdump.exe"):
        shutil.copy2(os.path.join(ROOT, src), host)

    # 2. The shader translator, with tools/xenosrecomp/run.sh's flags, and the packer.
    say("== shader translator and packer")
    tr = os.path.join(ROOT, "tools", "xenosrecomp", "translator")
    tp = os.path.join(ROOT, "sdk", "thirdparty")
    clang = tc["CLANG"]
    for cmd in (
        [clang, "-std=c++23", "-O1", "-w", f"-I{tr}", f"-I{tp}/fmt/include", f"-I{tp}/xxHash",
         "-include", os.path.join(tr, "pch_min.h"), "-include", os.path.join(ROOT, "tools", "xenosrecomp",
                                                                              "assert_throw.h"),
         "-DFMT_HEADER_ONLY", "-DXXH_INLINE_ALL", "-DBO2ZM_RECOMP", "-D_CRT_SECURE_NO_WARNINGS",
         os.path.join(ROOT, "tools", "xenosrecomp", "bo2zm_hlsl.cpp"), os.path.join(tr, "shader_recompiler.cpp"),
         "-o", os.path.join(host, "bo2zm_hlsl.exe")],
        [clang, "-std=c++23", "-O2", "-w", f"-I{ROOT}/games/bo2/src/native/render", f"-I{tp}/xxHash",
         os.path.join(ROOT, "tools", "xenosrecomp", "bo2zm_pack.cpp"),
         os.path.join(ROOT, "games", "bo2", "src", "native", "render", "bo2zm_shader_library.cpp"),
         os.path.join(ROOT, "games", "bo2", "src", "native", "render", "bo2zm_shader_index.cpp"),
         "-o", os.path.join(host, "bo2zm_pack.exe")],
    ):
        r = subprocess.run(cmd)
        if r.returncode != 0:
            sys.exit(f"failed: {os.path.basename(cmd[-1])}")
    for stale in ("bo2zm_hlsl.lib", "bo2zm_hlsl.exp", "bo2zm_pack.lib", "bo2zm_pack.exp"):
        if os.path.exists(os.path.join(host, stale)):
            os.remove(os.path.join(host, stale))

    # 3. The shader compilers from the Vulkan SDK.
    vk = tc["VULKAN_SDK"]
    for name in ("dxc.exe", "dxcompiler.dll", "spirv-val.exe"):
        shutil.copy2(os.path.join(vk, "Bin", name), host)
    shutil.copy2(os.path.join(vk, "Licenses", "LICENSE.txt"), os.path.join(licenses, "vulkan-sdk-LICENSE.txt"))

    # 4. The Switch's Vulkan driver.
    mesa_sdk = None
    if args.target in ("switch", "all"):
        mesa_dir = args.mesa_dir.replace("\\", "/")
        if " " in mesa_dir:
            sys.exit("--mesa-dir must not contain spaces (Mesa's build passes paths unquoted)")
        mesa_sdk = f"{mesa_dir}/mesa-unified-install/opt/devkitpro/portlibs/switch"
        zstd_dir = os.environ.get("ZSTD_DIR", f"{mesa_dir}-zstd").replace("\\", "/")
        env = dict(os.environ, MESA_DIR=mesa_dir, MESA_SDK=mesa_sdk, ZSTD_DIR=zstd_dir)
        if not args.skip_mesa_build:
            say("== Mesa/NVK (tools/build_mesa.sh) and zstd (tools/build_zstd_switch.sh)")
            bash("tools/build_mesa.sh && tools/build_zstd_switch.sh", env=env)
        check = subprocess.run(["git", "-C", mesa_dir, "apply", "--check", "-R",
                                os.path.join(ROOT, "patches", "mesa-bo2zm.patch")], capture_output=True)
        if check.returncode != 0:
            sys.exit(f"{mesa_dir} does not carry the current patches/mesa-bo2zm.patch: build it again")
        if not os.path.isfile(os.path.join(mesa_sdk, "lib", "libzstd.a")):
            sys.exit(f"{mesa_sdk} has no libzstd.a: run tools/build_zstd_switch.sh with MESA_SDK set to it")
        dst = os.path.join(out, "switch", "portlibs")
        shutil.rmtree(dst, ignore_errors=True)
        shutil.copytree(mesa_sdk, dst)
        shutil.copy2(os.path.join(mesa_dir, "docs", "license.rst"), os.path.join(licenses, "mesa-license.rst"))
        shutil.copy2(os.path.join(zstd_dir, "LICENSE"), os.path.join(licenses, "zstd-LICENSE.txt"))

    # 5. What is inside.
    files = {}
    for dp, _, fn in os.walk(out):
        for n in fn:
            path = os.path.join(dp, n)
            rel = os.path.relpath(path, out).replace("\\", "/")
            if rel != "manifest.json":
                files[rel] = {"size": os.path.getsize(path), "sha256": sha256(path)}
    manifest = {
        "name": NAME,
        "built": datetime.datetime.now().isoformat(timespec="seconds"),
        "commit": git("rev-parse", "HEAD") + ("-dirty" if git("status", "--porcelain", "--untracked-files=no") else ""),
        "mesa": {"base": "danfromtico/mesa-switch 1a8c1a66d6f",
                 "patch_sha256": sha256(os.path.join(ROOT, "patches", "mesa-bo2zm.patch"))} if mesa_sdk else None,
        "files": files,
    }
    with open(os.path.join(out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    total = sum(f["size"] for f in files.values())
    say(f"runtime package: {out} ({len(files)} files, {total / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
