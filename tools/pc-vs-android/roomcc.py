"""Room instance creation code (gml_RoomCC_<room>_<n>_Create): the per-room index n differs between builds, so
match by content within each room. Cost of a pair = number of tokens (vars, numbers, calls, strings, asset names)
present on one side only, after asset-index normalisation (funcdiff rules). Greedy minimum-cost matching.
-> out/roomcc.txt: pairs whose best match still differs, plus unmatched entries."""
import json, os, re, sys, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import linediff as LD  # noqa: E402
import funcdiff as FD  # noqa: E402

P, A = LD.P, LD.A


def toks(f):
    s = set()
    for ln, (addr, t) in f['bylines'].items():
        for x in t:
            if not LD.keep(x) or x.startswith('i:'):
                continue
            x = LD.norm(x)
            if x.startswith('d:') and float(x[2:]) in FD.NOISE_D:
                continue
            s.add(x)
    return s


def cost(pn, an):
    ps, as_ = toks(P[pn]), toks(A[an])
    po, ao = ps - as_, as_ - ps
    # asset normalisation like funcdiff
    pa = set()
    for t in ps:
        if t.startswith('d:'):
            pa |= FD.asset_names_pc(float(t[2:]))
        elif t.startswith('a:'):
            pa.add(tuple(t.split(':', 2)[1:]))
    aa = set()
    for t in as_:
        if t.startswith('d:'):
            aa |= FD.asset_names_an(float(t[2:]))
        elif t.startswith('a:'):
            aa.add(tuple(t.split(':', 2)[1:]))
    po = {t for t in po if not ((t.startswith('d:') and FD.asset_names_pc(float(t[2:])) & aa) or
                                (t.startswith('a:') and tuple(t.split(':', 2)[1:]) in aa))}
    ao = {t for t in ao if not ((t.startswith('d:') and FD.asset_names_an(float(t[2:])) & pa) or
                                (t.startswith('a:') and tuple(t.split(':', 2)[1:]) in pa))}
    return len(po) + len(ao), sorted(po), sorted(ao)


def main():
    rooms = collections.defaultdict(lambda: ([], []))
    for side, F in ((0, P), (1, A)):
        for n in F:
            m = re.match(r'gml_RoomCC_(.+)_(\d+)_Create$', n)
            if m:
                rooms[m.group(1)][side].append(n)
    lines, stats = [], collections.Counter()
    for r, (pl, al) in sorted(rooms.items()):
        cands = sorted((cost(p, a)[0], p, a) for p in pl for a in al)
        up, ua, pairs = set(), set(), []
        for c, p, a in cands:
            if p in up or a in ua:
                continue
            up.add(p)
            ua.add(a)
            pairs.append((p, a))
        for p, a in pairs:
            c, po, ao = cost(p, a)
            stats['pairs'] += 1
            if p.rsplit('_', 2)[1] != a.rsplit('_', 2)[1]:
                stats['renumbered'] += 1
            if c:
                stats['differs'] += 1
                lines.append(f'{p[11:]} ~ {a[11:]}  PC-only {po}  Android-only {ao}')
            else:
                stats['identical'] += 1
        for p in set(pl) - up:
            lines.append(f'{p[11:]}  PC only (no Android counterpart)')
            stats['pc_only'] += 1
        for a in set(al) - ua:
            lines.append(f'{a[11:]}  Android only')
            stats['android_only'] += 1
    open(os.path.join(LD.OUT, 'roomcc.txt'), 'w').write('\n'.join(lines) + '\n')
    print(dict(stats))


if __name__ == '__main__':
    main()
