#!/usr/bin/env python3
"""Extract a game's own title picture and turn a PNG into a Mac icon.

  python3 -I tools/mac/game_icon.py <game folder | default.xex> <icon.png> --rexglue <path>

The XEX is copied into temporary storage before rexglue loads it, so extraction never writes metadata beside the
player's original. No game picture is written into the source tree unless the caller explicitly chooses that path.
"""

import argparse
import base64
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ICON_SIZES = (16, 32, 128, 256, 512)


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'{Path(str(cmd[0])).name} failed ({r.returncode}): '
                           f'{(r.stderr or r.stdout).strip()[-1500:]}')
    return r.stdout


def game_executable(game):
    game = Path(game).expanduser().resolve()
    if game.is_file() and game.suffix.lower() == '.xex':
        return game
    if not game.is_dir():
        raise RuntimeError(f'game folder does not exist: {game}')
    names = {p.name.lower(): p for p in game.iterdir() if p.is_file()}
    for name in ('default.xex', 'default_mp.xex'):
        if name in names:
            return names[name]
    raise RuntimeError(f'no default.xex or default_mp.xex in {game}')


def png_size(path):
    data = path.read_bytes()
    if len(data) < 24 or data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
        raise RuntimeError(f'rexglue did not produce a PNG title picture: {path}')
    width, height = struct.unpack('>II', data[16:24])
    if not width or not height:
        raise RuntimeError(f'the title picture is empty: {path}')
    return width, height


def extract_icon(game, out, rexglue):
    source = game_executable(game)
    rexglue = Path(rexglue).expanduser().resolve() if rexglue else shutil.which('rexglue')
    if not rexglue or not Path(rexglue).is_file():
        raise RuntimeError('rexglue is needed to extract the game icon; pass --rexglue <path>')
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.game-icon-', dir=out.parent) as t:
        tmp = Path(t)
        copied = tmp / 'input' / source.name
        copied.parent.mkdir()
        shutil.copyfile(source, copied)
        metadata = tmp / 'metadata'
        try:
            run([rexglue, 'init', '--project-name', 'icon', '--xex-path', copied,
                 'achievements', copied, metadata])
        except RuntimeError as e:
            raise RuntimeError(f'could not extract the title picture from {source}: {e}') from None
        title = metadata / 'icons' / 'title.png'
        if not title.is_file():
            raise RuntimeError(f'no XDBF title picture (image 0x8000) in {source}')
        width, height = png_size(title)
        shutil.copyfile(title, out)
    return width, height


def make_icns(png, out):
    """Lanczos-resample the picture into a centred, rounded Mac icon, then make every required size."""
    png = Path(png).resolve()
    out = Path(out)
    with tempfile.TemporaryDirectory(prefix='.iconbig-', dir=out.parent) as t:
        big = Path(t) / 'big.png'   # smooth (not blocky) enlargement of the small title picture
        run(['sips', '-s', 'format', 'png', '-z', 824, 824, png, '--out', big])
        encoded = base64.b64encode(big.read_bytes()).decode('ascii')
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
           f'width="1024" height="1024" viewBox="0 0 1024 1024">'
           f'<defs><clipPath id="icon"><rect x="100" y="100" width="824" height="824" rx="185"/></clipPath></defs>'
           f'<g clip-path="url(#icon)"><image x="100" y="100" width="824" height="824" '
           f'preserveAspectRatio="xMidYMid meet" xlink:href="data:image/png;base64,{encoded}"/></g></svg>')
    with tempfile.TemporaryDirectory(prefix='.iconset-', dir=out.parent) as t:
        tmp = Path(t)
        source = tmp / 'icon.svg'
        source.write_text(svg)
        iconset = tmp / 'AppIcon.iconset'
        iconset.mkdir()
        for n in ICON_SIZES:
            for scale, suffix in ((1, ''), (2, '@2x')):
                size = n * scale
                run(['sips', '-s', 'format', 'png', '-z', size, size, source,
                     '--out', iconset / f'icon_{n}x{n}{suffix}.png'])
        run(['iconutil', '-c', 'icns', iconset, '-o', out])


def main(argv=None):
    ap = argparse.ArgumentParser(description="Extract an Xbox 360 game's own title picture")
    ap.add_argument('game', help='game folder or its XEX executable')
    ap.add_argument('out', help='PNG to write')
    ap.add_argument('--rexglue', default=os.environ.get('REXGLUE'), help='rexglue host tool (or $REXGLUE)')
    a = ap.parse_args(argv)
    try:
        width, height = extract_icon(a.game, a.out, a.rexglue)
    except (OSError, RuntimeError) as e:
        raise SystemExit(str(e))
    print(f'{a.out}: {width}x{height} title picture')


if __name__ == '__main__':
    main(sys.argv[1:])
