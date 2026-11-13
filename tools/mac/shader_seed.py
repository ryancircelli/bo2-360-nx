"""Builds the shipped Metal shader seed from recorded shader storage.

  python3 -I tools/mac/shader_seed.py --out <shader folder>/shader_seed <cache folder or .metal.xpso> [...]

Each input is a game cache folder (its cache/shaders/shareable/ is searched) or a <title>.metal.xpso beside its
<title>.xsh. The pipelines of every input recorded at the newest pipeline storage version are merged (standard
pipelines only), with the guest shaders they use, into <out>/<title>.metal.xpso and <out>/<title>.xsh in the
game's own storage format. pack_app.py ships <shader folder>/shader_seed; at boot the Metal renderer creates
the seed's pipelines the local cache lacks in the background (metal_shader_seed), so a new install or update
does not drop draws while it translates shaders it meets for the first time.

The xsh holds guest shader microcode: like the shader library it is game-derived, so --out must be outside
the repository. Nothing here is tied to the GPU or the macOS version.
"""
import argparse
import glob
import os
import struct
import sys

PIPELINE_MAGIC = 0x53504558  # 'XEPS'
METAL_API_MAGIC = 0x4C544D58  # 'XMTL'
SHADER_MAGIC = 0x48534558  # 'XESH'
DESCRIPTION_SIZE = 120  # MetalPipelineStoredDescription
KIND_OFFSET = 64  # MetalPipelineDescription::kind, after the 8-byte hash
KIND_STANDARD = 0
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def swap32(v):
    return struct.unpack('>I', struct.pack('<I', v))[0]


def find_inputs(paths):
    found = []
    for p in paths:
        if os.path.isdir(p):
            found += sorted(glob.glob(os.path.join(p, '**', 'shareable', '*.metal.xpso'), recursive=True))
        elif p.endswith('.metal.xpso'):
            found.append(p)
        else:
            raise SystemExit(f'not a cache folder or .metal.xpso: {p}')
    return found


def read_pipelines(path):
    data = open(path, 'rb').read()
    if len(data) < 12:
        return None, []
    magic, api, version = struct.unpack_from('<III', data, 0)
    if magic != PIPELINE_MAGIC or api != METAL_API_MAGIC:
        return None, []
    records = [data[o:o + DESCRIPTION_SIZE] for o in range(12, len(data) - DESCRIPTION_SIZE + 1, DESCRIPTION_SIZE)]
    return swap32(version), records


def read_shaders(path, shaders):
    """Adds the complete entries of a .xsh to shaders (ucode hash -> entry bytes); returns its header."""
    data = open(path, 'rb').read()
    if len(data) < 8 or struct.unpack_from('<I', data, 0)[0] != SHADER_MAGIC:
        return None
    o = 8
    while o + 12 <= len(data):
        ucode_hash, packed = struct.unpack_from('<QI', data, o)
        end = o + 12 + (packed & 0x7FFFFFFF) * 4
        if end > len(data):
            break
        shaders.setdefault(ucode_hash, data[o:end])
        o = end
    return data[:8]


def main():
    ap = argparse.ArgumentParser(description='Build the Metal shader seed from recorded shader storage')
    ap.add_argument('--out', required=True, help='folder to write <title>.metal.xpso and <title>.xsh to')
    ap.add_argument('inputs', nargs='+', help='cache folders or <title>.metal.xpso files')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if out == ROOT or out.startswith(ROOT + os.sep):
        raise SystemExit('--out must be outside the repository (the seed holds game-derived shader microcode)')

    inputs = []
    for path in find_inputs(a.inputs):
        version, records = read_pipelines(path)
        if version is not None:
            inputs.append((path, version, records))
    if not inputs:
        raise SystemExit('no Metal pipeline storage found in the inputs')
    newest = max(v for _, v, _ in inputs)
    titles = {os.path.basename(p).split('.')[0] for p, v, _ in inputs if v == newest}
    if len(titles) != 1:
        raise SystemExit(f'inputs mix titles: {sorted(titles)}')
    title = titles.pop()

    pipelines, shaders, header, used, stale = {}, {}, None, 0, 0
    for path, version, records in inputs:
        if version != newest:
            stale += 1
            continue
        used += 1
        header = read_shaders(path[:-len('.metal.xpso')] + '.xsh', shaders) or header
        for r in records:
            if struct.unpack_from('<I', r, KIND_OFFSET)[0] == KIND_STANDARD:
                pipelines.setdefault(r[:8], r)
    if header is None:
        raise SystemExit('no shader storage (.xsh) beside the inputs')

    kept, needed = [], set()
    missing = 0
    for r in pipelines.values():
        vs, _, ps = struct.unpack_from('<QQQ', r, 8)
        if vs not in shaders or (ps and ps not in shaders):
            missing += 1
            continue
        kept.append(r)
        needed.add(vs)
        if ps:
            needed.add(ps)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, title + '.metal.xpso'), 'wb') as f:
        f.write(struct.pack('<III', PIPELINE_MAGIC, METAL_API_MAGIC, swap32(newest)))
        f.write(b''.join(sorted(kept)))
    with open(os.path.join(out, title + '.xsh'), 'wb') as f:
        f.write(header)
        f.write(b''.join(shaders[h] for h in sorted(needed)))
    print(f'shader seed {out}: {len(kept)} pipelines, {len(needed)} shaders, storage version {newest:08X}; '
          f'{used} inputs merged, {stale} at older versions skipped, {missing} pipelines missing a shader')


if __name__ == '__main__':
    sys.exit(main())
