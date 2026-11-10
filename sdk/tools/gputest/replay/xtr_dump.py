#!/usr/bin/env python3
"""List the commands in a Xenia-format GPU trace (.xtr).

  xtr_dump.py TRACE [--packets] [--limit N]

Prints the header, a summary (packets per type 3 opcode, draws, swaps, memory
read bytes) and, with --packets, one line per command.
"""

import argparse
import collections
import struct
import sys

NAMES = ["PrimaryBufferStart", "PrimaryBufferEnd", "IndirectBufferStart", "IndirectBufferEnd",
         "PacketStart", "PacketEnd", "MemoryRead", "MemoryWrite", "EdramSnapshot", "Event",
         "Registers", "GammaRamp"]
DRAW_OPCODES = {0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2"}
OPCODES = {0x10: "NOP", 0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x2B: "IM_LOAD_IMMEDIATE",
           0x27: "IM_LOAD", 0x2F: "LOAD_ALU_CONSTANT", 0x3F: "INDIRECT_BUFFER", 0x64: "XE_SWAP",
           0x3C: "WAIT_REG_MEM", 0x2D: "SET_CONSTANT", 0x46: "EVENT_WRITE", 0x58: "EVENT_WRITE_SHD",
           0x3D: "MEM_WRITE", 0x48: "INVALIDATE_STATE", 0x44: "SET_SHADER_CONSTANTS",
           0x26: "WAIT_FOR_IDLE", 0x45: "INTERRUPT", 0x25: "SET_CONSTANT2"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--packets", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()
    data = open(args.trace, "rb").read()
    version, = struct.unpack_from("<I", data, 0)
    sha = data[4:44].rstrip(b"\0").decode("ascii", "replace")
    title, = struct.unpack_from("<I", data, 44)
    print(f"version {version} build '{sha}' title {title:08X} size {len(data)}")
    off = 48
    counts = collections.Counter()
    ops = collections.Counter()
    mem_bytes = 0
    mem_stored = 0
    printed = 0
    while off < len(data):
        t, = struct.unpack_from("<I", data, off)
        name = NAMES[t] if t < len(NAMES) else f"?{t}"
        counts[name] += 1
        line = None
        if t in (0, 2):
            base, count = struct.unpack_from("<II", data, off + 4)
            line = f"{name} base {base:08X} count {count}"
            off += 12
        elif t in (1, 3, 5):
            line = name
            off += 4
        elif t == 4:
            base, count = struct.unpack_from("<II", data, off + 4)
            words = struct.unpack_from(f">{count}I", data, off + 12)
            off += 12 + count * 4
            h = words[0] if words else 0
            ptype = h >> 30
            desc = f"type{ptype}"
            if ptype == 3:
                op = (h >> 8) & 0x7F
                desc = OPCODES.get(op, f"op{op:02X}")
                ops[desc] += 1
            elif ptype == 0:
                desc = f"type0 reg {h & 0x7FFF:04X} x{((h >> 16) & 0x3FFF) + 1}"
                ops["type0"] += 1
            else:
                ops[desc] += 1
            line = f"{name} {base:08X} {count} dw: {desc}"
        elif t in (6, 7):
            base, enc, elen, dlen = struct.unpack_from("<IIII", data, off + 4)
            off += 20 + elen
            if t == 6:
                mem_bytes += dlen
                mem_stored += elen
            enc_name = {0: "raw", 1: "snappy", 2: "zlib"}.get(enc, str(enc))
            line = f"{name} {base:08X} len {dlen} enc {enc_name} stored {elen}"
        elif t == 8:
            enc, elen, dlen = struct.unpack_from("<III", data, off + 4)
            off += 16 + elen
            line = name
        elif t == 9:
            ev, = struct.unpack_from("<I", data, off + 4)
            off += 8
            line = f"{name} {'swap' if ev == 0 else ev}"
        elif t == 10:
            first, count, cb, enc, elen = struct.unpack_from("<IIBxxxII", data, off + 4)
            off += 24 + elen
            line = f"{name} first {first:04X} count {count:X}"
        elif t == 11:
            rw, enc, elen = struct.unpack_from("<BxxxII", data, off + 4)
            off += 16 + elen
            line = name
        else:
            print(f"unknown command {t} at {off}")
            return 1
        if args.packets and (not args.limit or printed < args.limit):
            print(line)
            printed += 1
    print("commands:", dict(counts))
    print("packets:", dict(ops))
    print(f"draws {sum(v for k, v in ops.items() if k in DRAW_OPCODES.values())} "
          f"swaps {counts['Event']} memory read bytes {mem_bytes} (stored {mem_stored})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
