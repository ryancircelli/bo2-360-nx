#!/usr/bin/env python3
"""Packs the macOS build of a game into a double-clickable app: "Black Ops II.app" by default, any other game of
games/ with --game <game> (its games/<game>/app.toml names the app, its saves folder and its game data folder).

Borrowed from switch2mac's dm/mac/pack_app.py (MIT, Copyright (c) 2026 seedds): the APFS clone copy, the .icns
from a picture with sips and iconutil (or the game's own title picture), the atomic swap of the finished app, the codesign retry on detritus. The
rest is written for this project. No game data is ever written inside the repository.

  python3 -I tools/mac/pack_app.py --build <build folder> --shaders <shader folder> --data <data folder> [options]
  python3 -I tools/mac/pack_app.py --game nfsmw --build <build folder> [options]   (tools/mac/make_game_app.sh)

  --build    the folder tools/build_mac.sh leaves ($BO2_BUILD/out/mac-arm64): bo2zm, its librex*.dylib files and
             the vulkan/ runtime (loader, MoltenVK, icd file). With --binary a bare executable instead.
  --shaders  folder with bo2zm_shaders.spvpack and bo2zm_shaders.spvidx; its optional bo2zm_pso.psolist and
             shader_seed/ (tools/mac/shader_seed.py: Metal pipelines precreated in the background) and
             shader_prebuild/ (tools/mac/shader_prebuild.py: build-time-compiled shaders + GPU binaries) go in too
  --data     the data folder (game/, update/, content/; tools/data_dir.sh). Not needed with --data-mode external.
  --data-mode clone     (default with --data) cp -c clones of the data into Contents/Resources/data: no extra
                        space on the same APFS volume, a plain copy elsewhere
  --data-mode external  nothing is copied. The app reads the path from ~/Library/Application Support/
                        bo2-360-nx/data_path.txt, $BO2_DATA, or --data-path (baked into Info.plist: BO2DataPath)

Layout of the app:
  Contents/MacOS/Black Ops II       the launcher (tools/mac/app/launcher.c), CFBundleExecutable
  Contents/MacOS/bo2zm              the game; the SDK finds beside it the shader library, bo2zm.toml, librexgpu-*.dylib
  Contents/MacOS/*.dylib            the build's own libraries (the GPU plugin is loaded from here)
  Contents/MacOS/vulkan, bo2zm_shaders.spvpack/.spvidx, shader_seed/, bo2zm.toml
                                    relative links into Resources/runtime (codesign refuses plain data files in
                                    MacOS/); the SDK finds all of them beside the executable
  Contents/Frameworks/              any other non-system library the binaries linked (copied, install names fixed)
  Contents/Resources/AppIcon.icns   from --icon, else the game title picture
  Contents/Resources/runtime/       the real vulkan/ (loader, MoltenVK, icd file), shader library, bo2zm.toml
  Contents/Resources/data/          the clones, with --data-mode clone

Large files (the shader library, the cloned game data) sit in signed data bundles Contents/PlugIns/<name>.bundle
behind relative links in Resources, so the app's seal holds each by its code hash and a pack that changes only the
host re-signs in seconds: a bundle of the app being replaced is cloned with its signature while <out.app>.key says
it was made from the same inputs and its files are as the last pack left them. Idea from nxnative's dm/mac/pack_app.py
data bundles (MIT, Copyright (c) 2026 seedds), written for this project.

Every Mach-O gets @loader_path and @executable_path/../Frameworks as its only rpaths (build-machine rpaths are
removed), then everything is ad-hoc signed (--sign - ; no hardened runtime, so the game's dlopen of the GPU plugin
needs no entitlement), inner files first.
"""
import argparse, ctypes, hashlib, json, os, plistlib, re, shutil, subprocess, sys, tempfile, tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # -I drops the script folder from sys.path
from game_icon import extract_icon, make_icns  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
EXE_NAME = 'Black Ops II'   # the launcher; --game sets both from games/<game>/app.toml
GAME = 'bo2zm'              # the game executable
SYSTEM_PREFIXES = ('/usr/lib/', '/System/', '/Library/Apple/')
LOADER_RPATH = '@loader_path'
FRAMEWORKS_RPATH = '@executable_path/../Frameworks'


