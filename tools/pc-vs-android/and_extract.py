"""Android libyoyo.so (YYC ARMv7): per-GML-function features -> out/android_features.json, out/android_names.json
Uses work/decomp/annotate.py (Binary: symbols, GOT, PLT, veneers).
usage: python and_extract.py              (all gml_* functions, multiprocess)
       python and_extract.py SUBSTR ...   (print features of matching functions)"""
import sys, os, json, capstone
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'decomp'))
os.chdir(os.path.join(HERE, '..', '..', 'decomp'))
from annotate import Binary, short, demangle
from tokens import pair_words, summarize, hi2d, is_hi
A = capstone.arm
PREFIXES = ('gml_Object_', 'gml_Script_', 'gml_GlobalScript_', 'gml_RoomCC_', 'gml_Room_', 'gml_Timeline')


def extract(b, md, addr, size, name):
    code = b.data[b.off(addr):b.off(addr) + size]
    insns = [i for i in md.disasm(code, addr) if i.id != 0]
    pool = set()
    for ins in insns:
        if ins.mnemonic in ('ldr', 'vldr') and len(ins.operands) >= 2:
            m = ins.operands[1]
            if m.type == A.ARM_OP_MEM and m.mem.base == A.ARM_REG_PC and m.mem.index == 0:
                a = ins.address + 8 + m.mem.disp
                pool.add(a)
                if ins.mnemonic == 'vldr':
                    pool.add(a + 4)
    regs = {}
    ev = []
    line_slot = None
    n = 0
    for ins in insns:
        if ins.address in pool:
            continue
        n += 1
        at = ins.address
        ops = ins.operands
        mn = ins.mnemonic
        dst = ins.reg_name(ops[0].reg) if ops and ops[0].type == A.ARM_OP_REG else None
        known = None
        if mn == 'ldr' and len(ops) == 2 and ops[1].type == A.ARM_OP_MEM:
            m = ops[1].mem
            base = ins.reg_name(m.base) if m.base else None
            idx = ins.reg_name(m.index) if m.index else None
            if base == 'pc' and idx is None:
                known = ('lit', b.u32(at + 8 + m.disp))
            elif base == 'pc' and idx in regs and regs[idx][0] == 'lit':
                slot = (at + 8 + regs[idx][1]) & 0xffffffff
                if slot in b.got:
                    known = ('sym', b.got[slot])
            elif base in regs and idx is None and regs[base][0] == 'sym':
                sn = regs[base][1]
                if sn.startswith('g_VAR_') and m.disp == 4:
                    ev.append(('v', sn[6:], at))
                elif sn.startswith('g_FUNC_') and m.disp == 4:
                    ev.append(('c', 'F:' + sn[7:], at))
            elif base in regs and idx is None and regs[base][0] == 'addr':
                known = ('lit', b.u32(regs[base][1] + m.disp))
        elif mn == 'add' and len(ops) == 3 and ops[1].type == A.ARM_OP_REG and ins.reg_name(ops[1].reg) == 'pc' \
                and ops[2].type == A.ARM_OP_REG and ins.reg_name(ops[2].reg) in regs \
                and regs[ins.reg_name(ops[2].reg)][0] == 'lit':
            va = (at + 8 + regs[ins.reg_name(ops[2].reg)][1]) & 0xffffffff
            known = ('addr', va)
            s = b.cstr(va, 400)
            if s is not None and not s.startswith('gml_'):
                ev.append(('s', s, at))
            sn = b.sym_by_addr.get(va)
            if sn and sn.startswith('g_VAR_'):
                ev.append(('v', sn[6:], at))
        elif mn == 'vldr' and ops[1].type == A.ARM_OP_MEM and ins.reg_name(ops[0].reg).startswith('d'):
            m = ops[1].mem
            base = ins.reg_name(m.base)
            v = None
            if base == 'pc':
                v = b.f64(at + 8 + m.disp)
            elif base in regs and regs[base][0] == 'addr':
                v = b.f64(regs[base][1] + m.disp)
            if v is not None and v == v:
                ev.append(('d', abs(v), at))
        elif mn == 'vmov.f64' and len(ops) == 2 and ops[1].type == A.ARM_OP_FP:
            ev.append(('d', abs(ops[1].fp), at))
        elif mn in ('mov', 'mvn', 'movw') and len(ops) == 2 and ops[1].type == A.ARM_OP_IMM:
            v = ops[1].imm & 0xffffffff
            if mn == 'mvn':
                v = ~v & 0xffffffff
            known = ('lit', v)
        elif mn == 'movt' and len(ops) == 2:
            lo = regs[dst][1] & 0xffff if dst in regs and regs[dst][0] == 'lit' else 0
            known = ('lit', lo | ((ops[1].imm & 0xffff) << 16))
        elif mn == 'mov' and len(ops) == 2 and ops[1].type == A.ARM_OP_REG and ins.reg_name(ops[1].reg) in regs:
            known = regs[ins.reg_name(ops[1].reg)]
        elif mn in ('add', 'adds', 'sub', 'subs') and len(ops) == 3 and ops[1].type == A.ARM_OP_REG                 and ops[2].type == A.ARM_OP_IMM and ins.reg_name(ops[1].reg) in regs                 and regs[ins.reg_name(ops[1].reg)][0] == 'lit' and ins.reg_name(ops[1].reg) != 'pc':
            v0 = regs[ins.reg_name(ops[1].reg)][1]
            if v0 is not None:
                known = ('lit', (v0 + (ops[2].imm if mn.startswith('add') else -ops[2].imm)) & 0xffffffff)
        elif mn in ('sbc', 'adc') and len(ops) == 3 and ops[2].type == A.ARM_OP_IMM and ops[2].imm == 0                 and ins.reg_name(ops[1].reg) in regs:
            known = regs[ins.reg_name(ops[1].reg)]   # carry of the paired low-word add/sub ignored
        elif mn in ('bl', 'blx', 'b') and ops and ops[0].type == A.ARM_OP_IMM:
            cn = b.name_at(ops[0].imm)
            if cn and not (mn == 'b' and cn.startswith(name)):
                s = short(cn)
                if s and '+0x' not in s:
                    ev.append(('c', s, at))
                    # integer operand of an RValue operator / int constructor: a GML integer literal
                    full = demangle([cn])[0] if cn.startswith('_Z') else cn
                    if 'int)' in full and ('operator' in full or 'YYRValue(int' in full):
                        for r in ('r1', 'r2'):
                            if r in regs and regs[r][0] == 'lit' and regs[r][1] is not None and regs[r][1] < 100000:
                                ev.append(('i', '%d' % regs[r][1], at))
        elif mn == 'strd' and len(ops) == 3 and ops[2].type == A.ARM_OP_MEM:
            r0, r1 = ins.reg_name(ops[0].reg), ins.reg_name(ops[1].reg)
            if r0 in regs and r1 in regs and regs[r0][0] == 'lit' and regs[r1][0] == 'lit' and is_hi(regs[r1][1]):
                ev.append(('d', abs(hi2d(regs[r1][1], regs[r0][1])), at))
        elif mn == 'vmov' and len(ops) == 3 and ops[0].type == A.ARM_OP_REG and ins.reg_name(ops[0].reg).startswith('d'):
            r0, r1 = ins.reg_name(ops[1].reg), ins.reg_name(ops[2].reg)
            if r0 in regs and r1 in regs and regs[r0][0] == 'lit' and regs[r1][0] == 'lit' and is_hi(regs[r1][1]):
                ev.append(('d', abs(hi2d(regs[r1][1], regs[r0][1])), at))
        if mn == 'str' and len(ops) == 2 and ops[1].type == A.ARM_OP_MEM:
            src = ins.reg_name(ops[0].reg)
            m = ops[1].mem
            slot = (ins.reg_name(m.base), m.disp)
            if src in regs and regs[src][0] == 'addr' and (b.cstr(regs[src][1]) or '').startswith('gml_'):
                line_slot = (slot[0], slot[1] + 4)
            elif src in regs and regs[src][0] == 'lit':
                ev.append(('W', slot, regs[src][1], at, n))
        if ins.group(capstone.CS_GRP_CALL) or mn == 'blx':
            for r in ('r0', 'r1', 'r2', 'r3', 'ip', 'lr'):
                regs.pop(r, None)
        elif dst:
            if known is not None:
                regs[dst] = known
            elif dst in regs and mn not in ('str', 'strd', 'cmp', 'cmn', 'tst', 'vstr', 'push', 'stm'):
                del regs[dst]
    pair_words(ev)
    r = summarize(ev, line_slot)
    r.update(addr=addr, size=size)
    return r


