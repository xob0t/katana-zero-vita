"""Disassemble a PC YYC function with gml_* call targets named. usage: python -I pcdis.py NAME [maxbytes]"""
import struct, sys, re, capstone
import os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pcfunc import pe, d, base, va2off, off2va, find
# table: scan for (ptr to "gml_..." string, ptr into .text) pairs near the found entry
names = {}
text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
tlo, thi = base + text.VirtualAddress, base + text.VirtualAddress + text.Misc_VirtualSize
entry = int(find('gml_Object_obj_bullet_Draw_0')[1][1], 16)
o = va2off(entry)
# walk the table both ways in 8-byte steps
for step in (8, -8):
    k = o
    while True:
        n, f = struct.unpack_from('<II', d, k)
        so = va2off(n) if n > base else None
        if so is None or not (tlo <= f < thi): break
        s = d[so:d.index(b'\0', so)]
        if not s.startswith(b'gml_'): break
        names[f] = s.decode()
        k += step
print(len(names), 'named functions', file=sys.stderr)
name = sys.argv[1]
addr = [f for f, n in names.items() if n == name][0]
size = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
code = d[va2off(addr):va2off(addr) + size]
for ins in md.disasm(code, addr):
    c = ''
    if ins.mnemonic == 'call' and ins.op_str.startswith('0x'):
        t = int(ins.op_str, 16); c = names.get(t, '')
    m = re.search(r'0x([0-9a-f]{6,8})', ins.op_str)
    if not c and m and ins.mnemonic in ('push', 'mov'):
        v = int(m.group(1), 16); so = va2off(v) if v > base else None
        if so and 0 < so < len(d):
            s = d[so:so + 48].split(b'\0')[0]
            if len(s) > 3 and all(32 <= b < 127 for b in s): c = '"%s"' % s.decode()
    print('%x: %-6s %-40s %s' % (ins.address, ins.mnemonic, ins.op_str, c))
    if ins.mnemonic == 'ret' and ins.address > addr + 64 and size == 20000:
        pass
