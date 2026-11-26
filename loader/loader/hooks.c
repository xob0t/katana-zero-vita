/*
 * hooks.c
 *
 * Hooking helpers shared by the port's modules (moved out of katana.c unchanged): the RX arena for trampolines and
 * veneers, entry_hook, gml_replace and stub_event.
 *
 * Registry: every entry_hook and stub_event is recorded with its caller. Hooking a function twice turns one of the
 * hooks off (a second entry_hook finds `ldr pc, [pc, #-4]` instead of a prologue and fails; a stub overwrites an
 * entry hook), so both cases are logged as "[HOOKS] ... already hooked by ..." when they happen. The hooks
 * themselves behave exactly as before: the registry only logs. hooks_report() logs one summary line at startup
 * listing every hook that did not install. Owners are code addresses (the call site of entry_hook): look them up in
 * build/app/YoYoLoader with arm-vita-eabi-addr2line -f.
 *
 * The other ways the port changes the runner are recorded the same way, through these helpers:
 *  - hook_raw (so_util.h): hook_addr, a jump over a function's entry with no trampoline;
 *  - got_write: a PLT GOT slot (so_got_slot) set to a replacement;
 *  - builtin_swap / builtin_chain: a GML builtin's function pointer in the runner's registry (the_functions).
 * They write exactly what the bare write did, and log "[HOOKS] CONFLICT ..." when a second module takes a target
 * someone else holds (a raw hook replaces the first one silently; a second swap hides the first module's
 * replacement from anyone who checks the entry, the vcapture/pause_stack bug of #115) or when the value replaced isn't
 * the runner's own (someone wrote it without the registry). builtin_chain is for wrappers that pass every call
 * they don't handle on to the entry they found (instance_create_depth): there a recorded earlier swap is expected.
 * hooks_report() logs a second [HOOKS] line with these counts and the conflicts.
 */

#include <vitasdk.h>
#include <kubridge.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "so_util.h"
#include "hooks.h"

// Executable memory for trampolines of native replacements (entry hooks that still call the
// original): a small bump allocator over one RX block below the runner
#define RX_ARENA_ADDRESS (LOAD_ADDRESS + 0xE00000)
#define RX_ARENA_SIZE 0x10000
static uint8_t *rx_arena;
static size_t rx_arena_used;
static int rx_arena_refused;

void *katana_rx_alloc(size_t size) {
	if (!rx_arena) {
		SceKernelAllocMemBlockKernelOpt opt;
		memset(&opt, 0, sizeof(opt));
		opt.size = sizeof(opt);
		opt.attr = 0x1;
		opt.field_C = RX_ARENA_ADDRESS;
		SceUID block = kuKernelAllocMemBlock("katana rx", 0x0C20D050 /* USER_RX */, RX_ARENA_SIZE, &opt);
		// Only if the fixed address is taken, as in the Vita3K emulator, which puts a 9 MB block of uncached
		// memory at 0x985C6000-0x98E8C000 (on the Vita the fixed address always succeeds): right under the
		// runner's patch block, in bl range of the same code, else anywhere. Any address does for the
		// trampolines (absolute jumps); the veneer users check their range.
		if (block < 0) {
			SceUID fixed = block;
			opt.field_C = yoyoloader_mod.patch_base - RX_ARENA_SIZE;
			block = kuKernelAllocMemBlock("katana rx", 0x0C20D050 /* USER_RX */, RX_ARENA_SIZE, &opt);
			if (block < 0) {
				opt.attr = 0;
				opt.field_C = 0;
				block = kuKernelAllocMemBlock("katana rx", 0x0C20D050 /* USER_RX */, RX_ARENA_SIZE, &opt);
			}
			if (block >= 0 && sceKernelGetMemBlockBase(block, (void **)&rx_arena) >= 0)
				debugPrintf("rx arena: fixed address taken (0x%08X), at %p instead\n", fixed, rx_arena);
		}
		if (block < 0 || sceKernelGetMemBlockBase(block, (void **)&rx_arena) < 0) {
			debugPrintf("rx arena: allocation failed (0x%08X)\n", block);
			rx_arena = NULL;
			rx_arena_refused++;
			return NULL;
		}
	}
	size = (size + 15) & ~15;
	if (rx_arena_used + size > RX_ARENA_SIZE) {
		rx_arena_refused++;
		return NULL;
	}
	void *p = rx_arena + rx_arena_used;
	rx_arena_used += size;
	return p;
}

// --- Registry (log only) ---

