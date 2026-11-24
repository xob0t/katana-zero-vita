"""Function-level diff (robust to line attribution): numbers and gml calls present on only one side.

A number counts as present if it appears anywhere in the function. PC integral numbers that index an asset whose
name Android references (kind-15 ref or its own index) are asset references and cancel.
Output: out/funcdiff.json, out/funcdiff.txt (each finding with the PC/Android lines where the value occurs)."""
import json, os, sys, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import linediff as LD  # noqa: E402

P, A = LD.P, LD.A
NOISE_D = LD.NOISE_D | {16777216.0, 4294967295.0}


def tokens(f, pc):
    """-> {token: [lines]} for d:, a:, c:gml_ tokens."""
    out = collections.defaultdict(list)
    for ln, (addr, toks) in f['bylines'].items():
        for t in toks:
            if not LD.keep(t):
                continue
            t = LD.norm(t)
            if t.startswith('d:'):
                if float(t[2:]) in NOISE_D:
                    continue
            elif t.startswith('c:gml_') or t.startswith('a:') or t.startswith('i:'):
                pass
            else:
                continue
            out[t].append(int(ln))
    return out


def asset_names_pc(v):
    if v != int(v) or v < 0:
        return set()
    v = int(v)
    return {(c, LD.PC_ASSETS[c][v]) for c in LD.ASSET_CHUNKS if v < len(LD.PC_ASSETS.get(c, []))}


def asset_names_an(v):
    if v != int(v) or v < 0:
        return set()
    v = int(v)
    return {(c, LD.AN_ASSETS[c][v]) for c in LD.ASSET_CHUNKS if v < len(LD.AN_ASSETS.get(c, []))}


def diff(n):
    pt, at = tokens(P[n], True), tokens(A[n], False)
    # integer operands of runtime operators ('i:') are seen with different recall on the two sides: they only
    # cancel a double of the same value on the other side and are never reported themselves
    def present(t, other):
        if t in other:
            return True
        if t.startswith('d:'):
            v = float(t[2:])
            return v == int(v) and ('i:%d' % v) in other
        return False
    pc_only = {t: l for t, l in pt.items() if not t.startswith('i:') and not present(t, at)}
    an_only = {t: l for t, l in at.items() if not t.startswith('i:') and not present(t, pt)}
    # asset cancellation
    an_assets = set()
    for t in at:
        if t.startswith('a:'):
            _, c, nm = t.split(':', 2)
            an_assets.add((c, nm))
        elif t.startswith('d:'):
            an_assets |= asset_names_an(float(t[2:]))
    pc_assets = set()
    for t in pt:
        if t.startswith('a:'):
            _, c, nm = t.split(':', 2)
            pc_assets.add((c, nm))
        elif t.startswith('d:'):
            pc_assets |= asset_names_pc(float(t[2:]))
    # x86 folds division by a constant into multiplication by its reciprocal (and the other way round)
    def has_recip(t, other):
        v = float(t[2:])
        return v != 0 and any(o.startswith('d:') and abs(float(o[2:]) * v - 1) < 1e-9 for o in other)
    for t in list(pc_only):
        if t.startswith('d:') and has_recip(t, at):
            del pc_only[t]
    for t in list(an_only):
        if t.startswith('d:') and has_recip(t, pt):
            del an_only[t]
    for t in list(pc_only):
        if t.startswith('d:') and asset_names_pc(float(t[2:])) & an_assets:
            del pc_only[t]
        elif t.startswith('a:') and tuple(t.split(':', 2)[1:]) in an_assets:
            del pc_only[t]
    for t in list(an_only):
        if t.startswith('a:') and tuple(t.split(':', 2)[1:]) in pc_assets:
            del an_only[t]
        elif t.startswith('d:') and asset_names_an(float(t[2:])) & pc_assets:
            del an_only[t]
    return pc_only, an_only


def main():
    out = {}
    stats = collections.Counter()
    for n in sorted(set(P) & set(A)):
        pc_only, an_only = diff(n)
        stats['pairs'] += 1
        d1 = {t: l for t, l in pc_only.items() if t.startswith('d:')}
        d2 = {t: l for t, l in an_only.items() if t.startswith('d:')}
        c1 = {t: l for t, l in pc_only.items() if t.startswith(('c:', 'a:'))}
        c2 = {t: l for t, l in an_only.items() if t.startswith(('c:', 'a:'))}
        if d1 or d2:
            stats['pairs_const_diff'] += 1
        if c1 or c2:
            stats['pairs_call_or_asset_diff'] += 1
        if d1 or d2 or c1 or c2:
            out[n] = dict(pc_addr=hex(P[n]['addr']), and_addr=hex(A[n]['addr']), pc_const=d1, and_const=d2,
                          pc_calls=c1, and_calls=c2)
    json.dump(dict(stats=stats, diffs=out), open(os.path.join(LD.OUT, 'funcdiff.json'), 'w'), indent=0)
    with open(os.path.join(LD.OUT, 'funcdiff.txt'), 'w') as f:
        for n, d in out.items():
            f.write(f'== {n}  pc {d["pc_addr"]}  android {d["and_addr"]}\n')
            for k in ('pc_const', 'and_const', 'pc_calls', 'and_calls'):
                if d[k]:
                    f.write(f'   {k}: ' + ', '.join(f'{t[2:]}@L{sorted(set(l))[:6]}' for t, l in sorted(d[k].items())) + '\n')
    print(dict(stats))


if __name__ == '__main__':
    main()