def step(msg): print(f'step: {msg}', flush=True)


def run(cmd, check=True):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit(f'{Path(str(cmd[0])).name} failed ({r.returncode}): {(r.stderr or r.stdout).strip()[-1500:]}')
    return r.stdout


def can_clone(src, dst_dir):
    """Whether a file of src can be cloned into dst_dir (the same APFS volume)."""
    f = src
    if src.is_dir():
        f = next((Path(r) / n for r, _, fs in os.walk(src) for n in fs), None)
    if f is None:
        return False
    t = dst_dir / '.clone_test'
    try:
        t.unlink(missing_ok=True)
        return ctypes.CDLL(None, use_errno=True).clonefile(str(f).encode(), str(t).encode(), 0) == 0
    finally:
        t.unlink(missing_ok=True)


def copy_tree(src, dst, clone):
    """cp -R (with -c: APFS clones; a file that cannot be cloned is copied)."""
    run(['/bin/cp', '-R'] + (['-c'] if clone else []) + [src, dst])


def free_bytes(path):
    """Bytes a write the user asked for can use on the volume of path: statvfs counts only unallocated blocks, but on
    APFS macOS also frees purgeable space (local snapshots, caches) on demand, and reports it as the volume's
    NSURLVolumeAvailableCapacityForImportantUsage. Never less than statvfs. Idea from nxnative's vol_space.py (MIT, seedds)."""
    raw = shutil.disk_usage(path).free
    try:
        cf = ctypes.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
        cf.CFURLCreateFromFileSystemRepresentation.restype = ctypes.c_void_p
        cf.CFURLCreateFromFileSystemRepresentation.argtypes = (ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_bool)
        cf.CFURLCopyResourcePropertyForKey.argtypes = (ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
        cf.CFNumberGetValue.argtypes = (ctypes.c_void_p, ctypes.c_long, ctypes.c_void_p)
        key = ctypes.c_void_p.in_dll(cf, 'kCFURLVolumeAvailableCapacityForImportantUsageKey')
        b = os.fsencode(Path(path).resolve())
        url = cf.CFURLCreateFromFileSystemRepresentation(None, b, len(b), True)
        v, n = ctypes.c_void_p(), ctypes.c_int64()
        if url and cf.CFURLCopyResourcePropertyForKey(url, key, ctypes.byref(v), None) and v and cf.CFNumberGetValue(v, 4, ctypes.byref(n)):
            return max(raw, n.value)
    except (AttributeError, OSError, ValueError):
        pass
    return raw


def tree_bytes(p):
    return p.stat().st_size if p.is_file() else sum((Path(r) / f).stat().st_size for r, _, fs in os.walk(p) for f in fs)


# ---- Mach-O handling -------------------------------------------------------------------------------------------

def is_macho(p):
    try:
        with open(p, 'rb') as f:
            return f.read(4) in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca')
    except OSError:
        return False


def deps(p):
    """Install names the binary links (otool -L, without its own id)."""
    lines = run(['otool', '-L', p]).splitlines()[1:]
    out = [l.strip().split(' (')[0] for l in lines]
    if str(p).endswith('.dylib') and out:
        out = out[1:]   # first entry of a dylib is its own id
    return out


def rpaths(p):
    out, lines = [], run(['otool', '-l', p]).splitlines()
    for i, l in enumerate(lines):
        if l.strip() == 'cmd LC_RPATH':
            m = re.search(r'path (.+) \(offset', lines[i + 2])
            if m:
                out.append(m.group(1))
    return out


def resolve(dep, binary, search):
    """The file a dependency name refers to on this machine, or None for system libraries and unknowns."""
    if dep.startswith(SYSTEM_PREFIXES):
        return None
    if dep.startswith('/'):
        return Path(dep) if Path(dep).exists() else None
    name = dep.split('/', 1)[1] if dep.startswith(('@rpath/', '@loader_path/', '@executable_path/')) else dep
    for d in search:
        if (Path(d) / name).exists():
            return Path(d) / name
        if (Path(d) / Path(name).name).exists():
            return Path(d) / Path(name).name
    return None


def fix_macho(p, in_frameworks):
    """Dependencies by @rpath/<name>; rpaths: @loader_path and the Frameworks folder only."""
    for d in deps(p):
        if d.startswith(SYSTEM_PREFIXES) or d.startswith('@rpath/'):
            continue
        base = Path(d).name
        run(['install_name_tool', '-change', d, f'@rpath/{base}', p], check=False)
    for r in rpaths(p):
        run(['install_name_tool', '-delete_rpath', r, p], check=False)
    wanted = [LOADER_RPATH] if not in_frameworks else [LOADER_RPATH]
    if not in_frameworks:
        wanted.append(FRAMEWORKS_RPATH)
    for r in wanted:
        run(['install_name_tool', '-add_rpath', r, p], check=False)
    if p.suffix == '.dylib':
        run(['install_name_tool', '-id', f'@rpath/{p.name}', p], check=False)


def bundle_dependencies(macos, frameworks, search):
    """Copies every non-system library the Mach-O files in macos/ (and, transitively, in frameworks/) link."""
    queue = [p for p in macos.rglob('*') if p.is_file() and not p.is_symlink() and is_macho(p)]
    seen = {p.name for p in queue}
    copied = []
    while queue:
        p = queue.pop()
        for d in deps(p):
            if d.startswith(SYSTEM_PREFIXES):
                continue
            src = resolve(d, p, search + [macos, frameworks])
            name = Path(d).name
            if src is None:
                if not (macos / name).exists() and not (frameworks / name).exists():
                    print(f'warning: {p.name} links {d}, which is not found on this machine', file=sys.stderr)
                continue
            if name in seen or (macos / name).exists() or (frameworks / name).exists():
                continue
            frameworks.mkdir(exist_ok=True)
            dst = frameworks / name
            shutil.copy2(src.resolve(), dst)
            dst.chmod(0o755)
            seen.add(name)
            copied.append(name)
            queue.append(dst)
    return copied


def vulkan_runtime_from_homebrew(dst):
    """Assembles vulkan/lib + share/vulkan/icd.d from Homebrew's vulkan-loader and molten-vk, as the SDK stages it."""
    prefix = Path(run(['brew', '--prefix']).strip())
    loader = prefix / 'lib/libvulkan.1.dylib'
    mvk = prefix / 'lib/libMoltenVK.dylib'
    icd = prefix / 'etc/vulkan/icd.d/MoltenVK_icd.json'
    if not (loader.exists() and mvk.exists()):
        raise SystemExit(f'no vulkan/ in the build folder and no Homebrew vulkan-loader / molten-vk in {prefix}')
    (dst / 'lib').mkdir(parents=True)
    (dst / 'share/vulkan/icd.d').mkdir(parents=True)
    shutil.copy2(loader.resolve(), dst / 'lib/libvulkan.1.dylib')
    shutil.copy2(mvk.resolve(), dst / 'lib/libMoltenVK.dylib')
    if icd.exists():
        shutil.copy2(icd, dst / 'share/vulkan/icd.d/MoltenVK_icd.json')
    else:
        (dst / 'share/vulkan/icd.d/MoltenVK_icd.json').write_text(
            '{"file_format_version": "1.0.0", "ICD": {"library_path": "../../../lib/libMoltenVK.dylib", "api_version": "1.4.0"}}\n')


# ---- signing and replacing -------------------------------------------------------------------------------------

def link_into_macos(mac, rt, name):
    """codesign treats a plain file or folder in Contents/MacOS as a nested code object, so data files live in
    Resources/runtime and Contents/MacOS gets a relative link: the SDK looks for them beside the executable."""
    link = mac / name
    link.unlink(missing_ok=True)
    link.symlink_to(Path('..') / 'Resources' / 'runtime' / name)


def sign_one(target, identity):
    try:
        run(['codesign', '--force', '--sign', identity, target])
    except SystemExit as e:
        if 'detritus' not in str(e):
            raise
        run(['xattr', '-cr', target])
        run(['codesign', '--force', '--sign', identity, target])


def sign_all(app, identity, nested=()):
    for b in nested:   # new data bundles first: the app's seal then takes each one's code hash, not its files
        sign_one(b, identity)
    plugins = app / 'Contents/PlugIns'
    machos = sorted((p for p in app.rglob('*') if p.is_file() and not p.is_symlink() and plugins not in p.parents and is_macho(p)),
                    key=lambda p: -len(p.parts))
    launcher = app / 'Contents/MacOS' / EXE_NAME
    for p in machos:
        if p != launcher:
            sign_one(p, identity)
    sign_one(launcher, identity)
    sign_one(app, identity)


# ---- data bundles (reused, signed) --------------------------------------------------------------------------------

DATA_FORMAT = 'bo2 data bundle 1'


def inventory(root):
    """Rows (relative path, mode, size, mtime_ns, ctime_ns, link target) of a tree or file: stat only, no reads."""
    root = Path(root)
    rows = []
    for p in sorted([root, *root.rglob('*')] if root.is_dir() else [root]):
        st = p.lstat()
        rows.append([str(p.relative_to(root)) if p != root else '.', st.st_mode, st.st_size, st.st_mtime_ns, st.st_ctime_ns,
                     os.readlink(p) if p.is_symlink() else None])
    return rows


def sha(obj): return hashlib.sha256(json.dumps(obj, sort_keys=True).encode()).hexdigest()


def read_key(out):
    try: return json.loads(Path(f'{out}.key').read_text())
    except (OSError, ValueError): return {}


def data_bundle(res, name): return res.parent / 'PlugIns' / f'{name}.bundle'


def place_data(c, old_app, previous, name, items, bundle_id, sign_id, fresh, data):
    """Put the files of items [(path in Resources, source, copy function)] into PlugIns/<name>.bundle and link them
    at their usual place in Resources. The bundle of the old app is cloned when the last pack made it from these
    inputs and nothing in it changed since (its seal in the app is its code hash only, so a changed file would go in
    unseen); otherwise it is filled here and signed in sign_all. Returns True when reused."""
    res = c / 'Resources'
    b = data_bundle(res, name)
    key = sha([DATA_FORMAT, hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), sign_id, bundle_id, name, [(t, inventory(src)) for t, src, _ in items]])
    data[name] = {'key': key}
    reuse = False
    if old_app and previous.get('data', {}).get(name, {}).get('key') == key:
        ob = data_bundle(old_app / 'Contents/Resources', name)
        try: reuse = ob.is_dir() and sha(inventory(ob)) == previous['data'][name].get('files')
        except OSError: reuse = False
    if reuse:
        b.parent.mkdir(exist_ok=True)
        run(['/bin/cp', '-cR', ob, b])
    else:
        (b / 'Contents/Resources').mkdir(parents=True)
        (b / 'Contents/Info.plist').write_bytes(plistlib.dumps({
            'CFBundleIdentifier': f'{bundle_id}.data.{name.lower()}', 'CFBundleName': name, 'CFBundlePackageType': 'BNDL',
            'CFBundleInfoDictionaryVersion': '6.0'}))
        for top, src, fill in items:
            dst = b / 'Contents/Resources' / top
            dst.parent.mkdir(parents=True, exist_ok=True)
            fill(src, dst)
        fresh.append(b)
    for top, _, _ in items:
        link = res / top
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(os.path.relpath(b / 'Contents/Resources' / top, link.parent))
    return reuse


