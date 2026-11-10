#!/usr/bin/env python3
"""Packs the macOS build of bo2-zm-nx into a double-clickable "Black Ops II.app".

Borrowed from switch2mac's dm/mac/pack_app.py (MIT, Copyright (c) 2026 seedds): the APFS clone copy, the .icns
from a picture with sips and iconutil, the atomic swap of the finished app, the codesign retry on detritus. The
rest is written for this project. No game data is ever written inside the repository.

  python3 -I tools/mac/pack_app.py --build <build folder> --shaders <shader folder> --data <data folder> [options]

  --build    the folder tools/build_mac.sh leaves ($BO2_BUILD/out/mac-arm64): bo2zm, its librex*.dylib files and
             the vulkan/ runtime (loader, MoltenVK, icd file). With --binary a bare executable instead.
  --shaders  folder with bo2zm_shaders.spvpack and bo2zm_shaders.spvidx
  --data     the data folder (game/, update/, content/; tools/data_dir.sh). Not needed with --data-mode external.
  --data-mode clone     (default with --data) cp -c clones of the data into Contents/Resources/data: no extra
                        space on the same APFS volume, a plain copy elsewhere
  --data-mode external  nothing is copied. The app reads the path from ~/Library/Application Support/
                        bo2-360-nx/data_path.txt, $BO2_DATA, or --data-path (baked into Info.plist: BO2DataPath)

Layout of the app:
  Contents/MacOS/Black Ops II       the launcher (tools/mac/app/launcher.c), CFBundleExecutable
  Contents/MacOS/bo2zm              the game; the SDK finds beside it the shader library, bo2zm.toml, librexgpu-*.dylib
  Contents/MacOS/*.dylib            the build's own libraries (the GPU plugin is loaded from here)
  Contents/MacOS/vulkan, bo2zm_shaders.spvpack/.spvidx, bo2zm.toml
                                    relative links into Resources/runtime (codesign refuses plain data files in
                                    MacOS/); the SDK finds all of them beside the executable
  Contents/Frameworks/              any other non-system library the binaries linked (copied, install names fixed)
  Contents/Resources/AppIcon.icns   from docs/images/icon.png
  Contents/Resources/runtime/       the real vulkan/ (loader, MoltenVK, icd file), shader library, bo2zm.toml
  Contents/Resources/data/          the clones, with --data-mode clone

Every Mach-O gets @loader_path and @executable_path/../Frameworks as its only rpaths (build-machine rpaths are
removed), then everything is ad-hoc signed (--sign - ; no hardened runtime, so the game's dlopen of the GPU plugin
needs no entitlement), inner files first.
"""
import argparse, ctypes, os, plistlib, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE_NAME = 'Black Ops II'
GAME = 'bo2zm'
ICON_SIZES = (16, 32, 128, 256, 512)
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


def tree_bytes(p):
    return p.stat().st_size if p.is_file() else sum((Path(r) / f).stat().st_size for r, _, fs in os.walk(p) for f in fs)


def make_icns(png, out):
    with tempfile.TemporaryDirectory(dir=out.parent) as t:
        s = Path(t) / 'AppIcon.iconset'
        s.mkdir()
        for n in ICON_SIZES:
            for k, suffix in ((1, ''), (2, '@2x')):
                run(['sips', '-s', 'format', 'png', '-z', n * k, n * k, png, '--out', s / f'icon_{n}x{n}{suffix}.png'])
        run(['iconutil', '-c', 'icns', s, '-o', out])


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


