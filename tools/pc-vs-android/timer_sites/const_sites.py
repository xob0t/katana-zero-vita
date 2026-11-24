"""Find the instructions that build the constant stored into a variable (libyoyo.so, APK 1.1.10).

usage: python const_sites.py VAR FUNC [FUNC...]      (FUNC: exact demangled name, e.g. gml_Object_obj_hud_Draw_0)
       python const_sites.py VAR @names.txt

Linear register/stack-slot tracking per function: the pointer returned by a getVar/setVar call with r1 = varid(VAR)
is followed through movs and stack spills; a store of its high word (strd rLo, rHi, [ptr] or str rHi, [ptr, #4]) whose
register was built by movw/movt (or mov #imm) is a site. Prints one line per site:
  FUNC  movw_addr movt_addr  value  (+ 'UNHANDLED' lines for stores of the pointer it can't attribute)."""
import os, re, struct, sys
import capstone
from capstone import arm_const as A
HERE = os.path.dirname(os.path.abspath(__file__))
DECOMP = os.path.join(HERE, '..', 'decomp')
sys.path.insert(0, DECOMP)
os.chdir(DECOMP)
from annotate import Binary  # noqa: E402

b = Binary('../apk110/lib/armeabi-v7a/libyoyo.so')
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
md.detail = True
byname = {}
for a, sz, n in b.funcs:
    byname.setdefault(n, (a, sz))


def demangle_map():
    m = {}
    for n in byname:
        mm = re.match(r'_Z(\d+)(gml_\w+)', n)
        if mm:
            m[mm.group(2)[:int(mm.group(1))]] = n
    return m


DEM = demangle_map()
RNAME = {getattr(A, 'ARM_REG_R%d' % i): 'r%d' % i for i in range(13)}
RNAME.update({A.ARM_REG_SB: 'r9', A.ARM_REG_SL: 'r10', A.ARM_REG_FP: 'r11', A.ARM_REG_IP: 'r12', A.ARM_REG_SP: 'sp',
              A.ARM_REG_LR: 'lr', A.ARM_REG_PC: 'pc'})


def scan(var, fname):
    mangled = DEM.get(fname, fname)
    if mangled not in byname:
        return None, ['NOFUNC']
    addr, size = byname[mangled]
    code = b.data[b.off(addr):b.off(addr) + size]
    st, stack, sites, notes = {}, {}, [], []
    def insns():
        for k in range(0, len(code) - 3, 4):
            yield from md.disasm(code[k:k + 4], addr + k)

    for ins in insns():
        ops = ins.operands
        mn = ins.mnemonic
        def R(i):
            return RNAME.get(ops[i].reg)
        written = [RNAME.get(r) for r in ins.regs_access()[1]]
        new = {}
        if mn == 'ldr' and ops[1].type == A.ARM_OP_MEM:
            base, idx, disp = RNAME.get(ops[1].mem.base), RNAME.get(ops[1].mem.index), ops[1].mem.disp
            if base == 'pc' and not idx:
                new[R(0)] = ('lit', b.u32(ins.address + 8 + disp))
            elif base == 'pc' and idx and st.get(idx, ('',))[0] == 'lit':
                slot = (ins.address + 8 + st[idx][1]) & 0xffffffff
                new[R(0)] = ('sym', b.got.get(slot))
            elif base in ('sp', 'r11') and not idx:
                new[R(0)] = stack.get((base, disp), ('?',))
            elif st.get(base, ('',))[0] == 'sym' and disp == 4 and not idx:
                new[R(0)] = ('varid', (st[base][1] or '').replace('g_VAR_', ''))
        elif mn == 'mov' and ops[1].type == A.ARM_OP_REG:
            new[R(0)] = st.get(R(1), ('?',))
        elif mn == 'mov' and ops[1].type == A.ARM_OP_IMM:
            new[R(0)] = ('imm', ops[1].imm & 0xffffffff, ins.address, None)
        elif mn == 'movw':
            new[R(0)] = ('imm', ops[1].imm & 0xffff, ins.address, None)
        elif mn == 'movt':
            prev = st.get(R(0), ('?',))
            if prev[0] == 'imm' and prev[1] <= 0xffff:
                new[R(0)] = ('imm', (ops[1].imm << 16) | prev[1], prev[2], ins.address)
        elif mn == 'str' and ops[1].type == A.ARM_OP_MEM:
            base, disp = RNAME.get(ops[1].mem.base), ops[1].mem.disp
            if base in ('sp', 'r11') and not ops[1].mem.index:
                stack[(base, disp)] = st.get(R(0), ('?',))
            elif st.get(base, ('',))[0] == 'ptr' and disp == 4:
                v = st.get(R(0), ('?',))
                if v[0] == 'imm' and v[3]:
                    sites.append((v[2], v[3], v[1]))
                else:
                    notes.append('UNHANDLED str %x %s' % (ins.address, v))
        elif mn == 'strd' and ops[2].type == A.ARM_OP_MEM:
            base, disp = RNAME.get(ops[2].mem.base), ops[2].mem.disp
            if st.get(base, ('',))[0] == 'ptr' and disp == 0:
                v = st.get(R(1), ('?',))
                if v[0] == 'imm' and v[3]:
                    sites.append((v[2], v[3], v[1]))
                else:
                    notes.append('UNHANDLED strd %x %s' % (ins.address, v))
        elif mn in ('vstr', 'vst1', 'vst1.64', 'vst1.32', 'stm', 'stmia') or mn.startswith('vst'):
            for o in ops:
                if o.type == A.ARM_OP_MEM and st.get(RNAME.get(o.mem.base), ('',))[0] == 'ptr':
                    notes.append('UNHANDLED %s %x' % (mn, ins.address))
        if mn in ('bl', 'blx'):
            r1 = st.get('r1', ('',))
            for r in ('r0', 'r1', 'r2', 'r3', 'r12', 'lr'):
                st.pop(r, None)
            if mn == 'blx' and r1[0] == 'varid' and r1[1] == var:
                st['r0'] = ('ptr',)
            continue
        for r in written:
            st.pop(r, None)
        st.update({k: v for k, v in new.items() if k})
    return sites, notes


if __name__ == '__main__':
    var = sys.argv[1]
    names = sys.argv[2:]
    if names and names[0].startswith('@'):
        names = [l.strip() for l in open(names[0][1:]) if l.strip()]
    for n in names:
        sites, notes = scan(var, n)
        for s in sites or []:
            v = struct.unpack('<d', struct.pack('<Q', s[2] << 32))[0]
            print('%s %#x %#x %#010x %g' % (n, s[0], s[1], s[2], v))
        for x in notes:
            print(n, x)
        if not sites and not notes:
            print(n, 'NONE')