def replace_app(tmp, out):
    if out.exists():
        swap = ctypes.CDLL(None, use_errno=True).renamex_np   # RENAME_SWAP: atomic, a failure leaves the old app
        swap.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        swap.restype = ctypes.c_int
        if swap(os.fsencode(tmp), os.fsencode(out), 2):
            err = ctypes.get_errno()
            raise OSError(err, os.strerror(err), str(out))
    else:
        tmp.rename(out)


# ---- main ------------------------------------------------------------------------------------------------------

def info_plist(a, has_icon):
    p = {
        'CFBundleName': a.name, 'CFBundleDisplayName': a.name, 'CFBundleExecutable': EXE_NAME,
        'CFBundleIdentifier': a.bundle_id, 'CFBundleVersion': a.version, 'CFBundleShortVersionString': a.version,
        'CFBundlePackageType': 'APPL', 'CFBundleInfoDictionaryVersion': '6.0',
        'LSMinimumSystemVersion': a.min_os, 'LSArchitecturePriority': ['arm64'], 'NSHighResolutionCapable': True,
        'LSApplicationCategoryType': 'public.app-category.games',
        'LSSupportsGameMode': True, 'GCSupportsGameMode': True,   # Game Mode (macOS 14+) prioritizes the CPU and GPU
        'NSSupportsAutomaticGraphicsSwitching': True,
        # GameController needs no permission prompt; HID pads outside it and voice chat do ask once
        'NSMicrophoneUsageDescription': a.microphone,
        'GCSupportedGameControllers': [{'ProfileName': 'ExtendedGamepad'}, {'ProfileName': 'MicroGamepad'}],
        'BO2PackedBy': 'tools/mac/pack_app.py',
    }
    if has_icon:
        p['CFBundleIconFile'] = 'AppIcon'
    if a.data_mode == 'external' and a.data_path:
        p['BO2DataPath'] = str(Path(a.data_path).expanduser())
    if a.game_manifest:   # read by the launcher (tools/mac/app/launcher.c); absent = Black Ops II
        m = a.game_manifest
        p.update({'RexGameExecutable': GAME, 'RexGameTitle': a.name, 'RexSupportFolder': m['support_folder'],
                  'RexEnvPrefix': m['env_prefix']})
        if a.toml:
            p['RexSettingsFile'] = f'{GAME}.toml'
        if 'BO2DataPath' in p:
            p['RexDataPath'] = p.pop('BO2DataPath')
    return p


