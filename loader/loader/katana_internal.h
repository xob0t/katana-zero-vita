/*
 * katana_internal.h
 *
 * Types and symbols shared by katana.c and the katana_*.c files split out of it.
 */
#ifndef KATANA_INTERNAL_H
#define KATANA_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

// GameMaker RValue (16 bytes): payload, flags, kind
typedef struct {
	double val;
	uint32_t flags;
	uint32_t kind;
} gml_rvalue;

typedef void (*builtin_fn)(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args);

typedef void (*event_func)(void *self, void *other);

typedef struct {
	const char *name;
	event_func func;
	void *unk;
} gml_func_entry;

typedef struct {
	const char *name;
	void *func;
	int argc;
} runner_function;
typedef void *(*gml_script_fn)(void *self, void *other, void *result, int argc, void **args);

extern int debugMode;
extern int t30_active;
extern int fps30;
void *katana_rx_alloc(size_t size);
void katana_cloth_report(int frames);
void callprof_install(so_module *mod);
void evprof_install(so_module *mod);
void katana_menu_install(gml_func_entry *table);
void katana_display_install(void);
void katana_display_hook_draw(void);
void katana_prof_install(void);

extern uint32_t *frame_count;
extern void **global_obj_ptr;
extern int *current_room;
extern int *var_recolor_timer;
extern uint32_t recolor_changes, recolor_frames;
extern double last_recolor_timer;
extern int light_fast_mode;
extern int t30_installed;
extern int enemy_half_rate, eh_installed; // half-rate enemies (katana_t30.c)
void *entry_hook(uintptr_t entry, void *replacement);
event_func gml_replace(gml_func_entry *table, const char *name, event_func wrapper);
gml_rvalue *global_var(int *var);
gml_rvalue *gml_vget(void *obj, int slot);
double global_real(int *var, double fallback);
gml_rvalue *instance_var(void *self, int *var);
void t30_install(gml_func_entry *table);
void t30_update(uint32_t work_us);
void fps_menu_install(gml_func_entry *table);
void text_smooth_sync(void);
void fps_mode_sync(void);
void appsurf_copy_install(void);

#endif
