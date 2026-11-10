"""Decrypt and decompress a BO2 Xbox 360 fastfile (.ff), then list the shader blobs inside. Read-only on game data.

Container format follows OpenAssetTools (refs/OpenAssetTools, git-ignored):
  ZoneLoaderFactoryT6.cpp, ProcessorXChunks.cpp, AbstractSalsa20Processor.cpp,
  XChunkProcessorSalsa20Decryption.cpp, XMemDecompress.cpp, thirdparty/lzx/lzx.c.
Stdlib only (pure-Python Salsa20 and LZX), so it fans the work out over processes.

The Salsa20 key is never stored in this repo. By default it is rebuilt from your own decrypted XEX image
(--xex-image, default work/dump_tu18/default_mp.image.bin), the same way the game builds it: the zone auth-header
function stores the 32 key bytes as immediates and memcpy's them into the loader state. Fallbacks: --key-file
(32 raw bytes, or text holding 32 hex bytes), then the git-ignored OAT clone. Decompressed zones go to a scratch
folder outside the repo (default: %TEMP%/bo2_ff); the tool refuses to write inside the repo.

usage: python -I tools/inspect/ff_shaders.py <file.ff> [...] [--out DIR] [--xex-image IMG | --key-file FILE]
                                             [--list N] [--dump] [--image-shaders]
--image-shaders adds the shaders linked into the XEX image itself (find_image_shaders), written to <out>/<image>_shaders/.
"""
import argparse
import hashlib
import multiprocessing
import os
import re
import struct
import sys
import tempfile

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
OAT_CONSTANTS = os.path.join(REPO, "refs", "OpenAssetTools", "src", "ZoneCommon", "Game", "T6", "ZoneConstantsT6.h")
DEFAULT_IMAGE = os.path.join(REPO, "work", "dump_tu18", "default_mp.image.bin")

HEADER_SIZE = 12          # magic[8] + version u32
AUTH_SIZE = 8 + 4 + 32 + 256  # "PHEEBs71" + flags + zone name[32] + RSA-2048 signature
STREAM_COUNT = 4
XCHUNK_SIZE = 0x8000
VANILLA_BUFFER = 0x80000
BLOCK_HASHES = 200
ZONE_VERSION_XENON = 146
BLOCK_NAMES = ("temp", "runtime_virtual", "runtime_physical", "delay_virtual", "delay_physical",
               "virtual", "physical", "streamer_reserve")


# ---------------------------------------------------------------------------------------------------------------
# key


def _s16(v):
    return v - 0x10000 if v & 0x8000 else v


