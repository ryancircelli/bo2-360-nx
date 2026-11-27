"""Finds the player's Black Ops II files and lays out the ones each game mode needs.

The Mac build (tools/build_mac.sh, tools/mac/build_app.py, tools/mac/build_shaders.py) uses this module. It does three things:

  scan()     looks through a folder, whatever its layout, for the game itself (an Xbox 360 disc image, .iso, or a
             folder already extracted from one), Title Update 18 and the DLC packages. Packages are recognised by
             their contents, not their names, so renamed files are found too.
  prepare()  writes the files of the chosen modes into a data folder: game/ (the disc's files plus the title
             update's executable patches), update/ (the rest of Title Update 18) and content/ (the DLC, in the layout
             the runtime's content manager reads).
  the prompts the two front ends share.

How the files are split between the modes
-----------------------------------------
Zombies and Multiplayer are one program on the disc (default_mp.xex), Campaign is another (default.xex). The game
names its files after the mode that loads them, and the split below follows those names:

  fastfiles (.ff)    a "zm" part in the name is Zombies (zm_transit.ff, common_zm.ff, patch_ui_zm.ff); an "mp" part
                     is Multiplayer (mp_hijacked.ff, ui_mp.ff, faction_seals_mp.ff); everything else is Campaign
                     (the missions, so_/su_ Strike Force, common.ff, code_post_gfx.ff, frontend.ff). The executable
                     builds the base zone names (code_post_gfx, patch, common) with the mode's suffix, and the
                     language zones the same way (en_patch_loc.ff, en_patch_loc_mp.ff, en_patch_loc_zm.ff), so a
                     zone without a suffix belongs to the campaign.
                     Exceptions: patch.ff, frontend_patch.ff and weapon_attach_shared.ff are loaded by every mode;
                     the four Strike Force maps (mp_dockside, mp_drone, mp_overflow, mp_socotra) are Multiplayer maps
                     the campaign loads as well; patch_ui_zm.ff goes with every mode because the Multiplayer and
                     Campaign shader libraries take some of their 2D shaders from it.
  sound banks        load/ and stream/: zmb_ Zombies, mpl_ Multiplayer, spl_ and so_ Campaign, cmn_ and wpn_ all.
  videos             the missions' cutscenes, loading videos and credits are Campaign (named after the missions,
                     a1_..c3_ endings, crc_ credits, frontend, Strike Force); the rest (logos and intros) go with
                     every mode.
  images, others     base.ipak, lowmip.ipak, patch_all.ipak, weapons.clump and anything not named above: every mode.
  executables        default_mp.xex/.xexp for Zombies and Multiplayer, default.xex/.xexp for Campaign.
  DLC                each pack's Zombies files (zm/zmb names) go with Zombies, its multiplayer files (mp_/mpl_ names,
                     _mp zones) with Multiplayer; other languages' files are left out. A pack with nothing left for
                     the chosen modes is skipped.

A file the rules do not recognise always goes in, so a mode never misses something it needs; the cost is only disk
space. Everything here only reads the player's files; it writes only into the folders it is given.
"""
import hashlib
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
for path in (os.path.join(TOOLS, "inspect"), TOOLS):
    if path not in sys.path:
        sys.path.insert(0, path)

from stfs_extract import Stfs  # noqa: E402
import install_dlc  # noqa: E402
import title_packs  # noqa: E402
import xiso  # noqa: E402

MODES = {"zm": "Zombies", "mp": "Multiplayer", "cp": "Campaign"}
ALL = frozenset(MODES)

TITLE_ID = 0x415608C3
CONTENT_DLC = 0x00000002
CONTENT_TITLE_UPDATE = 0x000B0000
DISC_XEX_SHA256 = "f6827cbbef86b18728097114f772f48961a84e4d67f3aa754df37c0284ee74f6"  # default_mp.xex
DISC_CAMPAIGN_XEX_SHA256 = "3b679d48bfee1d2fdd6fdb826bd30481607c45307fd74aaa949045182ab3b3f4"  # default.xex
TU18_XEXP_SHA256 = "5d5780970f6e639b17c0570bae0e52a13bd1038e23c969428aef9d0c7d78681b"
TU18_NAME = "tu00000002_00000000"
STFS_MAGICS = (b"CON ", b"LIVE", b"PIRS")

