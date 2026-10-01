/*
 * callprof.c
 *
 * Debug-mode call counter: every PLT slot of a module is redirected to a tiny ARM stub
 * that bumps a counter and tail-jumps to the original target. Reports the most called
 * functions per frame, which covers libc imports and the runner's exported functions.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)
#endif

#define STUB_WORDS 9
#define REPORT_TOP 40

static uint32_t *call_counts;
static const char **call_names;
static int call_num;

void callprof_install(so_module *mod) {
	call_num = mod->num_relplt;
	call_counts = calloc(call_num, sizeof(uint32_t));
	call_names = calloc(call_num, sizeof(char *));

	SceSize size = (call_num * STUB_WORDS * 4 + 0xfff) & ~0xfff;
	// Place stubs in the slot reserved for libc++_shared.so, which Katana ZERO doesn't ship
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = 0x1;
	opt.field_C = LOAD_ADDRESS;
	SceUID block = kuKernelAllocMemBlock("callprof", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, size, &opt);
	if (block < 0) {
		debugPrintf("callprof: failed to allocate %d bytes (0x%08X)\n", size, block);
		return;
	}
	uint32_t *stubs_rx;
	sceKernelGetMemBlockBase(block, (void **)&stubs_rx);
	uint32_t *stubs = malloc(size);

	for (int i = 0; i < call_num; i++) {
		Elf32_Rel *rel = &mod->relplt[i];
		uintptr_t *slot = (uintptr_t *)(mod->text_base + rel->r_offset);
		call_names[i] = mod->dynstr + mod->dynsym[ELF32_R_SYM(rel->r_info)].st_name;

		uint32_t *s = &stubs[i * STUB_WORDS];
		s[0] = 0xe92d0003; // push {r0, r1}
		s[1] = 0xe59f0010; // ldr r0, [pc, #16]  -> s[7]
		s[2] = 0xe5901000; // ldr r1, [r0]
		s[3] = 0xe2811001; // add r1, r1, #1
		s[4] = 0xe5801000; // str r1, [r0]
		s[5] = 0xe8bd0003; // pop {r0, r1}
		s[6] = 0xe59ff000; // ldr pc, [pc, #0]   -> s[8]
		s[7] = (uint32_t)&call_counts[i];
		s[8] = *slot;
		*slot = (uintptr_t)&stubs_rx[i * STUB_WORDS];
	}

	kuKernelCpuUnrestrictedMemcpy(stubs_rx, stubs, call_num * STUB_WORDS * 4);
	kuKernelFlushCaches(stubs_rx, size);
	free(stubs);
	debugPrintf("callprof: counting %d PLT slots\n", call_num);
}

void callprof_report(int frames) {
	if (!call_counts)
		return;
	int top[REPORT_TOP];
	int n = 0;
	for (int i = 0; i < call_num; i++) {
		if (!call_counts[i])
			continue;
		// Insertion into a small sorted top list
		int pos = n < REPORT_TOP ? n++ : REPORT_TOP;
		while (pos > 0 && call_counts[top[pos - 1]] < call_counts[i]) {
			if (pos < REPORT_TOP)
				top[pos] = top[pos - 1];
			pos--;
		}
		if (pos < REPORT_TOP)
			top[pos] = i;
	}
	for (int i = 0; i < n; i++)
		debugPrintf("[CALLS] %9.1f/frame %s\n", (float)call_counts[top[i]] / frames, call_names[top[i]]);
	memset(call_counts, 0, call_num * sizeof(uint32_t));
}