B = MD = None


def winit():
    global B, MD
    B = Binary('../apk110/lib/armeabi-v7a/libyoyo.so')
    MD = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    MD.detail = True
    MD.skipdata = True
    demangle(sorted({x for x in list(B.sym_by_addr.values()) + list(B.plt.values()) + [f[2] for f in B.funcs]
                     if x.startswith('_Z')}))


def job(f):
    a, sz, n = f
    return short(n) if n.startswith('_Z') else n, extract(B, MD, a, sz, n)


def main():
    import multiprocessing as mp
    winit()
    only = sys.argv[1:]
    fs = [f for f in B.funcs if any(p in f[2] for p in PREFIXES)]
    fs = [f for f in fs if (short(f[2]) if f[2].startswith('_Z') else f[2]).startswith('gml_')
          and (not only or any(o in short(f[2]) for o in only))]
    if only:
        for f in fs:
            n, r = job(f)
            print(n, r['dbl'])
            print(json.dumps(r['bylines'])[:4000])
        return
    fs.sort(key=lambda f: -f[1])
    with mp.Pool(min(14, os.cpu_count()), initializer=winit) as p:
        out = dict(p.imap_unordered(job, fs, chunksize=4))
    od = os.path.join(HERE, '..', 'out')
    allvars = sorted({s[6:] for s in B.sym_by_addr.values() if s.startswith('g_VAR_')})
    allfuncs = sorted({s[7:] for s in B.sym_by_addr.values() if s.startswith('g_FUNC_')})
    json.dump(out, open(os.path.join(od, 'android_features.json'), 'w'))
    json.dump(dict(vars=allvars, funcs=allfuncs), open(os.path.join(od, 'android_names.json'), 'w'))
    print(len(out), 'functions', len(allvars), 'vars')


if __name__ == '__main__':
    main()