LANGUAGE = re.compile(r"^(en|bp|fj|fr|ge|it|ja|ms|po|ru|sp)_")
SHARED_ZONES = {"patch", "frontend_patch", "weapon_attach_shared"}
SHADER_SHARED_ZONES = {"patch_ui_zm"}
STRIKE_FORCE_MAPS = {"mp_dockside", "mp_drone", "mp_overflow", "mp_socotra"}
CAMPAIGN_VIDEOS = ("afghanistan", "angola", "blackout", "haiti", "karma", "la_", "monsoon", "nicaragua", "pakistan",
                   "panama", "yemen", "a1_", "a2_", "a3_", "b1_", "b2_", "b3_", "c1_", "c2_", "c3_", "crc_", "vtol_",
                   "frontend", "so_", "su_", "strike_force", "spider_bootup", "prologue", "eye_v5", "generalstore",
                   "sam_gizmos")
VIDEO_EXTENSIONS = (".webm", ".bik")
# Folders of a dump that are not the game's files: the console's system update, and the packages a dump may keep
# next to the game (they are found and handled as packages).
SKIP_FOLDERS = {"$systemupdate", "dlc", "content"}


# --- Talking to the player -------------------------------------------------------------------------------------------

def say(text=""):
    print(text, flush=True)


def ask(question, default=None):
    """One line of input; Enter gives the default. End of input (a closed console) counts as the default too."""
    try:
        answer = input(question + " ").strip()
    except EOFError:
        answer = ""
    return answer or (default or "")


def ask_yes_no(question, default=None):
    hint = {True: "(Y/n)", False: "(y/N)", None: "(y/n)"}[default]
    while True:
        answer = ask(f"{question} {hint}").lower()
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False
        if not answer and default is not None:
            return default
        say("  Please type y or n.")


def ask_choices(question, options, default=None):
    """Lets the player pick one or more of options (a list of (key, label)) by number. Returns the chosen keys."""
    for i, (_, label) in enumerate(options, 1):
        say(f"  {i}. {label}")
    keys = [k for k, _ in options]
    hint = ""
    if default:
        hint = f" [Enter = {' '.join(str(keys.index(k) + 1) for k in default)}]"
    while True:
        answer = ask(f"{question}{hint}:")
        if not answer and default:
            return list(default)
        picked = []
        for part in re.split(r"[\s,]+", answer):
            if part.isdigit() and 1 <= int(part) <= len(options):
                key = keys[int(part) - 1]
                if key not in picked:
                    picked.append(key)
            elif part:
                picked = None
                break
        if picked:
            return [k for k in keys if k in picked]
        say(f"  Type one or more numbers from 1 to {len(options)}, separated by spaces (for example: 1 3).")


def gb(size):
    return f"{size / 1e9:.1f} GB"


# --- Files -----------------------------------------------------------------------------------------------------------

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
    return h.hexdigest()


