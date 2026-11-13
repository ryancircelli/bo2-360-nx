"""Read embedded names from compiled T6 objects without loading or executing them.

Usage: python -I tools/inspect/gsc_names.py FILE_OR_FOLDER [FILE_OR_FOLDER ...]
Only .gsc files are selected inside folders. Scans share a budget of 4096 entries
and 128 files, descend at most 8 levels, and skip links and Windows reparse points.
Each regular file is read at most 1 MiB + 1 byte. Exit 1 means an input was rejected,
ambiguous, unreadable or skipped by a limit; loader path support is advisory.
--config MODE treats all supplied paths as one mod and prints an INI recommendation
only for a complete scan of unique, non-generic names within the loader path policy.
MODE is zombies or multiplayer; it is supplied explicitly, never inferred. Nothing
is written. An unavailable recommendation also exits 1.

Header fields follow games/bo2/src/bo2zm_gsc_object.h: magic at 0, code offset at
0x14, exports at 0x1c, stored code size at 0x2c, and u16 name offset at 0x30.
This inspects metadata only; it does not validate tables, bytecode or compatibility.
Routing hints preserve embedded names and flag the known Studio placeholder. They
do not infer replacement targets from filenames or change mod settings.
"""

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import stat


MAGIC = bytes.fromhex("804753430d0a0006")
HEADER_SIZE = 64
MAX_FILE_SIZE = 1024 * 1024
MAX_NAME = 127
STUDIO_PLACEHOLDER = "maps/mp/_imcsx_gsc_studio.gsc"
LOADER_ROOTS = ("maps/mp/", "common_scripts/")


@dataclass(frozen=True)
class Candidate:
    endian: str
    name: str | None
    detail: str
    plausible: bool = False


@dataclass(frozen=True)
class Inspection:
    kind: str
    detail: str = ""
    candidates: tuple[Candidate, ...] = ()


def loader_path(name):
    """Describe path policy separately from whether the embedded name is readable."""
    if not name.startswith(LOADER_ROOTS):
        return "unsupported root for current loader (maps/mp/ or common_scripts/ only)"
    if not re.fullmatch(r"[a-z0-9_./-]+", name):
        return "unsupported name characters for current loader"
    return "within current loader path policy"


def is_studio_placeholder(name):
    return name.rsplit("/", 1)[-1] == STUDIO_PLACEHOLDER.rsplit("/", 1)[-1]


def routing_advice(name):
    if is_studio_placeholder(name):
        return (
            "generic Studio placeholder; intended replacement target is unknown",
            "routing: consider names=files only with known target paths; filenames, "
            "relative folders and required base= must express those targets. Use base=/ for a full game hierarchy. No target guessed.",
        )
    return (
        "routing: names=internal preserves this exact embedded path; files may be flat or nested.",
        "Use names=files only with known filename-based targets; relative folders and "
        "required base= then determine routing. Use base=/ for a full game hierarchy. Target existence and execution are not checked.",
    )


def _candidate(data, byteorder, label):
    def number(offset, size=4):
        return int.from_bytes(data[offset:offset + size], byteorder)

    offset = number(0x30, 2)
    if not HEADER_SIZE <= offset < len(data):
        return Candidate(label, None, "name offset at 0x30 outside object metadata")
    end = data.find(b"\0", offset, min(len(data), offset + MAX_NAME + 1))
    if end < 0:
        return Candidate(label, None, "missing NUL within 127 name bytes")
    raw = data[offset:end]
    if not raw or any(c < 0x21 or c > 0x7e for c in raw):
        return Candidate(label, None, "name must be nonempty printable ASCII without spaces")
    name = raw.decode("ascii")
    if (not re.fullmatch(r"[A-Za-z0-9_./-]+", name)
            or any(part in ("", ".", "..") for part in name.split("/"))
            or not name.endswith(".gsc")):
        return Candidate(label, None, "unsafe internal name path or missing .gsc suffix")
    code = number(0x14)
    size = number(0x2c)
    legacy = size == 0
    if legacy:
        size = number(0x1c) - code
    if code < HEADER_SIZE or size < 8 or code + size > len(data):
        return Candidate(label, name, "invalid code interval (0x14/0x2c or legacy 0x1c)")
    if offset < code + size and end + 1 > code:
        return Candidate(label, name, "internal name overlaps code interval")
    basis = "legacy exports end at 0x1c" if legacy else "stored size at 0x2c"
    return Candidate(label, name, f"code=[0x{code:x},0x{code + size:x}), {basis}", True)


def inspect_bytes(data):
    if len(data) > MAX_FILE_SIZE:
        return Inspection("oversized", "file exceeds 1 MiB")
    if not data or (len(data) < len(MAGIC) and MAGIC.startswith(data)):
        return Inspection("truncated", "incomplete T6 magic/header")
    if data.startswith(b"GSCR"):
        return Inspection("GSCR", "container, not a raw T6 object")
    if not data.startswith(MAGIC):
        # Text is a heuristic: printable UTF-8 is not proof of GSC source syntax.
        try:
            text = data.decode("utf-8-sig")
        except UnicodeDecodeError:
            text = ""
        if text and all(c.isprintable() or c in "\r\n\t" for c in text):
            return Inspection("source-like text", "not compiled T6; source syntax not checked")
        return Inspection("invalid", "expected full T6 magic 804753430d0a0006")
    if len(data) < HEADER_SIZE:
        return Inspection("truncated", "T6 header shorter than 64 bytes")
    candidates = (_candidate(data, "big", "BE"), _candidate(data, "little", "LE"))
    count = sum(c.plausible for c in candidates)
    kind = "T6" if count == 1 else "ambiguous" if count == 2 else "invalid"
    return Inspection(kind, "endianness unresolved" if count != 1 else "", candidates)


