import struct, sys
def parse(fn):
    data = open(fn,"rb").read()
    def u32(o): return struct.unpack_from("<I", data, o)[0]
    # scan every "ECEC" (0x43454345) signature; accept the one whose ROMHDR.physfirst is self-consistent
    start = 0
    while True:
        sig = data.find(b"ECEC", start)
        if sig < 0: raise SystemExit(f"{fn}: no valid ROM signature")
        start = sig + 1
        H = sig - 0x40                       # ROM file base (file off that maps to physfirst)
        pTOC = u32(sig+4)
        for cand in (0x8C010000,0x8C000000,0x80000000,0x80010000,0x8C0E0000,0x8C200000):
            rh = H + (pTOC - cand)
            if 0 <= rh and rh+0x54 <= len(data) and u32(rh+8) == cand:
                physfirst, romhdr = cand, rh
                nummods = u32(romhdr+0x10)
                if 0 < nummods < 4000:
                    toc = romhdr+0x54
                    v2f = lambda v: H + (v - physfirst)
                    mods=[]
                    for i in range(nummods):
                        e = toc + i*0x20
                        nf = v2f(u32(e+0x10))
                        if nf<0 or nf>=len(data): break
                        end = data.find(b"\0", nf)
                        mods.append((data[nf:end].decode("ascii","replace"), u32(e+0xC)))
                    return physfirst, nummods, mods
print("=== GAME 0WINCEOS.BIN (c:/dev/gdi2data/data) ===")
pf,n,gmods = parse(r"C:\dev\gdi2data\data\0WINCEOS.BIN")
print(f"physfirst={pf:08x} nummods={n}")
print("=== OURS (retail 0winceos.bin) ===")
pf2,n2,omods = parse(r"C:\wcedreamcast\release\retail\0winceos.bin")
print(f"physfirst={pf2:08x} nummods={n2}")
gset = {m.lower() for m,_ in gmods}
oset = {m.lower() for m,_ in omods}
print(f"\n=== modules the GAME has that OURS lacks ({len(gset-oset)}) ===")
for m,sz in sorted(gmods, key=lambda x:x[0].lower()):
    if m.lower() not in oset: print(f"  {sz:>9}  {m}")
print(f"\n=== common modules where SIZE differs (version mismatch suspects) ===")
osz = {m.lower():sz for m,sz in omods}
for m,sz in sorted(gmods, key=lambda x:x[0].lower()):
    if m.lower() in oset and osz.get(m.lower()) != sz:
        print(f"  {m:<16} game={sz} ours={osz[m.lower()]}")
