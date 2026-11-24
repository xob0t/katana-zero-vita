import os, sys, struct, bisect
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import const_sites as C
b=C.b
vals=[9000,10500,12000,11850,12450,12900,13500,6000,760]
for s in b.elf.iter_sections():
    if not s['sh_size'] or s['sh_type']=='SHT_NOBITS': continue
    d=b.data[s['sh_offset']:s['sh_offset']+s['sh_size']]
    for v in vals:
        pat=struct.pack('<d',float(v)); i=d.find(pat)
        while i>=0:
            if i%4==0:
                a=s['sh_addr']+i; k=bisect.bisect_right(b.func_addrs,a)-1
                fn=b.funcs[k][2] if k>=0 and a<b.funcs[k][0]+b.funcs[k][1] else ''
                print(s.name, hex(a), v, fn[:80])
            i=d.find(pat,i+1)