def _is_link(info):
    return (stat.S_ISLNK(info.st_mode)
            or bool(getattr(info, "st_file_attributes", 0) & 0x400))


def _no_link_path(path):
    # Inspect ancestors too, so an explicit junction/child.gsc cannot bypass scanning.
    for part in reversed((path, *path.parents)):
        if _is_link(part.lstat()):
            raise ValueError("links/junctions/reparse points are not followed")


def inspect_file(path):
    path = Path(os.path.abspath(path))
    try:
        _no_link_path(path)
        before = path.lstat()
        if not stat.S_ISREG(before.st_mode):
            return Inspection("skipped", "not a regular file")
        if before.st_size > MAX_FILE_SIZE:
            return Inspection("oversized", "file exceeds 1 MiB")
        flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
        with os.fdopen(os.open(path, flags), "rb") as stream:
            opened = os.fstat(stream.fileno())
            if (_is_link(opened) or not stat.S_ISREG(opened.st_mode)
                    or not os.path.samestat(before, opened)):
                return Inspection("skipped", "file changed while opening")
            data = stream.read(MAX_FILE_SIZE + 1)
        return inspect_bytes(data)
    except ValueError as error:
        return Inspection("skipped", str(error))
    except OSError as error:
        return Inspection("error", str(error))


@dataclass(frozen=True)
class ScanLimits:
    entries: int = 4096
    files: int = 128
    depth: int = 8


def scan(paths, limits=ScanLimits()):
    """Stream results with a shared budget, including non-GSC directory entries."""
    entries = files = 0

    class LimitReached(Exception):
        pass

    def walk(path, depth, explicit=False):
        nonlocal entries, files
        if entries >= limits.entries:
            yield path, Inspection("scan-limit", f"entry limit {limits.entries} reached")
            raise LimitReached
        entries += 1
        try:
            _no_link_path(path)
            info = path.lstat()
            if stat.S_ISDIR(info.st_mode):
                if depth > limits.depth:
                    yield path, Inspection("scan-limit", f"depth limit {limits.depth} reached")
                    return
                with os.scandir(path) as children:
                    for child in children:
                        yield from walk(Path(child.path), depth + 1)
            elif explicit or path.suffix.lower() == ".gsc":
                if files >= limits.files:
                    yield path, Inspection("scan-limit", f"file limit {limits.files} reached")
                    raise LimitReached
                files += 1
                yield path, inspect_file(path)
        except ValueError as error:
            yield path, Inspection("skipped", str(error))
        except OSError as error:
            yield path, Inspection("error", str(error))

    try:
        for path in paths:
            yield from walk(Path(os.path.abspath(path)), 0, explicit=True)
    except LimitReached:
        return


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", metavar="FILE_OR_FOLDER", type=Path)
    parser.add_argument("--config", choices=("zombies", "multiplayer"), metavar="MODE",
                        help="print an advisory mod.ini for one mod; never write or convert files")
    args = parser.parse_args(argv)
    print("Read-only T6 metadata inspection; not compatibility proof. No scripts executed.")
    print("BE/LE candidates describe header byte order, not Xbox 360 vs PS3 origin.")
    print("Limits: 1 MiB/file, 128 files, 4096 entries, directory depth 8; no links/junctions.")
    failed = False
    found = False
    config_names = set()
    config_blockers = set()
    for path, result in scan(args.paths):
        found = True
        failed |= result.kind != "T6"
        if args.config:
            if result.kind != "T6":
                config_blockers.add("incomplete or rejected metadata scan")
            else:
                chosen = next(c for c in result.candidates if c.plausible)
                name = chosen.name
                if chosen.endian != "BE":
                    config_blockers.add("little-endian bytecode is unsupported by this big-endian loader")
                if is_studio_placeholder(name):
                    config_blockers.add("generic Studio placeholder needs explicit target mapping")
                if name in config_names:
                    config_blockers.add("duplicate internal names need explicit target mapping")
                if not name.startswith(LOADER_ROOTS) or not re.fullmatch(r"[a-z0-9_./-]+", name):
                    config_blockers.add("embedded name is outside loader path policy")
                config_names.add(name)
        # Quote filesystem text so control characters in filenames cannot affect the terminal.
        print(f"{json.dumps(str(path), ensure_ascii=True)}: {result.kind} {result.detail}".rstrip())
        for candidate in result.candidates:
            name = json.dumps(candidate.name) if candidate.name is not None else "<unavailable>"
            status = "plausible" if candidate.plausible else "rejected"
            print(f"  {candidate.endian} candidate {status}: internal-name={name}; {candidate.detail}")
            if candidate.name is not None:
                print(f"    {loader_path(candidate.name)}")
                if result.kind == "T6" and candidate.plausible:
                    for line in routing_advice(candidate.name):
                        print(f"    {line}")
    if not found:
        print("No .gsc files found.")
    if args.config:
        if not config_names:
            config_blockers.add("no unambiguous compiled internal names found")
        if config_blockers:
            print("No INI recommendation: " + "; ".join(sorted(config_blockers)) + ".")
            print("No targets guessed; no files written or converted.")
            failed = True
        else:
            print("Recommended mod.ini (routing metadata only; compatibility and execution not checked):")
            print(f"mode={args.config}\nnames=internal")
            print("Recommendation only; no files written or converted.")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
