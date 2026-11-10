#!/usr/bin/env python3
"""Merge and check pipeline state lists (docs/mac/pso-list-format.md).

  pso_list.py merge a.psolist b.psolist -o merged.psolist
  pso_list.py check merged.psolist [--shaderpack-index bo2zm_shaders.spvidx]
  pso_list.py diff before.psolist after.psolist      (keys only in one of the two)

Standard library only. The 64-bit key is the game's job to compute; this tool treats it as an opaque identity.
"""
import argparse, hashlib, re, sys
from collections import OrderedDict

KEY = re.compile(r'^[0-9a-f]{16}$')


def read(path):
    meta, entries, end = OrderedDict(), OrderedDict(), None
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')
    if not lines or lines[0] != '# bo2pso 1':
        raise SystemExit(f'{path}: first line must be "# bo2pso 1"')
    for n, line in enumerate(lines[1:], 2):
        if not line:
            continue
        if line.startswith('# end'):
            end = dict(kv.split('=', 1) for kv in line.split('\t')[1:] if '=' in kv)
        elif line.startswith('#'):
            p = line[1:].strip().split('\t')
            meta[p[0]] = p[1:]
        else:
            f_ = line.split('\t')
            kind, key = f_[0], f_[1] if len(f_) > 1 else ''
            want = {'G': 7, 'C': 4}.get(kind)
            if want is None or len(f_) != want or not KEY.match(key):
                raise SystemExit(f'{path}:{n}: bad line: {line[:80]}')
            if not f_[-1].isdigit():
                raise SystemExit(f'{path}:{n}: uses must be a decimal number')
            if key in entries:
                old = entries[key]
                entries[key] = old[:-1] + [str(int(old[-1]) + int(f_[-1]))]
            else:
                entries[key] = f_
    return meta, entries, end


def write(path, meta, entries):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# bo2pso 1\n')
        for k, v in meta.items():
            f.write('# ' + '\t'.join([k] + v) + '\n')
        for e in entries.values():
            f.write('\t'.join(e) + '\n')
        f.write(f'# end\tpipelines={len(entries)}\n')


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('merge'); m.add_argument('lists', nargs='+'); m.add_argument('-o', required=True)
    c = sub.add_parser('check'); c.add_argument('list'); c.add_argument('--shaderpack-index')
    d = sub.add_parser('diff'); d.add_argument('a'); d.add_argument('b')
    a = ap.parse_args()
    if a.cmd == 'merge':
        meta, merged = OrderedDict(), OrderedDict()
        for p in a.lists:
            mt, es, _ = read(p)
            for k in ('renderer', 'shaderpack'):
                if k in mt and k in meta and mt[k] != meta[k]:
                    raise SystemExit(f'{p}: {k} {mt[k]} differs from {meta[k]}: lists from different renderers or shader libraries cannot be merged')
            for k, v in mt.items():
                meta.setdefault(k, v)
            for key, e in es.items():
                if key in merged:
                    merged[key][-1] = str(int(merged[key][-1]) + int(e[-1]))
                else:
                    merged[key] = list(e)
        write(a.o, meta, merged)
        print(f'{a.o}: {len(merged)} pipelines from {len(a.lists)} lists')
    elif a.cmd == 'check':
        meta, es, end = read(a.list)
        print(f'{a.list}: {len(es)} pipelines ({sum(1 for e in es.values() if e[0] == "G")} graphics, '
              f'{sum(1 for e in es.values() if e[0] == "C")} compute), renderer {meta.get("renderer", ["?"])[0]}')
        if end is None:
            print('warning: no "# end" line: the file is incomplete', file=sys.stderr)
        elif int(end.get('pipelines', -1)) != len(es):
            print(f'warning: "# end" says {end.get("pipelines")} pipelines, found {len(es)}', file=sys.stderr)
        if a.shaderpack_index:
            h = hashlib.sha256(open(a.shaderpack_index, 'rb').read()).hexdigest()
            want = meta.get('shaderpack', ['', '', ''])
            if len(want) < 2 or want[1] != h:
                raise SystemExit(f'list was recorded against a different shader library (index sha256 {h[:16]}... vs {want[1][:16] if len(want) > 1 else "none"}...)')
            print('shader library index matches')
    else:
        _, x, _ = read(a.a); _, y, _ = read(a.b)
        print(f'only in {a.a}: {len(set(x) - set(y))}   only in {a.b}: {len(set(y) - set(x))}   shared: {len(set(x) & set(y))}')
        for k in sorted(set(y) - set(x))[:50]:
            print('  new', '\t'.join(y[k]))


if __name__ == '__main__':
    main()
