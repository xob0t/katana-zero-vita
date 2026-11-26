"""Rewrites calls that go through the PLT (optionally via a linker veneer) into direct `bl`
instructions when the callee is defined inside libyoyo.so itself and is in branch range.

YYC code calls runner helpers ~60k times per frame through veneer -> PLT -> GOT hops; on the
Vita's Cortex-A9 each hop is an extra indirect branch. Branch offsets are PC-relative, so the
patch is independent of where the loader maps the library.

Usage: python scripts/patch_plt_calls.py <in.so> <out.so> [--exclude sym ...] [--keep-scripts]
"""
import argparse
import bisect
import hashlib
import io
import struct

from elftools.common.exceptions import ELFError
from elftools.elf.elffile import ELFFile

# Symbols the loader intercepts through their GOT slot must keep going through the PLT
DEFAULT_EXCLUDE = {
    "_Z12DoAStep_Drawv",  # lighting hooks (katana_display.c)
    "BZ2_bzBuffToBuffDecompress",  # LZ4 texture pages (katana.c); a missed hook breaks every texture
}

BL_RANGE = 32 * 1024 * 1024
QOI_READER = "_Z12ReadQOIFFilePviPiS0_b"
BZIP2 = "BZ2_bzBuffToBuffDecompress"

# GNU build ID -> digest of (file offset, original instruction, replacement) for every
# rewrite in the unmodified runner. Check the full plan even with --exclude/--keep-scripts.
EXPECTED_PATCH_SITES = {
    "f186b4cc3ca915130277166840c2df3503b6f8ec": "b7a82f25f563cbe87e58b8f35ba653e7eecde4d0bbfd68c7b45e563385adcf1e",
}
WORD = struct.Struct("<I")
CHANGE = struct.Struct("<III")


def rot_imm(ins):
    imm, r = ins & 0xff, ((ins >> 8) & 0xf) * 2
    return ((imm >> r) | (imm << (32 - r))) & 0xffffffff


def bl_target(addr, ins):
    offset = ins & 0xffffff
    if offset & 0x800000:
        offset -= 0x1000000
    return (addr + 8 + (offset << 2)) & 0xffffffff


def plt_slots(src, plt_lo, plt_hi, rels):
    """Validate each expected ARM PLT stub against its jump-slot relocation."""
    layouts = ((32, 16), (20, 12))  # LLD, GNU ld
    for header, stride in layouts:
        if plt_hi - plt_lo == header + stride * len(rels):
            break
    else:
        raise ValueError("unexpected runner PLT layout")

    slots = {}
    for i, rel in enumerate(rels):
        entry = plt_lo + header + stride * i
        p0, p1, p2 = struct.unpack_from("<III", src, entry)
        slot = entry + 8 + rot_imm(p0) + rot_imm(p1) + (p2 & 0xfff)
        if ((p0 & 0xfffff000, p1 & 0xfffff000, p2 & 0xfffff000) != (0xe28fc000, 0xe28cc000, 0xe5bcf000)
                or rel["r_info_type"] != 22 or slot != rel["r_offset"]):  # R_ARM_JUMP_SLOT
            raise ValueError(f"unexpected runner PLT instructions or relocation at {entry:#x}")
        slots[entry] = slot
    return slots


