"""Function-level variable-set differences (non-builtin names referenced on one side only).
-> out/vardiff.json, out/vardiff.txt. Recall differs between extractors (x86 caches variable pointers), so an
entry is a lead, not evidence."""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from linediff import BUILTIN, P, A  # noqa: E402

OUT = os.path.join(HERE, '..', 'out')


def main():
    out = {}
    for n in sorted(set(P) & set(A)):
        pv = set(P[n]['vars']) - BUILTIN
        av = set(A[n]['vars']) - BUILTIN
        po, ao = sorted(pv - av), sorted(av - pv)
        if po or ao:
            out[n] = (po, ao)
    json.dump(out, open(os.path.join(OUT, 'vardiff.json'), 'w'), indent=0)
    with open(os.path.join(OUT, 'vardiff.txt'), 'w') as f:
        for n, (po, ao) in out.items():
            f.write(f'{n[4:]} | PC-only {po} | AND-only {ao}\n')
    print(len(out), 'functions with variable-set differences')


if __name__ == '__main__':
    main()
