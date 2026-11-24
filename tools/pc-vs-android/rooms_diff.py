"""Compare room instance placement (ROOM chunk) between PC data.win and Android game.droid.
Each instance -> (object name, x, y). Reports per room the instances present on one side only.
-> out/rooms_diff.txt"""
import struct, sys, os, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'decomp'))
os.chdir(os.path.join(HERE, '..', '..', 'decomp'))
import assets  # noqa: E402

DATA_WIN = r'C:/Program Files (x86)/Steam/steamapps/common/Katana ZERO/data.win'
DROID = '../apk110/assets/assets/game.droid'


def rooms(path):
    D = open(path, 'rb').read()
    u = lambda a: struct.unpack_from('<I', D, a)[0]
    names = assets.load(path)
    OBJ = names['OBJT']
    pos, chunks = 8, {}
    while pos < len(D):
        chunks[D[pos:pos + 4].decode()] = (pos + 8, u(pos + 4))
        pos += 8 + u(pos + 4)
    p = chunks['ROOM'][0]
    out = {}
    for i in range(u(p)):
        e = u(p + 4 + i * 4)
        rname = names['ROOM'][i]
        w, h, spd = u(e + 8), u(e + 12), u(e + 16)
        objs_ptr = u(e + 48)
        inst = collections.Counter()
        for j in range(u(objs_ptr)):
            ie = u(objs_ptr + 4 + j * 4)
            x, y, oi = struct.unpack_from('<iii', D, ie)
            inst[(OBJ[oi] if 0 <= oi < len(OBJ) else oi, x, y)] += 1
        out[rname] = dict(size=(w, h), speed=spd, inst=inst)
    return out


def main():
    pc, an = rooms(DATA_WIN), rooms(DROID)
    lines = []
    st = collections.Counter()
    for r in sorted(set(pc) | set(an)):
        if r not in pc or r not in an:
            lines.append(f'{r}: only in {"PC" if r in pc else "Android"}')
            st['room_one_side'] += 1
            continue
        a, b = pc[r], an[r]
        st['rooms'] += 1
        st['inst_pc'] += sum(a['inst'].values())
        st['inst_an'] += sum(b['inst'].values())
        po, ao = a['inst'] - b['inst'], b['inst'] - a['inst']
        hdr = []
        if a['size'] != b['size']:
            hdr.append(f'size PC {a["size"]} Android {b["size"]}')
        if a['speed'] != b['speed']:
            hdr.append(f'speed PC {a["speed"]} Android {b["speed"]}')
        if po or ao or hdr:
            st['rooms_differ'] += 1
            lines.append(f'{r}: {"; ".join(hdr)}')
            for k, v in sorted(po.items(), key=str):
                lines.append(f'   PC only  {v}x {k}')
            for k, v in sorted(ao.items(), key=str):
                lines.append(f'   AND only {v}x {k}')
    open(os.path.join(HERE, '..', 'out', 'rooms_diff.txt'), 'w').write('\n'.join(lines) + '\n')
    print(dict(st))


if __name__ == '__main__':
    main()