def patch(src, exclude=(), keep_scripts=False):
    """Returns the patched library bytes and the summary lines for the runner bytes `src`."""
    exclude = set(exclude)
    try:
        elf = ELFFile(io.BytesIO(src))
    except ELFError as exc:
        raise ValueError(f"invalid runner ELF: {exc}") from exc
    if elf["e_type"] != "ET_DYN" or elf["e_machine"] != "EM_ARM" or elf.elfclass != 32 or not elf.little_endian:
        raise ValueError("expected a little-endian 32-bit ARM shared library")
    text = elf.get_section_by_name(".text")
    plt = elf.get_section_by_name(".plt")
    dynsym = elf.get_section_by_name(".dynsym")
    rel_plt = elf.get_section_by_name(".rel.plt")
    if any(section is None for section in (text, plt, dynsym, rel_plt)):
        raise ValueError("runner is missing required ELF sections")
    # vaddr == file offset for the text segment of this library
    for section in (text, plt):
        addr, size = section["sh_addr"], section["sh_size"]
        if addr != section["sh_offset"] or addr % 4 or size % 4 or addr + size > len(src):
            raise ValueError(f"unexpected runner {section.name} section bounds or mapping")
    text_lo, text_hi = text["sh_addr"], text["sh_addr"] + text["sh_size"]
    plt_lo, plt_hi = plt["sh_addr"], plt["sh_addr"] + plt["sh_size"]
    if text_lo < plt_hi and plt_lo < text_hi:
        raise ValueError("runner .text and .plt sections overlap")

    # Resolve every PLT entry so a damaged stub cannot silently disappear from the candidate list.
    rels = list(rel_plt.iter_relocations())
    slots = plt_slots(src, plt_lo, plt_hi, rels)
    slot_target = {}
    bzip_slots = set()
    for rel in rels:
        sym = dynsym.get_symbol(rel["r_info_sym"])
        if sym.name == BZIP2:
            bzip_slots.add(rel["r_offset"])
        if sym["st_shndx"] != "SHN_UNDEF" and sym["st_value"] and sym.name not in DEFAULT_EXCLUDE:
            slot_target[rel["r_offset"]] = (sym["st_value"], sym.name)

    build_note = elf.get_section_by_name(".note.gnu.build-id")
    build_id = next((note["n_desc"] for note in build_note.iter_notes() if note["n_type"] == "NT_GNU_BUILD_ID"),
                    None) if build_note is not None else None
    expected_sites = EXPECTED_PATCH_SITES.get(build_id)
    plan = hashlib.sha256() if expected_sites else None
    data = bytearray(src)
    unpack_word = WORD.unpack_from
    pack_word = WORD.pack_into

    def w(addr):
        return unpack_word(src, addr)[0]

    def rewrite(addr, original, replacement, enabled):
        if plan is not None:
            plan.update(CHANGE.pack(addr, original, replacement))
        if enabled:
            if unpack_word(data, addr)[0] != original:
                raise ValueError(f"runner patch site at {addr:#x} no longer matches the original instruction")
            pack_word(data, addr, replacement)

    def selected(name):
        return name not in exclude and not (keep_scripts and "gml_Script_" in name)

    # ARM-mode function ranges (thumb functions have bit 0 set)
    arm_funcs = sorted((s["st_value"], s["st_value"] + s["st_size"]) for s in dynsym.iter_symbols()
                       if s["st_info"]["type"] == "STT_FUNC" and s["st_value"] and not s["st_value"] & 1
                       and s["st_size"] and text_lo <= s["st_value"] < text_hi)
    starts = [a for a, _ in arm_funcs]

    def in_arm_func(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and arm_funcs[i][0] <= addr < arm_funcs[i][1]

    def veneer_target(v):
        # movw ip, #lo ; movt ip, #hi ; add ip, ip, pc ; bx ip
        i0, i1, i2, i3 = w(v), w(v + 4), w(v + 8), w(v + 12)
        if (i0 & 0xfff0f000) != 0xe300c000 or (i1 & 0xfff0f000) != 0xe340c000 or i2 != 0xe08cc00f or i3 != 0xe12fff1c:
            return None
        lo = ((i0 >> 4) & 0xf000) | (i0 & 0xfff)
        hi = ((i1 >> 4) & 0xf000) | (i1 & 0xfff)
        return (v + 16 + ((hi << 16) | lo)) & 0xffffffff

    def target_slot(target):
        if target in slots:
            return slots[target]
        if text_lo <= target <= text_hi - 16:
            return slots.get(veneer_target(target))
        return None

    # The Vita loader calls this same instruction in its texture self-test. It must go
    # through the bzip2 GOT slot, which the loader replaces with the LZ4 implementation.
    qoi_readers = dynsym.get_symbol_by_name(QOI_READER) or []
    if len(qoi_readers) != 1:
        raise ValueError(f"runner must define exactly one {QOI_READER}")
    reader = qoi_readers[0]
    site = reader["st_value"] + 0xdc
    if (reader["st_info"]["type"] != "STT_FUNC" or site % 4 or not text_lo <= site <= text_hi - 4
            or reader["st_size"] < 0xe0):
        raise ValueError("unexpected runner QOI decoder layout")
    ins = w(site)
    if ins & 0xff000000 != 0xeb000000 or target_slot(bl_target(site, ins)) not in bzip_slots:
        raise ValueError(f"unexpected bzip2 call at runner QOI decoder site {site:#x}")
    if plan is not None:
        plan.update(CHANGE.pack(site, ins, ins))

    cache = {}

    def resolve(target):
        if target not in cache:
            cache[target] = slot_target.get(target_slot(target))
        return cache[target]

    patched = out_of_range = thumb_target = 0
    names = {}
    for addr in range(text_lo, text_hi - 3, 4):
        ins = w(addr)
        if (ins & 0xff000000) != 0xeb000000:  # unconditional bl <imm24>
            continue
        final = resolve(bl_target(addr, ins))
        if final is None or not in_arm_func(addr):
            continue
        dest, name = final
        enabled = selected(name)
        if dest & 1:
            thumb_target += enabled
            continue
        if dest % 4 or not text_lo <= dest <= text_hi - 4:
            raise ValueError(f"invalid ARM branch destination for {name} at {addr:#x}")
        delta = dest - (addr + 8)
        if not -BL_RANGE <= delta < BL_RANGE:
            out_of_range += enabled
            continue
        rewrite(addr, ins, 0xeb000000 | ((delta >> 2) & 0xffffff), enabled)
        if enabled:
            patched += 1
            names[name] = names.get(name, 0) + 1

    # Calls that are out of bl range keep their veneer, but the veneer itself can jump
    # straight to the function instead of going through the PLT (still PC-relative)
    veneers = 0
    for addr in range(text_lo, text_hi - 15, 4):
        if (w(addr) & 0xfff0f000) != 0xe300c000:
            continue
        nxt = veneer_target(addr)
        final = slot_target.get(slots.get(nxt))
        if final is None:
            continue
        dest, name = final
        if (not dest & 1 and dest % 4) or not text_lo <= (dest & ~1) < text_hi:
            raise ValueError(f"invalid veneer destination for {name} at {addr:#x}")
        enabled = selected(name)
        rel = (dest - (addr + 16)) & 0xffffffff
        lo, hi = rel & 0xffff, rel >> 16
        rewrite(addr, w(addr), 0xe300c000 | ((lo >> 12) << 16) | (lo & 0xfff), enabled)
        rewrite(addr + 4, w(addr + 4), 0xe340c000 | ((hi >> 12) << 16) | (hi & 0xfff), enabled)
        veneers += enabled

    if plan is not None and plan.hexdigest() != expected_sites:
        raise ValueError("runner patch sites differ from the tested build")
    lines = [f"patched {patched} call sites ({out_of_range} out of range, {thumb_target} thumb targets skipped)",
             f"retargeted {veneers} veneers straight to their functions"]
    lines += [f"  {n:6d} {name}" for name, n in sorted(names.items(), key=lambda kv: -kv[1])[:15]]
    return bytes(data), lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--exclude", nargs="*", default=[])
    ap.add_argument("--keep-scripts", action="store_true",
                    help="leave every gml_Script_* call on the PLT (for hooks through GOT slots)")
    args = ap.parse_args()
    data, lines = patch(open(args.src, "rb").read(), args.exclude, args.keep_scripts)
    open(args.dst, "wb").write(data)
    print("\n".join(lines))


if __name__ == "__main__":
    main()
