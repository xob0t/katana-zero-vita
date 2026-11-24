"""Reduce out/funcdiff.json to a hand-review list (out/const_review.txt, out/const_review.json).
Drops as weak (counted, listed in REPORT.md as noise classes):
- integers 0..12 (array indices / argument slots / enum operands the two compilers encode differently)
- large powers of two (x86 int<->double conversion helpers)
- values equal within 1e-6 relative (float32 rounding on x86, decimal literal precision)
- 2^-7 rounding constant variants"""
import json, os, math
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'out')
D = json.load(open(os.path.join(OUT, 'funcdiff.json')))['diffs']


def close(a, b):
    return abs(a - b) <= 1e-6 * max(abs(a), abs(b), 1e-9)


def weak(t, other):
    v = float(t[2:])
    if v == int(v) and 0 <= v <= 12:
        return 'smallint'
    if v >= 2 ** 19 and v == 2 ** round(math.log2(v)):
        return 'pow2'
    if abs(v - 0.0078125) < 1e-8:
        return 'round'
    if any(close(float(o[2:]), v) for o in other if o.startswith('d:')):
        return 'approx'
    return None


def main():
    keep, dropped = {}, {}
    for n, d in D.items():
        pc, an = {}, {}
        for src, dst, oth in ((d['pc_const'], pc, d['and_const']), (d['and_const'], an, d['pc_const'])):
            for t, l in src.items():
                w = weak(t, oth)
                if w:
                    dropped[w] = dropped.get(w, 0) + 1
                else:
                    dst[t[2:]] = sorted(set(l))
        if pc or an:
            keep[n] = dict(pc=pc, android=an, pc_addr=d['pc_addr'], and_addr=d['and_addr'])
    json.dump(keep, open(os.path.join(OUT, 'const_review.json'), 'w'), indent=0)
    with open(os.path.join(OUT, 'const_review.txt'), 'w') as f:
        for n, d in sorted(keep.items()):
            f.write(f'{n[4:]} | PC {d["pc"]} | AND {d["android"]}\n')
    print(len(keep), 'functions to review; dropped', dropped)


if __name__ == '__main__':
    main()
