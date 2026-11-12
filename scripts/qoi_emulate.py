"""Runs the runner's own ReadQOIFFile (libyoyo.so, ARM) in Unicorn on QOI streams and compares the pixels.

Checks that a re-encoded texture page decodes exactly as intended before it goes near the Vita (a page the
runner can't decode crashes the GPU). Validates itself on the original page first.

Needs unicorn and pyelftools (pip install unicorn pyelftools) and the APK's runner extracted to SO below.
usage: python scripts/qoi_emulate.py [game.droid]
"""
import bz2
import struct
import sys

sys.path.insert(0, "scripts")
import unicorn as U
from unicorn.arm_const import UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, \
    UC_ARM_REG_R3, UC_ARM_REG_SP
from elftools.elf.elffile import ELFFile
from PIL import Image

from extract_art import GameData, decode_gm_qoi
from vita_glyphs import encode_gm_qoi, patched_pages

SO = "work/apk110/lib/armeabi-v7a/libyoyo.so"
READ_QOI = 0xE80414
ALLOC, FREE, MEMCLR8, STACK_CHK_FAIL = 0x19EDD00, 0x19EDCE0, 0x19F2580, 0x19ECDF0
GUARD_GOT = 0x4B3FED4  # GOT slot holding &__stack_chk_guard
HEAP, HEAP_SIZE = 0x60000000, 0x08000000
STACK, STACK_SIZE = 0x70000000, 0x00100000
GUARD = 0x7F000000
RET = 0x7F001000  # return address: emulation stops here


def page_align(v, up=False):
    return (v + 0xFFF) & ~0xFFF if up else v & ~0xFFF


class Runner:
    def __init__(self):
        self.mu = U.Uc(U.UC_ARCH_ARM, U.UC_MODE_ARM)
        self.mu.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20)  # enable VFP/NEON
        self.mu.reg_write(UC_ARM_REG_FPEXC, 1 << 30)
        elf = ELFFile(open(SO, "rb"))
        for seg in elf.iter_segments():
            if seg["p_type"] != "PT_LOAD":
                continue
            lo, hi = page_align(seg["p_vaddr"]), page_align(seg["p_vaddr"] + seg["p_memsz"], True)
            self.mu.mem_map(lo, hi - lo)
            self.mu.mem_write(seg["p_vaddr"], seg.data())
        self.mu.mem_map(HEAP, HEAP_SIZE)
        self.mu.mem_map(STACK, STACK_SIZE)
        self.mu.mem_map(GUARD, 0x2000)
        self.mu.mem_write(GUARD, struct.pack("<I", 0x12345678))
        self.mu.mem_write(GUARD_GOT, struct.pack("<I", GUARD))
        self.mu.hook_add(U.UC_HOOK_CODE, self.stub, begin=ALLOC, end=ALLOC)
        self.mu.hook_add(U.UC_HOOK_CODE, self.stub, begin=FREE, end=FREE)
        self.mu.hook_add(U.UC_HOOK_CODE, self.stub, begin=MEMCLR8, end=MEMCLR8)
        self.mu.hook_add(U.UC_HOOK_CODE, self.stub, begin=STACK_CHK_FAIL, end=STACK_CHK_FAIL)
        self.brk = HEAP

    def stub(self, mu, addr, size, _):
        r0, r1 = mu.reg_read(UC_ARM_REG_R0), mu.reg_read(UC_ARM_REG_R1)
        if addr == ALLOC:
            ptr = self.brk
            self.brk = (self.brk + r0 + 15) & ~15
            if self.brk > HEAP + HEAP_SIZE:
                raise MemoryError("emulated heap exhausted")
            mu.reg_write(UC_ARM_REG_R0, ptr)
        elif addr == MEMCLR8:
            mu.mem_write(r0, bytes(r1))
        elif addr == STACK_CHK_FAIL:
            raise RuntimeError("stack check failed")
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def read_qoi(self, blob):
        """Returns (width, height, pixel bytes) as ReadQOIFFile produces them, or None on failure."""
        self.brk = HEAP
        src = self.brk
        self.mu.mem_write(src, blob)
        self.brk = (src + len(blob) + 15) & ~15
        out_w, out_h = self.brk, self.brk + 4
        self.brk += 16
        self.mu.reg_write(UC_ARM_REG_R0, src)
        self.mu.reg_write(UC_ARM_REG_R1, len(blob))
        self.mu.reg_write(UC_ARM_REG_R2, out_w)
        self.mu.reg_write(UC_ARM_REG_R3, out_h)
        self.mu.reg_write(UC_ARM_REG_SP, STACK + STACK_SIZE - 0x100)
        self.mu.reg_write(UC_ARM_REG_LR, RET)
        self.mu.emu_start(READ_QOI, RET)
        pixels = self.mu.reg_read(UC_ARM_REG_R0)
        if not pixels:
            return None
        w, h = struct.unpack("<II", self.mu.mem_read(out_w, 8))
        return w, h, bytes(self.mu.mem_read(pixels, w * h * 4))


def page_qoi(game, n):
    base = game.chunks["TXTR"]
    entry = game.u32(base + 4 + 4 * n)
    blob = game.d[game.u32(entry + 24):game.u32(entry + 24) + game.u32(entry + 8)]
    return bz2.decompress(blob[12:]) if blob[:4] == b"2zoq" else blob


def check(runner, qoi, expected, label):
    res = runner.read_qoi(qoi)
    if res is None:
        print(f"{label}: ReadQOIFFile FAILED")
        return False
    w, h, pixels = res
    # The runner writes channels in stream order: red and blue swapped relative to the image
    r, g, b, a = expected.split()
    ok = (w, h) == expected.size and pixels == Image.merge("RGBA", (b, g, r, a)).tobytes()
    print(f"{label}: {w}x{h}, {'identical' if ok else 'MISMATCH'}")
    return ok


def main():
    data = open(sys.argv[1] if len(sys.argv) > 1 else "work/apk110/assets/assets/game.droid", "rb").read()
    game = GameData(data)
    runner = Runner()
    ok = True
    for n, img in patched_pages(data).items():
        orig = page_qoi(game, n)
        ok &= check(runner, orig, decode_gm_qoi(orig), f"page {n} original (harness check)")
        new = encode_gm_qoi(img)
        ok &= check(runner, new, img, f"page {n} re-encoded ({len(orig)} -> {len(new)} bytes)")
    print("ALL OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