enum { HOOK_ENTRY, HOOK_STUB, HOOK_RAW, HOOK_GOT, HOOK_BUILTIN };
static const char *const hook_kind[] = {"entry_hook", "stub_event", "hook_raw", "got_write", "builtin_swap"};
// Code patches (a function's entry) and pointer writes (a GOT slot, a registry entry) are separate targets
#define CODE_KIND(k) ((k) <= HOOK_RAW)

typedef struct {
	uintptr_t target, replacement, caller;
	int kind;
} hook_record;

typedef struct {
	uintptr_t target, caller;
	const char *why;
	char name[96];
} hook_failure;

#define MAX_HOOKS 512 // virtual_capture.c alone raw-hooks ~250 runner functions
#define MAX_FAILURES 32
#define MAX_CONFLICTS 16
static hook_record hooks[MAX_HOOKS];
static hook_failure failures[MAX_FAILURES];
static int n_hooks, n_failures, n_entry_hooks, n_stubs, hooks_unrecorded;
static int n_raw, n_got, n_builtin, n_chained, n_conflicts;
static char conflicts[MAX_CONFLICTS][96];

// The runner symbol at addr, NULL if none (only on the logging paths: a linear walk of .dynsym)
static const char *runner_symbol_at(uintptr_t addr) {
	so_module *mod = &yoyoloader_mod;
	if (!addr || !mod->dynsym || !mod->dynstr)
		return NULL;
	for (int i = 0; i < mod->num_dynsym; i++) {
		Elf32_Sym *sym = &mod->dynsym[i];
		if (sym->st_value && sym->st_name && mod->text_base + sym->st_value == addr)
			return mod->dynstr + sym->st_name;
	}
	return NULL;
}

// The latest record on target: a code patch (code != 0) or a pointer write
static hook_record *hook_find_kind(uintptr_t target, int code) {
	for (int i = n_hooks - 1; target && i >= 0; i--)
		if (hooks[i].target == target && !CODE_KIND(hooks[i].kind) == !code)
			return &hooks[i];
	return NULL;
}

static hook_record *hook_find(uintptr_t target) {
	return hook_find_kind(target, 1);
}

static void hook_add(uintptr_t target, uintptr_t replacement, uintptr_t caller, int kind) {
	if (n_hooks < MAX_HOOKS)
		hooks[n_hooks++] = (hook_record){target, replacement, caller, kind};
	else
		hooks_unrecorded++;
	if (kind == HOOK_STUB)
		n_stubs++;
	else if (kind == HOOK_ENTRY)
		n_entry_hooks++;
	else if (kind == HOOK_RAW)
		n_raw++;
	else if (kind == HOOK_GOT)
		n_got++;
	else
		n_builtin++;
}

static void hook_failed(uintptr_t target, uintptr_t caller, const char *why, const char *name) {
	if (n_failures >= MAX_FAILURES) {
		n_failures++;
		return;
	}
	hook_failure *f = &failures[n_failures++];
	f->target = target;
	f->caller = caller;
	f->why = why;
	if (!name)
		name = runner_symbol_at(target);
	if (name)
		snprintf(f->name, sizeof(f->name), "%s", name);
	else if (target)
		snprintf(f->name, sizeof(f->name), "0x%08X", target);
	else
		snprintf(f->name, sizeof(f->name), "(no symbol)");
}

static void log_already_hooked(const char *what, uintptr_t target, uintptr_t caller, hook_record *by) {
	const char *name = runner_symbol_at(target);
	debugPrintf("[HOOKS] %s %s (0x%08X) from 0x%08X: already hooked by %s 0x%08X from 0x%08X\n", what,
		name ? name : "?", target, caller, hook_kind[by->kind], by->replacement, by->caller);
}

// A second module on a target, or a replaced value that wasn't the runner's: logged at once and in the report
static void conflict(const char *what, const char *name, uintptr_t target, uintptr_t caller, hook_record *by,
		uintptr_t found) {
	if (by)
		debugPrintf("[HOOKS] CONFLICT %s %s (0x%08X) from 0x%08X: already taken by %s 0x%08X from 0x%08X\n", what,
			name ? name : "?", target, caller, hook_kind[by->kind], by->replacement, by->caller);
	else
		debugPrintf("[HOOKS] CONFLICT %s %s (0x%08X) from 0x%08X: replaces 0x%08X, not the runner's own\n", what,
			name ? name : "?", target, caller, found);
	if (n_conflicts < MAX_CONFLICTS)
		snprintf(conflicts[n_conflicts], sizeof(conflicts[0]), "%s %s from 0x%08X", what, name ? name : "?", caller);
	n_conflicts++;
}

static int in_runner_text(uintptr_t p) {
	return p >= yoyoloader_mod.text_base && p < yoyoloader_mod.text_base + yoyoloader_mod.text_size;
}

