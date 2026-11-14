#!/usr/bin/env python3
"""Writes a small synthetic Xenos GPU trace (.xtr, Xenia trace format v1).

The trace needs no game: it carries hand-assembled Xenos vertex and pixel
shader ucode, a vertex buffer and PM4 packets that

  0. clear color (dark blue) and depth (1.0) through a resolve,
  1. draw a red triangle at z = 0.5,
  2. draw a green triangle at z = 0.25 (in front, overlapping 1),
  3. draw a blue triangle at z = 0.75 (behind 1 and 2, partly hidden),

on a 256x128 k_8_8_8_8 render target with a D24S8 depth buffer, followed by a
swap. It proves capture-format parsing, replay, per-draw readback and the
diff pipeline end to end. Pass --bug to make draw 3 use a different color,
which the diff tool must then report as the first divergent draw.

Usage: make_synthetic_trace.py out.xtr [--bug]
"""

import argparse
import struct

# Trace protocol (include/rex/graphics/trace/trace_protocol.h).
K_PRIMARY_START, K_PRIMARY_END = 0, 1
K_PACKET_START, K_PACKET_END = 4, 5
K_MEMORY_READ = 6
K_EVENT = 9
K_REGISTERS = 10
EVENT_SWAP = 0

# Register indices (include/rex/graphics/register_table.inc).
R = dict(
    RB_SURFACE_INFO=0x2000, RB_COLOR_INFO=0x2001, RB_DEPTH_INFO=0x2002,
    PA_SC_SCREEN_SCISSOR_TL=0x200E, PA_SC_SCREEN_SCISSOR_BR=0x200F,
    PA_SC_WINDOW_OFFSET=0x2080, PA_SC_WINDOW_SCISSOR_TL=0x2081,
    PA_SC_WINDOW_SCISSOR_BR=0x2082, VGT_MAX_VTX_INDX=0x2100,
    VGT_MIN_VTX_INDX=0x2101, VGT_INDX_OFFSET=0x2102, RB_COLOR_MASK=0x2104,
    PA_CL_VPORT_XSCALE=0x210F, PA_CL_VPORT_XOFFSET=0x2110,
    PA_CL_VPORT_YSCALE=0x2111, PA_CL_VPORT_YOFFSET=0x2112,
    PA_CL_VPORT_ZSCALE=0x2113, PA_CL_VPORT_ZOFFSET=0x2114,
    SQ_PROGRAM_CNTL=0x2180, SQ_CONTEXT_MISC=0x2181, RB_DEPTHCONTROL=0x2200,
    RB_BLENDCONTROL0=0x2201, RB_COLORCONTROL=0x2202, PA_CL_CLIP_CNTL=0x2204,
    PA_SU_SC_MODE_CNTL=0x2205, PA_CL_VTE_CNTL=0x2206, RB_MODECONTROL=0x2208,
    PA_SU_VTX_CNTL=0x2302, SQ_VS_CONST=0x2307, SQ_PS_CONST=0x2308,
    PA_SC_AA_MASK=0x2312, RB_COPY_CONTROL=0x2318, RB_COPY_DEST_BASE=0x2319,
    RB_COPY_DEST_PITCH=0x231A, RB_COPY_DEST_INFO=0x231B, RB_DEPTH_CLEAR=0x231D,
    RB_COLOR_CLEAR=0x231E, SHADER_CONSTANT_256_X=0x4400,
    SHADER_CONSTANT_FETCH_00_0=0x4800,
)
REGISTER_COUNT = 0x5003

PM4_IM_LOAD_IMMEDIATE = 0x2B
PM4_DRAW_INDX_2 = 0x36
PM4_XE_SWAP = 0x64

WIDTH, HEIGHT = 256, 128
PACKET_BASE = 0x00010000
VB_TRIS = 0x00100000
VB_CLEAR = 0x00101000
CLEAR_DEST = 0x00200000


def f2u(f):
    return struct.unpack("<I", struct.pack("<f", f))[0]


# ---------------------------------------------------------------------------
# Xenos ucode (include/rex/graphics/format/ucode.h)
# ---------------------------------------------------------------------------

def cf_exec(address, count, sequence, end):
    d0 = address | (count << 12) | (sequence << 16)
    d1 = (1 << 9) | ((2 if end else 1) << 12)  # predicate clean, kExec/kExecEnd
    return d0, d1


def cf_alloc(alloc_type, size=0):
    return size, (alloc_type << 9) | (12 << 12)


CF_NOP = (0, 0)


def pack_cf(cfs):
    assert len(cfs) % 2 == 0
    out = []
    for (a0, a1), (b0, b1) in zip(cfs[0::2], cfs[1::2]):
        out += [a0, (a1 & 0xFFFF) | ((b0 & 0xFFFF) << 16), (b0 >> 16) | ((b1 & 0xFFFF) << 16)]
    return out


