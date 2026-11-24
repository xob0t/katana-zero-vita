"""Site table for timer_slowmo.c: Android max_time/bullettime_max constants and the PC value of the same statement."""
import json, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import const_sites as C
OUT = os.path.join(HERE, '..', 'timingaudit', 'out')
P = json.load(open(os.path.join(OUT, 'pc_features.json')))
AN2PC = {9000: 6000, 10500: 7000, 12000: 8000, 11850: 8100, 12450: 8300, 12900: 8600, 13500: 9000, 6000: 4000, 760: 380}
PCNAME = {'gml_Object_obj_init_Create_0': 'gml_Object_obj_init_Create_0', 'gml_Script_mobile_reset_game_data': 'gml_Object_obj_init_Create_0',
          'gml_Script_xbox_reset_game_data': 'gml_Object_obj_init_Create_0'}
rows = []
for line in open(os.path.join(HERE, 'maxtime_sites.txt')).read().splitlines() + [
        'gml_Object_obj_player_Other_13 0x462e0e4 0x462e0ec 0x4087c000 760']:
    f = line.split()
    if len(f) != 5:
        continue
    fn, lo, hi, word, val = f[0], int(f[1], 16), int(f[2], 16), int(f[3], 16), int(f[4])
    wl, wh = C.b.u32(lo), C.b.u32(hi)
    assert wl & 0x0ff00000 == 0x03000000 and wh & 0x0ff00000 == 0x03400000, (fn, hex(wl), hex(wh))
    assert (wl >> 12) & 15 == (wh >> 12) & 15
    imm = lambda w: ((w >> 4) & 0xf000) | (w & 0xfff)
    assert (imm(wh) << 16 | imm(wl)) == word, fn
    assert struct.unpack('<d', struct.pack('<Q', word << 32))[0] == val
    pc = AN2PC[val]
    # the PC function must assign exactly this value (its constants of 1000+)
    pf = P[PCNAME.get(fn, fn)]
    pcvals = {float(x[2:]) for _, t in pf['bylines'].values() for x in t if x.startswith('d:')}
    assert float(pc) in pcvals and float(val) not in pcvals or (val == 6000 and fn == 'gml_Object_obj_master_Create_0') \
        or (val == 9000 and 9000.0 in pcvals and float(pc) in pcvals) or fn.endswith('reset_game_data'), (fn, val, pc)
    pcw = struct.unpack('<Q', struct.pack('<d', float(pc)))[0]
    assert pcw & 0xffffffff == 0
    rows.append((lo, hi, val, pc, fn))
rows.sort()
print(len(rows), 'sites')
with open(os.path.join(HERE, 'sites_table.c'), 'w') as o:
    for lo, hi, val, pc, fn in rows:
        o.write('\t{ 0x%07x, 0x%07x, %5d, %4d }, // %s\n' % (lo, hi, val, pc, fn[4:]))
