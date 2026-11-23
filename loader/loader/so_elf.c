#include <string.h>

#include "so_elf.h"

static int in_range(uint64_t limit, uint64_t offset, uint64_t size) {
	return offset <= limit && size <= limit - offset;
}

// Return the PT_LOAD segment containing the entire range.
static const Elf32_Phdr *mapped_range(const so_elf *elf, Elf32_Addr addr, size_t size, int file_backed) {
	for (int i = 0; i < elf->header->e_phnum; i++) {
		const Elf32_Phdr *p = &elf->segments[i];
		if (p->p_type == PT_LOAD && addr >= p->p_vaddr &&
			in_range(file_backed ? p->p_filesz : p->p_memsz, addr - p->p_vaddr, size))
			return p;
	}
	return NULL;
}

static int validate_segments(const so_elf *elf, size_t size, uint32_t load_addr) {
	int loads = 0;
	uint64_t end = 0;
	for (int i = 0; i < elf->header->e_phnum; i++) {
		const Elf32_Phdr *p = &elf->segments[i];
		if (!in_range(size, p->p_offset, p->p_filesz))
			return 0;
		if (p->p_type != PT_LOAD)
			continue;
		// _so_load maps one executable segment at the base and up to four
		// subsequent data segments. Each allocation starts at the previous end.
		uint64_t align = p->p_align;
		if (!align || (align & (align - 1)) || !p->p_memsz || p->p_filesz > p->p_memsz ||
			p->p_vaddr < end || (!loads && p->p_vaddr) ||
			(!!(p->p_flags & PF_X) != (loads == 0)) || loads > MAX_DATA_SEG)
			return 0;
		uint64_t bytes = (uint64_t)p->p_vaddr + p->p_memsz - end;
		end += (bytes + align - 1) & ~(align - 1);
		if (end > UINT32_MAX - load_addr)
			return 0;
		loads++;
	}
	return loads > 0;
}

static int validate_hash(const uint32_t *words, size_t n, size_t symbols, int gnu) {
	if (n < (gnu ? 4u : 2u) || !words[0])
		return 0;
	uint32_t buckets = words[0];
	if (gnu) {
		uint32_t first = words[1], bloom = words[2];
		if (first > symbols || !bloom || !in_range(n, 4, (uint64_t)bloom + buckets + symbols - first))
			return 0;
		for (size_t i = 0; i < buckets; i++) {
			uint32_t index = words[4 + bloom + i];
			if (index && (index < first || index >= symbols))
				return 0;
		}
	} else {
		if (words[1] != symbols || !in_range(n, 2, (uint64_t)buckets + symbols))
			return 0;
		for (size_t i = 2; i < 2 + buckets + symbols; i++)
			if (words[i] >= symbols)
				return 0;
	}
	return 1;
}

static int validate_dynamic(const unsigned char *data, so_elf *elf) {
	const Elf32_Shdr *const *t = elf->tables;
	const Elf32_Dyn *entries = (const void *)(data + t[SO_DYNAMIC]->sh_offset);
	size_t count = t[SO_DYNAMIC]->sh_size / sizeof(*entries);
	for (size_t i = 0; i < count; i++) {
		uint32_t value = entries[i].d_un.d_val;
		const Elf32_Shdr *section;
		switch (entries[i].d_tag) {
		case DT_NULL: elf->dynamic_count = i; return 1;
		case DT_NEEDED:
		case DT_SONAME:
			if (value >= t[SO_DYNSTR]->sh_size) return 0;
			continue;
		case DT_RELENT: if (value != sizeof(Elf32_Rel)) return 0; continue;
		case DT_SYMENT: if (value != sizeof(Elf32_Sym)) return 0; continue;
		case DT_PLTREL: if (value != DT_REL) return 0; continue;
		case DT_RELSZ: if (value != t[SO_RELDYN]->sh_size) return 0; continue;
		case DT_PLTRELSZ: if (value != t[SO_RELPLT]->sh_size) return 0; continue;
		case DT_STRSZ: if (value != t[SO_DYNSTR]->sh_size) return 0; continue;
		case DT_INIT_ARRAYSZ:
			if (value != (t[SO_INIT_ARRAY] ? t[SO_INIT_ARRAY]->sh_size : 0)) return 0;
			continue;
		case DT_STRTAB: section = t[SO_DYNSTR]; break;
		case DT_SYMTAB: section = t[SO_DYNSYM]; break;
		case DT_REL: section = t[SO_RELDYN]; break;
		case DT_JMPREL: section = t[SO_RELPLT]; break;
		case DT_INIT_ARRAY: section = t[SO_INIT_ARRAY]; break;
		case DT_HASH: section = t[SO_HASH]; break;
		case DT_GNU_HASH: section = t[SO_GNU_HASH]; break;
		default: continue;
		}
		if (!section || value != section->sh_addr)
			return 0;
	}
	return 0;
}