def find_clang():
    return Path(run(['xcrun', '--find', 'clang']).strip())


def pack(a):
    out = Path(a.out).resolve()
    if out.suffix != '.app':
        raise SystemExit(f'the output must end in .app: {out}')
    # inputs
    if a.binary:
        binary, build = Path(a.binary).resolve(), Path(a.binary).resolve().parent
    else:
        build = Path(a.build).resolve()
        binary = build / GAME
    if not binary.is_file():
        raise SystemExit(f'no game executable at {binary}')
    shaders = Path(a.shaders).resolve() if a.shaders else None
    shader_files = []
    if shaders:
        for f in (f'{GAME}_shaders.spvpack', f'{GAME}_shaders.spvidx', f'{GAME}_pso.psolist', 'shader_seed',
                  'shader_prebuild'):
            if (shaders / f).exists():
                shader_files.append(shaders / f)
            elif f.endswith('spvpack'):
                raise SystemExit(f'no {f} in {shaders}')
    elif not a.allow_missing_shaders and not (a.game_manifest and not a.game_manifest.get('shaders', True)):
        raise SystemExit('--shaders is required (or --allow-missing-shaders for a test pack)')
    data = Path(a.data).resolve() if a.data else None
    if a.data_mode == 'clone' and not data:
        raise SystemExit('--data-mode clone needs --data')
    if data and not (data / 'game').is_dir() and not (data / 'default.xex').is_file():
        raise SystemExit(f'{data} has neither a game/ folder nor default.xex')
    icon = Path(a.icon).expanduser() if a.icon else None
    if icon and not icon.is_file():
        raise SystemExit(f'icon PNG does not exist: {icon}')
    toml = Path(a.toml) if a.toml else None
    if toml is None and not a.game_manifest and (ROOT / 'tools/mac/bo2zm.toml').is_file():
        toml = ROOT / 'tools/mac/bo2zm.toml'   # the build's own settings file, when this branch has it

    tmp = out.with_name(out.name + '.partial')
    shutil.rmtree(tmp, ignore_errors=True)
    c = tmp / 'Contents'
    mac, res, fw = c / 'MacOS', c / 'Resources', c / 'Frameworks'
    mac.mkdir(parents=True)
    res.mkdir()
    rt = res / 'runtime'
    rt.mkdir()

    step('launcher')
    clang = find_clang()
    sdk = run(['xcrun', '--sdk', 'macosx', '--show-sdk-path']).strip()
    run([clang, '-isysroot', sdk, '-arch', 'arm64', f'-mmacosx-version-min={a.min_os}', '-O2', '-Wall', '-framework', 'CoreFoundation',
         '-o', mac / EXE_NAME, ROOT / 'tools/mac/app/launcher.c'])

    step('game and libraries')
    shutil.copy2(binary, mac / GAME)
    for lib in sorted(build.glob('*.dylib')):
        shutil.copy2(lib, mac / lib.name)
    for lib in sorted((build / 'sdk-out').glob('*.dylib')) if (build / 'sdk-out').is_dir() else []:
        if not (mac / lib.name).exists():
            shutil.copy2(lib, mac / lib.name)
    step('libusb')
    libusb = Path('/opt/homebrew/lib/libusb-1.0.0.dylib')
    if not libusb.is_file():
        raise SystemExit(f'no libusb at {libusb}; install it with brew install libusb')
    shutil.copy2(libusb.resolve(), mac / 'libusb-1.0.dylib')
    (mac / 'libusb-1.0.dylib').chmod(0o755)
    for extra in a.extra_file or []:
        shutil.copy2(extra, mac / Path(extra).name)

    step('vulkan runtime (loader, MoltenVK)')
    if (build / 'vulkan').is_dir():
        shutil.copytree(build / 'vulkan', rt / 'vulkan', symlinks=True)
    else:
        vulkan_runtime_from_homebrew(rt / 'vulkan')
    link_into_macos(mac, rt, 'vulkan')

    old_app = out if out.is_dir() else None
    previous = read_key(out) if old_app else {}
    fresh, data_keys = [], {}   # data bundles filled in this pack (signed below), and every bundle's key for <out.app>.key
    sign_id = f'{a.sign}'
    step('shader library')
    clone_shaders = bool(shader_files) and can_clone(shader_files[0], mac)
    if shader_files:
        if place_data(c, old_app, previous, 'ShaderLibrary', [(f'runtime/{f.name}', f, lambda s_, d_: copy_tree(s_, d_, clone_shaders)) for f in shader_files],
                      a.bundle_id, sign_id, fresh, data_keys):
            step('shader library (reused, signed)')
    for f in shader_files:
        link_into_macos(mac, rt, f.name)

    if toml:
        shutil.copyfile(toml, rt / f'{GAME}.toml')
        if not a.game_manifest:   # BO2 reads it beside the game; the others from the support folder (the launcher seeds it)
            link_into_macos(mac, rt, f'{GAME}.toml')

    if a.data_mode == 'clone':
        step('game data (APFS clones)' if can_clone(data / 'game', res) else 'game data (plain copies: not the same APFS volume)')
        clone = can_clone(data / 'game', res)
        if not clone:
            free = free_bytes(res)
            need = sum(tree_bytes(data / d) for d in ('game', 'update', 'content') if (data / d).exists()) or tree_bytes(data)
            if need + (1 << 30) > free:
                raise SystemExit(f'the data needs {need / 2**30:.1f} GB on the volume of {out.parent}, {free / 2**30:.1f} GB are free; use --data-mode external')
        def fill_data(src, dst):
            if (src / 'game').is_dir():
                dst.mkdir()
                for d in ('game', 'update', 'content'):
                    if (src / d).exists():
                        copy_tree(src / d, dst / d, clone)
            else:   # the extracted disc itself
                copy_tree(src, dst, clone)
        if place_data(c, old_app, previous, 'GameData', [('data', data, fill_data)], a.bundle_id, sign_id, fresh, data_keys):
            step('game data (reused, signed)')

    has_icon = False
    if icon is not None:
        step('icon (supplied PNG)')
        make_icns(icon, res / 'AppIcon.icns', rounded=False)
        has_icon = True
    else:   # no supplied picture: use the game's own title picture, which only passes through a temp folder
        game_data = data
        if game_data is None and a.data_path:
            game_data = Path(a.data_path).expanduser()
        if game_data and (game_data / 'game').is_dir():
            game_data = game_data / 'game'
        rexglue = a.rexglue or next((p for p in (build / 'rexglue', build / 'bin/rexglue',
                                                ROOT / 'sdk/out/mac-arm64/rexglue') if p.is_file()), None)
        step('icon (extracting the game title picture)')
        try:
            if game_data is None:
                raise RuntimeError('no game data to take the icon from')
            with tempfile.TemporaryDirectory(prefix='.title-', dir=tmp.parent) as td:
                title = Path(td) / 'title.png'
                extract_icon(game_data, title, rexglue)
                make_icns(title, res / 'AppIcon.icns')
            has_icon = True
        except (OSError, RuntimeError) as e:
            print(f'warning: no game icon, the app gets the generic one: {e}', flush=True)

    step('fixing install names and rpaths')
    search = [Path(p) for p in (a.lib_dir or [])] + [build, Path(run(['brew', '--prefix'], check=False).strip() or '/opt/homebrew') / 'lib']
    copied = bundle_dependencies(mac, fw, search)
    for p in list(mac.glob('*')) + list(fw.glob('*') if fw.is_dir() else []):
        if p.is_file() and not p.is_symlink() and is_macho(p):
            fix_macho(p, in_frameworks=(p.parent == fw))
    for p in (mac / 'vulkan').rglob('*.dylib'):
        if not p.is_symlink():
            fix_macho(p, in_frameworks=True)   # @loader_path only; the loader finds MoltenVK through the icd file
    if copied:
        print('bundled libraries: ' + ', '.join(sorted(copied)))

    (c / 'Info.plist').write_bytes(plistlib.dumps(info_plist(a, has_icon)))
    (c / 'PkgInfo').write_text('APPL????')

    step(f'signing ({"ad-hoc" if a.sign == "-" else a.sign})')
    sign_all(tmp, a.sign, fresh)
    # no --deep: it reads every byte of every data bundle again (the seal holds a bundle by its code hash); a bundle made here was verified after signing
    run(['codesign', '--verify', '--strict', tmp])
    for b in fresh:
        run(['codesign', '--verify', '--strict', b])

    replace_app(tmp, out)
    shutil.rmtree(tmp, ignore_errors=True)   # the old app after a swap
    for name, k in data_keys.items():
        k['files'] = sha(inventory(data_bundle(out / 'Contents/Resources', name)))   # as the last pack left it, signature included
    Path(f'{out}.key').write_text(json.dumps({'data': data_keys}, sort_keys=True) + '\n')
    print(f'app: {out}', flush=True)


