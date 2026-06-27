import struct, re
def load(fn):
    data=open(fn,"rb").read()
    def u32(o): return struct.unpack_from("<I",data,o)[0]
    s=0
    while True:
        sig=data.find(b"ECEC",s); s=sig+1
        if sig<0: raise SystemExit(fn+": no sig")
        H=sig-0x40; pTOC=u32(sig+4)
        for c in (0x8C010000,):
            rh=H+(pTOC-c)
            if 0<=rh and u32(rh+8)==c and 0<u32(rh+0x10)<4000:
                return data,H,c,rh
def romhdr(fn):
    data,H,base,rh=load(fn)
    g=lambda o: struct.unpack_from("<I",data,rh+o)[0]
    return dict(physfirst=g(8),physlast=g(0xC),nummods=g(0x10),RAMStart=g(0x14),
        RAMFree=g(0x18),RAMEnd=g(0x1C),numfiles=g(0x30),KernelFlags=g(0x34),
        FSRamPercent=g(0x38),DrivglobStart=g(0x3C),DrivglobLen=g(0x40))
for tag,fn in [("GAME",r"C:\dev\gdi2data\data\0WINCEOS.BIN"),("OURS",r"C:\wcedreamcast\release\retail\0winceos.bin")]:
    h=romhdr(fn)
    print(f"--- {tag} ROMHDR ---")
    for k in ("physfirst","physlast","RAMStart","RAMFree","RAMEnd","DrivglobStart","DrivglobLen"):
        print(f"   {k:14}= {h[k]:08x}")
    print(f"   {'nummods':14}= {h['nummods']}   numfiles={h['numfiles']}")
    print(f"   {'FSRamPercent':14}= {h['FSRamPercent']:08x}   KernelFlags={h['KernelFlags']:08x}")
    print(f"   RAM region size = {h['RAMEnd']-h['RAMStart']:#x} ({(h['RAMEnd']-h['RAMStart'])//1024} KB)")