// Install lines logged with "NOT <word>" ("virtual enemy captures: NOT installed", "glow events at rest: tiles NOT
// hooked"): every feature prints installed or NOT installed (docs/DEVELOPING.md, hook rule 6), but a native that
// declines in its own checks never reaches the registry above. debugPrintf hands each line here until hooks_report
// runs, so those failures are in the [HOOKS] line too. "NOT drawn" is a setting (psychEffects=0), not a failure.
static int hooks_reported;

void hooks_note_log(const char *line, int len) {
	if (hooks_reported || len < 4 || !memchr(line, 'N', len) || !strncmp(line, "[HOOKS]", 7))
		return;
	for (const char *p = line; (p = strstr(p, "NOT ")) != NULL; p += 4) {
		if (p[4] < 'a' || p[4] > 'z' || !strncmp(p + 4, "drawn", 5))
			continue;
		int n = len;
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
			n--;
		if (n_failures < MAX_FAILURES) {
			hook_failure *f = &failures[n_failures];
			memset(f, 0, sizeof(*f));
			f->why = "logged";
			snprintf(f->name, sizeof(f->name), "\"%.*s\"", n < 90 ? n : 90, line);
		}
		n_failures++;
		return;
	}
}

// --- Hookers ---

// Replaces a function at its entry with a native wrapper, returns a trampoline to the original
// (NULL if the prologue isn't the relocatable push {..., lr}; add fp, sp, #imm or mov fp, sp).
static void *entry_hook_install(uintptr_t entry, void *replacement, const char **why) {
	uint32_t *w = (uint32_t *)entry;
	if (!entry || (entry & 3) || (w[0] & 0xffff4000) != 0xe92d4000 ||
			((w[1] & 0xfffff000) != 0xe28db000 && w[1] != 0xe1a0b00d)) {
		*why = !entry ? "no symbol" : (entry & 3) ? "not ARM" : w[0] == 0xe51ff004 ? "already hooked" : "prologue";
		return NULL;
	}
	uint32_t *tramp = katana_rx_alloc(16);
	if (!tramp) {
		*why = "rx arena full";
		return NULL;
	}
	uint32_t code[4] = { w[0], w[1], 0xe51ff004, (uint32_t)(entry + 8) };
	kuKernelCpuUnrestrictedMemcpy(tramp, code, sizeof(code));
	kuKernelFlushCaches(tramp, sizeof(code));
	hook_addr(entry, (uintptr_t)replacement);
	kuKernelFlushCaches((void *)entry, 8);
	return tramp;
}

void *entry_hook(uintptr_t entry, void *replacement) {
	uintptr_t caller = (uintptr_t)__builtin_return_address(0);
	const char *why = NULL;
	void *tramp = entry_hook_install(entry, replacement, &why);
	if (tramp) {
		hook_add(entry, (uintptr_t)replacement, caller, HOOK_ENTRY);
		return tramp;
	}
	hook_record *by = hook_find(entry);
	if (by) {
		why = "already hooked";
		log_already_hooked("entry_hook NOT installed on", entry, caller, by);
	}
	hook_failed(entry, caller, why, NULL);
	return NULL;
}

event_func gml_replace(gml_func_entry *table, const char *name, event_func wrapper) {
	for (int i = 0; i < GML_FUNCS_SIZE / sizeof(gml_func_entry); i++) {
		if (table[i].name && !strcmp(table[i].name, name)) {
			event_func orig = table[i].func;
			table[i].func = wrapper;
			return orig;
		}
	}
	debugPrintf("Katana: %s not found in g_GMLFuncs\n", name);
	hook_failed(0, (uintptr_t)__builtin_return_address(0), "not in g_GMLFuncs", name);
	return NULL;
}

uintptr_t stub_event(const char *symbol) {
	static const uint32_t bx_lr = 0xe12fff1e;
	uintptr_t entry = so_symbol(&yoyoloader_mod, symbol);
	if (entry) {
		uintptr_t caller = (uintptr_t)__builtin_return_address(0);
		hook_record *by = hook_find(entry);
		if (by && by->kind != HOOK_STUB)
			log_already_hooked("stub_event disables the hook on", entry, caller, by);
		kuKernelCpuUnrestrictedMemcpy((void *)entry, &bx_lr, 4);
		kuKernelFlushCaches((void *)entry, 4);
		hook_add(entry, 0, caller, HOOK_STUB);
	}
	return entry;
}

