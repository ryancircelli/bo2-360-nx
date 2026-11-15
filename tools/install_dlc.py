"""Installs BO2's DLC packages (Xbox 360 STFS "LIVE" files) the way the runtime looks for them.

The runtime's content manager (sdk/src/system/xam/content_manager.cpp) lists marketplace content as folders under
  <user root>/0000000000000000/415608C3/00000002/<package file name>/
and reads each one's name and license bits from
  <user root>/0000000000000000/415608C3/Headers/00000002/<package file name>.header
(an XCONTENT_AGGREGATE_DATA, 0x148 bytes, followed by the license mask). The game then mounts each package and
looks for its dlcN.id / dlczmN.id file. This script unpacks the packages into that layout, once, on the PC.

By default it keeps only what Zombies in English needs: other languages' text and sound banks and multiplayer-only
files are skipped, and a pack with nothing left (Nuketown 2025) is skipped whole.
--multiplayer retains English content for both modes; --all keeps every language.

usage: python -I tools/install_dlc.py <folder with the packages> [--out <data folder>/content]
       [--multiplayer | --all] [--package <filename> ...] [--list]
Then tools/stage_sd.sh hard-links the result into the SD staging (user/), and on the PC
  <Documents>/bo2zm/0000000000000000 can be a junction to <out>/0000000000000000.
The output is game data: never commit it.
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "inspect"))
from stfs_extract import BLOCK, END_OF_CHAIN, Stfs  # noqa: E402

TITLE_ID = 0x415608C3
MARKETPLACE = 0x00000002
LANG_PREFIX = re.compile(r"^(bp|fj|fr|ge|it|ja|ms|po|ru|sp)_")
LANG_BANK = re.compile(r"\.(french|fulljap|german|italian|japanese|mexicanspanish|polish|portuguese|russian|spanish)"
                       r"\.sab[ls]$")


def multiplayer_only(name):
    base = LANG_PREFIX.sub("", name)
    base = re.sub(r"^en_", "", base)
    stem = base.split(".")[0]
    return (stem.startswith(("mp_", "mpl_")) or stem.endswith(("_mp", "_mp_loc")) or "_mp_" in stem)


def wanted(name, keep_all, include_multiplayer=False):
    if keep_all:
        return True
    if LANG_PREFIX.match(name) or LANG_BANK.search(name):
        return False
    return include_multiplayer or not multiplayer_only(name)


def license_mask(path):
    with open(path, "rb") as f:
        head = f.read(0x22C + 16 * 0x10)
    mask = 0
    for i in range(16):
        _, bits, flags = struct.unpack_from(">QII", head, 0x22C + 16 * i)
        if flags:
            mask |= bits
    return mask


def header_bytes(file_name, display_name, title_id=TITLE_ID, content_type=MARKETPLACE):
    data = bytearray(0x148)
    struct.pack_into(">II", data, 0, 1, content_type)  # device_id HDD, content_type
    name16 = display_name.encode("utf-16-be")[: 2 * 127]
    data[8:8 + len(name16)] = name16
    fn = file_name.encode("ascii")[:42]
    data[0x108:0x108 + len(fn)] = fn
    struct.pack_into(">QI", data, 0x138, 0, title_id)  # xuid 0 (shared), title id
    return bytes(data)


def copy_file(stfs, entry, dst):
    """Streams one file, reading runs of blocks that are contiguous in the package in one go."""
    remaining, block = entry["size"], entry["start"]
    tmp = dst + ".part"
    with open(tmp, "wb") as out:
        while remaining and block != END_OF_CHAIN:
            start = stfs.block_offset(block)
            count = 1
            nxt = stfs.next_block(block)
            while (count * BLOCK < remaining and count < 2048 and nxt != END_OF_CHAIN
                   and stfs.block_offset(nxt) == start + count * BLOCK):
                count += 1
                block = nxt
                nxt = stfs.next_block(block)
            n = min(count * BLOCK, remaining)
            stfs.f.seek(start)
            out.write(stfs.f.read(n))
            remaining -= n
            block = nxt
    if remaining:
        os.remove(tmp)
        raise IOError(f"{entry['path']}: chain ended {remaining} bytes early")
    os.replace(tmp, dst)


def data_dir():
    """The data folder: $BO2_DATA, else the path in bo2_data.txt at the repository's root, else <root>/data. Git Bash
    exports Windows paths as /e/..., which Windows Python cannot open, so those are turned back into E:/..."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
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
    ap = argparse.ArgumentParser()
    ap.add_argument("source", help="folder with the DLC packages (DLC/415608C3/00000002 of a 360 dump)")
    ap.add_argument("--out", default=os.path.join(data_dir(), "content"))
    ap.add_argument("--all", action="store_true", help="keep every language and the multiplayer files")
    ap.add_argument("--multiplayer", action="store_true", help="keep English Multiplayer and Zombies content")
    ap.add_argument("--package", action="append", default=[], help="install only this package filename; repeat to select more")
    ap.add_argument("--list", action="store_true", help="report selected files without extracting or writing headers")
    a = ap.parse_args()
    for name in a.package:
        if not re.fullmatch(r"[0-9A-Fa-f]{40,42}", name):
            ap.error("--package requires the original hexadecimal package filename")
    names = sorted(os.listdir(a.source))
    package_names = {name.casefold(): name for name in names}
    for name in a.package:
        actual = package_names.get(name.casefold())
        if actual is None or not os.path.isfile(os.path.join(a.source, actual)):
            ap.error(f"package not found: {name}")
    selected = {name.casefold() for name in a.package}
    sys.stdout.reconfigure(encoding="utf-8")
    root = os.path.join(a.out, "0000000000000000", f"{TITLE_ID:08X}")
    total = 0
    for name in names:
        if selected and name.casefold() not in selected:
            continue
        path = os.path.join(a.source, name)
        if not os.path.isfile(path):
            continue
        try:
            stfs = Stfs(path)
        except ValueError:
            continue
        if stfs.content_type != MARKETPLACE or stfs.title_id != TITLE_ID:
            print(f"skip {name}: type {stfs.content_type:08X}, not a DLC package of this game")
            continue
        files = [e for e in stfs.entries() if not e["dir"]]
        keep = [e for e in files if wanted(e["name"], a.all, a.multiplayer)]
        has_ids = any(e["name"].endswith(".id") for e in files)
        zombies = any(re.search(r"(^|_)(zm|zmb)[_.]", e["name"]) for e in files)
        if not a.all and not a.multiplayer and has_ids and not zombies:
            print(f"skip '{stfs.display_name}' ({name}): multiplayer only")
            continue
        dst_dir = os.path.join(root, f"{MARKETPLACE:08X}", name)
        size = sum(e["size"] for e in keep)
        print(f"'{stfs.display_name}' ({name}): {len(keep)} of {len(files)} files, {size / 2**20:.0f} MB")
        if a.list:
            for e in keep:
                print(f"  {e['path']} ({e['size']} bytes)")
            total += size
            continue
        os.makedirs(dst_dir, exist_ok=True)
        for e in keep:
            dst = os.path.join(dst_dir, *e["path"].split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.isfile(dst) and os.path.getsize(dst) == e["size"]:
                continue
            copy_file(stfs, e, dst)
        header_dir = os.path.join(root, "Headers", f"{MARKETPLACE:08X}")
        os.makedirs(header_dir, exist_ok=True)
        mask = license_mask(path)
        with open(os.path.join(header_dir, name + ".header"), "wb") as f:
            f.write(header_bytes(name, stfs.display_name))
            if mask:
                f.write(struct.pack("<I", mask))
        total += size
    verb = "selected" if a.list else "installed"
    print(f"{verb} {total / 2**30:.2f} GB into {root}")


if __name__ == "__main__":
    main()