def game_manifest(game):
    path = ROOT / 'games' / game / 'app.toml'
    if not path.is_file():
        raise SystemExit(f'no app manifest {path}')
    m = tomllib.loads(path.read_text())
    if not m.get('name'):
        raise SystemExit(f'{path}: name is missing')
    # Every other field follows from the folder name unless app.toml says otherwise.
    defaults = {
        'executable': game,                                  # the game binary in the build folder
        'bundle_id': f'io.github.ryancircelli.{game}-360',
        'support_folder': f'{game}-360',                     # ~/Library/Application Support/<this> and ~/Library/Logs/<this>
        'env_prefix': game.upper(),                          # <PREFIX>_DATA, <PREFIX>_SUPPORT_DIR, <PREFIX>_LOG_DIR, ...
        'data_path': f'~/Games/Xbox360/{game}/disc',         # the extracted disc; never copied into the app
        # first settings file; the live copy is in the support folder
        'settings': 'app_settings.toml' if (path.parent / 'app_settings.toml').is_file()
                    else str(ROOT / 'tools/mac/game_app_settings.toml'),
        'shaders': False,                                    # no Vulkan shader library: draws with Metal
        'microphone': f"{m['name']} uses the microphone only if the game asks for it.",
    }
    m = defaults | m
    m['dir'] = path.parent
    return m


