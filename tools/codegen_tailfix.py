"""Module-aware version of tools/codegen_fix_unresolved.py's single step: read unresolved tail-branch targets from a codegen log,
declare them in an overrides file when the preceding word ends a function. usage: tailfix.py LOG IMAGE BASE OVERRIDES"""
import re,struct,sys,capstone
log,img,base,ov=sys.argv[1],sys.argv[2],int(sys.argv[3],16),sys.argv[4]
d=open(img,'rb').read()
md=capstone.Cs(capstone.CS_ARCH_PPC,capstone.CS_MODE_32|capstone.CS_MODE_BIG_ENDIAN)
w=lambda e:struct.unpack(">I",d[e-base:e-base+4])[0]
def dis(e):
    i=next(md.disasm(d[e-base:e-base+4],e),None); return f"{i.mnemonic} {i.op_str}".strip() if i else f".long {w(e):08X}"
def ends(e):
    x=w(e); return x==0 or x in(0x4E800020,0x4E800420) or ((x>>26)==18 and not x&1)
R=re.compile(r"^\s+0x([0-9A-F]{8}) from 0x([0-9A-F]{8}): (b\w*) .*target not in any function")
errs=sorted({m.groups() for m in map(R.match,open(log,errors='replace').read().splitlines()) if m})
known=open(ov).read(); out=[]; human=[]
for t,s,m in errs:
    ti=int(t,16)
    if f'"0x{t}"' in known: continue
    if m!="b" or not ends(ti-4): human.append((t,s,dis(ti-4))); continue
    out.append(f'"0x{t}" = {{ }}   # {dis(ti)}   <- 0x{s}')
print("human:",human)
open(ov,"a").write("\n# tail-branch targets (tailfix.py)\n"+"\n".join(out)+"\n"); print("\n".join(out))