def sign_all(app, identity):
    machos = sorted((p for p in app.rglob('*') if p.is_file() and not p.is_symlink() and is_macho(p)),
                    key=lambda p: -len(p.parts))
    launcher = app / 'Contents/MacOS' / EXE_NAME
    for p in machos:
        if p != launcher:
            sign_one(p, identity)
    sign_one(launcher, identity)
    sign_one(app, identity)


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
        'NSMicrophoneUsageDescription': 'Black Ops II uses the microphone for in-game voice chat.',
        'GCSupportedGameControllers': [{'ProfileName': 'ExtendedGamepad'}, {'ProfileName': 'MicroGamepad'}],
        'BO2PackedBy': 'tools/mac/pack_app.py',
    }
    if has_icon:
        p['CFBundleIconFile'] = 'AppIcon'
    if a.data_mode == 'external' and a.data_path:
        p['BO2DataPath'] = str(Path(a.data_path).expanduser())
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
        for f in ('bo2zm_shaders.spvpack', 'bo2zm_shaders.spvidx', 'bo2zm_pso.psolist'):
            if (shaders / f).is_file():
                shader_files.append(shaders / f)
            elif f.endswith('spvpack'):
                raise SystemExit(f'no {f} in {shaders}')
    elif not a.allow_missing_shaders:
        raise SystemExit('--shaders is required (or --allow-missing-shaders for a test pack)')
    data = Path(a.data).resolve() if a.data else None
    if a.data_mode == 'clone' and not data:
        raise SystemExit('--data-mode clone needs --data')
    if data and not (data / 'game').is_dir():
        raise SystemExit(f'{data} has no game/ folder')
    icon = Path(a.icon)
    toml = Path(a.toml) if a.toml else None
    if toml is None and (ROOT / 'tools/mac/bo2zm.toml').is_file():
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
    for extra in a.extra_file or []:
        shutil.copy2(extra, mac / Path(extra).name)

    step('vulkan runtime (loader, MoltenVK)')
    if (build / 'vulkan').is_dir():
        shutil.copytree(build / 'vulkan', rt / 'vulkan', symlinks=True)
    else:
        vulkan_runtime_from_homebrew(rt / 'vulkan')
    link_into_macos(mac, rt, 'vulkan')

    step('shader library')
    clone_shaders = bool(shader_files) and can_clone(shader_files[0], mac)
    for f in shader_files:
        copy_tree(f, rt / f.name, clone_shaders)
        link_into_macos(mac, rt, f.name)

    if toml:
        shutil.copyfile(toml, rt / 'bo2zm.toml')
        link_into_macos(mac, rt, 'bo2zm.toml')

    if a.data_mode == 'clone':
        step('game data (APFS clones)' if can_clone(data / 'game', res) else 'game data (plain copies: not the same APFS volume)')
        clone = can_clone(data / 'game', res)
        if not clone:
            free = shutil.disk_usage(res).free
            need = sum(tree_bytes(data / d) for d in ('game', 'update', 'content') if (data / d).exists())
            if need + (1 << 30) > free:
                raise SystemExit(f'the data needs {need / 2**30:.1f} GB on the volume of {out.parent}, {free / 2**30:.1f} GB are free; use --data-mode external')
        (res / 'data').mkdir()
        for d in ('game', 'update', 'content'):
            if (data / d).exists():
                copy_tree(data / d, res / 'data' / d, clone)

    has_icon = False
    if icon.is_file():
        step('icon')
        make_icns(icon, res / 'AppIcon.icns')
        has_icon = True

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
    sign_all(tmp, a.sign)
    run(['codesign', '--verify', '--deep', '--strict', tmp])

    replace_app(tmp, out)
    shutil.rmtree(tmp, ignore_errors=True)   # the old app after a swap
    print(f'app: {out}', flush=True)


def parse(argv):
    ap = argparse.ArgumentParser(description='Pack the macOS build of bo2-zm-nx into a double-clickable app')
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
    ap.add_argument('--icon', default=str(ROOT / 'docs/images/icon.png'))
    ap.add_argument('--name', default='Black Ops II')
    ap.add_argument('--version', default='1.1.0')
    ap.add_argument('--bundle-id', default='io.github.ryancircelli.bo2-360-nx')
    ap.add_argument('--min-os', default='15.0')
    ap.add_argument('--sign', default='-', help='codesign identity (default: - = ad-hoc)')
    a = ap.parse_args(argv)
    if not a.build and not a.binary:
        ap.error('--build or --binary is required')
    if a.data_mode is None:
        a.data_mode = 'clone' if a.data else 'external'
    return a


if __name__ == '__main__':
    if sys.platform != 'darwin':
        raise SystemExit('pack_app.py runs on macOS')
    pack(parse(sys.argv[1:]))