def link_or_copy(src, dst):
    """A hard link when src and dst are on the same drive (no second copy), else a copy."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst + ".part")
        os.replace(dst + ".part", dst)


def same_file(a, b):
    try:
        return os.path.samefile(a, b) or os.path.getsize(a) == os.path.getsize(b)
    except OSError:
        return False


def sync_folder(wanted, root, progress=None):
    """Makes root hold exactly the files of wanted ({relative path: source file}). Files already there with the same
    size are kept and files no longer wanted are removed, so a second run with other modes only changes the
    difference. Returns (written, kept, removed)."""
    written = kept = removed = 0
    os.makedirs(root, exist_ok=True)
    wanted_norm = {os.path.normcase(os.path.normpath(rel)): (rel, src) for rel, src in wanted.items()}
    for dp, dn, fn in os.walk(root, topdown=False):
        for n in fn:
            rel = os.path.normcase(os.path.relpath(os.path.join(dp, n), root))
            if rel not in wanted_norm:
                os.remove(os.path.join(dp, n))
                removed += 1
        if dp != root and not os.listdir(dp):
            os.rmdir(dp)
    total = len(wanted)
    for i, (rel, src) in enumerate(sorted(wanted.items()), 1):
        dst = os.path.join(root, rel)
        if os.path.isfile(dst) and same_file(src, dst):
            kept += 1
        else:
            link_or_copy(src, dst)
            written += 1
        if progress and (i % 50 == 0 or i == total):
            progress(i, total)
    return written, kept, removed


def folder_size(path):
    total = 0
    for dp, _, fn in os.walk(path):
        total += sum(os.path.getsize(os.path.join(dp, n)) for n in fn)
    return total


def free_space(path):
    while not os.path.isdir(path):
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    return shutil.disk_usage(path).free


# --- Which mode a file belongs to ------------------------------------------------------------------------------------

def zone_modes(stem):
    """The modes that load the fastfile with this name (lower case, without .ff)."""
    base = LANGUAGE.sub("", stem)
    if base in SHARED_ZONES or base in SHADER_SHARED_ZONES:
        return ALL
    if base in STRIKE_FORCE_MAPS:
        return frozenset({"mp", "cp"})
    if re.search(r"(^|_)zm(_|$)", base):
        return frozenset({"zm"})
    if base.startswith(("so_", "su_")):
        return frozenset({"cp"})
    if re.search(r"(^|_)mp(_|$)", base):
        return frozenset({"mp"})
    return frozenset({"cp"})


def file_modes(rel):
    """The modes that need the disc or title update file at rel (a path inside the game folder), or None for a file
    that is not the game's (the system update, packages kept next to the game)."""
    parts = re.split(r"[\\/]", rel)
    if any(p.lower() in SKIP_FOLDERS for p in parts[:-1]):
        return None
    name = parts[-1].lower()
    stem, ext = os.path.splitext(name)
    if name in ("default_mp.xex", "default_mp.xexp"):
        return frozenset({"zm", "mp"})
    if name in ("default.xex", "default.xexp"):
        return frozenset({"cp"})
    if ext == ".ff":
        return zone_modes(stem)
    if ext in (".sabl", ".sabs"):
        bank = name.split(".")[0]
        if bank.startswith("zmb_"):
            return frozenset({"zm"})
        if bank.startswith("mpl_"):
            return frozenset({"mp"})
        if bank.startswith(("spl_", "so_")):
            return frozenset({"cp"})
        return ALL
    if ext in VIDEO_EXTENSIONS:
        return frozenset({"cp"}) if stem.startswith(CAMPAIGN_VIDEOS) else ALL
    return ALL


def wanted(rel, modes):
    m = file_modes(rel)
    return m is not None and bool(m & set(modes))


def dlc_file_modes(name):
    """The modes a file of a DLC package goes with, or an empty set for a file left out (other languages)."""
    if install_dlc.LANG_PREFIX.match(name) or install_dlc.LANG_BANK.search(name):
        return frozenset()
    if re.search(r"(^|_)(zm|zmb)[_.]", name.lower()):
        return frozenset({"zm"})
    if install_dlc.multiplayer_only(name.lower()):
        return frozenset({"mp"})
    return ALL


# --- What a scan finds -----------------------------------------------------------------------------------------------

class Game:
    """The game itself: an extracted folder or a disc image."""

    def __init__(self, kind, path, xex_ok=True):
        self.kind, self.path, self.xex_ok = kind, path, xex_ok

    def label(self):
        return ("disc image " if self.kind == "iso" else "extracted game folder ") + self.path


class Package:
    def __init__(self, path, stfs):
        self.path = path
        self.file_name = os.path.basename(path)
        self.display_name = stfs.display_name.replace("®", "").replace("�", "").strip() or self.file_name
        self.content_type = stfs.content_type
        self.size = os.path.getsize(path)
        self.files = [e for e in stfs.entries() if not e["dir"]]
        names = [e["name"] for e in self.files]
        self.has_id = any(n.endswith(".id") for n in names)
        # The modes the pack has maps for: its files named after a mode (the .id file and the like go with any).
        self.modes = frozenset(m for n in names if dlc_file_modes(n) != ALL for m in dlc_file_modes(n))

    def kind_label(self):
        if not self.has_id:
            return "licence only, nothing to install"
        return " + ".join(MODES[m] for m in ("zm", "mp") if m in self.modes) or "no maps"


