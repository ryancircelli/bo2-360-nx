"""List or extract files from an Xbox 360 STFS package (CON/LIVE/PIRS), read-only.

Block math follows ReXGlue's stfs_container_device.cpp (Xenia-derived).

usage: python -I stfs_extract.py <package> [--out DIR] [--only NAME ...]
"""
import argparse
import os
import struct
import sys

BLOCK = 0x1000
PER_LEVEL = (170, 170 * 170, 170 * 170 * 170)
END_OF_CHAIN = 0xFFFFFF


def u24le(b):
    return b[0] | (b[1] << 8) | (b[2] << 16)


class Stfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(0x971A)
        self.magic = head[:4]
        if self.magic not in (b"CON ", b"LIVE", b"PIRS"):
            raise ValueError(f"not an STFS package: {self.magic!r}")
        self.header_size = struct.unpack_from(">I", head, 0x340)[0]
        self.content_type = struct.unpack_from(">I", head, 0x344)[0]
        self.title_id = struct.unpack_from(">I", head, 0x360)[0]
        self.display_name = head[0x411:0x511].decode("utf-16-be", "ignore").split("\0")[0]
        vd = head[0x379:0x379 + 0x24]
        self.flags = vd[2]
        self.read_only = bool(self.flags & 1)
        self.root_active = bool(self.flags & 2)
        self.ft_count = struct.unpack_from("<H", vd, 3)[0]
        self.ft_block = u24le(vd[5:8])
        self.total_blocks = struct.unpack_from(">I", vd, 0x1C)[0]
        self.bpht = 1 if self.read_only else 2
        self.step0 = PER_LEVEL[0] + self.bpht
        self.step1 = PER_LEVEL[1] + (PER_LEVEL[0] + 1) * self.bpht
        self.base = (self.header_size + BLOCK - 1) & ~(BLOCK - 1)
        self._tables = {}

    def block_offset(self, index):
        base, block = PER_LEVEL[0], index
        for _ in range(3):
            block += ((index + base) // base) * self.bpht
            if index < base:
                break
            base *= PER_LEVEL[0]
        return self.base + (block << 12)

    def _hash_block_number(self, index, level):
        if level == 0:
            if index < PER_LEVEL[0]:
                return 0
            block = (index // PER_LEVEL[0]) * self.step0
            block += ((index // PER_LEVEL[1]) + 1) * self.bpht
            return block if index < PER_LEVEL[1] else block + self.bpht
        if level == 1:
            if index < PER_LEVEL[1]:
                return self.step0
            return (index // PER_LEVEL[1]) * self.step1 + self.bpht
        return self.step1

    def _table(self, offset):
        if offset not in self._tables:
            self.f.seek(offset)
            self._tables[offset] = self.f.read(BLOCK)
        return self._tables[offset]

    def _hash_offset(self, index, level):
        return self.base + (self._hash_block_number(index, level) << 12)

    def next_block(self, index):
        secondary = 0
        if not self.read_only:
            secondary = BLOCK if self.root_active else 0
            if self.total_blocks > PER_LEVEL[0]:
                if self.total_blocks > PER_LEVEL[1]:
                    t2 = self._table(self._hash_offset(index, 2) + secondary)
                    rec = (index // PER_LEVEL[1]) % PER_LEVEL[0]
                    secondary = BLOCK if (t2[rec * 24 + 20] & 0x40) else 0
                t1 = self._table(self._hash_offset(index, 1) + secondary)
                rec = (index // PER_LEVEL[0]) % PER_LEVEL[0]
                secondary = BLOCK if (t1[rec * 24 + 20] & 0x40) else 0
        t0 = self._table(self._hash_offset(index, 0) + secondary)
        info = struct.unpack_from(">I", t0, (index % PER_LEVEL[0]) * 24 + 20)[0]
        return info & 0xFFFFFF

    def entries(self):
        out, block = [], self.ft_block
        seen = set()
        for _ in range(self.ft_count):
            if block in seen or block >= self.total_blocks:
                raise ValueError("invalid STFS file-table chain")
            seen.add(block)
            self.f.seek(self.block_offset(block))
            data = self.f.read(BLOCK)
            if len(data) != BLOCK:
                raise ValueError("truncated STFS file table")
            for i in range(BLOCK // 0x40):
                e = data[i * 0x40:(i + 1) * 0x40]
                if e[0] == 0:
                    break
                flags = e[0x28]
                out.append({
                    "name": e[: flags & 0x3F].decode("ascii", "replace"),
                    "dir": bool(flags & 0x80),
                    "blocks": u24le(e[0x2C:0x2F]),
                    "start": u24le(e[0x2F:0x32]),
                    "parent": struct.unpack_from(">H", e, 0x32)[0],
                    "size": struct.unpack_from(">I", e, 0x34)[0],
                })
            block = self.next_block(block)
            if block == END_OF_CHAIN:
                break
        for e in out:
            parts, p = [e["name"]], e["parent"]
            parents = set()
            while p != 0xFFFF:
                if p in parents or p >= len(out) or not out[p]["dir"]:
                    raise ValueError("invalid STFS parent directory")
                parents.add(p)
                parts.insert(0, out[p]["name"])
                p = out[p]["parent"]
            e["path"] = "/".join(parts)
        return out

    def read(self, entry):
        chunks, block, remaining = [], entry["start"], entry["size"]
        while remaining and block != END_OF_CHAIN:
            self.f.seek(self.block_offset(block))
            n = min(BLOCK, remaining)
            chunks.append(self.f.read(n))
            remaining -= n
            block = self.next_block(block)
        if remaining:
            raise IOError(f"{entry['path']}: chain ended {remaining} bytes early")
        return b"".join(chunks)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("package")
    ap.add_argument("--out")
    ap.add_argument("--only", nargs="*")
    a = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8")
    s = Stfs(a.package)
    print(f"{s.magic.decode()} '{s.display_name}' title={s.title_id:08X} type={s.content_type:08X} "
          f"read_only={s.read_only} blocks={s.total_blocks}")
    for e in s.entries():
        print(f"  {'<dir>' if e['dir'] else e['size']:>12}  {e['path']}")
        if a.out and not e["dir"] and (not a.only or e["name"] in a.only):
            dst = os.path.join(a.out, *e["path"].split("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(s.read(e))
            print(f"    -> {dst}")


if __name__ == "__main__":
    main()
