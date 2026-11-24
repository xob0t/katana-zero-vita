"""PC Katana ZERO.exe (YYC x86) loader: GML function table + YYVAR name table. Read-only."""
import struct, pefile, bisect, functools
EXE = r"C:/Program Files (x86)/Steam/steamapps/common/Katana ZERO/Katana ZERO.exe"

class PC:
    def __init__(self):
        self.pe = pefile.PE(EXE, fast_load=True)
        self.d = open(EXE, 'rb').read()
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.secs = [(self.base + s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData, s.Name.rstrip(b'\0').decode()) for s in self.pe.sections]
        t = [s for s in self.secs if s[4] == '.text'][0]
        self.tlo, self.thi = t[0], t[0] + t[1]

    def va2off(self, v):
        for va, vs, ro, rs, n in self.secs:
            if va <= v < va + min(vs, rs):
                return ro + v - va
        return None

    def off2va(self, o):
        for va, vs, ro, rs, n in self.secs:
            if ro <= o < ro + rs:
                return va + o - ro
        return None

    def cstr(self, va, limit=200):
        o = self.va2off(va)
        if o is None: return None
        e = self.d.find(b'\0', o, o + limit)
        if e < 0: return None
        s = self.d[o:e]
        if not s or any(c < 32 or c > 126 for c in s): return None
        return s.decode()

    def u32(self, va):
        o = self.va2off(va)
        return None if o is None else struct.unpack_from('<I', self.d, o)[0]

    def f64(self, va):
        o = self.va2off(va)
        return None if o is None or o + 8 > len(self.d) else struct.unpack_from('<d', self.d, o)[0]

    @functools.cached_property
    def funcs(self):
        """{name: addr} from the {char* name, void* func} table (walk from a known entry)."""
        anchor = b'gml_Object_obj_bullet_Draw_0\0'
        i = self.d.find(anchor)
        while self.d[i - 1] != 0: i = self.d.find(anchor, i + 1)
        sva = self.off2va(i)
        j = self.d.find(struct.pack('<I', sva))
        while j >= 0 and not (self.tlo <= struct.unpack_from('<I', self.d, j + 4)[0] < self.thi and self.off2va(j) >= self.secs[1][0]):
            j = self.d.find(struct.pack('<I', sva), j + 1)
        names = {}
        for step in (8, -8):
            k = j
            while True:
                n, f = struct.unpack_from('<II', self.d, k)
                s = self.cstr(n) if n > self.base else None
                if s is None or not (self.tlo <= f < self.thi) or not s.startswith('gml_'): break
                names[s] = f
                k += step
        return names

    @functools.cached_property
    def sorted_starts(self):
        return sorted(set(self.funcs.values()))

    def extent(self, addr):
        i = bisect.bisect_right(self.sorted_starts, addr)
        end = self.sorted_starts[i] if i < len(self.sorted_starts) else addr + 0x40000
        return addr, min(end, addr + 0x100000)

    @functools.cached_property
    def yyvars(self):
        """id -> name from YYVAR {char* name, int id} structs in .data."""
        out = {}
        for va, vs, ro, rs, n in self.secs:
            if n not in ('.data', '.rdata'): continue
            for o in range(ro, ro + rs - 8, 4):
                p, vid = struct.unpack_from('<Ii', self.d, o)
                if 0 <= vid < 200000 and self.base < p < self.base + 0x3000000:
                    pass
        return out

if __name__ == '__main__':
    import collections, sys
    pc = PC()
    f = pc.funcs
    c = collections.Counter(n.split('_')[1] for n in f)
    print(len(f), c)
    for n in list(f)[:5]: print(n, hex(f[n]))
