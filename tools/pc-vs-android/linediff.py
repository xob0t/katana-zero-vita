"""Line-level diff of matched PC/Android GML functions.

Each function becomes a list of GML source lines in code order, each with a token multiset (doubles, variables,
gml/builtin calls, strings). Lines are aligned with difflib on a structural signature (variables + calls), then
aligned line pairs are compared on all tokens. Output: out/linediff.json and out/linediff.txt.

Noise suppressed (calibrated on functions known to be unchanged, see REPORT.md):
- doubles the x86 compiler emits for int conversion / rounding (2^-7, 2^-11, 2^-15, 2^31), 1e-10 epsilon, 0, 0.5, 1
- builtin instance variables (Android 2.3 exposes them as g_VAR, PC uses builtin accessors)
- calls the 2.3 compiler inserts (F:Global, F:Other, F:instance_exists, array helpers)
"""
import json, os, sys, collections, difflib
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'out')
sys.path.insert(0, HERE)
from match_diff import ALIAS  # noqa: E402  (also loads the feature files)
import match_diff  # noqa: E402

P, A = match_diff.P, match_diff.A
sys.path.insert(0, os.path.join(HERE, '..', '..', 'decomp'))
import assets  # noqa: E402  (work/decomp/assets.py: asset name lists of a GameMaker data file)
DATA_WIN = r'C:/Program Files (x86)/Steam/steamapps/common/Katana ZERO/data.win'
_cwd = os.getcwd()
os.chdir(os.path.join(HERE, '..', '..', 'decomp'))
AN_ASSETS = assets.load()
os.chdir(_cwd)
PC_ASSETS = assets.load(DATA_WIN)
PC_INDEX = {c: {n: i for i, n in enumerate(lst)} for c, lst in PC_ASSETS.items()}
REFTYPE = {0: 'OBJT', 1: 'SPRT', 2: 'SOND', 3: 'ROOM', 4: 'BGND', 5: 'PATH', 6: 'SCPT', 7: 'FONT', 8: 'TMLN', 10: 'SHDR'}
ASSET_CHUNKS = ('OBJT', 'SPRT', 'SOND', 'ROOM', 'BGND')


def ref_name(v):
    t, i = v.split(':')
    src = AN_ASSETS
    if t.startswith('P'):
        t, src = t[1:], PC_ASSETS
    t, i = int(t), int(i)
    c = REFTYPE.get(t)
    lst = src.get(c, [])
    return 'a:%s:%s' % (c, lst[i] if c and i < len(lst) else '#%d' % i)


def asset_equiv(pt, at):
    """PC token pt (a double) and Android token at name the same asset: Android 'a:CHUNK:name' with the PC index
    of that name, or two integral doubles that index same-named assets of one chunk in each build."""
    if not pt.startswith('d:'):
        return False
    v = float(pt[2:])
    if v != int(v) or v < 0:
        return False
    v = int(v)
    if at.startswith('a:'):
        _, c, n = at.split(':', 2)
        return PC_INDEX.get(c, {}).get(n) == v
    if at.startswith('d:'):
        w = float(at[2:])
        if w != int(w) or w < 0:
            return False
        w = int(w)
        for c in ASSET_CHUNKS:
            pl, al = PC_ASSETS.get(c, []), AN_ASSETS.get(c, [])
            if v < len(pl) and w < len(al) and pl[v] == al[w] and pl[v] is not None:
                return True
    return False
NOISE_D = {0.0, 0.5, 1.0, 1e-10, 0.0078125, 3.0517578125e-05, 0.00048828125, 2147483648.0, 4294967296.0, 1e-05}
BUILTIN = set('''x y xprevious yprevious xstart ystart hspeed vspeed speed direction friction gravity gravity_direction
image_index image_speed image_xscale image_yscale image_angle image_alpha image_blend sprite_index mask_index depth
visible solid persistent object_index id room room_width room_height room_speed alarm bbox_left bbox_right bbox_top
bbox_bottom sprite_width sprite_height image_number fps fps_real current_time mouse_x mouse_y keyboard_lastkey
application_surface view_camera view_current async_load event_data undefined infinity pointer_null path_position
path_speed timeline_position timeline_index layer instance_count instance_id view_xview view_yview view_wview view_hview
current_second delta_time sprite_xoffset sprite_yoffset room_persistent keyboard_key keyboard_string score lives health
argument argument0 argument1 argument2 argument3 argument4 argument5 argument6 argument7 argument_count self other
global all noone true false pi'''.split())
NOISE_C = {'F:Global', 'F:Other', 'F:instance_exists', 'F:NewGMLArray', 'F:@@NewGMLArray@@', 'F:array_length_1d',
           'F:array_length', 'F:@@This@@', 'F:@@Other@@', 'F:@@Global@@', 'F:method', 'F:@@NewGMLObject@@'}


def keep(t):
    k, v = t.split(':', 1)
    if k == 'd':
        return float(v) not in NOISE_D
    if k == 'v':
        return v not in BUILTIN
    if k == 'c':
        c = v.replace('gml_GlobalScript_', 'gml_Script_')
        return c not in NOISE_C and (c.startswith('gml_') or c.startswith('F:'))
    return True


def norm(t):
    if t.startswith('r:'):
        return ref_name(t[2:])
    return t.replace('gml_GlobalScript_', 'gml_Script_')


def lines_of(f):
    out = []
    for ln, (addr, toks) in f['bylines'].items():
        out.append((int(ln), addr, frozenset(norm(t) for t in toks if keep(t))))
    out.sort()
    return out


