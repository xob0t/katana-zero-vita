"""Checks the loader's QOI decoder (loader/loader/gm_qoi.h) against the runner's own ReadQOIFFile.

Every texture page of an LZ4 game.apk (made by prepare_apk.py) is decoded twice: by the runner's ReadQOIFFile, run in
Unicorn (scripts/qoi_emulate.py), and by gm_qoi.h built for the host with gcc. The pixels must be identical and the
stream fully consumed. The loader decodes the title screen's pages with gm_qoi.h (loader/loader/qoi_pages.c).

Needs gcc on PATH and the runner extracted to qoi_emulate.SO.
usage: uv run --with unicorn --with pyelftools --with pillow --with lz4 python scripts/gm_qoi_check.py game.apk
"""
import os
import subprocess
import sys
import tempfile
import zipfile

import lz4.block

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qoi_emulate
from prepare_apk import parse_chunks, u32

HOST_TEST = r'''
#include <stdio.h>
#include <stdlib.h>
#include "gm_qoi.h"
int main(int argc, char **argv) {
	FILE *f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc(n);
	fread(d, 1, n, f);
	fclose(f);
	uint32_t w = d[4] | d[5] << 8, h = d[6] | d[7] << 8;
	uint32_t len = d[8] | d[9] << 8 | d[10] << 16 | (uint32_t)d[11] << 24;
	uint32_t *out = malloc((size_t)w * h * 4);
	printf("%d\n", gm_qoi_decode(d + 12, d + 12 + len, out, w * h));
	f = fopen(argv[2], "wb");
	fwrite(out, 4, (size_t)w * h, f);
	fclose(f);
	return 0;
}
'''


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    tmp = tempfile.mkdtemp()
    src, exe = os.path.join(tmp, 'gm_qoi_test.c'), os.path.join(tmp, 'gm_qoi_test.exe')
    open(src, 'w').write(HOST_TEST)
    subprocess.run(['gcc', '-O2', '-I', os.path.join(root, 'loader', 'loader'), '-o', exe, src], check=True)

    z = zipfile.ZipFile(sys.argv[1])
    data = z.read([n for n in z.namelist() if n.endswith('game.droid')][0])
    _, txtr, _ = [c for c in parse_chunks(data) if c[0] == 'TXTR'][0]
    runner = qoi_emulate.Runner()
    bad = 0
    for n in range(u32(data, txtr + 8)):
        e = u32(data, txtr + 12 + 4 * n)
        blob = data[u32(data, e + 24):u32(data, e + 24) + u32(data, e + 8)]
        if blob[:4] != b'2zoq' or blob[12:16] != b'LZ4H':
            print(f'page {n}: not an LZ4 page, skipped')
            continue
        qoi = lz4.block.decompress(blob[20:20 + u32(blob, 16)], uncompressed_size=u32(blob, 8))
        pin, pout = os.path.join(tmp, 'p.fioq'), os.path.join(tmp, 'p.raw')
        open(pin, 'wb').write(qoi)
        left = int(subprocess.run([exe, pin, pout], capture_output=True, text=True, check=True).stdout)
        w, h, ref = runner.read_qoi(qoi)
        ok = left == 0 and open(pout, 'rb').read() == ref
        bad += not ok
        print(f'page {n}: {w}x{h} {"identical" if ok else "MISMATCH"}' + (f' ({left} stream bytes left)' if left else ''))
    print('ALL IDENTICAL' if not bad else f'{bad} pages differ')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