def key_from_xex_image(path):
    """Rebuild the zone Salsa20 key from a decrypted XEX image (PE, as written by xexdump).

    TU18 evidence (default_mp): sub_822AA5D8 handles the zone auth header (it compares "PHEEBs71" at 0x822AA6E8).
    Mixed into its code are li/stb pairs that write 32 bytes to sp+0x50. At 0x822AA870-0x822AA880 it calls
    memcpy(0x82CBC578 + 0x4948 = 0x82CC0EC0, sp+0x50, 0x20). The Salsa20 helper sub_822AA0D8 passes 0x82CC0EC0 to
    keysetup (0x822CC7C8). Nothing here is hard-coded to TU18: the function is found through its "PHEEBs71"
    reference, and the bytes by emulating its immediate stores. Returns (key, description)."""
    d = open(path, "rb").read()
    if d[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE image")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    code = []
    for i in range(nsec):
        sh = pe + 24 + opt + 40 * i
        vsize, va = struct.unpack_from("<2I", d, sh + 8)
        if struct.unpack_from("<I", d, sh + 36)[0] & 0x20:
            code.append((va, va + vsize))

    def w(off):
        return struct.unpack_from(">I", d, off)[0]

    anchor = d.find(b"PHEEBs71\0")
    if anchor < 0:
        raise ValueError(f"{path}: no PHEEBs71 string")
    target = base + anchor
    hi = ((target + 0x8000) >> 16) & 0xFFFF
    refs = []
    for lo_off, hi_off in code:
        for k, (v,) in enumerate(struct.iter_unpack(">I", d[lo_off:hi_off])):
            if (v & 0xFC1FFFFF) != (0x3C000000 | hi):
                continue
            off = lo_off + 4 * k
            rd = (v >> 21) & 31
            for j in range(1, 12):
                u = w(off + 4 * j)
                if u >> 26 == 14 and (u >> 16) & 31 == rd and ((hi << 16) + _s16(u & 0xFFFF)) & 0xFFFFFFFF == target:
                    refs.append(off)
                    break
    for ref in refs:
        start = ref
        while start > 0 and w(start) != 0x7D8802A6:  # mflr r12
            start -= 4
        end = min(start + 0x2000, len(d))
        regs, stack, recent = {}, {}, []
        for off in range(start, end, 4):
            u = w(off)
            op, rt, ra, imm = u >> 26, (u >> 21) & 31, (u >> 16) & 31, u & 0xFFFF
            if off > start and u == 0x7D8802A6:
                break
            if op == 14:  # addi / li
                if ra == 0:
                    regs[rt] = _s16(imm) & 0xFFFFFFFF
                    recent.append(("li", rt, _s16(imm)))
                elif ra == 1:
                    regs.pop(rt, None)
                    recent.append(("sp", rt, _s16(imm)))
                elif ra in regs:
                    regs[rt] = (regs[ra] + _s16(imm)) & 0xFFFFFFFF
                else:
                    regs.pop(rt, None)
            elif op == 15:  # lis
                if ra == 0:
                    regs[rt] = (imm << 16) & 0xFFFFFFFF
                else:
                    regs.pop(rt, None)
            elif op == 38 and ra == 1 and rt in regs:  # stb rX, d(r1)
                stack[_s16(imm)] = regs[rt] & 0xFF
            elif op == 18 and (u & 1):  # bl: memcpy(dst, sp+D, 0x20)?
                src = [x[2] for x in recent[-8:] if x[0] == "sp" and x[1] == 4]
                size = [x[2] for x in recent[-8:] if x[0] == "li" and x[1] == 5]
                if src and size and size[-1] == 0x20 and all(src[-1] + i in stack for i in range(32)):
                    key = bytes(stack[src[-1] + i] for i in range(32))
                    return key, (f"rebuilt from {os.path.basename(path)}: immediates in sub_{base + start:08X}, "
                                 f"memcpy at 0x{base + off:08X}")
                for r in range(3, 13):
                    regs.pop(r, None)
                recent.clear()
            elif op in (3, 10, 11, 16, 17, 18, 19, 36, 37, 38, 39, 44, 45, 52, 53, 54, 55, 62):
                pass  # compares, branches and stores write no GPR (update forms are not used here)
            elif op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30):
                regs.pop(ra, None)  # rotates and logical immediates write rA
            elif op == 31:
                xo = (u >> 1) & 0x3FF
                if xo == 444 and rt == (u >> 11) & 31 and rt in regs:  # mr rA, rS
                    regs[ra] = regs[rt]
                elif xo in (0, 32, 151, 183, 215, 247, 407, 439, 663, 695, 727, 759, 149, 181, 231, 487):
                    pass  # cmp/cmpl and indexed stores
                elif xo in (24, 26, 27, 28, 58, 60, 124, 284, 316, 412, 444, 476, 536, 539, 792, 794, 824,
                            826, 827, 922, 954, 986):
                    regs.pop(ra, None)  # logical/shift ops write rA
                else:
                    regs.pop(rt, None)
            else:
                regs.pop(rt, None)
    raise ValueError(f"{path}: key builder not found ({len(refs)} PHEEBs71 references)")


