"""Which game is this? Reads the title id from a disc's executable and finds the title pack (games/<name>/game.toml)
that lists it.

Every Xbox 360 executable (XEX2) carries its title id in the execution-info header, which is never encrypted, so a
disc folder or disc image can be identified without decrypting anything. The installer uses this to tell the game it
was given apart from the one it builds (Black Ops and Black Ops II both ship a default_mp.xex).

    python3 tools/install/title_packs.py <disc folder | .iso | .xex> ...
"""

import os
import struct
import sys
import tomllib

import xiso

XEX2_MAGIC = b"XEX2"
EXECUTION_INFO = 0x00040006  # optional header: media id, versions, title id, platform, disc number...
# The executables a disc may start from, in the order they are tried.
DISC_EXECUTABLES = ("default.xex", "default_mp.xex")

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def xex_title_id(header):
    """The title id ("415608C3") in the first bytes of an XEX2 file, or None when they are not an executable's."""
    if len(header) < 0x18 or header[:4] != XEX2_MAGIC:
        return None
    count = struct.unpack_from(">I", header, 0x14)[0]
    for i in range(min(count, 64)):
        at = 0x18 + 8 * i
        if at + 8 > len(header):
            return None
        key, value = struct.unpack_from(">II", header, at)
        if key == EXECUTION_INFO:
            if value + 16 > len(header):
                return None
            return f"{struct.unpack_from('>I', header, value + 12)[0]:08X}"
    return None


HEADER_BYTES = 64 * 1024  # the optional headers sit well inside the first 64 KB


def packs(repo=REPO):
    """{title id: pack folder name} for every games/<name>/game.toml in repo."""
    found = {}
    games = os.path.join(repo, "games")
    for name in sorted(os.listdir(games)) if os.path.isdir(games) else ():
        path = os.path.join(games, name, "game.toml")
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as f:
            game = tomllib.load(f).get("game", {})
        for title in game.get("title_ids", []):
            found[title.upper()] = name
    return found


def _folder_ids(folder):
    lower = {n.lower(): n for n in os.listdir(folder)}
    for exe in DISC_EXECUTABLES:
        if exe in lower:
            with open(os.path.join(folder, lower[exe]), "rb") as f:
                yield exe, xex_title_id(f.read(HEADER_BYTES))


def _iso_ids(path):
    with xiso.Xiso(path) as image:
        root = {name.lower(): (start, size) for name, is_dir, start, size in image.root() if not is_dir}
        for exe in DISC_EXECUTABLES:
            if exe in root:
                start, size = root[exe]
                yield exe, xex_title_id(image._read(start, min(size, HEADER_BYTES)))


def title_ids(path):
    """[(executable, title id or None)] for a disc folder, a disc image or one .xex file."""
    if os.path.isdir(path):
        return list(_folder_ids(path))
    if path.lower().endswith(".xex"):
        with open(path, "rb") as f:
            return [(os.path.basename(path), xex_title_id(f.read(HEADER_BYTES)))]
    if xiso.is_xbox_image(path):
        return list(_iso_ids(path))
    return []


def identify(path, known=None):
    """(title id, pack name or None) of the game at path; (None, None) when no executable there names one."""
    known = packs() if known is None else known
    for _, title in title_ids(path):
        if title:
            return title, known.get(title)
    return None, None


def main(argv):
    known = packs()
    for path in argv or ["."]:
        title, pack = identify(path, known)
        if title is None:
            print(f"{path}: no Xbox 360 executable found")
        else:
            print(f"{path}: title {title}, " + (f"games/{pack}" if pack else "no title pack in this project"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
