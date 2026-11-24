/*
 * fast_find.c
 *
 * Code_Function_Find(name, &id) with hashed lookups during the runner's Startup.
 *
 * LoadGameData resolves every function name the game's code refers to (7074 at boot) with Code_Function_Find, which
 * tries in order: Script_Find (a strcmp walk over all scripts, from the last one down), Extension_Function_GetId, and a
 * strcmp walk over the builtin table (the_functions). That was 44.5 million strcmp calls, ~4.5 s of every boot.
 *
 * The replacement takes the same three steps and returns the same results:
 * - scripts: of the indices whose name equals `name`, the highest one whose Script_Main_items entry is set;
 * - Extension_Function_GetId: the runner's own function;
 * - builtins: the lowest index whose name equals `name`.
 * The name tables are hashed once and rehashed when a table's count or base pointer changes; every candidate is checked
 * against the live tables (strcmp, items entry), so the hash only narrows the search. Active from just before Startup
 * until it returns (later calls are rare and use the runner's code). exp_findverify also runs the runner's own function on
 * every call and logs any difference; exp_nofastfind turns the replacement off.
 */

#include <vitasdk.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "so_util.h"
#include "hooks.h"

typedef struct {
	uint32_t hash;
	int index; // -1: empty slot
} slot;

typedef struct {
	slot *slots;
	uint32_t mask;
	int count;
	const void *base;
} name_table;

static int (*orig_code_function_find)(const char *name, int *out);
static int (*extension_function_getid)(const char *name);
static int *script_number;
static const char ***script_names;
static uintptr_t script_items; // struct whose +4 is the items array
static int *the_numb;
static const char **the_functions_var; // the_functions: pointer to 12-byte entries, name first
static int fast_on, verify_on;
static uint32_t calls, mismatches;

static uint32_t name_hash(const char *s) {
	uint32_t h = 2166136261u;
	while (*s)
		h = (h ^ (uint8_t)*s++) * 16777619u;
	return h;
}

// (Re)builds t over count names; name_at(i) gives name i (NULL: no name). Returns 0 if out of memory.
static int build(name_table *t, int count, const void *base, const char *(*name_at)(int)) {
	uint32_t size = 64;
	while (size < (uint32_t)count * 2)
		size <<= 1;
	slot *slots = malloc(size * sizeof(slot));
	if (!slots)
		return 0;
	for (uint32_t i = 0; i < size; i++)
		slots[i].index = -1;
	for (int i = 0; i < count; i++) {
		const char *n = name_at(i);
		if (!n)
			continue;
		uint32_t h = name_hash(n), k = h & (size - 1);
		while (slots[k].index >= 0)
			k = (k + 1) & (size - 1);
		slots[k].hash = h;
		slots[k].index = i;
	}
	free(t->slots);
	t->slots = slots;
	t->mask = size - 1;
	t->count = count;
	t->base = base;
	return 1;
}

static const char *script_name_at(int i) {
	return (*script_names)[i];
}

static const char *builtin_name_at(int i) {
	return *(const char **)((const uint8_t *)*the_functions_var + i * 12);
}

static name_table scripts, builtins;

// Script_Find: the highest index whose items entry is set and whose name equals name; -1 if none
static int script_find(const char *name) {
	int count = *script_number;
	if (count < 1)
		return -1;
	const char **names = *script_names;
	void **items = *(void ***)(script_items + 4);
	if ((scripts.count != count || scripts.base != names) && !build(&scripts, count, names, script_name_at))
		return -2; // no memory: caller falls back
	uint32_t h = name_hash(name);
	int best = -1;
	for (uint32_t k = h & scripts.mask; scripts.slots[k].index >= 0; k = (k + 1) & scripts.mask) {
		int i = scripts.slots[k].index;
		if (scripts.slots[k].hash == h && i > best && i < count && items[i] && names[i] && !strcmp(names[i], name))
			best = i;
	}
	return best;
}

