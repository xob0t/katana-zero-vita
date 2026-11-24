"""Readers and writers of Android variables (libyoyo.so), and presence of the name in the PC exe.

usage: python varrefs.py NAME [NAME...]  (or @file with one name per line) -> out/varrefs.txt (appended)
Classification per reference: after the varid load, the instance vtable slot +0xc (writable ref) or
Variable_SetValue* = write; vtable +8 or Variable_GetValue* = read; '?' otherwise. Compound assignments
(x += 1) take a writable ref and show as write."""
import sys, os, json, capstone
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'decomp'))
OUT = os.path.join(HERE, '..', 'out')
A = json.load(open(os.path.join(OUT, 'android_features.json')))
os.chdir(os.path.join(HERE, '..', '..', 'decomp'))
from annotate import Binary, short  # noqa: E402
EXE = r"C:/Program Files (x86)/Steam/steamapps/common/Katana ZERO/Katana ZERO.exe"


def classify(b, md, addr, size, var):
    code = b.data[b.off(addr):b.off(addr) + size]
    insns = [i for i in md.disasm(code, addr)]
    sym = 'g_VAR_' + var
    got = {a for a, n in b.got.items() if n == sym}
    out = []
    for k, ins in enumerate(insns):
        # ldr rX, [pc, rY] where the slot is g_VAR_var: look back for the literal is costly; use the annotate trick:
        if ins.mnemonic != 'ldr' or '[pc, ' not in ins.op_str or ins.op_str.count(',') != 2:
            continue
        # find literal load of rY just before
        ry = ins.op_str.split('[pc, ')[1].rstrip(']')
        lit = None
        for j in range(k - 1, max(0, k - 12), -1):
            p = insns[j]
            if p.mnemonic == 'ldr' and p.op_str.startswith(ry + ', [pc, #'):
                disp = int(p.op_str.split('#')[1].rstrip(']'), 0)
                lit = b.u32(p.address + 8 + disp)
                break
        if lit is None or ((ins.address + 8 + lit) & 0xffffffff) not in got:
            continue
        kind = '?'
        for j in range(k + 1, min(len(insns), k + 20)):
            p = insns[j]
            if p.mnemonic == 'blx' and not p.op_str.startswith('#'):
                rn = p.op_str.strip()
                for q in range(j - 1, max(0, k - 12), -1):
                    x = insns[q]
                    if x.mnemonic == 'ldr' and x.op_str.startswith(rn + ', [') and x.op_str.endswith(('#0xc]', '#8]')):
                        kind = 'W' if x.op_str.endswith('#0xc]') else 'R'
                        break
                break
            if p.mnemonic in ('bl', 'blx') and p.op_str.startswith('#'):
                n = short(b.name_at(int(p.op_str[1:], 16)) or '') or ''
                if 'SetValue' in n or 'LVAL' in n:
                    kind = 'W'
                    break
                if 'GetValue' in n:
                    kind = 'R'
                    break
        out.append((ins.address, kind))
    return out


def main():
    names = []
    for a in sys.argv[1:]:
        names += open(a[1:]).read().split() if a.startswith('@') else [a]
    b = Binary('../apk110/lib/armeabi-v7a/libyoyo.so')
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    exe = open(EXE, 'rb').read()
    with open(os.path.join(OUT, 'varrefs.txt'), 'a') as f:
        for v in names:
            on_pc = (b'\0' + v.encode() + b'\0') in exe
            fs = [(n, r) for n, r in A.items() if v in r['vars']]
            f.write(f'## {v}  (name in PC exe: {"yes" if on_pc else "no"})  functions: {len(fs)}\n')
            for n, r in sorted(fs):
                refs = classify(b, md, r['addr'], r['size'], v)
                kinds = ''.join(sorted({k for _, k in refs})) or '-'
                lines = sorted({int(l) for l, (_, toks) in r['bylines'].items() if 'v:' + v in toks})
                f.write(f'   {kinds:3s} {n} @{r["addr"]:#x} lines {lines[:12]}\n')
            f.flush()
            print(v, len(fs))


if __name__ == '__main__':
    main()
