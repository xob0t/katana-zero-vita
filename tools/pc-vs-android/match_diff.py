"""Match PC and Android GML functions by name and diff their features.
writes out/match.json (counts, lists) and out/diff.tsv (one row per matched pair with any difference)."""
import json, os, collections, re
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'out')
P = json.load(open(os.path.join(OUT, 'pc_features.json')))
A = json.load(open(os.path.join(OUT, 'android_features.json')))
PN = json.load(open(os.path.join(OUT, 'pc_names.json')))
AN = json.load(open(os.path.join(OUT, 'android_names.json')))

NOISE_DBL = {0.0, 0.5, 1e-10, 1.0, 1e-5}
pcvars, avars = set(PN['vars']), set(AN['vars'])


def norm_calls(c):
    out = collections.Counter()
    for k, v in c.items():
        if k.startswith('gml_'):
            out[k.replace('gml_GlobalScript_', 'gml_Script_')] += v
        elif k.startswith('F:'):
            out[k] += v
    return out


def dbls(f):
    return collections.Counter({float(k): v for k, v in f['dbl'].items() if float(k) not in NOISE_DBL})


# renamed events: PC name -> Android name (structural moves, GUID-named collision events)
ALIAS = {f'gml_Object_obj_{o}_Step_0': f'gml_Object_obj_{o}_Other_{u}' for o, u in [
    ('camera', 15), ('continuegame', 15), ('cutscene_camera', 15), ('cutscene_master', 10), ('lightmaster', 10),
    ('master', 10), ('miscmaster', 15), ('multiplayer_camera', 15), ('multiplayer_master', 10), ('musicmaster', 15),
    ('opto_master', 15), ('replay_controller', 15), ('savegame', 15), ('speedrun_master', 15), ('stealthmaster', 15),
    ('testcamera', 15), ('transition', 15), ('transition_fade', 15)]}
ALIAS.update({
    'gml_Object_obj_enemy_slash_Collision_be0cdff6_1fdb_4c8d_9b46_000605fa573d': 'gml_Object_obj_enemy_slash_Collision_obj_slash',
    'gml_Object_obj_explosion_Collision_e61f8e37_a50c_4ebc_9e59_25e8bb598461': 'gml_Object_obj_explosion_Collision_obj_door',
    'gml_Object_obj_slash_Collision_be0cdff6_1fdb_4c8d_9b46_000605fa573d': 'gml_Object_obj_slash_Collision_obj_slash',
    'gml_Object_obj_slash_Collision_e61f8e37_a50c_4ebc_9e59_25e8bb598461': 'gml_Object_obj_slash_Collision_obj_door',
    'gml_Object_obj_slash_Collision_e643c6b4_f85c_4f18_b50e_b301ff55f515': 'gml_Object_obj_slash_Collision_obj_enemy_slash',
})
for k, v in ALIAS.items():
    if k in P and v in A and v not in P:
        A[k] = dict(A[v], alias=v)


def main():
    pn = set(P)
    an = (set(A) - {v for k, v in ALIAS.items() if k in A}) | {k for k in ALIAS if k in A}
    matched = sorted(pn & an)
    pc_only = sorted(pn - an)
    and_only = sorted(an - pn)
    rows = []
    for n in matched:
        p, a = P[n], A[n]
        pd, ad = dbls(p), dbls(a)
        d_pc = sorted(k for k in pd if k not in ad)
        d_an = sorted(k for k in ad if k not in pd)
        cnt = sorted((k, pd[k], ad[k]) for k in pd if k in ad and pd[k] != ad[k])
        pv, av = set(p['vars']), set(a['vars'])
        # only flag a var if the other side's binary knows the name at all (else it is reported as side-only var)
        v_pc = sorted(pv - av)
        v_an = sorted(av - pv)
        pcc, anc = norm_calls(p['calls']), norm_calls(a['calls'])
        c_pc = sorted(k for k in pcc if k not in anc)
        c_an = sorted(k for k in anc if k not in pcc)
        s_pc = sorted(set(p['strs']) - set(a['strs']))
        s_an = sorted(set(a['strs']) - set(p['strs']))
        pl, al = p['lines'], a['lines']
        lmax = (max(pl) if pl else None, max(al) if al else None)
        lcnt = (len(pl), len(al))
        rows.append(dict(name=n, and_name=a.get('alias', n), pc_addr=hex(p['addr']), and_addr=hex(a['addr']), pc_size=p['size'], and_size=a['size'],
                         dbl_pc_only=d_pc, dbl_and_only=d_an, dbl_count=cnt, var_pc_only=v_pc, var_and_only=v_an,
                         call_pc_only=c_pc, call_and_only=c_an, str_pc_only=s_pc, str_and_only=s_an,
                         line_max=lmax, line_count=lcnt, pc_lines=pl, and_lines=al))
    json.dump(dict(matched=len(matched), pc_only=pc_only, and_only=and_only, rows=rows),
              open(os.path.join(OUT, 'match.json'), 'w'), indent=0)
    print('PC', len(pn), 'Android', len(an), 'matched', len(matched), 'pc_only', len(pc_only), 'and_only', len(and_only))
    print('android-only var names', len(avars - pcvars), 'pc-only var names', len(pcvars - avars))
    json.dump(dict(android_only=sorted(avars - pcvars), pc_only=sorted(pcvars - avars)),
              open(os.path.join(OUT, 'var_names_diff.json'), 'w'), indent=0)


if __name__ == '__main__':
    main()
