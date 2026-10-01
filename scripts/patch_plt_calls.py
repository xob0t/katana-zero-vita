"""Rewrites calls that go through the PLT (optionally via a linker veneer) into direct `bl`
instructions when the callee is defined inside libyoyo.so itself and is in branch range.

YYC code calls runner helpers ~60k times per frame through veneer -> PLT -> GOT hops; on the
Vita's Cortex-A9 each hop is an extra indirect branch. Branch offsets are PC-relative, so the
patch is independent of where the loader maps the library.

Usage: python scripts/patch_plt_calls.py <in.so> <out.so> [--exclude sym ...] [--keep-scripts]
"""
import argparse
import bisect
import struct

from elftools.elf.elffile import ELFFile

# Symbols the loader intercepts through their GOT slot must keep going through the PLT
DEFAULT_EXCLUDE = {
    "_Z12DoAStep_Drawv",  # frame skip / lighting hooks
    "_Z24gml_Script_inside_screenP9CInstanceS0_R8YYRValueiPPS1_",  # native replacement (scripts_native.c)
    "BZ2_bzBuffToBuffDecompress",  # LZ4 texture pages (katana.c); a missed hook breaks every texture
}

BL_RANGE = 32 * 1024 * 1024


def rot_imm(ins):
    imm, r = ins & 0xff, ((ins >> 8) & 0xf) * 2
    return ((imm >> r) | (imm << (32 - r))) & 0xffffffff


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--exclude", nargs="*", default=[])
    ap.add_argument("--keep-scripts", action="store_true",
                    help="leave every gml_Script_* call on the PLT (profiling builds, see katana.c scriptprof)")
    args = ap.parse_args()
    exclude = DEFAULT_EXCLUDE | set(args.exclude)

    data = bytearray(open(args.src, "rb").read())
    elf = ELFFile(open(args.src, "rb"))
    text = elf.get_section_by_name(".text")
    plt = elf.get_section_by_name(".plt")
    dynsym = elf.get_section_by_name(".dynsym")
    # vaddr == file offset for the text segment of this library
    assert text["sh_addr"] == text["sh_offset"] and plt["sh_addr"] == plt["sh_offset"]
    text_lo, text_hi = text["sh_addr"], text["sh_addr"] + text["sh_size"]
    plt_lo, plt_hi = plt["sh_addr"], plt["sh_addr"] + plt["sh_size"]

    w = lambda a: struct.unpack_from("<I", data, a)[0]

    # GOT slot -> defined symbol (only symbols defined in this library can be called directly)
    slot_target = {}
    for rel in elf.get_section_by_name(".rel.plt").iter_relocations():
        sym = dynsym.get_symbol(rel["r_info_sym"])
        if sym["st_shndx"] != "SHN_UNDEF" and sym["st_value"] and sym.name not in exclude                 and not (args.keep_scripts and "gml_Script_" in sym.name):
            slot_target[rel["r_offset"]] = (sym["st_value"], sym.name)

    # ARM-mode function ranges (thumb functions have bit 0 set)
    arm_funcs = sorted((s["st_value"], s["st_value"] + s["st_size"]) for s in dynsym.iter_symbols()
                       if s["st_info"]["type"] == "STT_FUNC" and s["st_value"] and not s["st_value"] & 1
                       and s["st_size"] and text_lo <= s["st_value"] < text_hi)
    starts = [a for a, _ in arm_funcs]

    def in_arm_func(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and arm_funcs[i][0] <= addr < arm_funcs[i][1]

    def plt_entry_target(entry):
        # add ip, pc, #a ; add ip, ip, #b ; ldr pc, [ip, #c]!
        p0, p1, p2 = w(entry), w(entry + 4), w(entry + 8)
        if (p0 & 0xfffff000) != 0xe28fc000 or (p1 & 0xfffff000) != 0xe28cc000 or (p2 & 0xfffff000) != 0xe5bcf000:
            return None
        return slot_target.get(entry + 8 + rot_imm(p0) + rot_imm(p1) + (p2 & 0xfff))

    def veneer_target(v):
        # movw ip, #lo ; movt ip, #hi ; add ip, ip, pc ; bx ip
        i0, i1, i2, i3 = w(v), w(v + 4), w(v + 8), w(v + 12)
        if (i0 & 0xfff0f000) != 0xe300c000 or (i1 & 0xfff0f000) != 0xe340c000 or i2 != 0xe08cc00f or i3 != 0xe12fff1c:
            return None
        lo = ((i0 >> 4) & 0xf000) | (i0 & 0xfff)
        hi = ((i1 >> 4) & 0xf000) | (i1 & 0xfff)
        return (v + 16 + ((hi << 16) | lo)) & 0xffffffff

    cache = {}

    def resolve(target):
        if target in cache:
            return cache[target]
        final = None
        if plt_lo <= target < plt_hi:
            final = plt_entry_target(target)
        else:
            nxt = veneer_target(target) if text_lo <= target < text_hi - 16 else None
            if nxt is not None and plt_lo <= nxt < plt_hi:
                final = plt_entry_target(nxt)
        cache[target] = final
        return final

    patched = out_of_range = thumb_target = 0
    names = {}
    for addr in range(text_lo, text_hi - 3, 4):
        ins = w(addr)
        if (ins & 0xff000000) != 0xeb000000:  # unconditional bl <imm24>
            continue
        off = ins & 0xffffff
        if off & 0x800000:
            off -= 0x1000000
        target = (addr + 8 + (off << 2)) & 0xffffffff
        final = resolve(target)
        if final is None or not in_arm_func(addr):
            continue
        dest, name = final
        if dest & 1:
            thumb_target += 1
            continue
        delta = dest - (addr + 8)
        if not -BL_RANGE <= delta < BL_RANGE:
            out_of_range += 1
            continue
        struct.pack_into("<I", data, addr, 0xeb000000 | ((delta >> 2) & 0xffffff))
        patched += 1
        names[name] = names.get(name, 0) + 1

    # Calls that are out of bl range keep their veneer, but the veneer itself can jump
    # straight to the function instead of going through the PLT (still PC-relative)
    veneers = 0
    for addr in range(text_lo, text_hi - 15, 4):
        if (w(addr) & 0xfff0f000) != 0xe300c000:
            continue
        nxt = veneer_target(addr)
        if nxt is None or not plt_lo <= nxt < plt_hi:
            continue
        final = plt_entry_target(nxt)
        if final is None:
            continue
        rel = (final[0] - (addr + 16)) & 0xffffffff
        lo, hi = rel & 0xffff, rel >> 16
        struct.pack_into("<I", data, addr, 0xe300c000 | ((lo >> 12) << 16) | (lo & 0xfff))
        struct.pack_into("<I", data, addr + 4, 0xe340c000 | ((hi >> 12) << 16) | (hi & 0xfff))
        veneers += 1

    open(args.dst, "wb").write(data)
    print(f"patched {patched} call sites ({out_of_range} out of range, {thumb_target} thumb targets skipped)")
    print(f"retargeted {veneers} veneers straight to their functions")
    for name, n in sorted(names.items(), key=lambda kv: -kv[1])[:15]:
        print(f"  {n:6d} {name}")


if __name__ == "__main__":
    main()
