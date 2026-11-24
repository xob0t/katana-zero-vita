"""usage: python -I pcconst.py NAME LINESLOT  -> (gml line, double constant / gml call) list for a PC YYC function"""
import re, struct, sys, subprocess, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pcfunc import d, va2off
asm = subprocess.run([sys.executable, '-I', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pcdis.py'), sys.argv[1], sys.argv[3] if len(sys.argv) > 3 else '60000'], capture_output=True, text=True).stdout.split('\n')
slot = sys.argv[2]
if slot == 'auto':
    for ln in asm:
        m = re.search(r'\[esp \+ (0x[0-9a-f]+)\], 0x[0-9a-f]+ +"gml_', ln)
        if m: slot = hex(int(m.group(1), 16) + 4); break
cur = None; prev = None; started = False
for ln in asm:
    if '"gml_' in ln:
        if started: break
        started = True
    m = re.search(r'\[esp \+ ' + slot + r'\], (0x[0-9a-f]+|\d+)', ln)
    if m: cur = int(m.group(1), 0)
    x = None
    m2 = re.search(r'qword ptr \[(0x[0-9a-f]+)\]', ln)
    if m2: x = (cur, struct.unpack_from('<d', d, va2off(int(m2.group(1), 16)))[0])
    elif ' call ' in ln and 'gml_' in ln: x = (cur, ln.split()[-1])
    if x and x != prev and x[1] != 1e-10: print(x)
    if x: prev = x
