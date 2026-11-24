"""PC Katana ZERO.exe (YYC x86): find a GML function by name via its name string and the g_GMLFuncs-like table.
usage: python -I pcfunc.py NAME [NAME...]  -> prints address of function(s)"""
import struct, sys, pefile
EXE = r"C:/Program Files (x86)/Steam/steamapps/common/Katana ZERO/Katana ZERO.exe"
pe = pefile.PE(EXE, fast_load=True)
d = open(EXE, 'rb').read()
base = pe.OPTIONAL_HEADER.ImageBase
def off2va(o):
    for s in pe.sections:
        if s.PointerToRawData <= o < s.PointerToRawData + s.SizeOfRawData:
            return base + s.VirtualAddress + o - s.PointerToRawData
def va2off(v):
    for s in pe.sections:
        if s.VirtualAddress <= v - base < s.VirtualAddress + s.Misc_VirtualSize:
            return v - base - s.VirtualAddress + s.PointerToRawData
def find(name):
    res = []
    i = d.find(name.encode() + b'\0')
    while i >= 0:
        if d[i-1] == 0:
            va = off2va(i); pat = struct.pack('<I', va); j = d.find(pat)
            while j >= 0:
                f = struct.unpack_from('<I', d, j + 4)[0]
                res.append((hex(va), hex(off2va(j)), hex(f)))
                j = d.find(pat, j + 1)
        i = d.find(name.encode() + b'\0', i + 1)
    return res
if __name__ == '__main__':
    for n in sys.argv[1:]:
        print(n, find(n))