// The builtin walk: the lowest index whose name equals name; -1 if none
static int builtin_find(const char *name) {
	int count = *the_numb;
	if (count < 1)
		return -1;
	const void *base = *the_functions_var;
	if ((builtins.count != count || builtins.base != base) && !build(&builtins, count, base, builtin_name_at))
		return -2;
	uint32_t h = name_hash(name);
	int best = -1;
	for (uint32_t k = h & builtins.mask; builtins.slots[k].index >= 0; k = (k + 1) & builtins.mask) {
		int i = builtins.slots[k].index;
		const char *n = builtin_name_at(i);
		if (builtins.slots[k].hash == h && (best < 0 || i < best) && i < count && n && !strcmp(n, name))
			best = i;
	}
	return best;
}

// Code_Function_Find's three steps; -2 from a lookup (out of memory) hands the call to the runner's function
static int fast_code_function_find(const char *name, int *out) {
	*out = -1;
	if (!name || !*name)
		return 0;
	int r = script_find(name);
	if (r == -2)
		return orig_code_function_find(name, out);
	*out = r + 100000;
	if (r > -1)
		return 1;
	r = extension_function_getid(name);
	*out = r + 500000;
	if (r > -1)
		return 1;
	*out = -1;
	r = builtin_find(name);
	if (r == -2)
		return orig_code_function_find(name, out);
	if (r < 0)
		return 0;
	*out = r;
	return 1;
}

static int code_function_find_hook(const char *name, int *out) {
	if (!fast_on)
		return orig_code_function_find(name, out);
	if (!verify_on)
		return fast_code_function_find(name, out);
	int fast_id, orig_id;
	int fast_r = fast_code_function_find(name, &fast_id);
	int orig_r = orig_code_function_find(name, &orig_id);
	calls++;
	if (fast_r != orig_r || fast_id != orig_id) {
		if (mismatches++ < 20)
			debugPrintf("[FASTFIND] MISMATCH %s: fast %d/%d, runner %d/%d\n", name ? name : "(null)", fast_r, fast_id,
				orig_r, orig_id);
	}
	*out = orig_id;
	return orig_r;
}

void fast_find_install(void) {
	if (exp_flag("nofastfind"))
		return;
	uintptr_t entry = so_symbol(&yoyoloader_mod, "_Z18Code_Function_FindPKcPi");
	extension_function_getid = (void *)so_symbol(&yoyoloader_mod, "_Z24Extension_Function_GetIdPKc");
	script_number = (int *)so_symbol(&yoyoloader_mod, "Script_Main_number");
	script_names = (const char ***)so_symbol(&yoyoloader_mod, "Script_Main_names");
	script_items = so_symbol(&yoyoloader_mod, "Script_Main_items");
	the_numb = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	the_functions_var = (const char **)so_symbol(&yoyoloader_mod, "the_functions");
	// The replacement mirrors this runner's code: check its first instructions before taking over
	static const uint32_t expect[] = {0xe92d4df0, 0xe28db018, 0xe1a05000, 0xe3e00000, 0xe5810000};
	if (!entry || !extension_function_getid || !script_number || !script_names || !script_items || !the_numb ||
		!the_functions_var || memcmp((void *)entry, expect, sizeof(expect))) {
		debugPrintf("fast function lookup: not installed (runner layout differs)\n");
		return;
	}
	orig_code_function_find = entry_hook(entry, code_function_find_hook);
	if (!orig_code_function_find)
		return;
	verify_on = exp_flag("findverify");
	fast_on = 1;
	debugPrintf("fast function lookup: installed%s\n", verify_on ? " (verifying every call)" : "");
}

// After Startup: back to the runner's own lookups, tables freed
void fast_find_end(void) {
	if (!fast_on)
		return;
	fast_on = 0;
	free(scripts.slots);
	free(builtins.slots);
	scripts = builtins = (name_table){0};
	if (verify_on)
		debugPrintf("[FASTFIND] verified %u calls, %u mismatches\n", calls, mismatches);
}
