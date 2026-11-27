# Decode a Forza 'cxds' thumbnail: zlib raw payload = D3DTexture header (52 B) + tiled texel data.
import sys, zlib, struct, numpy as np
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
b = open(src, 'rb').read()
assert b[:4] == b'cxds', b[:4]
ver, usz, csz, usz2 = struct.unpack('>4I', b[4:20])
raw = zlib.decompress(b[20:20+csz], -15)
assert len(raw) == usz
hdr = struct.unpack('>13I', raw[:52])
print('container', ver, usz, csz, usz2, 'tail', len(b) - 20 - csz)
print('hdr', [hex(h) for h in hdr])
d0, d1, d2, d3, d4, d5 = hdr[7:]
pitch = ((d0 >> 22) & 0x1FF) * 32; tiled = d0 >> 31
fmt = d1 & 63; endian = (d1 >> 6) & 3
w = (d2 & 0x1FFF) + 1; h = ((d2 >> 13) & 0x1FFF) + 1
print(f'pitch {pitch} tiled {tiled} fmt {fmt} endian {endian} {w}x{h} mips {(d4>>6)&15}..{(d4>>10)&15} dim {(d5>>9)&3}')
data = np.frombuffer(raw[52:], dtype=np.uint8)
pw = pitch or ((w + 31) & ~31)
ph = len(data) // 4 // pw
print('stored rows', ph)
px = data.reshape(-1, 4)
if endian == 2: px = px[:, ::-1]          # 8in32
elif endian == 1: px = px[:, [1, 0, 3, 2]]
x, y = np.meshgrid(np.arange(pw), np.arange(ph))
if tiled:
    bl = 2
    macro = ((x >> 5) + (y >> 5) * (pw >> 5)) << (bl + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << bl
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    off = ((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) + ((((((y & 8) >> 2) + (x >> 3)) & 3)) << 6) + (off & 0x3F)
    idx = off >> 2
else:
    idx = y * pw + x
ok = idx < len(px)
img = np.zeros((ph, pw, 4), np.uint8)
img[ok] = px[idx[ok]]
# k_8_8_8_8 in memory after swap is BGRA (little-endian ARGB dword)
rgb = img[:, :, [2, 1, 0]]
Image.fromarray(rgb[:h, :w]).save(out)
np.save(out + '.npy', img[:h, :w])
