"""Diff the double constants of a GML function between Android (libyoyo annotate) and PC (exe).
usage (cwd work/decomp): python -I ../bulletfreeze/tools/constdiff.py ANDROID_SUBSTR PC_NAME"""
import sys, re, struct, subprocess, collections, os
T = os.path.dirname(os.path.abspath(__file__))
a = subprocess.run([sys.executable, 'annotate.py', sys.argv[1]], capture_output=True, text=True).stdout
ac = collections.Counter()
for m in re.finditer(r'vmov\.f64\s+d\d+, #(\S+)', a): ac[float(m.group(1))] += 1
for m in re.finditer(r'movt\s+\w+, #0x[0-9a-f]+\s+; = (0x[0-9a-f]{8})', a):
    v = struct.unpack('<d', struct.pack('<II', 0, int(m.group(1), 16)))[0]
    if 1e-6 < abs(v) < 1e7: ac[v] += 1
for m in re.finditer(r'; = (-?[0-9.]+(?:e[-+]?\d+)?)$', a, re.M):
    try:
        v = float(m.group(1))
        if '.' in m.group(1): ac[v] += 1
    except ValueError: pass
p = subprocess.run([sys.executable, '-I', os.path.join(T, 'pcdis.py'), sys.argv[2], '400000'], capture_output=True, text=True).stdout
p = p.split('\n'); out = []; n = 0
for ln in p:
    if '"gml_' in ln:
        n += 1
        if n > 1: break
    out.append(ln)
sys.path.insert(0, T)
from pcfunc import d, va2off
pc = collections.Counter()
for ln in out:
    m = re.search(r'qword ptr \[(0x[0-9a-f]+)\]', ln)
    if m:
        o = va2off(int(m.group(1), 16))
        if o: pc[struct.unpack_from('<d', d, o)[0]] += 1
    m = re.search(r', (0x(?:3f|40|c0|bf)[0-9a-f]{6})\s', ln)
    if m: pc[struct.unpack('<d', struct.pack('<II', 0, int(m.group(1), 16)))[0]] += 1
norm = lambda c: collections.Counter({abs(k): v for k, v in c.items() if k not in (0.5, 1e-10)})
A, P = collections.Counter(), collections.Counter()
for k, v in ac.items(): A[abs(k)] += v
for k, v in pc.items():
    if k not in (1e-10,): P[abs(k)] += v
print('Android only:', sorted(k for k in A if k not in P))
print('PC only:', sorted(k for k in P if k not in A))
