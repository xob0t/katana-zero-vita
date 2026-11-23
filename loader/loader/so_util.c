/* so_util.c -- utils to load and hook .so modules
 *
 * Copyright (C) 2021 Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

#include <vitasdk.h>
#include <kubridge.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "main.h"
#include "dialog.h"
#include "so_util.h"

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX_PHYCONT (0x0C80D050)

int text_phycont_result = 0;

// Experiment switches: present if ux0:data/gms/shared/exp_<name> exists
int exp_flag(const char *name) {
	char path[128];
	SceIoStat st;
	snprintf(path, sizeof(path), "ux0:data/gms/shared/exp_%s", name);
	return sceIoGetstat(path, &st) >= 0;
}
#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)
#endif

typedef struct b_enc {
	union {
		struct __attribute__((__packed__)) {
			int imm24: 24;
			unsigned int l: 1; // Branch with Link flag
			unsigned int enc: 3; // 0b101
			unsigned int cond: 4; // 0b1110
		} bits;
		uint32_t raw;
	};
} b_enc;

typedef struct ldst_enc {
	union {
		struct __attribute__((__packed__)) {
			int imm12: 12;
			unsigned int rt: 4; // Source/Destination register
			unsigned int rn: 4; // Base register
			unsigned int bit20_1: 1; // 0: store to memory, 1: load from memory
			unsigned int w: 1; // 0: no write-back, 1: write address into base
			unsigned int b: 1; // 0: word, 1: byte
			unsigned int u: 1; // 0: subtract offset from base, 1: add to base
			unsigned int p: 1; // 0: post indexing, 1: pre indexing
			unsigned int enc: 3;
			unsigned int cond: 4;
		} bits;
		uint32_t raw;
	};
} ldst_enc;

#define B(PC, DEST) ((b_enc){.bits = {.cond = 0b1110, .enc = 0b101, .l = 0, .imm24 = ((int64_t)(DEST) - (uintptr_t)(PC) - 8) / 4}})
#define LDR_OFFS(RT, RN, IMM) ((ldst_enc){.bits = {.cond = 0b1110, .enc = 0b010, .p = 1, .u = (IMM >= 0), .b = 0, .w = 0, .bit20_1 = 1, .rn = RN, .rt = RT, .imm12 = (IMM >= 0) ? IMM : -IMM}})

#define PATCH_SZ 0x10000 //64 KB-ish arenas

// Bumped by every code hook and every GOT slot lookup (the loader looks a slot up to rewrite it), so natives that
// check once for hooked runner functions know when to look again (smokedraw_native.c)
volatile unsigned so_hook_generation;
static so_module *head = NULL, *tail = NULL;

so_hook hook_thumb(uintptr_t addr, uintptr_t dst) {
	so_hook h;
	//printf("THUMB HOOK\n");
	if (addr == 0)
		return;
	so_hook_generation++;
	h.thumb_addr = addr;
	addr &= ~1;
	if (addr & 2) {
		uint16_t nop = 0xbf00;
		kuKernelCpuUnrestrictedMemcpy((void *)addr, &nop, sizeof(nop));
		addr += 2;
		//printf("THUMB UNALIGNED\n");
	}
	
	h.addr = addr;
	h.patch_instr[0] = 0xf000f8df; // LDR PC, [PC]
	h.patch_instr[1] = dst;
	kuKernelCpuUnrestrictedMemcpy(&h.orig_instr, (void *)addr, sizeof(h.orig_instr));
	kuKernelCpuUnrestrictedMemcpy((void *)addr, h.patch_instr, sizeof(h.patch_instr));

	return h;
}

so_hook hook_arm(uintptr_t addr, uintptr_t dst) {
	//printf("ARM HOOK\n");
	if (addr == 0)
		return;
	so_hook h;
	so_hook_generation++;
	h.thumb_addr = 0;
	h.addr = addr;
	h.patch_instr[0] = 0xe51ff004; // LDR PC, [PC, #-0x4]
	h.patch_instr[1] = dst;
	kuKernelCpuUnrestrictedMemcpy(&h.orig_instr, (void *)addr, sizeof(h.orig_instr));
	kuKernelCpuUnrestrictedMemcpy((void *)addr, h.patch_instr, sizeof(h.patch_instr));

	return h;
}

so_hook hook_addr(uintptr_t addr, uintptr_t dst) {
	if (addr == 0)
		return;
	if (addr & 1)
		return hook_thumb(addr, dst);
	else
		return hook_arm(addr, dst);
}

void so_flush_caches(so_module *mod) {
	kuKernelFlushCaches((void *)mod->text_base, mod->text_size);
}

static int _so_load(so_module *mod, SceUID so_blockid, const void *so_data, size_t so_size, uintptr_t load_addr) {
	int res = -1;
	uintptr_t data_addr = 0;
	so_elf elf;
	mod->patch_blockid = mod->text_blockid = -1;
	if (!so_elf_validate(so_data, so_size, load_addr, &elf))
		goto err_free_so;

	for (int i = 0; i < elf.header->e_phnum; i++) {
		const Elf32_Phdr *p = &elf.segments[i];
		if (p->p_type == PT_LOAD) {
			void *prog_data;
			size_t prog_size;

			if ((p->p_flags & PF_X) == PF_X) {
				// Allocate arena for code patches, trampolines, etc
				// Sits exactly under the desired allocation space
				mod->patch_size = ALIGN_MEM(PATCH_SZ, p->p_align);
				if (load_addr < mod->patch_size)
					goto err_free_so;
				SceKernelAllocMemBlockKernelOpt opt;
				memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
				opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
				opt.attr = 0x1;
				opt.field_C = (SceUInt32)load_addr - mod->patch_size;
				res = mod->patch_blockid = kuKernelAllocMemBlock("rx_block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, mod->patch_size, &opt);
				if (res < 0) {
					debugPrintf("Failed at allocating a patch block of %d bytes on address 0x%08X\n", mod->patch_size, opt.field_C);
					goto err_free_so;
				} else {
					debugPrintf("Patch Block: Addr: 0x%08X Size: %d\n", opt.field_C, mod->patch_size);
				}

				res = sceKernelGetMemBlockBase(mod->patch_blockid, &mod->patch_base);
				if (res < 0)
					goto err_free_blocks;
				mod->patch_head = mod->patch_base;
				
				prog_size = ALIGN_MEM(p->p_memsz, p->p_align);
				// Prefer physically contiguous, 1MB aligned code so the kernel can map it with
				// 1MB sections: large YYC runners thrash the TLB with 4KB pages
				memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
				opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
				opt.attr = 0x1 | 0x4;
				opt.field_C = (SceUInt32)load_addr;
				opt.alignment = 0x100000;
				res = exp_flag("nophycont") ? -1 : kuKernelAllocMemBlock("rx_block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX_PHYCONT, prog_size, &opt);
				mod->text_blockid = res;
				text_phycont_result = res;
				debugPrintf("Phycont prog block: 0x%08X\n", res);
				if (res < 0) {
					memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
					opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
					opt.attr = 0x1;
					opt.field_C = (SceUInt32)load_addr;
					res = mod->text_blockid = kuKernelAllocMemBlock("rx_block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, prog_size, &opt);
				}
				if (res < 0) {
					debugPrintf("Failed at allocating a prog block of %d bytes on address 0x%08X\n", prog_size, opt.field_C);
					goto err_free_blocks;
				} else {
					debugPrintf("Prog Block: Addr: 0x%08X Size: %d\n", opt.field_C, prog_size);
				}

				res = sceKernelGetMemBlockBase(mod->text_blockid, &prog_data);
				if (res < 0)
					goto err_free_blocks;
				mod->text_base = (uintptr_t)prog_data;
				mod->text_size = p->p_memsz;
		
				// Use the .text segment padding as a code cave
				// Word-align it to make it simpler for instruction arena allocation
				mod->cave_base = mod->cave_head = mod->text_base + ALIGN_MEM(p->p_memsz, 4);
				mod->cave_size = prog_size > ALIGN_MEM(p->p_memsz, 4) ? prog_size - ALIGN_MEM(p->p_memsz, 4) : 0;
				debugPrintf("code cave: %d bytes (@0x%08X).\n", mod->cave_size, mod->cave_base);

				data_addr = (uintptr_t)prog_data + prog_size;
			} else {
				prog_size = ALIGN_MEM(p->p_memsz + p->p_vaddr - (data_addr - mod->text_base), p->p_align);

				SceKernelAllocMemBlockKernelOpt opt;
				memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
				opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
				opt.attr = 0x1;
				opt.field_C = (SceUInt32)data_addr;
				res = mod->data_blockid[mod->n_data] = kuKernelAllocMemBlock("rw_block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, prog_size, &opt);
				if (res < 0) {
					debugPrintf("Failed at allocating a rw block of %d bytes on address 0x%08X\n", prog_size, opt.field_C);
					goto err_free_blocks;
				} else {
					debugPrintf("RW Block: Addr: 0x%08X Size: %d\n", opt.field_C, prog_size);
				}

				res = sceKernelGetMemBlockBase(mod->data_blockid[mod->n_data++], &prog_data);
				if (res < 0)
					goto err_free_blocks;
				if ((uintptr_t)prog_data != data_addr) {
					res = -1;
					goto err_free_blocks;
				}
				data_addr = (uintptr_t)prog_data + prog_size;

				mod->data_base[mod->n_data - 1] = mod->text_base + p->p_vaddr;
				mod->data_size[mod->n_data - 1] = p->p_memsz;
			}

			if (prog_size > p->p_filesz) {
				size_t padding = prog_size - p->p_filesz;
				char *zero = calloc(1, padding);
				if (!zero) {
					res = -1;
					goto err_free_blocks;
				}
				kuKernelCpuUnrestrictedMemcpy(prog_data + p->p_filesz, zero, padding);
				free(zero);
			}
			if (p->p_filesz)
				kuKernelCpuUnrestrictedMemcpy((void *)(mod->text_base + p->p_vaddr), so_data + p->p_offset, p->p_filesz);
		}
	}

	const Elf32_Shdr *const *t = elf.tables;
	#define TABLE_ADDRESS(index) (t[index] ? (void *)(mod->text_base + t[index]->sh_addr) : NULL)
	#define TABLE_COUNT(index, type) (t[index] ? t[index]->sh_size / sizeof(type) : 0)
	mod->dynamic = TABLE_ADDRESS(SO_DYNAMIC);
	mod->num_dynamic = elf.dynamic_count;
	mod->dynstr = TABLE_ADDRESS(SO_DYNSTR);
	mod->dynsym = TABLE_ADDRESS(SO_DYNSYM);
	mod->num_dynsym = TABLE_COUNT(SO_DYNSYM, Elf32_Sym);
	mod->reldyn = TABLE_ADDRESS(SO_RELDYN);
	mod->num_reldyn = TABLE_COUNT(SO_RELDYN, Elf32_Rel);
	mod->relplt = TABLE_ADDRESS(SO_RELPLT);
	mod->num_relplt = TABLE_COUNT(SO_RELPLT, Elf32_Rel);
	mod->init_array = TABLE_ADDRESS(SO_INIT_ARRAY);
	mod->num_init_array = TABLE_COUNT(SO_INIT_ARRAY, Elf32_Addr);
	mod->hash = TABLE_ADDRESS(SO_HASH);
	mod->hash_words = TABLE_COUNT(SO_HASH, uint32_t);
	mod->gnu_hash = TABLE_ADDRESS(SO_GNU_HASH);
	mod->gnu_hash_words = TABLE_COUNT(SO_GNU_HASH, uint32_t);
	#undef TABLE_ADDRESS
	#undef TABLE_COUNT

	for (int i = 0; i < mod->num_dynamic; i++) {
		switch (mod->dynamic[i].d_tag) {
		case DT_SONAME:
			mod->soname = mod->dynstr + mod->dynamic[i].d_un.d_ptr;
			break;
		default:
			break;
		}
	}

	if (so_blockid >= 0)
		sceKernelFreeMemBlock(so_blockid);

	if (!head && !tail) {
		head = mod;
		tail = mod;
	} else {
		tail->next = mod;
		tail = mod;
	}

	return 0;

err_free_blocks:
	for (int i = 0; i < mod->n_data; i++)
		sceKernelFreeMemBlock(mod->data_blockid[i]);
	if (mod->text_blockid >= 0)
		sceKernelFreeMemBlock(mod->text_blockid);
	if (mod->patch_blockid >= 0)
		sceKernelFreeMemBlock(mod->patch_blockid);
err_free_so:
	if (so_blockid >= 0)
		sceKernelFreeMemBlock(so_blockid);

	return res;
}

int so_mem_load(so_module *mod, void *buffer, size_t so_size, uintptr_t load_addr) {
	memset(mod, 0, sizeof(so_module));

	// Parse straight from the caller's buffer: copying it into a second memblock
	// doubles peak usage, which large YYC runners (80MB+) can't afford.
	return _so_load(mod, -1, buffer, so_size, load_addr);
}

int so_file_load(so_module *mod, const char *filename, uintptr_t load_addr) {
	SceUID so_blockid;
	void *so_data;

	memset(mod, 0, sizeof(so_module));

	SceUID fd = sceIoOpen(filename, SCE_O_RDONLY, 0);
	if (fd < 0)
		return fd;

	SceOff length = sceIoLseek(fd, 0, SCE_SEEK_END);
	if (length <= 0 || length > INT_MAX || sceIoLseek(fd, 0, SCE_SEEK_SET) != 0) {
		sceIoClose(fd);
		return -1;
	}
	size_t so_size = length;

	so_blockid = sceKernelAllocMemBlock("so block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (so_size + 0xfff) & ~0xfff, NULL);
	if (so_blockid < 0) {
		sceIoClose(fd);
		return so_blockid;
	}

	int res = sceKernelGetMemBlockBase(so_blockid, &so_data);
	size_t done = 0;
	while (res >= 0 && done < so_size) {
		int got = sceIoRead(fd, so_data + done, so_size - done);
		if (got <= 0)
			break;
		done += got;
	}
	if (sceIoClose(fd) < 0 || done != so_size) {
		sceKernelFreeMemBlock(so_blockid);
		return -1;
	}

	return _so_load(mod, so_blockid, so_data, so_size, load_addr);
}

int so_relocate(so_module *mod) {
	for (int i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
		Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i] : &mod->relplt[i - mod->num_reldyn];
		Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
		uintptr_t *ptr = (uintptr_t *)(mod->text_base + rel->r_offset);

		int type = ELF32_R_TYPE(rel->r_info);
		switch (type) {
		case R_ARM_ABS32:
			if (sym->st_shndx != SHN_UNDEF)
				*ptr += mod->text_base + sym->st_value;
			break;
		case R_ARM_RELATIVE:
			*ptr += mod->text_base;
			break;
		case R_ARM_GLOB_DAT:
		case R_ARM_JUMP_SLOT:
		{
			if (sym->st_shndx != SHN_UNDEF)
				*ptr = mod->text_base + sym->st_value;
			break;
		}
		default:
			fatal_error("Error unknown relocation type %x\n", type);
			break;
		}
	}

	return 0;
}

uintptr_t so_resolve_link(so_module *mod, const char *symbol) {
	for (int i = 0; i < mod->num_dynamic; i++) {
		switch (mod->dynamic[i].d_tag) {
		case DT_NEEDED:
		{
			so_module *curr = head;
			while (curr) {
				if (curr != mod && curr->soname && strcmp(curr->soname, mod->dynstr + mod->dynamic[i].d_un.d_ptr) == 0) {
					uintptr_t link = so_symbol(curr, symbol);
					if (link)
						return link;
				}
				curr = curr->next;
			}

			break;
		}
		default:
			break;
		}
	}

	return 0;
}

void reloc_err(uintptr_t got0)
{
	// Find to which module this missing symbol belongs
	int found = 0;
	so_module *curr = head;
	while (curr && !found) {
		for (int i = 0; i < curr->n_data; i++)
			if ((got0 >= curr->data_base[i]) && (got0 <= (uintptr_t)(curr->data_base[i] + curr->data_size[i])))
				found = 1;
		
		if (!found)
			curr = curr->next;
	}

	if (curr) {
		// Attempt to find symbol name and then display error
		for (int i = 0; i < curr->num_reldyn + curr->num_relplt; i++) {
			Elf32_Rel *rel = i < curr->num_reldyn ? &curr->reldyn[i] : &curr->relplt[i - curr->num_reldyn];
			Elf32_Sym *sym = &curr->dynsym[ELF32_R_SYM(rel->r_info)];
			uintptr_t *ptr = (uintptr_t *)(curr->text_base + rel->r_offset);

			int type = ELF32_R_TYPE(rel->r_info);
			switch (type) {
				case R_ARM_JUMP_SLOT:
				{
					if (got0 == (uintptr_t)ptr) {
						fatal_error("Unknown symbol \"%s\" (%p).\n", curr->dynstr + sym->st_name, (void*)got0);
					}
					break;
				}
			}
		}
	}

	// Ooops, this shouldn't have happened.
	fatal_error("Unknown symbol \"???\" (%p).\n", (void*)got0);
}

__attribute__((naked)) void plt0_stub()
{
	register uintptr_t got0 asm("r12");
	reloc_err(got0);
}

int so_resolve(so_module *mod, so_default_dynlib *default_dynlib, int size_default_dynlib, int default_dynlib_only) {
	for (int i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
		Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i] : &mod->relplt[i - mod->num_reldyn];
		Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
		uintptr_t *ptr = (uintptr_t *)(mod->text_base + rel->r_offset);

		int type = ELF32_R_TYPE(rel->r_info);
		switch (type) {
		case R_ARM_ABS32:
		case R_ARM_GLOB_DAT:
		case R_ARM_JUMP_SLOT:
		{
			if (sym->st_shndx == SHN_UNDEF) {
				int resolved = 0;
				if (!default_dynlib_only) {
					uintptr_t link = so_resolve_link(mod, mod->dynstr + sym->st_name);
					if (link) {
						// debugPrintf("Resolved from dependencies: %s\n", mod->dynstr + sym->st_name);
						if (type == R_ARM_ABS32)
							*ptr += link;
						else
							*ptr = link;
						resolved = 1;
					}
				}

				for (int j = 0; j < size_default_dynlib / sizeof(so_default_dynlib); j++) {
					if (strcmp(mod->dynstr + sym->st_name, default_dynlib[j].symbol) == 0) {
						*ptr = default_dynlib[j].func;
						resolved = 1;
						break;
					}
				}
				
				if (!resolved) {
					void *f = vglGetProcAddress(mod->dynstr + sym->st_name);
					if (f) {
						*ptr = f;
						resolved = 1;
						break;
					}
				}

				if (!resolved) {
					if (type == R_ARM_JUMP_SLOT) {
						debugPrintf("Unresolved import: %s\n", mod->dynstr + sym->st_name);
						*ptr = (uintptr_t)&plt0_stub;
					}
					else {
						//printf("Unresolved import: %s\n", mod->dynstr + sym->st_name);
					}
				}
			}

			break;
		}
		default:
			break;
		}
	}

	return 0;
}

int so_resolve_with_dummy(so_module *mod, so_default_dynlib *default_dynlib, int size_default_dynlib, int default_dynlib_only) {
	for (int i = 0; i < mod->num_reldyn + mod->num_relplt; i++) {
		Elf32_Rel *rel = i < mod->num_reldyn ? &mod->reldyn[i] : &mod->relplt[i - mod->num_reldyn];
		Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rel->r_info)];
		uintptr_t *ptr = (uintptr_t *)(mod->text_base + rel->r_offset);

		int type = ELF32_R_TYPE(rel->r_info);
		switch (type) {
		case R_ARM_ABS32:
		case R_ARM_GLOB_DAT:
		case R_ARM_JUMP_SLOT:
		{
			if (sym->st_shndx == SHN_UNDEF) {
				for (int j = 0; j < size_default_dynlib / sizeof(so_default_dynlib); j++) {
					if (strcmp(mod->dynstr + sym->st_name, default_dynlib[j].symbol) == 0) {
						*ptr = &ret0;
						break;
					}
				}
			}

			break;
		}
		default:
			break;
		}
	}

	return 0;
}

void so_initialize(so_module *mod) {
	for (int i = 0; i < mod->num_init_array; i++) {
		if (mod->init_array[i])
			mod->init_array[i]();
	}
}

uint32_t so_hash(const uint8_t *name) {
	uint64_t h = 0, g;
	while (*name) {
		h = (h << 4) + *name++;
		if ((g = (h & 0xf0000000)) != 0)
			h ^= g >> 24;
		h &= 0x0fffffff;
	}
	return h;
}

// GNU hash lookup (.gnu.hash: nbuckets, symoffset, bloom_size, bloom_shift, bloom[bloom_size], buckets[nbuckets],
// chain[]). Android libraries such as libyoyo.so only have this table; without it every so_symbol walked all ~55000
// dynamic symbols with strcmp (~18 ms for a symbol near the end, e.g. once per debug-mode report). -1 when not found.
static int so_gnu_symbol_index(so_module *mod, const char *symbol) {
	uint32_t *table = mod->gnu_hash;
	if (mod->gnu_hash_words < 4)
		return -1;
	uint32_t nbuckets = table[0], symoffset = table[1], bloom_size = table[2];
	size_t words = mod->gnu_hash_words - 4;
	if (!nbuckets || bloom_size > words || nbuckets > words - bloom_size)
		return -1;
	uint32_t *buckets = &table[4 + bloom_size];
	uint32_t *chain = &buckets[nbuckets];
	size_t chain_words = words - bloom_size - nbuckets;
	uint32_t h = 5381;
	for (const uint8_t *c = (const uint8_t *)symbol; *c; c++)
		h = h * 33 + *c;
	uint32_t i = buckets[h % nbuckets];
	if (!i || i < symoffset)
		return -1;
	for (; i < (uint32_t)mod->num_dynsym && i - symoffset < chain_words; i++) {
		uint32_t h2 = chain[i - symoffset];
		if ((h | 1) == (h2 | 1) && mod->dynsym[i].st_shndx != SHN_UNDEF && mod->dynsym[i].st_info != SHN_UNDEF &&
			strcmp(mod->dynstr + mod->dynsym[i].st_name, symbol) == 0)
			return i;
		if (h2 & 1)
			break;
	}
	return -1;
}

static int so_symbol_index(so_module *mod, const char *symbol)
{
	if (mod->gnu_hash) {
		int i = so_gnu_symbol_index(mod, symbol);
		if (i >= 0)
			return i;
	}
	if (mod->hash && mod->hash_words >= 2) {
		uint32_t hash = so_hash((const uint8_t *)symbol);
		uint32_t nbucket = mod->hash[0];
		size_t words = mod->hash_words - 2;
		if (nbucket && nbucket <= words) {
			uint32_t *bucket = &mod->hash[2];
			uint32_t *chain = &bucket[nbucket];
			size_t count = words - nbucket;
			uint32_t i = bucket[hash % nbucket];
			for (size_t visited = 0; i && i < count && i < (uint32_t)mod->num_dynsym && visited < count; visited++, i = chain[i]) {
				if (mod->dynsym[i].st_shndx != SHN_UNDEF && mod->dynsym[i].st_info != SHN_UNDEF &&
					strcmp(mod->dynstr + mod->dynsym[i].st_name, symbol) == 0)
					return i;
			}
		}
	}

	for (int i = 0; i < mod->num_dynsym; i++) {
		if (mod->dynsym[i].st_shndx == SHN_UNDEF)
			continue;
		if (mod->dynsym[i].st_info != SHN_UNDEF && strcmp(mod->dynstr + mod->dynsym[i].st_name, symbol) == 0)
			return i;
	}

	return -1;
}

static uintptr_t alloc_branch_space(uintptr_t site, uintptr_t base, size_t size, uintptr_t *head, size_t bytes) {
	size_t used = *head - base;
	int64_t delta = (int64_t)*head - site - 8; // A32 B/BL uses PC + 8
	if (!base || used > size || bytes > size - used ||
		(delta & 3) || delta < -(1 << 25) || delta >= (1 << 25))
		return 0;
	uintptr_t addr = *head;
	*head += bytes;
	return addr;
}

// Executable space reachable by A32 B/BL from site.
uintptr_t so_alloc_near(so_module *so, uintptr_t site, size_t sz) {
	if (!sz || sz > SIZE_MAX - 3)
		return 0;
	sz = ALIGN_MEM(sz, 4);
	uintptr_t addr = alloc_branch_space(site, so->patch_base, so->patch_size, &so->patch_head, sz);
	if (!addr)
		addr = alloc_branch_space(site, so->cave_base, so->cave_size, &so->cave_head, sz);
	return addr;
}

static void trampoline_ldm(so_module *mod, uint32_t *dst) {
	uint32_t trampoline[1];
	uint32_t funct[20] = {0xFAFAFAFA};
	uint32_t *ptr = funct;

	int cur = 0;
	int baseReg = ((*dst) >> 16) & 0xF;
	int bitMask = (*dst) & 0xFFFF;

	uint32_t stored = NULL;
	for (int i = 0; i < 16; i++) {
		if (bitMask & (1 << i)) {
			// If the register we're reading the offset from is the same as the one we're writing,
			// delay it to the very end so that the base pointer ins't clobbered
			if (baseReg == i)
				stored = LDR_OFFS(i, baseReg, cur).raw;
			else
				*ptr++ = LDR_OFFS(i, baseReg, cur).raw;
			cur += 4;
		}
	}

	// Perform the delayed load if needed
	if (stored) {
		*ptr++ = stored;
	}

	*ptr++ = 0xe51ff004; // LDR PC, [PC, -0x4] ; jmp to [dst+0x4]
	*ptr++ = dst+1; // .dword <...>	; [dst+0x4]

	size_t trampoline_sz =	((uintptr_t)ptr - (uintptr_t)&funct[0]);
	uintptr_t patch_addr = so_alloc_near(mod, (uintptr_t)dst, trampoline_sz);

	if (!patch_addr) {
		fatal_error("Failed to patch LDMIA at 0x%08X, unable to allocate space.\n", dst);
	}
	
	// Create sign extended relative address rel_addr
	trampoline[0] = B(dst, patch_addr).raw;

	kuKernelCpuUnrestrictedMemcpy((void*)patch_addr, funct, trampoline_sz);
	kuKernelCpuUnrestrictedMemcpy(dst, trampoline, sizeof(trampoline));
}

// Returns the resolved PLT GOT slot for symbol, so calls made through the PLT can be redirected
uintptr_t *so_got_slot(so_module *mod, const char *symbol) {
	so_hook_generation++;
	for (int i = 0; i < mod->num_relplt; i++) {
		Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(mod->relplt[i].r_info)];
		if (strcmp(mod->dynstr + sym->st_name, symbol) == 0)
			return (uintptr_t *)(mod->text_base + mod->relplt[i].r_offset);
	}
	return NULL;
}

uintptr_t so_symbol(so_module *mod, const char *symbol) {
	int index = so_symbol_index(mod, symbol);
	if (index == -1)
		return NULL;

	return mod->text_base + mod->dynsym[index].st_value;
}

void so_symbol_fix_ldmia(so_module *mod, const char *symbol) {
	// This is meant to work around crashes due to unaligned accesses (SIGBUS :/) due to certain
	// kernels not having the fault trap enabled, e.g. certain RK3326 Odroid Go Advance clone distros.
	// TODO:: Maybe enable this only with a config flag? maybe with a list of known broken functions?
	// Known to trigger on GM:S's "_Z11Shader_LoadPhjS_" - if it starts happening on other places,
	// might be worth enabling it globally.
	
	int idx = so_symbol_index(mod, symbol);
	if (idx == -1)
		return;

	uintptr_t st_addr = mod->text_base + mod->dynsym[idx].st_value;
	for (uintptr_t addr = st_addr; addr < st_addr + mod->dynsym[idx].st_size; addr+=4) {
		uint32_t inst = *(uint32_t*)(addr);
		
		//Is this an LDMIA instruction with a R0-R12 base register?
		if (((inst & 0xFFF00000) == 0xE8900000) && (((inst >> 16) & 0xF) < 13) ) {
			debugPrintf("Found possibly misaligned LDMIA on 0x%08X, trying to fix it... (instr: 0x%08X, to 0x%08X)\n", addr, *(uint32_t*)addr, mod->patch_head);
			trampoline_ldm(mod, addr);
		}
	}
}