class Found:
    def __init__(self):
        self.games = []
        self.tu18 = None
        self.other_updates = []
        self.dlc = []
        self.other_packages = []
        self.problems = []


def is_tu18(stfs):
    for e in stfs.entries():
        if not e["dir"] and e["name"].lower() == "default_mp.xexp":
            return hashlib.sha256(stfs.read(e)).hexdigest() == TU18_XEXP_SHA256
    return False


def other_title(path):
    """A problem line when the disc folder or image at path is another game (by the title id in its executable),
    None when it is Black Ops II or names no title. Black Ops also ships a default_mp.xex, so the file name alone
    is not enough."""
    title, pack = title_packs.identify(path)
    if title is None or int(title, 16) == TITLE_ID:
        return None
    what = f"games/{pack} in this project" if pack else "a game this project has no title pack for"
    return f"{path} is not Black Ops II: its title id is {title} ({what})"


def scan(folder, skip=(), progress=None):
    """Looks through folder (recursively) for the game, Title Update 18 and DLC packages. skip: folders to leave out
    (the kit's own output)."""
    found = Found()
    skip = [os.path.normcase(os.path.abspath(s)) for s in skip]
    seen_packages = set()
    folder = os.path.abspath(folder)
    for dp, dn, fn in os.walk(folder):
        here = os.path.normcase(os.path.abspath(dp))
        dn[:] = sorted(d for d in dn if not any(os.path.normcase(os.path.join(here, d)) == s for s in skip)
                       and d.lower() != "$systemupdate")
        lower = {n.lower(): n for n in fn}
        if "default_mp.xex" in lower:
            xex = os.path.join(dp, lower["default_mp.xex"])
            if progress:
                progress(f"checking {xex}")
            other = other_title(dp)
            if other:
                found.problems.append(other)
            else:
                found.games.append(Game("folder", dp, sha256(xex) == DISC_XEX_SHA256))
        for n in sorted(fn):
            path = os.path.join(dp, n)
            if n.lower().endswith(".iso"):
                if progress:
                    progress(f"reading {path}")
                other = other_title(path) if xiso.is_xbox_image(path) else None
                if other:
                    found.problems.append(other)
                elif "default_mp.xex" in xiso.root_names(path):
                    found.games.append(Game("iso", path))
                elif xiso.is_xbox_image(path):
                    found.problems.append(f"{path} is an Xbox disc image, but not of Black Ops II")
                continue
            try:
                with open(path, "rb") as f:
                    magic = f.read(4)
            except OSError:
                continue
            if magic not in STFS_MAGICS:
                continue
            try:
                stfs = Stfs(path)
            except (ValueError, OSError, struct.error):
                continue
            try:
                if stfs.title_id != TITLE_ID:
                    continue
                if stfs.content_type == CONTENT_TITLE_UPDATE:
                    if progress:
                        progress(f"checking title update {path}")
                    if is_tu18(stfs):
                        found.tu18 = found.tu18 or path
                    else:
                        found.other_updates.append(path)
                elif stfs.content_type == CONTENT_DLC:
                    key = n.upper()
                    if key not in seen_packages:
                        seen_packages.add(key)
                        found.dlc.append(Package(path, stfs))
                else:
                    found.other_packages.append(path)
            finally:
                stfs.f.close()
    return found


def describe(found):
    say("What was found:")
    if found.games:
        for g in found.games:
            note = "" if g.xex_ok else "  (its default_mp.xex is not the Xbox 360 disc's: this copy cannot be used)"
            say(f"  Game:          {g.label()}{note}")
    else:
        say("  Game:          not found")
    say(f"  Title Update:  {'Title Update 18, ' + found.tu18 if found.tu18 else 'Title Update 18 NOT found'}")
    for p in found.other_updates:
        say(f"                 another title update, not TU18 (ignored): {p}")
    if found.dlc:
        for p in found.dlc:
            say(f"  DLC:           {p.display_name} ({p.kind_label()})")
    else:
        say("  DLC:           none")
    for p in found.problems:
        say(f"  Note: {p}")


