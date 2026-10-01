/*
 * evprof.c
 *
 * Debug-mode object event profiler for YYC runners. Every gml_Object_* entry of the
 * runner's g_GMLFuncs table is replaced by a stub that times the original event, so we
 * can see which objects' Step/Draw events cost the most per frame. Times are inclusive.
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

#define STUB_WORDS 3
#define REPORT_TOP 30
#define EVPROF_ADDRESS (LOAD_ADDRESS + 0x800000)

typedef struct {
	const char *name;
	void (*func)(void *self, void *other);
	void *unk;
} gml_func_entry;

typedef void (*event_func)(void *self, void *other);

static event_func *ev_orig;
static const char **ev_names;
static uint32_t *ev_time;
static uint32_t *ev_calls;
static int ev_num;

// Reached from the per-entry stubs with the entry index in r2
static void evprof_call(void *self, void *other, int idx) {
	uint32_t start = sceKernelGetProcessTimeLow();
	ev_orig[idx](self, other);
	ev_time[idx] += sceKernelGetProcessTimeLow() - start;
	ev_calls[idx]++;
}

void evprof_install(so_module *mod) {
	gml_func_entry *table = (gml_func_entry *)so_symbol(mod, "g_GMLFuncs");
	if (!table)
		return;
	int count = 79992 / sizeof(gml_func_entry);

	ev_orig = calloc(count, sizeof(event_func));
	ev_names = calloc(count, sizeof(char *));
	ev_time = calloc(count, sizeof(uint32_t));
	ev_calls = calloc(count, sizeof(uint32_t));

	SceSize size = (count * STUB_WORDS * 4 + 0xfff) & ~0xfff;
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = 0x1;
	opt.field_C = EVPROF_ADDRESS;
	SceUID block = kuKernelAllocMemBlock("evprof", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, size, &opt);
	if (block < 0) {
		debugPrintf("evprof: failed to allocate %d bytes (0x%08X)\n", size, block);
		return;
	}
	uint32_t *stubs_rx;
	sceKernelGetMemBlockBase(block, (void **)&stubs_rx);
	uint32_t *stubs = calloc(1, size);

	for (int i = 0; i < count; i++) {
		// User events (Other_10..25) are left alone: stubbing them makes actors drawn through
		// event_user (obj_drawactors) disappear
		if (!table[i].name || !table[i].func || strncmp(table[i].name, "gml_Object_", 11) != 0 ||
			strstr(table[i].name, "_Other_"))
			continue;
		uint32_t *s = &stubs[ev_num * STUB_WORDS];
		s[0] = 0xE3002000 | ((ev_num >> 12) << 16) | (ev_num & 0xfff); // movw r2, #ev_num
		s[1] = 0xE51FF004;                                             // ldr pc, [pc, #-4]
		s[2] = (uint32_t)&evprof_call;
		ev_orig[ev_num] = table[i].func;
		ev_names[ev_num] = table[i].name;
		table[i].func = (event_func)&stubs_rx[ev_num * STUB_WORDS];
		ev_num++;
	}

	kuKernelCpuUnrestrictedMemcpy(stubs_rx, stubs, size);
	kuKernelFlushCaches(stubs_rx, size);
	free(stubs);
	debugPrintf("evprof: timing %d object events\n", ev_num);
}

void evprof_report(int frames) {
	if (!ev_num)
		return;
	int top[REPORT_TOP];
	int n = 0;
	for (int i = 0; i < ev_num; i++) {
		if (!ev_calls[i])
			continue;
		int pos = n < REPORT_TOP ? n++ : REPORT_TOP;
		while (pos > 0 && ev_time[top[pos - 1]] < ev_time[i]) {
			if (pos < REPORT_TOP)
				top[pos] = top[pos - 1];
			pos--;
		}
		if (pos < REPORT_TOP)
			top[pos] = i;
	}
	for (int i = 0; i < n; i++)
		debugPrintf("[EVENT] %6.2fms %6.1f calls %s\n", ev_time[top[i]] / 1000.0f / frames, (float)ev_calls[top[i]] / frames, ev_names[top[i]]);
	memset(ev_time, 0, ev_num * sizeof(uint32_t));
	memset(ev_calls, 0, ev_num * sizeof(uint32_t));
}