def sig(ts):
    return tuple(sorted(t for t in ts if not t.startswith(('d:', 'a:', 'i:'))))


WINDOW = 3
DWINDOW = 15


def diff_pair(p, a):
    """Align lines in source order; tokens are per-line sets (the x86 code tests a variable's fast and slow path,
    so multiplicity is not comparable). Tokens that differ on one row but reappear on the other side within
    WINDOW rows are code motion (hoisted constants, merged lines) and cancel."""
    pl, al = lines_of(p), lines_of(a)
    sm = difflib.SequenceMatcher(None, [sig(x[2]) for x in pl], [sig(x[2]) for x in al], autojunk=False)
    rows = []
    for op, i1, i2, j1, j2 in sm.get_opcodes():
        n = max(i2 - i1, j2 - j1)
        for k in range(n):
            x = pl[i1 + k] if i1 + k < i2 else None
            y = al[j1 + k] if j1 + k < j2 else None
            rows.append([x, y, set(x[2]) if x else set(), set(y[2]) if y else set()])
    def ival(t):
        return 'i:%d' % float(t[2:]) if t.startswith('d:') and float(t[2:]) == int(float(t[2:])) else None
    for r in rows:
        common = r[2] & r[3]
        r[2] -= common
        r[3] -= common
    # integer operands ('i:') only cancel a same-valued double on the other side within DWINDOW, never reported
    for i, r in enumerate(rows):
        near = range(max(0, i - DWINDOW), min(len(rows), i + DWINDOW + 1))
        for side, other in ((2, 1), (3, 0)):
            for t in list(r[side]):
                iv = ival(t)
                if iv and any(rows[j][other] and iv in rows[j][other][2] for j in near):
                    r[side].discard(t)
        r[2] = {t for t in r[2] if not t.startswith('i:')}
        r[3] = {t for t in r[3] if not t.startswith('i:')}
    for i, r in enumerate(rows):
        for t in list(r[2]):
            # ARM keeps a constant in a VFP register across statements (one load), x86 reloads it per statement
            w = DWINDOW if t.startswith('d:') else WINDOW
            for j in sorted(range(max(0, i - w), min(len(rows), i + w + 1)), key=lambda j: abs(j - i)):
                if t in rows[j][3] or (t.startswith('d:') and rows[j][1] and t in rows[j][1][2]):
                    r[2].discard(t)
                    if not t.startswith('d:'):
                        rows[j][3].discard(t)
                    break
    # Android-only constants that PC uses within DWINDOW rows (reused register on ARM, the other way round)
    for i, r in enumerate(rows):
        for t in list(r[3]):
            if t.startswith('d:') and any(t in rows[j][2] or t in (rows[j][0][2] if rows[j][0] else ())
                                          for j in range(max(0, i - DWINDOW), min(len(rows), i + DWINDOW + 1))):
                r[3].discard(t)
    # asset references: PC uses plain doubles (its own asset order), Android kind-15 refs or doubles (its order)
    for i, r in enumerate(rows):
        for t in list(r[2]):
            for j in range(max(0, i - WINDOW), min(len(rows), i + WINDOW + 1)):
                hit = next((u for u in rows[j][3] if asset_equiv(t, u)), None)
                if hit:
                    r[2].discard(t)
                    rows[j][3].discard(hit)
                    break
    res = []
    for x, y, pt, at in rows:
        if pt or at:
            res.append(dict(kind='changed' if x and y else ('pc_only_line' if x else 'android_only_line'),
                            pc_line=x[0] if x else None, pc_addr=hex(x[1]) if x else None,
                            and_line=y[0] if y else None, and_addr=hex(y[1]) if y else None,
                            pc=sorted(pt), android=sorted(at)))
    return res


def main():
    pn = set(P)
    an = (set(A) - {v for k, v in ALIAS.items() if k in A}) | {k for k in ALIAS if k in A}
    matched = sorted(pn & an)
    out = {}
    stats = collections.Counter()
    for n in matched:
        r = diff_pair(P[n], A[n])
        stats['pairs'] += 1
        stats['lines_pc'] += len(P[n]['bylines'])
        stats['lines_android'] += len(A[n]['bylines'])
        if r:
            out[n] = dict(and_name=A[n].get('alias', n), pc_addr=hex(P[n]['addr']), and_addr=hex(A[n]['addr']), diffs=r)
            stats['pairs_with_diff'] += 1
            stats['diff_hunks'] += len(r)
            if any(any(t.startswith('d:') for t in h['pc'] + h['android']) for h in r):
                stats['pairs_with_const_diff'] += 1
        else:
            stats['pairs_identical'] += 1
    json.dump(dict(stats=stats, diffs=out), open(os.path.join(OUT, 'linediff.json'), 'w'), indent=0)
    with open(os.path.join(OUT, 'linediff.txt'), 'w') as f:
        for n, d in out.items():
            f.write(f'== {n}  pc {d["pc_addr"]}  android {d["and_addr"]} {d["and_name"] if d["and_name"] != n else ""}\n')
            for h in d['diffs']:
                f.write(f'  ~ PC L{h["pc_line"]}@{h["pc_addr"]} / AND L{h["and_line"]}@{h["and_addr"]}: '
                        f'PC {h["pc"]}  AND {h["android"]}\n')
    print(dict(stats))


if __name__ == '__main__':
    main()
