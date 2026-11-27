#!/usr/bin/env python3
"""Install marketplace STFS packages from any title's content disc.

Accepts an extracted disc or an ISO. The embedded package title/type, never the
disc's directory names, determines the destination. Title updates are reported,
not applied: their executable patches require a matching recompile.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import struct
import sys
import tempfile

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "inspect"))
sys.path.insert(0, str(TOOLS / "install"))
from stfs_extract import BLOCK, END_OF_CHAIN, Stfs
from xiso import Xiso

MAGICS = (b"CON ", b"LIVE", b"PIRS")
MARKETPLACE = 2


def metadata(head, name):
    if head[:4] not in MAGICS:
        return None
    if len(head) < 0x511:
        raise ValueError(f"truncated STFS header: {name}")
    return dict(source=name, magic=head[:4].decode("ascii").strip(),
                title_id=f"{struct.unpack_from('>I', head, 0x360)[0]:08X}",
                content_type=f"{struct.unpack_from('>I', head, 0x344)[0]:08X}",
                display_name=head[0x411:0x511].decode("utf-16-be").split("\0")[0])


def aggregate_header(name, title, display, mask):
    """SDK XCONTENT_AGGREGATE_DATA, followed by the host-endian licence mask."""
    result = bytearray(0x148)
    struct.pack_into(">II", result, 0, 1, MARKETPLACE)
    encoded = display.encode("utf-16-be")[:254]
    # Avoid splitting a surrogate pair when truncating a display name.
    if encoded and 0xD800 <= int.from_bytes(encoded[-2:], "big") <= 0xDBFF:
        encoded = encoded[:-2]
    result[8:8 + len(encoded)] = encoded
    filename = name.encode("ascii")
    if not filename or len(filename) > 42:
        raise ValueError("content filename must be 1-42 ASCII bytes")
    result[0x108:0x108 + len(filename)] = filename
    struct.pack_into(">QI", result, 0x138, 0, int(title, 16))
    return bytes(result) + struct.pack("=I", mask)


def safe_path(name):
    path = PurePosixPath(name)
    if not name or "\\" in name or "\0" in name or path.is_absolute() or any(
            part in ("", ".", "..") for part in name.split("/")):
        raise ValueError(f"unsafe package path: {name!r}")
    return path


def copy_entry(package, entry, target):
    remaining, block = entry["size"], entry["start"]
    seen = set()
    with target.open("wb") as out:
        while remaining:
            if block == END_OF_CHAIN or block in seen or block >= package.total_blocks:
                raise ValueError(f"invalid or short block chain: {entry['path']}")
            seen.add(block)
            package.f.seek(package.block_offset(block))
            count = min(BLOCK, remaining)
            data = package.f.read(count)
            if len(data) != count:
                raise ValueError(f"truncated file: {entry['path']}")
            out.write(data)
            remaining -= count
            if remaining:
                block = package.next_block(block)


def install_package(source, info, out):
    # Hash-derived names also support discs whose installer renames the packages.
    name = source.name
    if len(name) > 42 or not name.isascii() or not all(c.isalnum() or c in "_-" for c in name):
        with source.open("rb") as f:
            name = hashlib.file_digest(f, "sha1").hexdigest().upper()
    out = out.resolve()
    root = out / "0000000000000000" / info["title_id"]
    packages = root / "00000002"
    headers = root / "Headers" / "00000002"
    destination = packages / name
    header = headers / (name + ".header")
    for folder in (out / "0000000000000000", root, packages, root / "Headers", headers):
        if folder.is_symlink():
            raise ValueError(f"staging content directory must not be a symlink: {folder}")
    if destination.exists() or destination.is_symlink() or header.exists() or header.is_symlink():
        raise FileExistsError(f"content already exists; use a fresh staging root: {destination}")
    package = Stfs(source)
    try:
        package.f.seek(0x3A9)
        if package.f.read(4) != b"\0\0\0\0":
            raise ValueError(f"SVOD content is not supported: {source}")
        entries = package.entries()
        for entry in entries:
            safe_path(entry["path"])
        package.f.seek(0x22C)
        mask = 0
        for _ in range(16):
            _, bits, _ = struct.unpack(">QII", package.f.read(16))
            # Flags are metadata, not an entry-present boolean. Retail PIRS
            # content discs carry real licence bits with flags == 0.
            mask |= bits
        packages.mkdir(parents=True, exist_ok=True)
        headers.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".content-", dir=root) as staging:
            stage = Path(staging)
            payload = stage / "payload"
            payload.mkdir()
            for entry in entries:
                target = payload.joinpath(*safe_path(entry["path"]).parts)
                if entry["dir"]:
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    copy_entry(package, entry, target)
            staged_header = stage / "header"
            staged_header.write_bytes(aggregate_header(name, info["title_id"], info["display_name"], mask))
            os.rename(staged_header, header)
            try:
                os.rename(payload, destination)
            except BaseException:
                header.unlink()
                raise
        return dict(info, package=name, files=sum(not e["dir"] for e in entries),
                    bytes=sum(e["size"] for e in entries if not e["dir"]), license_mask=mask)
    finally:
        package.f.close()


def inventory(source):
    if source.is_dir():
        for path in sorted(source.rglob("*")):
            if path.is_file() and not path.is_symlink() and "$systemupdate" not in (
                    p.lower() for p in path.relative_to(source).parts):
                with path.open("rb") as f:
                    info = metadata(f.read(0x1800), str(path.relative_to(source)))
                if info:
                    yield info, path
    else:
        with Xiso(source) as disc:
            for name, sector, size in disc.walk():
                safe_path(name)
                if name.lower().startswith("$systemupdate/"):
                    continue
                disc.f.seek(disc.base + sector * 2048)
                info = metadata(disc.f.read(min(size, 0x1800)), name)
                if info:
                    yield info, None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="content disc ISO or extracted directory")
    parser.add_argument("--out", type=Path, help="staging data/content root (never the game or saves directory)")
    parser.add_argument("--title-id", type=lambda s: f"{int(s, 16):08X}", required=True,
                        help="expected game title ID; packages for other titles are skipped")
    parser.add_argument("--list", action="store_true", help="inventory only; write nothing")
    parser.add_argument("--manifest", type=Path, help="write a JSON installation report")
    args = parser.parse_args(argv)
    if not args.list and args.out is None:
        parser.error("--out is required for installation")
    records = list(inventory(args.source))
    chosen = [(i, p) for i, p in records if i["title_id"] == args.title_id and i["content_type"] == "00000002"]
    for info, _ in records:
        action = "install" if (info["title_id"] == args.title_id and info["content_type"] == "00000002") else "report only"
        print(f"{action}: {info['display_name']} title={info['title_id']} type={info['content_type']} ({info['source']})", flush=True)
    if not chosen:
        raise ValueError("no marketplace packages for the requested title")
    report = dict(source=str(args.source), packages=[i for i, _ in records], installed=[])
    if not args.list:
        with tempfile.TemporaryDirectory(prefix="content-disc-") as temporary:
            if not args.source.is_dir():
                selected = {i["source"] for i, _ in chosen}
                with Xiso(args.source) as disc:
                    disc.extract(temporary, skip=lambda name: name not in selected)
                chosen = [(i, Path(temporary).joinpath(*safe_path(i["source"]).parts)) for i, _ in chosen]
            for info, path in chosen:
                report["installed"].append(install_package(path, info, args.out))
        if args.manifest:
            args.manifest.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{len(chosen)} marketplace package(s) {'selected' if args.list else 'installed'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error) as error:
        raise SystemExit(str(error))