def parse(argv):
    global EXE_NAME, GAME
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument('--game')
    game = pre.parse_known_args(argv)[0].game
    m = game_manifest(game) if game else None
    ap = argparse.ArgumentParser(description='Pack the macOS build of a game into a double-clickable app')
    ap.add_argument('--game', help='a game of games/ with an app.toml (default: Black Ops II, whose facts are the defaults below)')
    ap.add_argument('--out', default='Black Ops II.app', help='the app to write (default: ./"Black Ops II.app")')
    ap.add_argument('--build', help='tools/build_mac.sh output folder (bo2zm, librex*.dylib, vulkan/)')
    ap.add_argument('--binary', help='a bare game executable instead of --build (libraries beside it are used)')
    ap.add_argument('--lib-dir', action='append', help='extra folder to find non-system libraries in (repeatable)')
    ap.add_argument('--extra-file', action='append', help='an extra file to place beside the executable (repeatable)')
    ap.add_argument('--shaders', help='folder with bo2zm_shaders.spvpack (and .spvidx)')
    ap.add_argument('--allow-missing-shaders', action='store_true', help='test packs only')
    ap.add_argument('--data', help='data folder (game/, update/, content/)')
    ap.add_argument('--data-mode', choices=('clone', 'external'), default=None)
    ap.add_argument('--data-path', help='external mode: data folder baked into Info.plist (BO2DataPath)')
    ap.add_argument('--toml', help='settings file placed beside the executable (default tools/mac/bo2zm.toml when present)')
    ap.add_argument('--icon', default=str(ROOT / 'docs/images/icon.png'),
                    help='a supplied PNG (wins); without one the game\'s own title picture is extracted')
    ap.add_argument('--rexglue', help='rexglue host tool used to extract the title picture (default: beside the build)')
    ap.add_argument('--name', default='Black Ops II')
    ap.add_argument('--version', default='1.1.0')
    ap.add_argument('--bundle-id', default='io.github.ryancircelli.bo2-360-nx')
    ap.add_argument('--min-os', default='15.0')
    ap.add_argument('--sign', default='-', help='codesign identity (default: - = ad-hoc)')
    ap.add_argument('--microphone', default='Black Ops II uses the microphone for in-game voice chat.')
    if m:
        ap.set_defaults(out=f"{m['name']}.app", name=m['name'], bundle_id=m['bundle_id'], version=m.get('version', '0.1.0'),
                        data_path=m.get('data_path'), microphone=m.get('microphone', f"{m['name']} uses the microphone."),
                        toml=str(m['dir'] / m['settings']) if m.get('settings') else None,
                        icon=str(m['dir'] / m['icon']) if m.get('icon') else '')
    a = ap.parse_args(argv)
    a.game_manifest = m
    if m:
        EXE_NAME, GAME = a.name, m['executable']
    if not a.build and not a.binary:
        ap.error('--build or --binary is required')
    if a.data_mode is None:
        a.data_mode = 'clone' if a.data else 'external'
    return a


if __name__ == '__main__':
    if sys.platform != 'darwin':
        raise SystemExit('pack_app.py runs on macOS')
    pack(parse(sys.argv[1:]))
