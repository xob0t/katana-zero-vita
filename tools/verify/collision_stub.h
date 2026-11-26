#ifndef COLLISION_STUB_H
#define COLLISION_STUB_H
#include <stdint.h>
#include <stddef.h>
typedef struct so_module so_module;
typedef struct {
    uintptr_t addr, thumb_addr;
    uint32_t orig_instr[2], patch_instr[2];
} so_hook;
extern so_module yoyoloader_mod;
uintptr_t so_symbol(so_module *, const char *);
uintptr_t *so_got_slot(so_module *, const char *);
so_hook hook_addr(uintptr_t, uintptr_t);
void kuKernelCpuUnrestrictedMemcpy(void *, const void *, size_t);
void kuKernelFlushCaches(void *, size_t);
extern int debugMode;
int exp_flag(const char *name);
int debugPrintf(const char *fmt, ...);
#endif
