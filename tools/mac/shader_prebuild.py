"""Builds the shipped Metal shader prebuild: build-time-compiled shader state.

  python3 -I tools/mac/shader_prebuild.py --out <shader folder>/shader_prebuild \
      --dumps <dir with FABLE2_NS_DUMP_DIR metallibs> --archives <dir with .binarchive files>

Inputs, both from a headless shader-prep run of the packed app on the build
machine (tools/mac/runq or a quiet Mac):

  --dumps     FABLE2_NS_DUMP_DIR output: every translation the run installed,
              as <vs|ps>_<ucode hash>_<modification>.metallib. Shipped as-is;
              CompileNativeXlate installs them on the player's first launch
              instead of translating + compiling MSL (metal_shader_prebuild.h).
  --archives  The run's binary archive segments (*.binarchive). The pipeline
              descriptors are extracted with metal-source -flatbuffers=json and
              their libraries are re-pointed at the dumps (archives recorded
              in-memory libraries by UUID; the UUID is the SHA-256 of the
              library bytes, which is exactly the dump file's hash). Each
              archive set (rtv-bindless[.ns1|.ns2]) becomes one metal-tt
              pipeline script, translated for every targeted Apple GPU into a
              universal binary archive (*.gpubin). Pipeline creation on the
              player's Mac is then a lookup in that archive, not a driver
              compile.

The --out folder holds index.tsv, the *.metallib dumps and the *.gpubin
archives; pack_app.py ships it next to shader_seed. Everything in it is
game-derived: --out must be outside the repository.

The player's runtime keys the staged prebuild by the shader storage ABI tag
(metal_shader_prebuild.h), so an app update that changes the translator never
reads a stale prebuild; anything missing falls back to the runtime path.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
METAL_TOOLCHAIN = None  # resolved lazily


def metal_bin(name):
    """A Metal toolchain binary, preferring the installed asset toolchain."""
    global METAL_TOOLCHAIN
    if METAL_TOOLCHAIN is None:
        component = json.loads(subprocess.check_output(
            ['xcodebuild', '-showComponent', 'MetalToolchain', '-json'], text=True))
        METAL_TOOLCHAIN = Path(component['toolchainSearchPath']) / 'Metal.xctoolchain/usr/bin'
    return str(METAL_TOOLCHAIN / name)


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)


def entry_of_metallib(path):
    """The exported entry point of a dumped translation library."""
    r = run([metal_bin('metal-nm'), path])
    if r.returncode:
        return None, (r.stderr or r.stdout).strip()[-300:]
    entries = [line.split()[-1] for line in r.stdout.splitlines()
               if len(line.split()) >= 3 and line.split()[-2] in ('T', 'W')]
    if not entries:
        return None, 'no exported text symbol'
    # A translation library exports its single entry point; helpers are
    # internal. Prefer a native-looking name when several appear.
    for e in entries:
        if 'native' in e or 'main' in e.lower():
            return e, None
    return entries[0], None


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest().upper()


def extract_scripts(archive, outdir):
    """mtlp-json pipeline scripts from a binary archive segment."""
    r = run([metal_bin('metal-source'), '-flatbuffers=json', archive, '-o', outdir])
    if r.returncode:
        raise SystemExit(f'metal-source failed on {archive}: {(r.stderr or r.stdout)[-400:]}')
    scripts = []
    for p in sorted(Path(outdir).glob('*/descriptor/*.mtlp-json')):
        with open(p) as f:
            scripts.append(json.load(f))
    return scripts


def set_suffix_of(archive_name, title):
    """'rtv-bindless[.ns1|.ns2]' from a segment file name."""
    m = re.match(re.escape(title) + r'\.(.*)\.msc[0-9A-Fa-f]+-.*\.metal\.seg', archive_name)
    return m.group(1) if m else None


def compile_one(src, dumps):
    name = src.stem
    dst = dumps / (name + '.metallib')
    if dst.is_file():
        return None
    air = dumps / (name + '.air')
    flags = ['-std=metal3.2', '-fmetal-math-mode=safe', '-w']
    if name.startswith('vs_'):
        flags.append('-fpreserve-invariance')
    r = run(['xcrun', '-sdk', 'macosx', 'metal', *flags, '-c', src, '-o', air])
    if r.returncode:
        return f'{name}: {(r.stderr or r.stdout)[-200:]}'
    r = run(['xcrun', '-sdk', 'macosx', 'metallib', air, '-o', dst])
    air.unlink(missing_ok=True)
    return None if r.returncode == 0 else f'{name}: {(r.stderr or r.stdout)[-200:]}'


def compile_msl(msl, dumps):
    """Compile the harvested MSL sources the way CompileNativeXlate does (Metal 3.2, safe math,
    invariant vertex position); plain variants only (no _spec/_pts/_rect/... suffix)."""
    os.makedirs(dumps, exist_ok=True)
    srcs = [p for p in sorted(msl.glob('*.metal')) if re.fullmatch(r'(vs|ps)_[0-9A-F]{16}_[0-9A-F]+', p.stem)]
    with ThreadPoolExecutor(max_workers=8) as pool:
        errs = [e for e in pool.map(lambda p: compile_one(p, dumps), srcs) if e]
    print(f'{len(srcs)} MSL sources compiled, {len(errs)} failed')
    for e in errs[:10]:
        print('  ' + e, file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description='Build the shipped Metal shader prebuild')
    ap.add_argument('--out', required=True, help='folder to write the shader_prebuild into')
    ap.add_argument('--dumps', required=True, help='FABLE2_NS_DUMP_DIR harvest folder')
    ap.add_argument('--msl', default=None,
                    help='REX_METAL_NATIVE_XLATE_DUMP harvest folder (<vs|ps>_<hash>_<mod>.metal); '
                         'compiled here into .metallib files in --dumps with the runtime options')
    ap.add_argument('--archives', required=True, help='folder with the harvest .binarchive segments')
    ap.add_argument('--libs-only', action='store_true',
                    help='stage only the shader libraries and index.tsv (pass 1: harvest with these '
                         'loaded so the archives reference their exact content hashes)')
    ap.add_argument('--title', default=None, help='title id hex (default: from the archive names)')
    ap.add_argument('--macos-min', default='15.0', help='deployment target of the GPU binaries')
    ap.add_argument('--sets', default='rtv-bindless',
                    help='comma-separated archive sets to prebuild (default: the plain pipeline '
                         'set only; the .ns1/.ns2 sets reference optimized libraries the dumps '
                         'do not carry and are runtime-built as today)')
    ap.add_argument('--archs', default=None,
                    help='comma-separated applegpu archs (default: every arch the toolchain offers '
                         'for --macos-min, tried one by one)')
    a = ap.parse_args()
    out = Path(a.out).resolve()
    if out == ROOT or str(out).startswith(str(ROOT) + os.sep):
        raise SystemExit('--out must be outside the repository (game-derived shader state)')
    dumps = Path(a.dumps).resolve()
    archives = Path(a.archives).resolve()

    if a.msl:
        compile_msl(Path(a.msl).resolve(), dumps)
    metallibs = sorted(p for p in dumps.glob('*.metallib'))
    if not metallibs:
        raise SystemExit(f'no .metallib dumps in {dumps}')
    os.makedirs(out, exist_ok=True)

    # 1) Index the dumps: key (the file name the runtime looks up), stage, and
    #    the exported entry point; content hash for the archive descriptor
    #    library re-pointing below.
    index_lines = []
    by_hash = {}
    with ThreadPoolExecutor(max_workers=8) as pool:
        entries = list(pool.map(entry_of_metallib, metallibs))
    for path, (entry, err) in zip(metallibs, entries):
        if not entry:
            print(f'skip {path.name}: {err}', file=sys.stderr)
            continue
        stage = 'vs' if path.name.startswith('vs_') else 'ps'
        index_lines.append(f'shader\t{path.name}\t{stage}\t{entry}')
        shutil.copy2(path, out / path.name)
        by_hash[sha256_of(path)] = path.name
    print(f'{len(index_lines)} shader libraries indexed')

    if a.libs_only:
        with open(out / 'index.tsv', 'w') as f:
            f.write('\n'.join(index_lines) + '\n')
        print(f'libs-only prebuild {out}: {len(index_lines)} entries')
        return 0

    # 2) Extract the pipeline scripts from every archive segment and group
    #    them by archive set.
    segments = sorted(archives.glob('*.binarchive')) or sorted(archives.rglob('*.binarchive'))
    if not segments:
        raise SystemExit(f'no .binarchive segments in {archives}')
    title = a.title
    sets = {}
    unresolved = 0
    dropped = 0
    with tempfile.TemporaryDirectory(prefix='shader-prebuild-') as tmp:
        for seg in segments:
            if title is None:
                m = re.match(r'([0-9A-Fa-f]{8})\.', seg.name)
                if m:
                    title = m.group(1).upper()
            suffix = set_suffix_of(seg.name, title or '........')
            if suffix is None:
                print(f'skip {seg.name}: not a pipeline archive segment', file=sys.stderr)
                continue
            if suffix not in a.sets.split(','):
                print(f'skip {seg.name}: set {suffix!r} not in --sets', file=sys.stderr)
                continue
            bucket = sets.setdefault(suffix, {'libraries': {}, 'render': [], 'compute': []})
            for script in extract_scripts(seg, Path(tmp) / seg.stem):
                for lib in script.get('libraries', {}).get('paths', []):
                    label, path = lib.get('label', ''), lib.get('path', '')
                    m = re.match(r'_Path_not_available_for_lib_from_source_with_UUID_([0-9A-Fa-f]+)$', path)
                    if m:
                        file = by_hash.get(m.group(1).upper())
                        if file is None:
                            unresolved += 1
                            bucket.setdefault('bad', set()).add(label)
                            continue
                        bucket['libraries'][label] = file
                    else:
                        base = os.path.basename(path)
                        if not (dumps / base).is_file():
                            unresolved += 1
                            bucket.setdefault('bad', set()).add(label)
                            continue
                        bucket['libraries'][label] = base
                seen = {json.dumps(x, sort_keys=True) for x in bucket['render'] + bucket['compute']}
                for p in script.get('pipelines', {}).get('render_pipelines', []):
                    s = json.dumps(p, sort_keys=True)
                    if any(f'alias:{b}#' in s for b in bucket.get('bad', ()) if b not in bucket['libraries']):
                        dropped += 1
                        continue
                    if s not in seen:
                        bucket['render'].append(p)
                        seen.add(s)
                for p in script.get('pipelines', {}).get('compute_pipelines', []):
                    s = json.dumps(p, sort_keys=True)
                    if any(f'alias:{b}#' in s for b in bucket.get('bad', ()) if b not in bucket['libraries']):
                        dropped += 1
                        continue
                    if s not in seen:
                        bucket['compute'].append(p)
                        seen.add(s)
    print({k:(len(b['render']),len(b['compute']),len(b['libraries']),len(b.get('bad',()))) for k,b in sets.items()})
    print(f'{dropped} pipelines dropped (unresolved library)')
    if unresolved:
        print(f'warning: {unresolved} pipelines reference libraries with no matching dump '
              f'(their UUID is not the SHA-256 of any dump; harvest on one machine with '
              f'FABLE2_NS_DUMP_DIR set)', file=sys.stderr)
    if not sets or not any(b['render'] or b['compute'] for b in sets.values()):
        raise SystemExit('no pipelines extracted from the archives')

    # 3) metal-tt: translate every set's pipelines to GPU binaries for the
    #    targeted Apple GPU architectures, one universal binary archive each.
    if a.archs:
        archs = a.archs.split(',')
    else:
        r = run([metal_bin('applegpu-nt'), '-archs'])
        archs = [x for x in r.stdout.split() if x.startswith('applegpu_')]
    kept = []
    for arch in archs:
        r = run([metal_bin('applegpu-nt'), '-available', arch, 'macos', a.macos_min])
        if r.returncode == 0:
            kept.append(arch)
    print(f'{len(kept)}/{len(archs)} applegpu archs available for macOS {a.macos_min}')
    if not kept:
        raise SystemExit('no usable architectures')

    for suffix, bucket in sets.items():
        script = {
            'version': {'major': 0, 'minor': 3, 'sub_minor': 0},
            'generator': 'MetalFramework',
            'libraries': {'paths': [
                {'label': label, 'path': file}
                for label, file in sorted(bucket['libraries'].items())]},
            'pipelines': {},
        }
        if bucket['render']:
            script['pipelines']['render_pipelines'] = bucket['render']
        if bucket['compute']:
            script['pipelines']['compute_pipelines'] = bucket['compute']
        script_path = Path(tempfile.mkstemp(prefix=f'{suffix}-', suffix='.mtlp-json')[1])
        with open(script_path, 'w') as f:
            json.dump(script, f)
        gpubin = out / f'{suffix}.gpubin'
        cmd = [metal_bin('metal-tt')]
        for arch in kept:
            cmd += ['-arch', arch]
        cmd += ['-platform_version', 'macos', a.macos_min.split('.')[0], a.macos_min,
                '-L', dumps, script_path, '-o', gpubin]
        r = run(cmd)
        script_path.unlink()
        if r.returncode or not gpubin.is_file():
            raise SystemExit(f'metal-tt failed for {suffix}: {(r.stderr or r.stdout)[-500:]}')
        index_lines.append(f'archive\t{suffix}\t{gpubin.name}')
        print(f'{suffix}: {len(bucket["render"])} render + {len(bucket["compute"])} compute '
              f'pipelines -> {gpubin.name} ({gpubin.stat().st_size >> 10} KB, {len(kept)} archs)')

    with open(out / 'index.tsv', 'w') as f:
        f.write('\n'.join(index_lines) + '\n')
    print(f'shader prebuild {out}: {len(index_lines)} index entries')


if __name__ == '__main__':
    sys.exit(main())
