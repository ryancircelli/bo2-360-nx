"""xdk_sig.py IMAGE BASE key=addr:words ... : print signature rows for sdk/src/codegen/xdk_signatures.cpp.

IMAGE is a decrypted 360 image (e.g. games/<g>/work/default.image.bin), BASE its load address (0x82000000).
Each row: masked first 4 words + FNV-1a-64 of all masked words. Mask rule must match MaskWord() there."""
import sys, struct

DFORM = {14, 24, 32, 34, 36, 38, 40, 42, 44, 48, 50, 52, 54, 58, 62}  # D-form loads/stores/addi


def masked(ws):
    out, lis = [], set()
    for w in ws:
        op, m = w >> 26, 0xFFFFFFFF
        if op == 18: m = 0xFC000003  # b/bl displacement
        elif op == 16: m = 0xFFFF0003  # bc displacement
        elif op == 15: m = 0xFFFF0000; lis.add((w >> 21) & 31)  # lis immediate
        elif op in DFORM and ((w >> 16) & 31) in lis: m = 0xFFFF0000  # offset from a lis-loaded register
        out.append(w & m)
    return out


def fnv(ws):
    h = 0xcbf29ce484222325
    for w in ws:
        for b in struct.pack('>I', w): h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


if __name__ == '__main__':
    img, base = open(sys.argv[1], 'rb').read(), int(sys.argv[2], 16)
    for kv in sys.argv[3:]:
        k, rest = kv.split('='); a, n = rest.split(':'); a, n = int(a, 16), int(n)
        mw = masked(struct.unpack('>%dI' % n, img[a - base:a - base + 4 * n]))
        print('    {"%s", %d, {%s}, 0x%016XULL},' % (k, n, ', '.join('0x%08X' % x for x in mw[:4]), fnv(mw)))