int so_elf_validate(const void *buffer, size_t size, uint32_t load_addr, so_elf *elf) {
	static const struct {
		const char *name;
		Elf32_Word type;
		size_t stride;
	} tables[] = {
		[SO_DYNAMIC] = { ".dynamic", SHT_DYNAMIC, sizeof(Elf32_Dyn) },
		[SO_DYNSTR] = { ".dynstr", SHT_STRTAB, 1 },
		[SO_DYNSYM] = { ".dynsym", SHT_DYNSYM, sizeof(Elf32_Sym) },
		[SO_RELDYN] = { ".rel.dyn", SHT_REL, sizeof(Elf32_Rel) },
		[SO_RELPLT] = { ".rel.plt", SHT_REL, sizeof(Elf32_Rel) },
		[SO_INIT_ARRAY] = { ".init_array", SHT_INIT_ARRAY, 4 },
		[SO_HASH] = { ".hash", SHT_HASH, 4 },
		[SO_GNU_HASH] = { ".gnu.hash", SHT_GNU_HASH, 4 },
	};
	const unsigned char *data = buffer;
	const Elf32_Ehdr *h = buffer;
	if (!buffer || ((uintptr_t)buffer & 3) || size < sizeof(*h) || size > UINT32_MAX ||
		memcmp(h->e_ident, ELFMAG, SELFMAG) || h->e_ident[EI_CLASS] != ELFCLASS32 ||
		h->e_ident[EI_DATA] != ELFDATA2LSB || h->e_ident[EI_VERSION] != EV_CURRENT ||
		h->e_type != ET_DYN || h->e_machine != EM_ARM || h->e_version != EV_CURRENT ||
		h->e_ehsize != sizeof(*h) || h->e_phentsize != sizeof(Elf32_Phdr) ||
		h->e_shentsize != sizeof(Elf32_Shdr) || !h->e_shstrndx || h->e_shstrndx >= h->e_shnum ||
		((h->e_phoff | h->e_shoff) & 3) ||
		!in_range(size, h->e_phoff, (size_t)h->e_phnum * sizeof(Elf32_Phdr)) ||
		!in_range(size, h->e_shoff, (size_t)h->e_shnum * sizeof(Elf32_Shdr)))
		return 0;

	*elf = (so_elf){ .header = h, .segments = (const void *)(data + h->e_phoff) };
	if (!validate_segments(elf, size, load_addr))
		return 0;
	const Elf32_Shdr *sections = (const void *)(data + h->e_shoff);
	const Elf32_Shdr *names = &sections[h->e_shstrndx];
	if (names->sh_type != SHT_STRTAB || !names->sh_size ||
		!in_range(size, names->sh_offset, names->sh_size) ||
		data[names->sh_offset + names->sh_size - 1] != '\0')
		return 0;
	const char *shstr = (const void *)(data + names->sh_offset);
	for (int i = 0; i < h->e_shnum; i++) {
		const Elf32_Shdr *s = &sections[i];
		if (s->sh_name >= names->sh_size ||
			(s->sh_type != SHT_NOBITS && !in_range(size, s->sh_offset, s->sh_size)))
			return 0;
		for (int j = 0; j < SO_TABLE_COUNT; j++) {
			if (strcmp(shstr + s->sh_name, tables[j].name))
				continue;
			const Elf32_Phdr *p = mapped_range(elf, s->sh_addr, s->sh_size, 1);
			if (elf->tables[j] || s->sh_type != tables[j].type || s->sh_size % tables[j].stride ||
				(tables[j].stride > 1 && ((s->sh_addr | s->sh_offset) & 3)) ||
				!p || s->sh_offset != p->p_offset + (s->sh_addr - p->p_vaddr))
				return 0;
			elf->tables[j] = s;
		}
	}

	const Elf32_Shdr *const *t = elf->tables;
	if (!t[SO_DYNAMIC] || !t[SO_DYNSYM] || !t[SO_RELDYN] || !t[SO_RELPLT] ||
		!t[SO_DYNSTR] || !t[SO_DYNSTR]->sh_size ||
		data[t[SO_DYNSTR]->sh_offset + t[SO_DYNSTR]->sh_size - 1] != '\0' ||
		!validate_dynamic(data, elf))
		return 0;

	const Elf32_Sym *symbols = (const void *)(data + t[SO_DYNSYM]->sh_offset);
	size_t count = t[SO_DYNSYM]->sh_size / sizeof(*symbols);
	if (!count)
		return 0;
	for (size_t i = 0; i < count; i++)
		if (symbols[i].st_name >= t[SO_DYNSTR]->sh_size)
			return 0;

	for (int j = SO_RELDYN; j <= SO_RELPLT; j++) {
		const Elf32_Rel *rels = (const void *)(data + t[j]->sh_offset);
		for (size_t i = 0; i < t[j]->sh_size / sizeof(*rels); i++) {
			const Elf32_Phdr *p = mapped_range(elf, rels[i].r_offset, sizeof(Elf32_Addr), 0);
			// Relocations are applied with normal stores. The executable segment is RX on Vita.
			if (ELF32_R_SYM(rels[i].r_info) >= count || (rels[i].r_offset & 3) ||
				!p || (p->p_flags & PF_X))
				return 0;
		}
	}
	for (int j = SO_HASH; j <= SO_GNU_HASH; j++)
		if (t[j] && !validate_hash((const void *)(data + t[j]->sh_offset), t[j]->sh_size / 4, count, j == SO_GNU_HASH))
			return 0;
	return 1;
}
