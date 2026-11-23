#ifndef SO_ELF_H
#define SO_ELF_H

#include <stddef.h>
#include <stdint.h>

#include "elf.h"

#define MAX_DATA_SEG 4

enum so_elf_table {
	SO_DYNAMIC, SO_DYNSTR, SO_DYNSYM, SO_RELDYN, SO_RELPLT,
	SO_INIT_ARRAY, SO_HASH, SO_GNU_HASH, SO_TABLE_COUNT
};

typedef struct {
	const Elf32_Ehdr *header;
	const Elf32_Phdr *segments;
	const Elf32_Shdr *tables[SO_TABLE_COUNT];
	size_t dynamic_count;
} so_elf;

// Validate the ELF32 ARM layout consumed by this loader before mapping it.
int so_elf_validate(const void *data, size_t size, uint32_t load_addr, so_elf *elf);

#endif
