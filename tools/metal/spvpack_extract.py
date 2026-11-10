#!/usr/bin/env python3
"""Write every shader of a bo2zm .spvpack as <stage>_<hash16>.spv (stage vs or ps).

Pack v1 (app/src/native/render/bo2zm_shader_library.cpp): 24-byte header (8-byte signature BZMSPV\\0\\0,
u32 version 1, u32 count, u64 body hash), then per shader u32 container bytes, u32 SPIR-V words, u64 XXH3 of the
container, the container (big-endian Xenos shader container, signature bit 0 = vertex shader), the SPIR-V words.
The output is derived from game data: keep it outside the repository.
"""
import argparse
import os
import struct
import sys


def read_pack(path):
    with open(path, "rb") as f:
        header = f.read(24)
        if header[:8] != b"BZMSPV\0\0":
            sys.exit(f"{path}: not a bo2zm shader pack")
        version, count = struct.unpack_from("<II", header, 8)
        if version != 1:
            sys.exit(f"{path}: pack version {version}, this tool reads 1")
        for _ in range(count):
            original, words, container_hash = struct.unpack("<IIQ", f.read(16))
            container = f.read(original)
            spirv = f.read(words * 4)
            if len(container) != original or len(spirv) != words * 4:
                sys.exit(f"{path}: truncated")
            vertex = (struct.unpack(">I", container[:4])[0] & 1) != 0
            yield container_hash, vertex, container, spirv


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("pack")
    p.add_argument("out_dir")
    a = p.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    n = vs = 0
    for h, vertex, _, spirv in read_pack(a.pack):
        name = f"{'vs' if vertex else 'ps'}_{h:016x}.spv"
        with open(os.path.join(a.out_dir, name), "wb") as f:
            f.write(spirv)
        n += 1
        vs += vertex
    print(f"{n} shaders ({vs} vertex, {n - vs} pixel) -> {a.out_dir}")


if __name__ == "__main__":
    main()
