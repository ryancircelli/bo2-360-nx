"""Makes the release kit: one "<name>.zip" that builds bo2-360-nx for the Switch and for Windows.

The kit is one folder:
  Build bo2-360-nx.bat      double-click: scans Game Files, asks for the systems and modes, builds (install.py)
  Extract game files.bat    double-click: lays out only the chosen modes' game files (extract.py)
  install.py, extract.py    what the two .bat files start (they run source/tools/install/kit_build.py and
                            extract_game_files.py)
  README.txt                what the kit needs and does (tools/install/kit/README.txt)
  Game Files/               where the player puts the game, Title Update 18 and the DLC
  extras/                   only with --bots-pack: bo2zm_bots.gscpack, copied next to the game when it is there
  LICENSE, THIRD_PARTY_NOTICES.md
  source/                   the public source tree (--source, an export of main) plus the SDK's third-party
                            sources that tools/fetch_thirdparty.py fetched, so a build needs no network
  runtime/                  the prebuilt package (tools/install/make_runtime.py --target all): host tools, shader
                            compilers and the Switch's Vulkan driver

Nothing is published: the zip is only written to disk.

usage: python -I tools/install/make_kit.py --runtime <runtime package> [--source <public tree>] [--name "<name>"]
                                           [--out <folder>] [--bots-pack <file>] [--folder-only]

  --source       the public source tree to put in the kit as it is (default C:/dev/bo2-360-nx, the export); the SDK's
                 third-party sources come from this checkout
  --name         the kit's name: its folder, its zip and the title of its README
  --out          where the zip goes (default: release/ in the data folder, tools/data_dir.sh)
  --bots-pack    a bo2zm_bots.gscpack to ship in extras/
  --folder-only  write the kit folder without zipping it (for testing the kit as a player would unpack it)
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
TEMPLATE = os.path.join(ROOT, "tools", "install", "kit")
DEFAULT_SOURCE = "C:/dev/bo2-360-nx"
DEFAULT_NAME = "bo2-360-nx"  # the release's name is passed with --name


def say(text):
    print(text, flush=True)


def inside(path, root):
    """Whether path is root or under it (False across drives)."""
    try:
        return os.path.commonpath([path, root]) == root
    except ValueError:
        return False


def data_dir():
    bash = r"C:\Program Files\Git\bin\bash.exe" if os.path.isfile(r"C:\Program Files\Git\bin\bash.exe") else "bash"
    r = subprocess.run([bash, os.path.join(ROOT, "tools", "data_dir.sh")], capture_output=True, text=True)
    return r.stdout.strip()


def copy_tree(src, dst, skip_dirs=(".git",)):
    count = 0
    for dp, dn, fn in os.walk(src):
        dn[:] = [d for d in dn if d not in skip_dirs]
        for n in fn:
            target = os.path.join(dst, os.path.relpath(os.path.join(dp, n), src))
            if not os.path.exists(target):
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copy2(os.path.join(dp, n), target)
                count += 1
    return count


def export_source(dst, tree):
    """The public tree as it is, then the fetched third-party sources."""
    count = copy_tree(tree, dst)
    # sdk/thirdparty: everything fetch_thirdparty.py and build_host.sh left there (worktrees have junctions, which
    # are followed). The SDK's configure step wants a .git entry in each submodule's folder: an empty one.
    third = os.path.join(ROOT, "sdk", "thirdparty")
    for name in sorted(os.listdir(third)):
        src = os.path.join(third, name)
        if os.path.isdir(src):
            count += copy_tree(src, os.path.join(dst, "sdk", "thirdparty", name))
            os.makedirs(os.path.join(dst, "sdk", "thirdparty", name, ".git"), exist_ok=True)
    return count


def text_file(src, dst, **fields):
    """A template text file with Windows line endings, its {fields} filled in."""
    with open(src, encoding="utf-8") as f:
        text = f.read()
    for k, v in fields.items():
        text = text.replace("{" + k + "}", v)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)


def make_kit(name, runtime, out, folder_only, tree, bots_pack):
    kit = os.path.join(out, name)
    shutil.rmtree(kit, ignore_errors=True)
    os.makedirs(kit)
    say(f"== {name}")
    say(f"  source: {export_source(os.path.join(kit, 'source'), tree)} files")
    for part in ("host", os.path.join("switch", "portlibs")):
        if not os.path.isdir(os.path.join(runtime, part)):
            sys.exit(f"the runtime package lacks {part}: make it with make_runtime.py --target all")
    shutil.copytree(runtime, os.path.join(kit, "runtime"))
    for f in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        shutil.copy2(os.path.join(tree, f), kit)
    for f in ("Build bo2-360-nx.bat", "Extract game files.bat", "install.py", "extract.py"):
        shutil.copy2(os.path.join(TEMPLATE, f), kit)
    text_file(os.path.join(TEMPLATE, "README.txt"), os.path.join(kit, "README.txt"), name=name, rule="=" * len(name))
    text_file(os.path.join(TEMPLATE, "Game Files", "Put your game files here.txt"),
              os.path.join(kit, "Game Files", "Put your game files here.txt"))
    if bots_pack:
        os.makedirs(os.path.join(kit, "extras"))
        shutil.copy2(bots_pack, os.path.join(kit, "extras", "bo2zm_bots.gscpack"))
    if folder_only:
        say(f"  {kit}")
        return kit
    return zip_folder(kit)


def zip_folder(folder):
    """folder as <folder>.zip with the folder itself at its root, then the folder is removed. Empty folders are kept
    (the kit's empty .git entries)."""
    zip_path = folder + ".zip"
    parent = os.path.dirname(folder)
    if os.path.exists(zip_path):
        os.remove(zip_path)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for dp, dn, fn in os.walk(folder):
            rel_dir = os.path.relpath(dp, parent)
            if not fn and not dn:
                z.writestr(rel_dir.replace("\\", "/") + "/", "")
            for n in fn:
                z.write(os.path.join(dp, n), os.path.join(rel_dir, n))
    shutil.rmtree(folder)
    say(f"  {zip_path}: {os.path.getsize(zip_path) / 1e6:.0f} MB")
    return zip_path


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--runtime", required=True)
    p.add_argument("--source", default=DEFAULT_SOURCE)
    p.add_argument("--name", default=DEFAULT_NAME)
    p.add_argument("--out")
    p.add_argument("--bots-pack")
    p.add_argument("--folder-only", action="store_true")
    args = p.parse_args()
    data = os.path.abspath(data_dir())
    out = os.path.abspath(args.out or os.path.join(data, "release"))
    if inside(out, ROOT) and not inside(out, data):  # the data folder is git-ignored, even inside the checkout
        sys.exit("the kit must be written outside the repository or in its data folder")
    source = os.path.abspath(args.source)
    if not os.path.isfile(os.path.join(source, "games", "bo2", "CMakeLists.txt")):
        sys.exit(f"{source} is not a source tree (export main first)")
    if args.bots_pack and not os.path.isfile(args.bots_pack):
        sys.exit(f"{args.bots_pack} was not found")
    os.makedirs(out, exist_ok=True)
    make_kit(args.name, os.path.abspath(args.runtime), out, args.folder_only, source, args.bots_pack)


if __name__ == "__main__":
    main()
