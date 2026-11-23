/*
 * gcalloc.c
 *
 * Research (exp_gcalloc, debug mode): counts the GML heap objects the game creates, per call site. Periodic
 * MarkAndSweepGen passes (6-8 ms frames) come from short-lived arrays and structs made every frame
 * (draw_laser's _randvar_array, PR #40). ARRAY_RefAlloc (every GML array) and YYObjectBase::Alloc (structs,
 * method objects, ...) are entry-hooked; each call is counted under its return address and the event running
 * at the time (Current_Object / Current_Event_Type / Current_Event_Number). Every 120 frames the busiest call
 * sites are logged as runner offsets ("[GCALLOC]"), to be matched against libyoyo.so's symbols.
 */

#include <vitasdk.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

void *entry_hook(uintptr_t entry, void *replacement);
extern int debugMode;

#define SITES 512
typedef struct {
	uintptr_t caller;
	int object, type, number, kind;
	uint32_t count;
} site_t;
static site_t sites[SITES];
static uint32_t total[2], dropped;
static int *ev_type, *ev_number, *cur_object;
static const char *(*object_name)(int);
static void *(*orig_array_alloc)(void);
static void *(*orig_object_alloc)(unsigned, int, int, int);

static void count(uintptr_t caller, int kind) {
	total[kind]++;
	int obj = cur_object ? *cur_object : -1, type = ev_type ? *ev_type : -1, num = ev_number ? *ev_number : -1;
	unsigned h = (caller >> 2) * 2654435761u + (unsigned)obj * 40503u;
	for (int n = 0; n < SITES; n++) {
		site_t *s = &sites[(h + n) % SITES];
		if (!s->count) {
			*s = (site_t){caller, obj, type, num, kind, 1};
			return;
		}
		if (s->caller == caller && s->object == obj && s->kind == kind) {
			s->count++;
			return;
		}
	}
	dropped++;
}

static void *array_alloc_hook(void) {
	count((uintptr_t)__builtin_return_address(0), 0);
	return orig_array_alloc();
}

static void *object_alloc_hook(unsigned n, int a, int b, int c) {
	count((uintptr_t)__builtin_return_address(0), 1);
	return orig_object_alloc(n, a, b, c);
}

static int on;

void gcalloc_install(void) {
	if (!debugMode || !exp_flag("gcalloc"))
		return;
	ev_type = (int *)so_symbol(&yoyoloader_mod, "Current_Event_Type");
	ev_number = (int *)so_symbol(&yoyoloader_mod, "Current_Event_Number");
	cur_object = (int *)so_symbol(&yoyoloader_mod, "Current_Object");
	object_name = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_Namei");
	orig_array_alloc = entry_hook(so_symbol(&yoyoloader_mod, "_Z14ARRAY_RefAllocv"), array_alloc_hook);
	orig_object_alloc = entry_hook(so_symbol(&yoyoloader_mod, "_ZN12YYObjectBase5AllocEjiib"), object_alloc_hook);
	on = 1;
	debugPrintf("[GCALLOC] array alloc %s, object alloc %s\n", orig_array_alloc ? "hooked" : "NOT hooked",
		orig_object_alloc ? "hooked" : "NOT hooked");
}

void gcalloc_frame(void) {
	static int frames;
	if (!on || ++frames < 120)
		return;
	debugPrintf("[GCALLOC] per frame over %d frames: arrays %.1f objects %.1f (sites dropped %u)\n", frames,
		total[0] / (float)frames, total[1] / (float)frames, dropped);
	for (int k = 0; k < 12; k++) {
		site_t *best = NULL;
		for (int i = 0; i < SITES; i++)
			if (sites[i].count && (!best || sites[i].count > best->count))
				best = &sites[i];
		if (!best)
			break;
		debugPrintf("[GCALLOC]   %.2f/frame %s at runner+0x%x in %s event %d.%d\n", best->count / (float)frames,
			best->kind ? "object" : "array", best->caller - yoyoloader_mod.text_base,
			best->object >= 0 && object_name ? object_name(best->object) : "-", best->type, best->number);
		best->count = 0;
	}
	memset(sites, 0, sizeof(sites));
	total[0] = total[1] = dropped = 0;
	frames = 0;
}
