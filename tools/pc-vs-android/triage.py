"""Score matched pairs from out/match.json; write out/triage.tsv sorted by score and print a summary.
Line sets are aligned by the most common offset (2.3 script wrappers shift script lines)."""
import json, os, collections, sys
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, '..', 'out')
M = json.load(open(os.path.join(OUT, 'match.json')))
ANDONLY = set(open(os.path.join(OUT, 'android_only_vars.txt')).read().split())

# builtin instance variables that 2.3 exposes as g_VAR (Android) but the PC binary accesses as builtins
BUILTIN = set('''x y xprevious yprevious hspeed vspeed speed direction friction gravity image_index image_speed image_xscale
image_yscale image_angle image_alpha image_blend sprite_index mask_index depth visible solid persistent object_index id
room room_width room_height room_speed alarm bbox_left bbox_right bbox_top bbox_bottom sprite_width sprite_height
image_number fps fps_real current_time mouse_x mouse_y keyboard_lastkey application_surface view_camera view_current
async_load event_data undefined infinity pointer_null'''.split())


def align(pl, al):
    if not pl or not al:
        return 0, len(set(pl) ^ set(al))
    c = collections.Counter(a - p for p in pl for a in al if abs(a - p) < 40)
    best = None
    for off, _ in c.most_common(5):
        mis = len({p + off for p in pl} ^ set(al))
        if best is None or mis < best[1]:
            best = (off, mis)
    return best or (0, len(set(pl) ^ set(al)))


def main():
    rows = []
    for r in M['rows']:
        off, lmis = align(r['pc_lines'], r['and_lines'])
        va = [v for v in r['var_and_only'] if v not in BUILTIN]
        vp = [v for v in r['var_pc_only'] if v not in BUILTIN]
        dd = len(r['dbl_pc_only']) + len(r['dbl_and_only'])
        cc = len(r['call_pc_only']) + len(r['call_and_only'])
        andonly_vars = [v for v in va if v in ANDONLY]
        score = lmis * 3 + dd * 2 + len(va) + len(vp) + cc + 10 * len(andonly_vars)
        r.update(line_off=off, line_mis=lmis, var_and_only=va, var_pc_only=vp, andonly_vars=andonly_vars, score=score)
        rows.append(r)
    rows.sort(key=lambda r: -r['score'])
    json.dump(rows, open(os.path.join(OUT, 'triage.json'), 'w'), indent=0)
    zero = sum(1 for r in rows if r['score'] == 0)
    same_lines = sum(1 for r in rows if r['line_mis'] == 0)
    print('pairs', len(rows), 'score0', zero, 'identical line sets (after offset)', same_lines,
          'line offsets', collections.Counter(r['line_off'] for r in rows).most_common(6))
    with open(os.path.join(OUT, 'triage.tsv'), 'w') as f:
        f.write('score\tname\tline_off\tline_mis\tdbl_pc_only\tdbl_and_only\tvar_pc_only\tvar_and_only\tcall_pc_only\tcall_and_only\n')
        for r in rows:
            if r['score']:
                f.write('\t'.join(str(x) for x in (r['score'], r['name'], r['line_off'], r['line_mis'], r['dbl_pc_only'],
                                                   r['dbl_and_only'], r['var_pc_only'], r['var_and_only'],
                                                   r['call_pc_only'], r['call_and_only'])) + '\n')


if __name__ == '__main__':
    main()