def load_key(key_file, xex_image):
    """Returns (key, description of where it came from)."""
    if key_file:
        raw = open(key_file, "rb").read()
        if len(raw) == 32:
            return raw, f"--key-file {key_file}"
        hexes = re.findall(rb"0x([0-9A-Fa-f]{2})", raw) or re.findall(rb"([0-9A-Fa-f]{2})", raw)
        key, where = bytes(int(h, 16) for h in hexes), f"--key-file {key_file}"
    elif xex_image or os.path.exists(DEFAULT_IMAGE):
        key, where = key_from_xex_image(xex_image or DEFAULT_IMAGE)
    else:
        if not os.path.exists(OAT_CONSTANTS):
            sys.exit(f"no key: pass --xex-image or --key-file, or clone OpenAssetTools into "
                     f"{os.path.dirname(OAT_CONSTANTS)}")
        src = open(OAT_CONSTANTS, encoding="utf-8").read()
        m = re.search(r"SALSA20_KEY_TREYARCH_XENON\[\]\s*\{([^}]*)\}", src)
        if not m:
            sys.exit("SALSA20_KEY_TREYARCH_XENON not found in " + OAT_CONSTANTS)
        key = bytes(int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
        where = "OAT clone (refs/OpenAssetTools)"
    if len(key) != 32:
        sys.exit(f"key must be 32 bytes, got {len(key)}")
    return key, where


# ---------------------------------------------------------------------------------------------------------------
# Salsa20/20, 256-bit key, 64-bit IV, block counter starts at 0 for every XChunk (OAT thirdparty/salsa20.c)

M32 = 0xFFFFFFFF


def salsa20_keystream(key, iv, nbytes):
    k = struct.unpack("<8I", key)
    n = struct.unpack("<2I", iv)
    c = struct.unpack("<4I", b"expand 32-byte k")
    out = bytearray()
    nblocks = (nbytes + 63) // 64
    for ctr in range(nblocks):
        j = (c[0], k[0], k[1], k[2], k[3], c[1], n[0], n[1],
             ctr & M32, ctr >> 32, c[2], k[4], k[5], k[6], k[7], c[3])
        x0, x1, x2, x3, x4, x5, x6, x7, x8, x9, x10, x11, x12, x13, x14, x15 = j
        for _ in range(10):
            t = (x0 + x12) & M32; x4 ^= ((t << 7) | (t >> 25)) & M32
            t = (x4 + x0) & M32; x8 ^= ((t << 9) | (t >> 23)) & M32
            t = (x8 + x4) & M32; x12 ^= ((t << 13) | (t >> 19)) & M32
            t = (x12 + x8) & M32; x0 ^= ((t << 18) | (t >> 14)) & M32
            t = (x5 + x1) & M32; x9 ^= ((t << 7) | (t >> 25)) & M32
            t = (x9 + x5) & M32; x13 ^= ((t << 9) | (t >> 23)) & M32
            t = (x13 + x9) & M32; x1 ^= ((t << 13) | (t >> 19)) & M32
            t = (x1 + x13) & M32; x5 ^= ((t << 18) | (t >> 14)) & M32
            t = (x10 + x6) & M32; x14 ^= ((t << 7) | (t >> 25)) & M32
            t = (x14 + x10) & M32; x2 ^= ((t << 9) | (t >> 23)) & M32
            t = (x2 + x14) & M32; x6 ^= ((t << 13) | (t >> 19)) & M32
            t = (x6 + x2) & M32; x10 ^= ((t << 18) | (t >> 14)) & M32
            t = (x15 + x11) & M32; x3 ^= ((t << 7) | (t >> 25)) & M32
            t = (x3 + x15) & M32; x7 ^= ((t << 9) | (t >> 23)) & M32
            t = (x7 + x3) & M32; x11 ^= ((t << 13) | (t >> 19)) & M32
            t = (x11 + x7) & M32; x15 ^= ((t << 18) | (t >> 14)) & M32
            t = (x0 + x3) & M32; x1 ^= ((t << 7) | (t >> 25)) & M32
            t = (x1 + x0) & M32; x2 ^= ((t << 9) | (t >> 23)) & M32
            t = (x2 + x1) & M32; x3 ^= ((t << 13) | (t >> 19)) & M32
            t = (x3 + x2) & M32; x0 ^= ((t << 18) | (t >> 14)) & M32
            t = (x5 + x4) & M32; x6 ^= ((t << 7) | (t >> 25)) & M32
            t = (x6 + x5) & M32; x7 ^= ((t << 9) | (t >> 23)) & M32
            t = (x7 + x6) & M32; x4 ^= ((t << 13) | (t >> 19)) & M32
            t = (x4 + x7) & M32; x5 ^= ((t << 18) | (t >> 14)) & M32
            t = (x10 + x9) & M32; x11 ^= ((t << 7) | (t >> 25)) & M32
            t = (x11 + x10) & M32; x8 ^= ((t << 9) | (t >> 23)) & M32
            t = (x8 + x11) & M32; x9 ^= ((t << 13) | (t >> 19)) & M32
            t = (x9 + x8) & M32; x10 ^= ((t << 18) | (t >> 14)) & M32
            t = (x15 + x14) & M32; x12 ^= ((t << 7) | (t >> 25)) & M32
            t = (x12 + x15) & M32; x13 ^= ((t << 9) | (t >> 23)) & M32
            t = (x13 + x12) & M32; x14 ^= ((t << 13) | (t >> 19)) & M32
            t = (x14 + x13) & M32; x15 ^= ((t << 18) | (t >> 14)) & M32
        out += struct.pack("<16I", (x0 + j[0]) & M32, (x1 + j[1]) & M32, (x2 + j[2]) & M32, (x3 + j[3]) & M32,
                           (x4 + j[4]) & M32, (x5 + j[5]) & M32, (x6 + j[6]) & M32, (x7 + j[7]) & M32,
                           (x8 + j[8]) & M32, (x9 + j[9]) & M32, (x10 + j[10]) & M32, (x11 + j[11]) & M32,
                           (x12 + j[12]) & M32, (x13 + j[13]) & M32, (x14 + j[14]) & M32, (x15 + j[15]) & M32)
    return bytes(out[:nbytes])


def xor_bytes(a, b):
    n = len(a)
    return (int.from_bytes(a, "little") ^ int.from_bytes(b[:n], "little")).to_bytes(n, "little")


def decrypt_stream(job):
    """Decrypt the XChunks of one stream in order. The IV of each chunk is the first 8 bytes of the stream's current
    hash block; after each chunk the next hash block is XORed with SHA-1 of the decrypted chunk."""
    key, zone_name, stream, chunks = job
    name = zone_name.encode("latin-1")[:31]
    row = STREAM_COUNT * 20
    hashes = []
    for idx in range(BLOCK_HASHES):
        base = idx * row + stream * 20
        hashes.append(bytearray(name[((base + b) // 4) % len(name)] for b in range(20)))
    idx = 0
    out = []
    for chunk in chunks:
        plain = xor_bytes(chunk, salsa20_keystream(key, bytes(hashes[idx][:8]), len(chunk)))
        digest = hashlib.sha1(plain).digest()
        idx = (idx + 1) % BLOCK_HASHES
        nxt = hashes[idx]
        for b in range(20):
            nxt[b] ^= digest[b]
        out.append(plain)
    return out


# ---------------------------------------------------------------------------------------------------------------
# LZX (window 2^17), port of OAT thirdparty/lzx/lzx.c with XMemDecompress framing


class LzxError(Exception):
    pass


EXTRA_BITS = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
              12, 12, 13, 13, 14, 14, 15, 15, 16, 16] + [17] * 15
POSITION_BASE = []
_p = 0
for _e in EXTRA_BITS:
    POSITION_BASE.append(_p)
    _p += 1 << _e


def build_table(lengths, nsyms):
    """Canonical Huffman decode table: index = next maxlen bits, value = (symbol << 5) | code length."""
    maxlen = 0
    for i in range(nsyms):
        if lengths[i] > maxlen:
            maxlen = lengths[i]
    if maxlen == 0:
        return 0, None
    size = 1 << maxlen
    table = [0] * size
    pos = 0
    for length in range(1, maxlen + 1):
        fill = 1 << (maxlen - length)
        for sym in range(nsyms):
            if lengths[sym] == length:
                if pos + fill > size:
                    raise LzxError("huffman table overrun")
                table[pos:pos + fill] = [(sym << 5) | length] * fill
                pos += fill
    if pos != size:
        # like make_decode_table in lzx.c: an incomplete code is an error unless every length is zero
        if pos == 0:
            return 0, None
        raise LzxError("incomplete huffman table")
    return maxlen, table


class Lzx:
    NUM_CHARS = 256
    WINDOW_BITS = 17

    def __init__(self):
        self.window_size = 1 << self.WINDOW_BITS
        self.window = bytearray(self.window_size)
        self.main_elements = self.NUM_CHARS + ((self.WINDOW_BITS << 1) << 3)
        self.reset()

    def reset(self):
        self.r0 = self.r1 = self.r2 = 1
        self.header_read = False
        self.frames_read = 0
        self.block_remaining = 0
        self.block_length = 0
        self.block_type = 0
        self.intel_curpos = 0
        self.intel_started = False
        self.intel_filesize = 0
        self.window_posn = 0
        self.main_len = [0] * (self.main_elements + 64)
        self.length_len = [0] * (250 + 64)
        self.main_tbl = self.length_tbl = self.aligned_tbl = (0, None)

    def decompress(self, data, outlen):
        src = bytes(data) + b"\0" * 16
        endinp = len(data)
        ip = 0
        bitbuf = 0
        bitsleft = 0
        window = self.window
        wsize = self.window_size
        posn = self.window_posn
        r0, r1, r2 = self.r0, self.r1, self.r2

        def ensure(n):
            nonlocal bitbuf, bitsleft, ip
            while bitsleft < n:
                bitbuf = (bitbuf << 16) | src[ip] | (src[ip + 1] << 8)
                bitsleft += 16
                ip += 2

        def read(n):
            nonlocal bitbuf, bitsleft
            if n == 0:
                return 0
            ensure(n)
            bitsleft -= n
            v = bitbuf >> bitsleft
            bitbuf &= (1 << bitsleft) - 1
            return v

        def huff(tbl):
            nonlocal bitbuf, bitsleft
            maxlen, table = tbl
            if table is None:
                raise LzxError("empty huffman tree used")
            ensure(maxlen)
            e = table[bitbuf >> (bitsleft - maxlen)]
            bitsleft -= e & 31
            bitbuf &= (1 << bitsleft) - 1
            return e >> 5

        def read_lens(lens, first, last):
            pre = [read(4) for _ in range(20)]
            ptbl = build_table(pre, 20)
            x = first
            while x < last:
                z = huff(ptbl)
                if z == 17:
                    y = read(4) + 4
                    lens[x:x + y] = [0] * y
                    x += y
                elif z == 18:
                    y = read(5) + 20
                    lens[x:x + y] = [0] * y
                    x += y
                elif z == 19:
                    y = read(1) + 4
                    z = huff(ptbl)
                    z = lens[x] - z
                    if z < 0:
                        z += 17
                    lens[x:x + y] = [z] * y
                    x += y
                else:
                    z = lens[x] - z
                    if z < 0:
                        z += 17
                    lens[x] = z
                    x += 1

        if not self.header_read:
            i = j = 0
            if read(1):
                i = read(16)
                j = read(16)
            self.intel_filesize = (i << 16) | j
            self.header_read = True

        togo = outlen
        while togo > 0:
            if self.block_remaining == 0:
                if self.block_type == 3:
                    # CAB LZX (and OAT's lzx.c) skips a pad byte after an odd-length uncompressed block. XMemCompress
                    # does not: so_zclassic_zm_transit.ff xchunk 283 has a 0x1205-byte stored block and the next block
                    # header (aligned, 0x6dbb bytes = the rest of the frame) starts right after it.
                    bitbuf = bitsleft = 0
                self.block_type = read(3)
                i = read(16)
                j = read(8)
                self.block_remaining = self.block_length = (i << 8) | j
                bt = self.block_type
                if bt == 2:
                    self.aligned_tbl = build_table([read(3) for _ in range(8)], 8)
                if bt in (1, 2):
                    read_lens(self.main_len, 0, 256)
                    read_lens(self.main_len, 256, self.main_elements)
                    self.main_tbl = build_table(self.main_len, self.main_elements)
                    if self.main_len[0xE8]:
                        self.intel_started = True
                    read_lens(self.length_len, 0, 249)
                    self.length_tbl = build_table(self.length_len, 250)
                elif bt == 3:
                    self.intel_started = True
                    ensure(16)
                    if bitsleft > 16:
                        ip -= 2
                    r0, r1, r2 = struct.unpack_from("<3I", src, ip)
                    ip += 12
                    bitbuf = bitsleft = 0
                else:
                    raise LzxError(f"bad block type {bt}")

            if ip > endinp and (ip > endinp + 2 or bitsleft < 16):
                raise LzxError("input overrun")

            while self.block_remaining > 0 and togo > 0:
                this_run = min(self.block_remaining, togo)
                togo -= this_run
                self.block_remaining -= this_run
                posn &= wsize - 1
                if posn + this_run > wsize:
                    raise LzxError("run straddles window end")
                bt = self.block_type
                if bt == 3:
                    if ip + this_run > endinp:
                        raise LzxError("uncompressed run past input")
                    window[posn:posn + this_run] = src[ip:ip + this_run]
                    ip += this_run
                    posn += this_run
                    continue
                main_tbl = self.main_tbl
                length_tbl = self.length_tbl
                aligned_tbl = self.aligned_tbl
                aligned = bt == 2
                while this_run > 0:
                    sym = huff(main_tbl)
                    if sym < 256:
                        window[posn] = sym
                        posn += 1
                        this_run -= 1
                        continue
                    sym -= 256
                    mlen = sym & 7
                    if mlen == 7:
                        mlen += huff(length_tbl)
                    mlen += 2
                    slot = sym >> 3
                    if slot > 2:
                        extra = EXTRA_BITS[slot]
                        if aligned:
                            off = POSITION_BASE[slot] - 2
                            if extra > 3:
                                off += read(extra - 3) << 3
                                off += huff(aligned_tbl)
                            elif extra == 3:
                                off += huff(aligned_tbl)
                            elif extra > 0:
                                off += read(extra)
                            else:
                                off = 1
                        else:
                            if slot != 3:
                                off = POSITION_BASE[slot] - 2 + read(extra)
                            else:
                                off = 1
                        r2, r1, r0 = r1, r0, off
                    elif slot == 0:
                        off = r0
                    elif slot == 1:
                        off = r1
                        r1, r0 = r0, off
                    else:
                        off = r2
                        r2, r0 = r0, off
                    dst = posn
                    s = dst - off
                    posn += mlen
                    if posn > wsize:
                        raise LzxError("match past window end")
                    this_run -= mlen
                    if s < 0:
                        while s < 0 and mlen > 0:
                            window[dst] = window[s + wsize]
                            dst += 1
                            s += 1
                            mlen -= 1
                    if mlen > 0:
                        if off >= mlen:
                            window[dst:dst + mlen] = window[s:s + mlen]
                        else:
                            pat = bytes(window[s:dst])
                            window[dst:dst + mlen] = (pat * (mlen // len(pat) + 1))[:mlen]

        if togo != 0:
            raise LzxError("output underrun")
        end = posn if posn else wsize
        out = bytearray(window[end - outlen:end])
        self.window_posn = posn
        self.r0, self.r1, self.r2 = r0, r1, r2

        if self.frames_read < 32768 and self.intel_filesize != 0:
            if outlen <= 6 or not self.intel_started:
                self.intel_curpos += outlen
            else:
                curpos = self.intel_curpos
                filesize = self.intel_filesize
                self.intel_curpos = curpos + outlen
                p = 0
                dataend = outlen - 10
                while p < dataend:
                    if out[p] != 0xE8:
                        p += 1
                        curpos += 1
                        continue
                    p += 1
                    abs_off = struct.unpack_from("<i", out, p)[0]
                    if -curpos <= abs_off < filesize:
                        rel = abs_off - curpos if abs_off >= 0 else abs_off + filesize
                        struct.pack_into("<i", out, p, rel)
                    p += 4
                    curpos += 5
        self.frames_read += 1
        return bytes(out)


def xmem_decompress(chunk):
    """One XChunk: a run of LZX frames, each prefixed by [srcHi srcLo] (0x8000 output) or
    [FF dstHi dstLo srcHi srcLo] (smaller output, followed by a 5-byte suffix the game skips)."""
    lzx = Lzx()
    out = []
    i = 0
    n = len(chunk)
    while i < n:
        hi = chunk[i]
        i += 1
        if hi == 0xFF:
            if n - i < 4:
                raise LzxError("short frame header")
            dst = (chunk[i] << 8) | chunk[i + 1]
            src = (chunk[i + 2] << 8) | chunk[i + 3]
            i += 4
            suffix = 5
        else:
            if n - i < 1:
                raise LzxError("short frame header")
            dst = 0x8000
            src = (hi << 8) | chunk[i]
            i += 1
            suffix = 0
        if src == 0 or dst == 0:
            raise LzxError("zero-size frame")
        if i + src + suffix > n:
            raise LzxError("frame past chunk end")
        out.append(lzx.decompress(chunk[i:i + src], dst))
        i += src + suffix
    return b"".join(out)


def decompress_job(chunk):
    try:
        return xmem_decompress(chunk)
    except LzxError as e:
        return e


# ---------------------------------------------------------------------------------------------------------------
# container


def read_container(path):
    data = open(path, "rb").read()
    magic = data[:8]
    version = struct.unpack_from(">I", data, 8)[0]
    info = {"magic": magic.decode("latin-1"), "version": version, "file_size": len(data)}
    if magic not in (b"TAffx100", b"TAff0100") or version != ZONE_VERSION_XENON:
        raise ValueError(f"not a BO2 Xbox 360 zone: magic={magic!r} version={version}")
    auth = data[HEADER_SIZE:HEADER_SIZE + AUTH_SIZE]
    if auth[:8] != b"PHEEBs71":
        raise ValueError(f"missing PHEEBs71 auth header: {auth[:8]!r}")
    info["auth_flags"] = struct.unpack_from(">I", auth, 8)[0]
    info["auth_name"] = auth[12:44].split(b"\0")[0].decode("latin-1")
    info["signature_sha1"] = hashlib.sha1(auth[44:300]).hexdigest()

    pos = HEADER_SIZE + AUTH_SIZE
    vb = pos % VANILLA_BUFFER
    chunks = []
    while True:
        if vb + 4 > VANILLA_BUFFER:
            pos += VANILLA_BUFFER - vb
            vb = 0
        vb = (vb + 4) % VANILLA_BUFFER
        if pos + 4 > len(data):
            break
        size = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if size == 0:
            break
        if size > XCHUNK_SIZE or pos + size > len(data):
            raise ValueError(f"bad xchunk size 0x{size:x} at 0x{pos - 4:x}")
        chunks.append(data[pos:pos + size])
        pos += size
        vb = (vb + size) % VANILLA_BUFFER
    info["xchunks"] = len(chunks)
    info["xchunk_end"] = pos
    info["compression"] = "lzx" if magic == b"TAffx100" else "deflate"
    return info, chunks


def inflate_job(chunk):
    import zlib
    try:
        return zlib.decompress(chunk, -15)
    except zlib.error as e:
        return LzxError(str(e))


def decode_zone(path, key, pool):
    info, chunks = read_container(path)
    zone_name = os.path.splitext(os.path.basename(path))[0]
    if info["auth_name"] != zone_name:
        print(f"  warning: auth header names '{info['auth_name']}', file is '{zone_name}' (IV uses the file name)")
    jobs = [(key, zone_name, s, chunks[s::STREAM_COUNT]) for s in range(STREAM_COUNT)]
    per_stream = pool.map(decrypt_stream, jobs)
    plain = [None] * len(chunks)
    for s, lst in enumerate(per_stream):
        plain[s::STREAM_COUNT] = lst
    fn = decompress_job if info["compression"] == "lzx" else inflate_job
    parts = pool.map(fn, plain, chunksize=8)
    for i, p in enumerate(parts):
        if isinstance(p, Exception):
            raise ValueError(f"xchunk {i}: {p}")
    return info, b"".join(parts)


# ---------------------------------------------------------------------------------------------------------------
# zone content


def parse_xfile(zone):
    size, external = struct.unpack_from(">2I", zone, 0)
    blocks = struct.unpack_from(">8I", zone, 8)
    return {"size": size, "external_size": external, "blocks": dict(zip(BLOCK_NAMES, blocks))}


# Zero-filled space at the start of each shader's cached part, ahead of the XDK container. Measured on
# code_post_gfx_zm/common_zm/zm_transit (the struct's cachedPartSize == RUNTIME_HDR + container virtualSize).
# Inference: room for the D3D shader object the game builds in place at load time.
RUNTIME_HDR = {1: 0x368, 0: 0x28}  # vertex, pixel


def printable(b):
    return bool(b) and all(0x20 <= x < 0x7F for x in b)


def find_shaders(zone):
    """Find XDK shader containers (big-endian 0x102A1100 pixel / 0x102A1101 vertex, the format XenosRecomp reads) and
    the zone records around them. A material shader record in the zone stream is laid out as
        pixel : ptr, ptr, ptr, u16 cachedSize, u16 physicalSize                      (16 bytes)
        vertex: ptr, ptr, ptr, ptr, u16 cachedSize, u16 physicalSize, u32 0           (24 bytes)
        [name\\0 when the name pointer is -1; absent when it points at a string loaded earlier]
        physical part (microcode, physicalSize bytes)
        cached part (RUNTIME_HDR zeros + container, cachedSize bytes)
    so the container's own microcode is not next to it; the blob XenosRecomp wants is container || physical part.
    One other form exists ("shaders/gpu_transfer/gpu_transfer.xpu"): u32 size, name\\0, container || physical.
    Left out on purpose: the two vertex shaders of that form, gpu_transfer.xvu and gpu_transfer_128in256.xvu (flags
    0x102A1111). They write memory (memexport) for texture streaming and draw nothing; the native renderer runs
    their draws on the CPU, and only for draws whose vertex shader is not in the library."""
    shaders = []
    for m in re.finditer(rb"\x10\x2A\x11[\x00\x01]", zone):
        c = m.start()
        if c + 0x24 > len(zone):
            continue
        flags, virt, phys, _, _, _, soff, f1c, f20 = struct.unpack_from(">9I", zone, c)
        vs = flags & 1
        rec = {"container": c, "flags": flags, "kind": "vs" if vs else "ps", "virtual": virt, "physical": phys,
               "name": None, "record": None, "form": None, "ok": False}
        shaders.append(rec)
        if f1c or f20 or virt < 0x24 or soff >= virt or c + virt > len(zone):
            rec["why"] = "container header fields out of range"
            continue
        hdr = RUNTIME_HDR[vs]
        cached = c - hdr
        start = cached - phys
        sizes = struct.pack(">HH", hdr + virt, phys) + (b"\0\0\0\0" if vs else b"")
        rlen = 24 if vs else 16
        if start >= rlen and not any(zone[cached:c]):
            if zone[start - len(sizes):start] == sizes:
                rec.update(record=start - rlen, form="split, name by reference", ok=True)
            else:
                lo = max(0, start - 300)
                k = zone.rfind(sizes, lo, start)
                while k >= 0:
                    name = zone[k + len(sizes):start - 1]
                    if zone[start - 1] == 0 and printable(name):
                        rec.update(name=name.decode("ascii"), record=k + len(sizes) - rlen, form="split", ok=True)
                        break
                    k = zone.rfind(sizes, lo, k)
        if rec["ok"]:
            rec["blob"] = zone[c:c + virt] + zone[start:cached]
        else:
            # contiguous form, a RawFile-shaped record: ptr name (-1), u32 len (virtual + physical), ptr buffer (-1),
            # name\0, container || physical
            nul = c - 1
            k = nul - 1
            while k >= 0 and 0x20 <= zone[k] < 0x7F:
                k -= 1
            name = zone[k + 1:nul]
            if zone[nul] == 0 and printable(name) and k >= 11 and zone[k - 3:k + 1] == b"\xff" * 4 and \
                    struct.unpack_from(">I", zone, k - 7)[0] == virt + phys:
                rec.update(name=name.decode("ascii"), record=k - 11, form="contiguous (rawfile)", ok=True)
                rec["blob"] = zone[c:c + virt + phys]
            else:
                rec["why"] = "no size record before the microcode and not a contiguous blob"
                continue
        rec["sha1"] = hashlib.sha1(rec["blob"]).hexdigest()
    return shaders


def find_image_shaders(path):
    """Find the shader containers in a decrypted XEX image (PE, as written by xexdump): shaders that the game's code
    creates itself, with D3D's CreateVertexShader/CreatePixelShader, from microcode linked into the executable. No
    fastfile holds them. In TU18's default_mp image these are six of BO2's own (compiler 2.0.21119; sub_824E3108
    creates them at renderer start-up: five pixel shaders reading texIn/texIn0/texIn1 and one vertex shader) and the
    XDK libraries' (compiler 2.0.9407), which this game never creates. In the image a container is followed by its
    physical part (the microcode), so the blob XenosRecomp wants, container || physical, is contiguous.
    Returns records shaped as find_shaders' (record = the container's virtual address)."""
    d = open(path, "rb").read()
    if d[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE image")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    base = struct.unpack_from("<I", d, pe + 24 + 28)[0]
    shaders = []
    for m in re.finditer(rb"\x10\x2A\x11[\x00\x01]", d):
        c = m.start()
        if c + 0x24 > len(d):
            continue
        flags, virt, phys, _, _, _, soff, f1c, f20 = struct.unpack_from(">9I", d, c)
        if f1c or f20 or virt < 0x24 or soff >= virt or not phys or phys > 0x10000 or c + virt + phys > len(d):
            continue
        blob = d[c:c + virt + phys]
        shaders.append({"container": c, "flags": flags, "kind": "vs" if flags & 1 else "ps", "virtual": virt,
                        "physical": phys, "name": None, "record": base + c, "form": "executable image", "ok": True,
                        "blob": blob, "sha1": hashlib.sha1(blob).hexdigest()})
    return shaders


def is_inside(path, root):
    path, root = os.path.normcase(os.path.abspath(path)), os.path.normcase(os.path.abspath(root))
    return path == root or path.startswith(root + os.sep)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("ff", nargs="*")
    ap.add_argument("--out", default=os.path.join(tempfile.gettempdir(), "bo2_ff"),
                    help="scratch folder for decompressed zones (outside the repo)")
    ap.add_argument("--key-file", help="32-byte key file (raw or hex text)")
    ap.add_argument("--xex-image", help="decrypted XEX image to rebuild the key from "
                                        "(default: work/dump_tu18/default_mp.image.bin if present)")
    ap.add_argument("--image-shaders", action="store_true",
                    help="also list (and with --dump write) the shaders linked into the XEX image itself "
                         "(--xex-image, else the default), which no fastfile holds")
    ap.add_argument("--list", type=int, default=12, help="how many shader blobs to print per zone")
    ap.add_argument("--dump", action="store_true", help="also write each shader blob to <out>/<zone>_shaders/")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--no-cache", action="store_true")
    args = ap.parse_args()

    if is_inside(args.out, REPO):
        sys.exit(f"refusing to write game-derived data inside the repo: {args.out}")
    os.makedirs(args.out, exist_ok=True)
    if args.image_shaders:
        image = args.xex_image or DEFAULT_IMAGE
        print(f"== {image}")
        report_shaders(None, os.path.splitext(os.path.basename(image))[0], args, find_image_shaders(image))
    if not args.ff:
        return
    key, where = load_key(args.key_file, args.xex_image)
    print(f"key: {where}; sha1 {hashlib.sha1(key).hexdigest()[:12]}")

    with multiprocessing.Pool(max(args.jobs, STREAM_COUNT)) as pool:
        for path in args.ff:
            print(f"== {path}")
            zone_name = os.path.splitext(os.path.basename(path))[0]
            zone_path = os.path.join(args.out, zone_name + ".zone")
            st = os.stat(path)
            stamp = f"{st.st_size}:{int(st.st_mtime)}"
            stamp_path = zone_path + ".src"
            if (not args.no_cache and os.path.exists(zone_path) and os.path.exists(stamp_path)
                    and open(stamp_path).read() == stamp):
                info, _ = read_container(path)
                zone = open(zone_path, "rb").read()
                print(f"  (cached {zone_path})")
            else:
                info, zone = decode_zone(path, key, pool)
                with open(zone_path, "wb") as f:
                    f.write(zone)
                with open(stamp_path, "w") as f:
                    f.write(stamp)
            print(f"  magic {info['magic']} version {info['version']} compression {info['compression']} "
                  f"auth '{info['auth_name']}' flags {info['auth_flags']}")
            print(f"  {info['xchunks']} xchunks, container ends 0x{info['xchunk_end']:x} of 0x{info['file_size']:x}")
            xf = parse_xfile(zone)
            print(f"  zone 0x{len(zone):x} bytes -> {zone_path}")
            check = "ok" if xf["size"] == len(zone) - 0x28 else "MISMATCH (bad key or decoder bug)"
            print(f"  XFile size 0x{xf['size']:x} ({check}) external 0x{xf['external_size']:x} blocks "
                  + " ".join(f"{k}=0x{v:x}" for k, v in xf["blocks"].items() if v))
            report_shaders(zone, zone_name, args)


def report_shaders(zone, zone_name, args, shaders=None):
    if shaders is None:
        shaders = find_shaders(zone)
    good = [s for s in shaders if s["ok"]]
    print(f"  shader containers (0x102A110x): {len(shaders)}, matched to a zone record: {len(good)}")
    for kind in ("vs", "ps"):
        ks = [s for s in good if s["kind"] == kind]
        uniq = {s["sha1"] for s in ks}
        total = sum(len(s["blob"]) for s in ks)
        sizes = sorted(len(s["blob"]) for s in ks)
        span = f", blob 0x{sizes[0]:x}..0x{sizes[-1]:x}" if sizes else ""
        print(f"    {kind}: {len(ks)} records, {len(uniq)} unique blobs, 0x{total:x} bytes{span}")
    forms = {}
    for s in good:
        forms[s["form"]] = forms.get(s["form"], 0) + 1
    print("    record forms: " + ", ".join(f"{k} {v}" for k, v in sorted(forms.items())))
    for s in shaders:
        if not s["ok"]:
            print(f"    unmatched {s['kind']} container at 0x{s['container']:x} "
                  f"(virtual 0x{s['virtual']:x} physical 0x{s['physical']:x}): {s['why']}")
    for s in good[:args.list]:
        print(f"    {s['kind']} rec 0x{s['record']:08x} container 0x{s['container']:08x} virtual 0x{s['virtual']:x} "
              f"physical 0x{s['physical']:x} sha1 {s['sha1'][:16]} {s['name']}")
    if args.dump:
        out = os.path.join(args.out, zone_name + "_shaders")
        os.makedirs(out, exist_ok=True)
        seen = set()
        with open(os.path.join(out, "index.tsv"), "w", encoding="utf-8") as idx:
            idx.write("file\tkind\tname\trecord\tcontainer\tvirtual\tphysical\tsha1\n")
            for s in good:
                fn = f"{s['kind']}_{s['sha1'][:16]}.bin"
                if s["sha1"] not in seen:
                    seen.add(s["sha1"])
                    with open(os.path.join(out, fn), "wb") as f:
                        f.write(s["blob"])
                idx.write(f"{fn}\t{s['kind']}\t{s['name']}\t0x{s['record']:x}\t0x{s['container']:x}\t"
                          f"0x{s['virtual']:x}\t0x{s['physical']:x}\t{s['sha1']}\n")
        print(f"  wrote {len(seen)} unique blobs to {out}")


if __name__ == "__main__":
    main()
