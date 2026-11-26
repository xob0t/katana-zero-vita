/*
 * hooks.h
 *
 * Hooking helpers shared by the port's modules (hooks.c). See docs/DEVELOPING.md, "Hook rules".
 */
#ifndef HOOKS_H
#define HOOKS_H

#include <stdint.h>
#include <stddef.h>

#include "katana_internal.h"

// Size in bytes of the runner's g_GMLFuncs table (compiled event functions by name)
#define GML_FUNCS_SIZE 79992

// Executable memory for trampolines and veneers: 16-byte aligned, never freed. NULL once the 64 KB arena is full.
void *katana_rx_alloc(size_t size);

// Replaces a runner function at its entry with replacement and returns a trampoline to the original, or NULL if the
// function can't be hooked: no entry, a prologue other than push {..., lr}; add fp, sp, #imm (or mov fp, sp), or a
// function that is already hooked (a second entry_hook on one target always fails; the registry logs who holds it).
void *entry_hook(uintptr_t entry, void *replacement);

// Swaps the compiled event `name` in g_GMLFuncs for wrapper and returns the previous function (NULL if not found).
// Replacing one event twice chains: the second wrapper gets the first one as its original.
event_func gml_replace(gml_func_entry *table, const char *name, event_func wrapper);

// Makes the runner function `symbol` return at once (bx lr). Returns its address, 0 if the symbol isn't found.
uintptr_t stub_event(const char *symbol);

// Called by debugPrintf with each formatted log line (without its time prefix) until hooks_report: a line saying
// "NOT installed", "NOT hooked" (any "NOT <lowercase word>" except "NOT drawn") is listed as a failure in the report
void hooks_note_log(const char *line, int len);

// hook_raw, got_write, builtin_swap and builtin_chain (recorded here) are declared in so_util.h, next to hook_addr

// Logs the "[HOOKS]" lines: hook counts, rx arena use and every hook that did not install; then the raw hooks, GOT
// slots and registry swaps, and every conflict among them
void hooks_report(void);

#endif
