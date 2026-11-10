"""Builds the shader library (bo2zm_shaders.spvpack and its .spvidx) on macOS or Linux, with the steps of
docs/building.md step 5 and the zone choice of the build kit (tools/install/bo2zm_install.py, Installer.zone_files).

  python3 -I tools/mac/build_shaders.py --data <data folder> --image <work/dump_tu18/default_mp.image.bin>
                                        --work <scratch folder> [--modes zm|zm,mp] [--out <folder for the pack>]

<data folder> holds game/, update/ and (optionally) content/ as in docs/building.md. Everything written is
game-derived and goes to --work and --out, which must be outside the repository. Needs clang++, and dxc and
spirv-val from the Vulkan SDK (VULKAN_SDK, else PATH), which tools/xenosrecomp/run.sh uses.
"""
import argparse
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "install"))
import game_files  # noqa: E402
from bo2zm_install import MAX_SHADERS, OTHER_LANGUAGE  # noqa: E402


def zone_files(data, modes):
    """As Installer.zone_files for bo2zm: the chosen modes' zones, the shared ones and the DLC's, English only."""
    found = []
    for folder in ("game", "update", "content"):
        for dp, _, fn in os.walk(os.path.join(data, folder)):
            for n in sorted(fn):
                if not n.lower().endswith(".ff"):
                    continue
                stem = n[:-3].lower()
                if OTHER_LANGUAGE.match(stem):
                    continue
                if game_files.zone_modes(stem) & modes:
                    found.append(os.path.join(dp, n))
    return found


def run(cmd, **kw):
    print("$", " ".join(cmd[:4]), "..." if len(cmd) > 4 else "", flush=True)
    subprocess.run(cmd, check=True, **kw)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--data", required=True)
    p.add_argument("--image", required=True, help="the decrypted default_mp image (the zones' key is rebuilt from it)")
    p.add_argument("--work", required=True)
    p.add_argument("--out", help="where the pack and index go (default: --data)")
    p.add_argument("--modes", default="zm", help="zm, or zm,mp for one library serving both")
    a = p.parse_args()
    data, work = os.path.abspath(a.data), os.path.abspath(a.work)
    out = os.path.abspath(a.out or a.data)
    root = os.path.realpath(ROOT)
    for d in (work, out):
        if os.path.realpath(d).startswith(root + os.sep):
            sys.exit(f"refusing to write game-derived files inside the repository: {d}")
    modes = set(a.modes.split(","))

    zones_dir, unique, recomp = (os.path.join(work, d) for d in ("zones", "unique", "recomp"))
    if not os.path.isdir(unique) or not os.listdir(unique):
        for d in (zones_dir, unique):
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
        zones = zone_files(data, modes)
        workers = max(1, min(8, (os.cpu_count() or 2) // 2))
        print(f"extracting the shaders of {len(zones)} zones ({workers} at a time)", flush=True)
        size = max(1, min(40, -(-len(zones) // (workers * 3))))
        batches = [zones[i:i + size] for i in range(0, len(zones), size)]
        with ThreadPoolExecutor(workers) as pool:
            list(pool.map(lambda b: subprocess.run(
                [sys.executable, "-I", os.path.join(ROOT, "tools", "inspect", "ff_shaders.py"), *b, "--dump",
                 "--out", zones_dir, "--xex-image", a.image, "--jobs", "1"],
                check=True, stdout=subprocess.DEVNULL), batches))
        for d in os.listdir(zones_dir):
            path = os.path.join(zones_dir, d)
            if d.endswith("_shaders") and os.path.isdir(path):
                for n in os.listdir(path):
                    if n.endswith(".bin") and not os.path.exists(os.path.join(unique, n)):
                        os.replace(os.path.join(path, n), os.path.join(unique, n))
        shutil.rmtree(zones_dir, ignore_errors=True)
    # The shaders linked into the executable itself (not in any zone): BO2 creates six of them at renderer start
    # (sub_824E3108). Also run on an existing --work, which then gains only these.
    image_dir = os.path.join(work, "image")
    shutil.rmtree(image_dir, ignore_errors=True)
    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "inspect", "ff_shaders.py"), "--image-shaders",
                    "--dump", "--out", image_dir, "--xex-image", a.image], check=True, stdout=subprocess.DEVNULL)
    for dp, _, fn in os.walk(image_dir):
        for n in fn:
            if n.endswith(".bin") and not os.path.exists(os.path.join(unique, n)):
                os.replace(os.path.join(dp, n), os.path.join(unique, n))
    shutil.rmtree(image_dir, ignore_errors=True)
    count = len(os.listdir(unique))
    if count > MAX_SHADERS:
        sys.exit(f"{count} shaders, more than one library holds ({MAX_SHADERS})")
    print(f"{count} unique shaders", flush=True)

    # Translate (XenosRecomp fork), dxc to SPIR-V, spirv-val: tools/xenosrecomp/run.sh, into a fresh folder.
    shutil.rmtree(recomp, ignore_errors=True)
    run(["bash", os.path.join(ROOT, "tools", "xenosrecomp", "run.sh"), unique, recomp])

    packer = os.path.join(work, "bo2zm_pack")
    render = os.path.join(ROOT, "app", "src", "native", "render")
    run([os.environ.get("CXX", "clang++"), "-std=c++23", "-O2", "-w", f"-I{render}",
         f"-I{os.path.join(ROOT, 'sdk', 'thirdparty', 'xxHash')}",
         os.path.join(ROOT, "tools", "xenosrecomp", "bo2zm_pack.cpp"),
         os.path.join(render, "bo2zm_shader_library.cpp"), os.path.join(render, "bo2zm_shader_index.cpp"),
         "-o", packer])
    os.makedirs(out, exist_ok=True)
    pack = os.path.join(out, "bo2zm_shaders.spvpack")
    for f in (pack, os.path.splitext(pack)[0] + ".spvidx"):
        if os.path.exists(f):
            os.remove(f)
    run([packer, unique, os.path.join(recomp, "spirv"), pack])
    run([packer, "--index", pack])
    print(f"shader library: {pack} ({os.path.getsize(pack) / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