def alu_export_mov(dest, src_reg, src_is_temp):
    """max dest.xyzw, src, src with export_data; scalar retain_prev, no write."""
    w0 = dest | (1 << 15) | (0xF << 16) | (50 << 26)
    w1 = 0  # identity swizzles, no negation, not predicated
    sel = 1 if src_is_temp else 0
    w2 = (src_reg << 8) | (src_reg << 16) | (2 << 24) | (sel << 30) | (sel << 31)
    return [w0, w1, w2]


def vfetch_float4(dst_reg, fetch_const, stride_dwords):
    w0 = (0 | (0 << 5) | (dst_reg << 12) | (1 << 19) | ((fetch_const // 3) << 20) |
          ((fetch_const % 3) << 25))
    dst_swiz = 0 | (1 << 3) | (2 << 6) | (3 << 9)
    w1 = dst_swiz | (38 << 16)  # VertexFormat::k_32_32_32_32_FLOAT
    w2 = stride_dwords
    return [w0, w1, w2]


def vertex_shader():
    cfs = [cf_exec(2, 1, 0b01, False), cf_alloc(1), cf_exec(3, 1, 0b00, True), CF_NOP]
    return pack_cf(cfs) + vfetch_float4(0, 0, 4) + alu_export_mov(62, 0, True)


def pixel_shader():
    cfs = [cf_alloc(2), cf_exec(1, 1, 0b00, True)]
    return pack_cf(cfs) + alu_export_mov(0, 0, False)


# ---------------------------------------------------------------------------
# PM4
# ---------------------------------------------------------------------------

def type0(reg, values):
    return [((len(values) - 1) << 16) | reg] + list(values)


def type3(opcode, payload):
    return [(3 << 30) | ((len(payload) - 1) << 16) | (opcode << 8)] + list(payload)


def draw_autoindex(prim_type, count):
    initiator = prim_type | (2 << 6) | (count << 16)  # SourceSelect::kAutoIndex
    return type3(PM4_DRAW_INDX_2, [initiator])


class Trace:
    def __init__(self):
        self.buf = bytearray()
        self.buf += struct.pack("<I", 1) + b"synthetic".ljust(40, b"0") + struct.pack("<I", 0x5A5A0001)
        self.packet_ptr = PACKET_BASE

    def registers(self, values):
        data = struct.pack("<%dI" % len(values), *values)
        self.buf += struct.pack("<IIIBxxxII", K_REGISTERS, 0, len(values), 0, 0, len(data)) + data

    def memory(self, address, data):
        self.buf += struct.pack("<IIIII", K_MEMORY_READ, address, 0, len(data), len(data)) + data

    def packet(self, dwords):
        data = struct.pack(">%dI" % len(dwords), *dwords)
        self.buf += struct.pack("<III", K_PACKET_START, self.packet_ptr, len(dwords)) + data
        self.buf += struct.pack("<I", K_PACKET_END)
        self.packet_ptr += len(data)

    def regs(self, **kv):
        for name, value in kv.items():
            if isinstance(value, float):
                value = f2u(value)
            self.packet(type0(R[name], [value]))

    def swap(self):
        self.buf += struct.pack("<II", K_EVENT, EVENT_SWAP)


def build(bug, frames=1):
    t = Trace()
    t.buf += struct.pack("<III", K_PRIMARY_START, PACKET_BASE, 0)
    t.registers([0] * REGISTER_COUNT)

    # Vertex data, big endian floats as on the console (fetch endian k8in32).
    tris = [
        # red, z 0.5
        (-0.8, -0.8, 0.5), (0.4, -0.8, 0.5), (-0.2, 0.8, 0.5),
        # green, z 0.25
        (-0.4, -0.6, 0.25), (0.8, -0.6, 0.25), (0.2, 0.9, 0.25),
        # blue, z 0.75
        (-1.0, -0.2, 0.75), (1.0, -0.2, 0.75), (0.0, 1.0, 0.75),
    ]
    vb = b"".join(struct.pack(">4f", x, y, z, 1.0) for x, y, z in tris)
    t.memory(VB_TRIS, vb)
    t.memory(VB_CLEAR, struct.pack(">6f", 0.0, 0.0, float(WIDTH), 0.0, float(WIDTH), float(HEIGHT)))

    vs = vertex_shader()
    ps = pixel_shader()
    t.packet(type3(PM4_IM_LOAD_IMMEDIATE, [0, len(vs)] + vs))
    t.packet(type3(PM4_IM_LOAD_IMMEDIATE, [1, len(ps)] + ps))

    # Common state.
    t.regs(
        RB_SURFACE_INFO=WIDTH,
        RB_COLOR_INFO=0,  # base 0, k_8_8_8_8
        RB_DEPTH_INFO=64,  # base tile 64, kD24S8
        RB_COLOR_MASK=0xF,
        PA_SC_WINDOW_OFFSET=0,
        PA_SC_WINDOW_SCISSOR_TL=0x80000000,
        PA_SC_WINDOW_SCISSOR_BR=WIDTH | (HEIGHT << 16),
        PA_SC_SCREEN_SCISSOR_TL=0,
        PA_SC_SCREEN_SCISSOR_BR=WIDTH | (HEIGHT << 16),
        PA_SC_AA_MASK=0xFFFF,
        VGT_MAX_VTX_INDX=0xFFFFFF,
        VGT_MIN_VTX_INDX=0,
        VGT_INDX_OFFSET=0,
        SQ_VS_CONST=0x000FF000,
        SQ_PS_CONST=0x000FF100,
        SQ_PROGRAM_CNTL=1 | (1 << 8),
        SQ_CONTEXT_MISC=0,
        PA_SU_VTX_CNTL=1,
        PA_SU_SC_MODE_CNTL=0,
        PA_CL_CLIP_CNTL=1 << 19,  # dx_clip_space_def: z in [0, w]
        RB_COLORCONTROL=0x7,  # alpha func always
        RB_BLENDCONTROL0=0x00010001,  # one, zero
        RB_COPY_DEST_INFO=0,
    )

    # Every frame repeats the clear and the triangles; common state is only set
    # once, so a capture starting at frame 1 must restore it from its register
    # snapshot.
    for frame in range(frames):
        # 0: clear via resolve. Copy of the cleared surface goes to CLEAR_DEST.
        t.regs(
            RB_MODECONTROL=6,  # kCopy
            RB_COPY_CONTROL=(1 << 8) | (1 << 9) | (1 << 20),  # color+depth clear, convert
            RB_COPY_DEST_BASE=CLEAR_DEST,
            RB_COPY_DEST_PITCH=WIDTH | (HEIGHT << 16),
            RB_COPY_DEST_INFO=(1 << 3) | (6 << 7),  # array, k_8_8_8_8 (ColorFormat 6)
            RB_COLOR_CLEAR=0xFF402010,
            RB_DEPTH_CLEAR=0xFFFFFF00,
            PA_CL_VTE_CNTL=0,  # screen space rectangle
        )
        t.packet(type0(R["SHADER_CONSTANT_FETCH_00_0"], [(3 | ((VB_CLEAR >> 2) << 2)), 2 | (6 << 2)]))
        t.packet(draw_autoindex(8, 3))  # kRectangleList

        # Triangles.
        t.regs(
            RB_MODECONTROL=4,  # kColorDepth
            RB_DEPTHCONTROL=0x2 | 0x4 | (3 << 4),  # z enable, z write, LEQUAL
            PA_CL_VTE_CNTL=0x3F | (1 << 10),
            PA_CL_VPORT_XSCALE=WIDTH / 2.0,
            PA_CL_VPORT_XOFFSET=WIDTH / 2.0,
            PA_CL_VPORT_YSCALE=-HEIGHT / 2.0,
            PA_CL_VPORT_YOFFSET=HEIGHT / 2.0,
            PA_CL_VPORT_ZSCALE=1.0,
            PA_CL_VPORT_ZOFFSET=0.0,
        )
        vb_size_words = len(vb) // 4
        for i, color in enumerate([(1.0, 0.0, 0.0, 1.0), (0.0, 1.0, 0.0, 1.0),
                                   (1.0, 0.0, 1.0, 1.0) if bug else (0.0, 0.0, 1.0, 1.0)]):
            # Vertex fetch 0 points at triangle i.
            base = VB_TRIS + i * 48
            t.packet(type0(R["SHADER_CONSTANT_FETCH_00_0"], [3 | ((base >> 2) << 2), 2 | (12 << 2)]))
            t.packet(type0(R["SHADER_CONSTANT_256_X"], [f2u(c) for c in color]))
            t.packet(draw_autoindex(4, 3))  # kTriangleList

        t.packet(type3(PM4_XE_SWAP, [0, 0, 0, WIDTH, HEIGHT]))
        t.buf += struct.pack("<I", K_PRIMARY_END)
        t.swap()
        if frame + 1 < frames:
            t.buf += struct.pack("<III", K_PRIMARY_START, PACKET_BASE, 0)
    return bytes(t.buf)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--bug", action="store_true", help="change the color of draw 3")
    ap.add_argument("--frames", type=int, default=1, help="number of frames (swaps)")
    args = ap.parse_args()
    data = build(args.bug, args.frames)
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"wrote {args.out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
