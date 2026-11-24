import os, sys, struct, bisect
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import const_sites as C
b=C.b
sites={int(l.split(',')[0].strip(' \t{'),16) for l in open(os.path.join(HERE,'sites_table.c'))}
text=[s for s in b.elf.iter_sections() if s.name=='.text'][0]
base,size,off=text['sh_addr'],text['sh_size'],text['sh_offset']
words=struct.unpack('<%dI'%(size//4), b.data[off:off+size//4*4])
imm=lambda w: ((w>>4)&0xf000)|(w&0xfff)
targets={0x40c19400:9000,0x40c48200:10500,0x40c77000:12000,0x40c72500:11850,0x40c85100:12450,0x40c93200:12900,0x40ca5e00:13500,0x40b77000:6000,0x4087c000:760}
found=[]
for i,w in enumerate(words):
    if w&0x0ff00000==0x03400000:
        hi=imm(w); rd=(w>>12)&15
        for j in range(i-1,max(0,i-8),-1):
            v=words[j]
            if v&0x0ff00000==0x03000000 and (v>>12)&15==rd:
                val=(hi<<16)|imm(v)
                if val in targets:
                    a=base+j*4
                    k=bisect.bisect_right(b.func_addrs,a)-1
                    found.append((a,targets[val],b.funcs[k][2] if k>=0 else '?'))
                break
print(len(found), 'movw/movt pairs with these constants;', len(sites), 'in table')
for a,v,fn in found:
    if a not in sites: print('NOT IN TABLE', hex(a), v, fn[:90])
