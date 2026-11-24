"""Side-by-side per-line tokens of one GML function (PC | Android), from the extracted features.
usage: python -I show.py NAME [FROM_LINE TO_LINE]   (NAME without gml_ prefix is fine)
       python -I show.py --var VARNAME [VALUE_FILTER]  -> lines on both sides that reference VARNAME"""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'out')
P = json.load(open(os.path.join(OUT, 'pc_features.json')))
A = json.load(open(os.path.join(OUT, 'android_features.json')))
sys.path.insert(0, HERE)
from match_diff import ALIAS  # noqa: E402

SKIP = ('c:YY', 'c:FREE', 'c:operator', 'c:sub_')


def toks(t):
    return [x for x in t if not x.startswith(SKIP) and x not in ('d:1e-10',)]


def show(name, lo=-1, hi=10 ** 6):
    if not name.startswith('gml_'):
        name = 'gml_' + name
    an = ALIAS.get(name, name)
    p, a = P.get(name), A.get(an)
    print(f'{name}  PC {hex(p["addr"]) if p else None}  Android {an} {hex(a["addr"]) if a else None}')
    lines = sorted({int(k) for f in (p, a) if f for k in f['bylines']})
    for ln in lines:
        if not lo <= ln <= hi:
            continue
        pl = p['bylines'].get(str(ln)) if p else None
        al = a['bylines'].get(str(ln)) if a else None
        ps = f'{pl[0]:#x} {toks(pl[1])}' if pl else ''
        as_ = f'{al[0]:#x} {toks(al[1])}' if al else ''
        print(f'{ln:5d} PC {ps}\n      AN {as_}')


def var(v, filt=None):
    for side, F in (('PC', P), ('AN', A)):
        for n, f in sorted(F.items()):
            for ln, (addr, t) in f['bylines'].items():
                if 'v:' + v in t:
                    ds = [x[2:] for x in t if x.startswith('d:') and x != 'd:1e-10']
                    if filt and filt not in ds:
                        continue
                    print(side, n, ln, hex(addr), ds)


if __name__ == '__main__':
    if sys.argv[1] == '--var':
        var(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    else:
        show(sys.argv[1], *(int(x) for x in sys.argv[2:4]))
