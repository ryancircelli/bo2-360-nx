"""Install guard: what an install would LOSE, and which gates must have passed on this exact build.

Used by gate.py install. Pure functions over app bundles (no game, no runq), so tests use fake bundles.
Incident 2026-11-26: a merge wave replaced Black Ops.app (bo1 + bo1_mp) with a build holding only bo1; nobody noticed for 31 h.
"""
import hashlib, json, os, plistlib, tomllib

MANIFEST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'games')


def _files(app):
    """{relative path under Contents: abspath} for every file/symlink; Info.plist and the signature are compared separately."""
    out = {}
    base = os.path.join(app, 'Contents')
    for root, _, names in os.walk(base):
        for n in names:
            p = os.path.join(root, n)
            rel = os.path.relpath(p, base)
            if rel != 'Info.plist' and not rel.startswith('_CodeSignature'):
                out[rel] = p
    return out


def _dirs(app):
    """Directories at depth <= 2 under Contents (Resources/runtime, Resources/data, Frameworks/x ...) except the signature."""
    base = os.path.join(app, 'Contents')
    out = set()
    for d1 in os.listdir(base):
        p1 = os.path.join(base, d1)
        if os.path.isdir(p1) and d1 != '_CodeSignature':
            out.add(d1)
            out.update(f'{d1}/{d2}' for d2 in os.listdir(p1) if os.path.isdir(os.path.join(p1, d2)))
    return out


def _plist(app):
    with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f:
        return plistlib.load(f)


def fingerprint(app):
    """Content hash of everything in Contents/MacOS plus the bundle id: the identity of a build, stable across copies."""
    h = hashlib.sha256()
    base = os.path.join(app, 'Contents', 'MacOS')
    for root, _, names in sorted(os.walk(base)):
        for n in sorted(names):
            p = os.path.join(root, n)
            h.update(os.path.relpath(p, base).encode() + b'\0')
            with open(p, 'rb') as f:
                for chunk in iter(lambda: f.read(1 << 20), b''):
                    h.update(chunk)
    return h.hexdigest()[:16]


def load_manifest(game, games_dir=MANIFEST_DIR):
    p = os.path.join(games_dir, game, 'install_manifest.toml')
    if not os.path.isfile(p):
        return None
    with open(p, 'rb') as f:
        return tomllib.load(f)


def losses(new, old, manifest=None):
    """{loss id: why}. Ids are what --allow-loss names. old may be None (first install): then only manifest checks apply."""
    L = {}
    nf, nd, npl = _files(new), _dirs(new), _plist(new)
    if manifest:
        need = list(manifest.get('binaries', [])) + list(manifest.get('modes', {}).values())
        for b in need:
            if f'MacOS/{b}' not in nf:
                L[f'file:MacOS/{b}'] = f'manifest requires Contents/MacOS/{b}'
        for p in manifest.get('required_paths', []):
            if not os.path.exists(os.path.join(new, 'Contents', p)):
                L.setdefault(f'file:{p}', f'manifest requires Contents/{p}')
    if old is None:
        return L
    of, od, opl = _files(old), _dirs(old), _plist(old)
    for rel in sorted(set(of) - set(nf)):
        kind = 'dylib' if rel.endswith('.dylib') or '.dylib' in rel else 'file'
        L.setdefault(f'file:{rel}', f'{kind} disappears')
    for d in sorted(od - nd):
        L[f'dir:{d}'] = 'folder disappears'
    if npl.get('CFBundleIdentifier') != opl.get('CFBundleIdentifier'):
        L['bundle-id'] = f'{opl.get("CFBundleIdentifier")} -> {npl.get("CFBundleIdentifier")} (moves saves and settings)'
    for k in sorted(set(opl) - set(npl)):
        L[f'plist-key:{k}'] = 'Info.plist key disappears'
    icon = 'Resources/AppIcon.icns'
    if icon in of and icon not in nf:
        L['file:' + icon] = 'AppIcon.icns missing'
    return L


def report(new, old, L, allowed):
    """One-screen text: what is lost (and whether allowed), what is added or changed."""
    nf = _files(new)
    of = _files(old) if old else {}
    added = sorted(set(nf) - set(of))
    changed = [r for r in sorted(set(nf) & set(of)) if not _same(nf[r], of[r])]
    lines = [f'install diff: {old or "(nothing installed)"} -> {new}']
    lines += [f'  {"ALLOWED" if i in allowed else "LOST   "} {i}: {why}' for i, why in sorted(L.items())] or ['  no losses']
    lines.append(f'  added {len(added)} file(s), changed {len(changed)} file(s)')
    lines += [f'    + {r}' for r in added[:8]] + (['    + ...'] if len(added) > 8 else [])
    return '\n'.join(lines)


def _same(a, b):
    try:
        return os.path.getsize(a) == os.path.getsize(b) and open(a, 'rb').read() == open(b, 'rb').read()
    except OSError:
        return False


def gate_records(runs_dir):
    """Every gate.json under runs_dir: dicts with verdict/gate/game/build (older records have no build and never match)."""
    out = []
    if os.path.isdir(runs_dir):
        for d in os.listdir(runs_dir):
            try:
                with open(os.path.join(runs_dir, d, 'gate.json')) as f:
                    out.append(json.load(f))
            except (OSError, ValueError):
                pass
    return out


def missing_gates(game, build, manifest, records):
    """Gates the manifest lists that have no PASS record for this exact build."""
    ok = {r.get('gate') for r in records if r.get('game') == game and r.get('verdict') == 'PASS' and r.get('build') == build}
    return [g for g in (manifest or {}).get('gates', []) if g not in ok]
