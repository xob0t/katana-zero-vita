"""PC Katana ZERO.exe (YYC x86): per-GML-function features -> out/pc_features.json, out/pc_names.json
(same schema as and_extract.py). Extent of a function = start .. next gml_* function start (cap 1 MB).
usage: python -I pc_extract.py [name-substring ...]"""
import sys, os, json, re, struct, multiprocessing as mp
import capstone
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pcbin import PC
from tokens import pair_words, summarize

pc = None
VARNAMES = {}   # YYVAR id -> name
FUNCSLOT = {}   # VA of int slot holding a builtin's function id -> builtin name
FADD = {}       # builtin implementation VA -> name (Function_Add registrations)
NAMEOF = {}     # gml_* function VA -> name
# runtime variable accessors taking (inst, varid=[esp+4], arrayidx=[esp+8], ...): found by the 0x80000000
# no-array-index argument they share
ACCESSORS = {0x18fae90, 0x18fae20, 0x18fbca0, 0x18fbaa0, 0x18fba20, 0x18fb7b0}
# runtime helpers taking a GML integer literal as [esp+4] (RValue int operators / int constructor), found by the
# small immediate they receive; tools/README.md
INTOPS = {0x428bc0, 0x19013c0, 0x1901840, 0x428c60, 0x48d3d0, 0x432270, 0x432310, 0x49f1a0}
VARTABLE_ENTRY = 0x1e32668   # {char* "first_attack_timer", int 0x467} inside the YYVAR name table
FUNCSLOT_ENTRY = 0x1e3bc58   # {char* "round", int slot} inside the builtin function-id table
FUNCTION_ADD = 0x19a6080


def init():
    global pc, VARNAMES, FUNCSLOT, FADD, NAMEOF
    pc = PC()
    d = pc.d
    o = pc.va2off(VARTABLE_ENTRY)
    for step in (8, -8):
        k = o
        while True:
            p, vid = struct.unpack_from('<Ii', d, k)
            s = pc.cstr(p) if p > pc.base else None
            if s is None or not (0 <= vid < 100000):
                break
            VARNAMES[vid] = s
            k += step
    o = pc.va2off(FUNCSLOT_ENTRY)
    for step in (8, -8):
        k = o
        while True:
            p, v = struct.unpack_from('<II', d, k)
            s = pc.cstr(p) if p > pc.base else None
            if s is None:
                break
            FUNCSLOT[pc.off2va(k) + 4] = s
            k += step
    for m in re.finditer(rb'\x68(....)\x68(....)\xe8(....)', d):
        f, n, rel = struct.unpack('<IIi', m.group(1) + m.group(2) + m.group(3))
        if pc.off2va(m.start()) + 15 + rel == FUNCTION_ADD:
            s = pc.cstr(n)
            if s:
                FADD[f] = s
    NAMEOF = {v: k for k, v in pc.funcs.items()}


STORE = re.compile(r'dword ptr \[(e[a-z]{2})(?: \+ (0x[0-9a-f]+|\d+))?\], (0x[0-9a-f]+|\d+)$')