def choose_game(found):
    """The one game copy to use. Returns a Game or a reason it cannot go on."""
    usable = [g for g in found.games if g.xex_ok]
    if not usable:
        if found.games:
            return None, ("the game copy found is not the Xbox 360 disc version of Black Ops II that this port is "
                          "built from (default_mp.xex differs)")
        return None, ("the game was not found. Put your Xbox 360 copy of Black Ops II in the Game Files folder: an "
                      ".iso disc image, or the folder extracted from one (the folder with default_mp.xex)")
    # An extracted folder is preferred: nothing needs extracting.
    usable.sort(key=lambda g: g.kind != "folder")
    return usable[0], None


# --- Laying out the files --------------------------------------------------------------------------------------------

def game_files(source_root, modes):
    """{relative path: source} for the chosen modes' files of an extracted game folder."""
    result = {}
    for dp, dn, fn in os.walk(source_root):
        rel_dir = os.path.relpath(dp, source_root)
        dn[:] = [d for d in dn if d.lower() not in SKIP_FOLDERS]
        for n in fn:
            rel = os.path.normpath(os.path.join(rel_dir, n))
            if wanted(rel, modes):
                result[rel] = os.path.join(dp, n)
    return result


def estimate(game, modes, found):
    """About how many bytes the chosen modes' files take (for the free-space check)."""
    total = 0
    if game.kind == "folder":
        total = sum(os.path.getsize(p) for p in game_files(game.path, modes).values())
    else:
        try:
            with xiso.Xiso(game.path) as x:
                total = sum(size for rel, _, size in x.walk() if wanted(rel, modes))
        except xiso.XisoError:
            total = os.path.getsize(game.path)
    for p in found.dlc:
        total += sum(e["size"] for e in p.files if dlc_file_modes(e["name"]) & set(modes))
    return total + 100_000_000


def extract_image(game, modes, work, log, say_progress):
    """Extracts the chosen modes' files from the disc image into work/disc (xiso.py's reader; $SystemUpdate is never
    extracted) and returns that folder."""
    out = os.path.join(work, "disc")
    marker = out + ".complete"  # next to the folder, so it never counts as a game file
    # The built-in reader extracts only the chosen modes' files, so the marker says which.
    done_for = open(marker).read() if os.path.isfile(marker) else None
    if done_for in ("all", " ".join(sorted(modes))):
        say_progress("  the disc image was already extracted")
    else:
        say_progress(f"  extracting {os.path.basename(game.path)} (several minutes)")
        last = [0]

        def progress(done, total, rel):
            pct = done * 100 // total
            if pct >= last[0] + 10:
                last[0] = pct - pct % 10
                say_progress(f"    {last[0]} %")

        with xiso.Xiso(game.path) as x:
            x.extract(out, skip=lambda rel: not wanted(rel, modes), progress=progress)
        with open(marker, "w") as f:
            f.write(" ".join(sorted(modes)))
    # Our own extracted copy: its system update folder is not needed.
    for name in os.listdir(out):
        if name.lower() == "$systemupdate":
            shutil.rmtree(os.path.join(out, name), ignore_errors=True)
    return out


