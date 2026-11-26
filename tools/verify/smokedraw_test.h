#ifndef SMOKEDRAW_TEST_H
#define SMOKEDRAW_TEST_H
#include <stdint.h>
#include <stddef.h>
typedef struct so_module so_module;
typedef struct {
	uintptr_t addr, thumb_addr;
	uint32_t orig_instr[2], patch_instr[2];
} so_hook;
extern so_module yoyoloader_mod;
extern volatile unsigned so_hook_generation;
extern int debugMode;
uintptr_t so_symbol(so_module *, const char *);
uintptr_t *so_got_slot(so_module *, const char *);
so_hook hook_addr(uintptr_t, uintptr_t);
so_hook hook_raw(uintptr_t, uintptr_t);
void kuKernelCpuUnrestrictedMemcpy(void *, const void *, size_t);
void kuKernelFlushCaches(void *, size_t);
int exp_flag(const char *name);
int debugPrintf(const char *fmt, ...);
#endif