def extract(name):
    addr = pc.funcs[name]
    lo, hi = pc.extent(addr)
    o = pc.va2off(lo)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    ev = []
    line_slot = None
    arg0 = arg1 = arg2 = None
    n = 0
    for (at, sz, mn, ops) in md.disasm_lite(pc.d[o:o + (hi - lo)], lo):
        n += 1
        if mn == 'mov':
            m = STORE.match(ops)
            if m:
                slot = (m.group(1), int(m.group(2) or '0', 0))
                v = int(m.group(3), 0)
                s = pc.cstr(v) if v > pc.base else None
                if s and s.startswith('gml_'):
                    if line_slot is None and s == name:
                        line_slot = (slot[0], slot[1] + 4)
                elif s:
                    ev.append(('s', s, at))
                else:
                    ev.append(('W', slot, v, at, n))
                if slot == ('esp', 0):
                    arg0 = v
                elif slot == ('esp', 4):
                    arg1 = v
                elif slot == ('esp', 8):
                    arg2 = v
        elif mn == 'push' and ops.startswith('0x'):
            v = int(ops, 16)
            s = pc.cstr(v) if v > pc.base else None
            if s and not s.startswith('gml_'):
                ev.append(('s', s, at))
        m = re.search(r'qword ptr \[(0x[0-9a-f]+)\]', ops)
        if m:
            v = pc.f64(int(m.group(1), 16))
            if v is not None and v == v and (v == 0 or 1e-12 < abs(v) < 1e12):
                ev.append(('d', abs(v), at))
        m = re.search(r'dword ptr \[(0x[0-9a-f]{7})\]', ops)
        if m:
            s = FUNCSLOT.get(int(m.group(1), 16))
            if s:
                ev.append(('c', 'F:' + s, at))
        m = re.match(r'e\w\w, \[e(?!sp|bp)\w\w \+ (0x[0-9a-f]+)\]$', ops) if mn == 'lea' else None
        if m:
            v = int(m.group(1), 16)
            if v >= 0x10 and v % 16 == 0 and (v >> 4) in VARNAMES:
                ev.append(('v', VARNAMES[v >> 4], at))
        if mn == 'add' and ', 0x' in ops and ops.startswith('e') and not ops.startswith(('esp', 'ebp')):
            v = int(ops.split(', ')[1], 16)
            # self/global yyvars slot: base + id*16
            if v >= 0x10 and v % 16 == 0 and (v >> 4) in VARNAMES:
                ev.append(('v', VARNAMES[v >> 4], at))
        if mn == 'call':
            if ops.startswith('dword ptr [e') and arg0 in VARNAMES:
                # CInstance::GetYYVarRef(id) slow path through the vtable
                ev.append(('v', VARNAMES[arg0], at))
            elif ops.startswith('0x'):
                t = int(ops, 16)
                if t not in ACCESSORS and t not in NAMEOF and t not in FADD and t != 0x401070:
                    for a_ in (arg1, arg2):
                        if a_ is not None and a_ < 100000:
                            ev.append(('i', '%d' % a_, at))
                if t in ACCESSORS and arg1 in VARNAMES:
                    ev.append(('v', VARNAMES[arg1], at))
                if t in ACCESSORS and arg0 is not None and 0 <= arg0 < 0x10000:
                    # obj.var: target is an object index (PC asset order)
                    ev.append(('r', 'P0:%d' % arg0, at))
                cn = NAMEOF.get(t) or FADD.get(t)
                ev.append(('c', cn if cn else 'sub_%x' % t, at))
            arg0 = arg1 = arg2 = None
    pair_words(ev)
    r = summarize(ev, line_slot)
    r.update(addr=addr, size=hi - lo, line_slot=str(line_slot))
    return name, r


def main():
    init()
    only = sys.argv[1:]
    names = [n for n in pc.funcs if not only or any(o in n for o in only)]
    od = os.path.join(HERE, '..', 'out')
    if only:
        for n in names:
            r = extract(n)[1]
            print(n, r['dbl'])
            print(json.dumps(r['bylines'])[:4000])
        return
    with mp.Pool(min(12, os.cpu_count()), initializer=init) as p:
        out = dict(p.imap_unordered(extract, names, chunksize=20))
    json.dump(out, open(os.path.join(od, 'pc_features.json'), 'w'))
    json.dump(dict(vars=sorted(set(VARNAMES.values())), funcslots=sorted(set(FUNCSLOT.values())),
                   nvars=len(VARNAMES), nfadd=len(FADD)), open(os.path.join(od, 'pc_names.json'), 'w'))
    print(len(out), 'functions', len(VARNAMES), 'vars', len(FUNCSLOT), 'funcslots', len(FADD), 'Function_Add')


if __name__ == '__main__':
    main()