so_hook hook_raw(uintptr_t addr, uintptr_t dst) {
	uintptr_t caller = (uintptr_t)__builtin_return_address(0);
	if (addr) {
		hook_record *by = hook_find(addr);
		if (by && by->caller != caller)
			conflict("hook_raw on", runner_symbol_at(addr), addr, caller, by, 0);
		else if (!by && !(addr & 1) && *(const uint32_t *)addr == 0xe51ff004) // ldr pc, [pc, #-4]: hooked off the record
			conflict("hook_raw on", runner_symbol_at(addr), addr, caller, NULL, *(const uint32_t *)(addr + 4));
		hook_add(addr, dst, caller, HOOK_RAW);
	}
	return hook_addr(addr, dst);
}

// The imported or exported symbol whose PLT GOT slot is slot, NULL if none (logging and install only)
static const char *got_symbol(const uintptr_t *slot) {
	so_module *mod = &yoyoloader_mod;
	for (int i = 0; i < mod->num_relplt; i++)
		if ((uintptr_t *)(mod->text_base + mod->relplt[i].r_offset) == slot)
			return mod->dynstr + mod->dynsym[ELF32_R_SYM(mod->relplt[i].r_info)].st_name;
	return NULL;
}

uintptr_t got_write(uintptr_t *slot, uintptr_t fn) {
	uintptr_t caller = (uintptr_t)__builtin_return_address(0);
	uintptr_t prev = *slot;
	hook_record *by = hook_find_kind((uintptr_t)slot, 0);
	if (by && by->caller != caller)
		conflict("got_write on", got_symbol(slot), (uintptr_t)slot, caller, by, 0);
	else if (!by) {
		// A runner function's slot holds its address until someone writes it; an import's holds the loader's target
		const char *name = got_symbol(slot);
		uintptr_t own = name ? so_symbol(&yoyoloader_mod, name) : 0;
		if (own && in_runner_text(own) && prev != own)
			conflict("got_write on", name, (uintptr_t)slot, caller, NULL, prev);
	}
	hook_add((uintptr_t)slot, fn, caller, HOOK_GOT);
	*slot = fn;
	return prev;
}

// A registry entry is { const char *name; void *func; int argc; }: the name is the word before func
static void *builtin_write(void **func, void *fn, int chain, uintptr_t caller) {
	void *prev = *func;
	const char *name = func[-1];
	hook_record *by = hook_find_kind((uintptr_t)func, 0);
	if (by && by->caller != caller && !chain)
		conflict("builtin_swap on", name, (uintptr_t)func, caller, by, 0);
	else if (!by && !in_runner_text((uintptr_t)prev))
		conflict(chain ? "builtin_chain on" : "builtin_swap on", name, (uintptr_t)func, caller, NULL, (uintptr_t)prev);
	if (chain && by)
		n_chained++;
	hook_add((uintptr_t)func, (uintptr_t)fn, caller, HOOK_BUILTIN);
	*func = fn;
	return prev;
}

void *builtin_swap(void **func, void *fn) {
	return builtin_write(func, fn, 0, (uintptr_t)__builtin_return_address(0));
}

void *builtin_chain(void **func, void *fn) {
	return builtin_write(func, fn, 1, (uintptr_t)__builtin_return_address(0));
}

void hooks_report(void) {
	char list[1536];
	int len = 0;
	list[0] = 0;
	for (int i = 0; i < n_failures && i < MAX_FAILURES && len < (int)sizeof(list); i++) {
		hook_failure *f = &failures[i];
		if (f->caller)
			len += snprintf(list + len, sizeof(list) - len, "%s%s (%s, from 0x%08X)", i ? ", " : "", f->name, f->why,
				f->caller);
		else
			len += snprintf(list + len, sizeof(list) - len, "%s%s (%s)", i ? ", " : "", f->name, f->why);
	}
	hooks_reported = 1;
	debugPrintf("[HOOKS] %d entry hooks, %d stubs%s, rx arena %u of %u bytes%s; not installed: %s%s\n",
		n_entry_hooks, n_stubs, hooks_unrecorded ? " (registry full)" : "",
		(unsigned)rx_arena_used, RX_ARENA_SIZE, rx_arena_refused ? ", refused requests" : "",
		n_failures ? list : "none", n_failures > MAX_FAILURES ? ", ..." : "");
	len = 0;
	list[0] = 0;
	for (int i = 0; i < n_conflicts && i < MAX_CONFLICTS && len < (int)sizeof(list); i++)
		len += snprintf(list + len, sizeof(list) - len, "%s%s", i ? ", " : "", conflicts[i]);
	debugPrintf("[HOOKS] also %d raw hooks, %d GOT slots, %d registry swaps (%d chained); conflicts: %s%s\n", n_raw,
		n_got, n_builtin, n_chained, n_conflicts ? list : "none", n_conflicts > MAX_CONFLICTS ? ", ..." : "");
}
