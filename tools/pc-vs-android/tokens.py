"""Shared event-stream helpers for and_extract.py / pc_extract.py.

An extractor appends events while walking a function in address order:
  ('d', value, addr)              double constant
  ('v', name, addr)               variable reference (instance, global or other.name)
  ('c', name, addr)               call (gml_* script/event, F:builtin via function-id slot, or runtime helper)
  ('s', text, addr)               string literal
  ('W', slot, word, addr, n)      32-bit literal store to slot=(base, disp); n = instruction index
GML source line markers are 'W' stores to the line slot (name slot + 4).
"""
import struct, collections


def hi2d(hi, lo=0):
    return struct.unpack('<d', struct.pack('<II', lo & 0xffffffff, hi & 0xffffffff))[0]


def is_hi(w):
    """High word of a double with a plausible GML magnitude (~1e-9 .. 1e10)."""
    return w is not None and 0x3e0 <= (w >> 20) & 0x7ff <= 0x420


def pair_words(ev, maxgap=8):
    """Turn 32-bit literal stores into doubles: a hi word pairs with a lo word stored to slot-4 within maxgap
    instructions; an unpaired hi word with zero low 16 bits is a double whose lo word is 0 (stored elsewhere)."""
    ws = [i for i, e in enumerate(ev) if e[0] == 'W']
    used = set()
    # GMS 2.3 asset references (RValue kind 15): type word 0x0100000T at slot+4, asset index at slot
    for k, i in enumerate(ws):
        e = ev[i]
        w = e[2]
        if w is None or not (0x01000000 <= w <= 0x0100000f):
            continue
        for j in ws[max(0, k - 4):k + 5]:
            f = ev[j]
            if j != i and j not in used and f[1] == (e[1][0], e[1][1] - 4) and abs(f[4] - e[4]) <= maxgap                     and f[2] is not None and f[2] < 0x10000:
                used.update((i, j))
                ev[i] = ('r', '%d:%d' % (w & 0xf, f[2]), e[3])
                break
    for k, i in enumerate(ws):
        e = ev[i]
        if e[0] != 'W':
            continue
        w = e[2]
        if i in used or not is_hi(w):
            continue
        lo = None
        for j in ws[max(0, k - 4):k + 5]:
            f = ev[j]
            if j != i and j not in used and f[1] == (e[1][0], e[1][1] - 4) and abs(f[4] - e[4]) <= maxgap:
                lo = (j, f[2])
                break
        if lo and lo[1] and (lo[1] < 0x100000 or bin(lo[1]).count('1') <= 2) and (w & 0xffff) == 0:
            lo = None   # a small int / type word stored next to a round hi word, not a mantissa
        if lo:
            used.add(lo[0])
            v = hi2d(w, lo[1])
        elif (w & 0xffff) == 0:
            v = hi2d(w)
        else:
            continue
        used.add(i)
        ev[i] = ('d', abs(v), e[3])


def summarize(ev, line_slot):
    """-> lines, bylines {line: [first_addr, tokens]}, dbl Counter, vars set, calls Counter, strs set."""
    dbl, vars_, calls, strs = collections.Counter(), set(), collections.Counter(), set()
    lines = set()
    out = {}
    line = -1
    for e in ev:
        if e[0] == 'W':
            if e[1] == line_slot and e[2] is not None and 0 <= e[2] < 100000:
                line = e[2]
                lines.add(line)
                out.setdefault(str(line), [e[3], []])
            continue
        k, v = e[0], e[1]
        if k == 'i':
            out.setdefault(str(line), [e[2], []])[1].append('i:' + v)
            continue
        if k == 'd':
            dbl[v] += 1
        elif k == 'v':
            vars_.add(v)
        elif k == 'c':
            calls[v] += 1
            if not v.startswith(('gml_', 'F:')):
                continue
        elif k == 's':
            strs.add(v)
        elif k == 'r':
            calls['ref:' + v] += 1
        out.setdefault(str(line), [e[2], []])[1].append(k + ':' + (repr(v) if k == 'd' else v))
    return dict(lines=sorted(lines), bylines=out, dbl={repr(k): v for k, v in dbl.items()}, vars=sorted(vars_),
                calls=dict(calls), strs=sorted(strs))
