import struct,zlib,sys,array
def conv(src,dst):
    d=open(src,'rb').read(); hl=struct.unpack('<H',d[8:10])[0]; hdr=d[10:10+hl].decode()
    import re; h,w=map(int,re.search(r'\((\d+), (\d+), 4\)',hdr).groups())
    f=array.array('f'); f.frombytes(d[10+hl:10+hl+h*w*16])
    rows=[]
    for y in range(h):
        r=bytearray([0])
        for x in range(w):
            o=(y*w+x)*4
            for c in range(3): r.append(max(0,min(255,int(f[o+c]*255+.5))))
        rows.append(bytes(r))
    def ch(t,b): c=struct.pack('>I',len(b))+t+b; return c+struct.pack('>I',zlib.crc32(t+b))
    open(dst,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(b''.join(rows),3))+ch(b'IEND',b''))
for i in range(int(sys.argv[2])): conv(f'{sys.argv[1]}/draws/{i:06d}_frame.npy',f'{sys.argv[1]}/{i:02d}.png')
