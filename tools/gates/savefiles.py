"""Real title save files, excluding copied seed data and engine savepoints."""
import hashlib
from pathlib import Path
import re


# These paths are established by newgames-depth's write/reopen runs. Other games
# fail closed until their first real save path is established in a live run.
PATTERNS = {
    'gears': re.compile(r'default_checkpoint\.sav(?:/|$)', re.I),
    'ac1': re.compile(r'assassin\.dat$', re.I),
    'nfsmw2012': re.compile(r'Hawaii\.sav(?:/|$)', re.I),
}


def inventory(root, game):
    root = Path(root).resolve()
    pattern = PATTERNS.get(game)
    if not pattern or not root.exists():
        return {}
    result = {}
    for path in root.rglob('*'):
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root):
            continue
        relative = path.relative_to(root).as_posix()
        if pattern.search(relative):
            data = path.read_bytes()
            result[relative] = dict(size=len(data), sha256=hashlib.sha256(data).hexdigest())
    return result


def changed(before, after):
    return {name: value for name, value in after.items()
            if value['size'] > 0 and before.get(name) != value}


def reopened(log, writes):
    """Require the title's successful content-open path, not mere file presence."""
    for name in writes:
        parts = Path(name).parts
        # Content directories can contain the payload (e.g. Hawaii.sav/MUD.29).
        names = [part for part in parts if re.search(r'\.(?:sav|dat)$', part, re.I)]
        opened = False
        for line in log.splitlines():
            relevant = (any(part.lower() in line.lower() for part in names)
                        and re.search(r'XamContentCreate', line, re.I))
            if relevant and opened and re.search(r'mode\s*[=:]?\s*1\b.*create', line, re.I):
                # A new campaign after probing old content is not a reload proof.
                opened = False
                break
            if (relevant
                    and re.search(r'(?:mode|disposition)\s*[=:]?\s*3\b', line, re.I)
                    and re.search(r'(?:result|returned)\s*[=:]?\s*(?:0+|0x0+)\b', line, re.I)):
                opened = True
        if opened:
            return True
    return False