def extract_package(path, out, keep=lambda name: True):
    """Every file of an STFS package for which keep(name) holds, into out. Returns the names written."""
    stfs = Stfs(path)
    names = []
    try:
        for e in stfs.entries():
            if e["dir"] or not keep(e["name"]):
                continue
            dst = os.path.join(out, *e["path"].split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if not (os.path.isfile(dst) and os.path.getsize(dst) == e["size"]):
                install_dlc.copy_file(stfs, e, dst)
            names.append(e["path"])
    finally:
        stfs.f.close()
    return names


def install_packages(packages, modes, content):
    """The DLC, as tools/install_dlc.py lays it out, keeping only the chosen modes' files. Returns the packs
    installed."""
    root = os.path.join(content, "0000000000000000", f"{TITLE_ID:08X}")
    installed = []
    wanted_dirs = set()
    for p in packages:
        if p.has_id and not (p.modes & set(modes)):
            say(f"  {p.display_name}: nothing for the chosen modes, skipped")
            continue
        keep = lambda name: bool(dlc_file_modes(name) & set(modes))  # noqa: E731
        size = sum(e["size"] for e in p.files if keep(e["name"]))
        say(f"  {p.display_name}: {gb(size)}")
        dst = os.path.join(root, f"{CONTENT_DLC:08X}", p.file_name)
        stale = [os.path.join(dp, n) for dp, _, fn in os.walk(dst) for n in fn]
        written = extract_package(p.path, dst, keep)
        keep_paths = {os.path.normcase(os.path.join(dst, *w.split("/"))) for w in written}
        for f in stale:
            if os.path.normcase(f) not in keep_paths:
                os.remove(f)
        header_dir = os.path.join(root, "Headers", f"{CONTENT_DLC:08X}")
        os.makedirs(header_dir, exist_ok=True)
        mask = install_dlc.license_mask(p.path)
        with open(os.path.join(header_dir, p.file_name + ".header"), "wb") as f:
            f.write(install_dlc.header_bytes(p.file_name, p.display_name))
            if mask:
                f.write(struct.pack("<I", mask))
        wanted_dirs.add(p.file_name.upper())
        installed.append(p)
    # Packs installed by an earlier run that are no longer chosen.
    packs = os.path.join(root, f"{CONTENT_DLC:08X}")
    if os.path.isdir(packs):
        for name in os.listdir(packs):
            if name.upper() not in wanted_dirs:
                shutil.rmtree(os.path.join(packs, name), ignore_errors=True)
                header = os.path.join(root, "Headers", f"{CONTENT_DLC:08X}", name + ".header")
                if os.path.isfile(header):
                    os.remove(header)
    return installed


def prepare(found, game, modes, packages, data, work, log):
    """Writes the chosen modes' files into data/game, data/update and data/content (see the top of this file). work
    holds the extracted disc image and the title update while they are needed."""
    modes = set(modes)
    os.makedirs(work, exist_ok=True)

    say("  Game files")
    disc = game.path if game.kind == "folder" else extract_image(game, modes, work, log, say)
    files = game_files(disc, modes)
    if "cp" in modes and not any(os.path.basename(r).lower() == "default.xex" for r in files):
        raise RuntimeError("the game copy has no default.xex, the campaign's program: is it complete?")
    update = os.path.join(work, "title_update")
    say("  Title Update 18")
    extract_package(found.tu18, update)
    patches = {}
    for name in os.listdir(update):
        if wanted(name, modes):
            patches[name] = os.path.join(update, name)
            if name.lower().endswith(".xexp"):
                files[name] = os.path.join(update, name)
    say(f"  copying {len(files)} game files")
    w, k, r = sync_folder(files, os.path.join(data, "game"),
                          progress=lambda i, n: say(f"    {i} of {n}") if i % 100 == 0 else None)
    say(f"    {w} written, {k} already in place, {r} removed")
    sync_folder(patches, os.path.join(data, "update"))
    xexp = os.path.join(data, "update", "default_mp.xexp")
    if modes & {"zm", "mp"} and (not os.path.isfile(xexp) or sha256(xexp) != TU18_XEXP_SHA256):
        raise RuntimeError("the title update's default_mp.xexp is not TU18's")

    say("  DLC")
    installed = install_packages(packages, modes, os.path.join(data, "content"))
    if not installed:
        say("    none")
    if game.kind == "iso":
        shutil.rmtree(os.path.join(work, "disc"), ignore_errors=True)  # hard links or copies are in data/game now
        if os.path.isfile(os.path.join(work, "disc.complete")):
            os.remove(os.path.join(work, "disc.complete"))
    shutil.rmtree(update, ignore_errors=True)
    return installed


def title_thumbnail(package):
    """The game's 64x64 tile (PNG) from an Xbox 360 package's header, or None."""
    with open(package, "rb") as f:
        head = f.read(0x971A)
    if len(head) < 0x971A:
        return None
    size = struct.unpack_from(">I", head, 0x1716)[0]
    data = head[0x571A:0x571A + size]
    return data if size and data[:8] == b"\x89PNG\r\n\x1a\n" else None


def png_to_ico(png, path):
    """An .ico holding one PNG image (Windows reads PNG icons since Vista)."""
    width, height = struct.unpack(">II", png[16:24])
    entry = struct.pack("<BBBBHHII", width % 256, height % 256, 0, 0, 1, 32, len(png), 6 + 16)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, 1) + entry + png)
