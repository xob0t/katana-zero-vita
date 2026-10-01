/*
 * scripts_native.c
 *
 * Native replacements for small, hot Katana ZERO GML scripts. Each replacement handles the common
 * case exactly like the compiled script (same variable reads, the runner's epsilon comparison and
 * result kind) and calls the original compiled function for anything else.
 * Reconstruction notes: work/codex/small_scripts.md.
 */

#include <vitasdk.h>
#include <math.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

typedef struct {
	union {
		double real;
		uint64_t bits;
		void *ptr;
	} value;
	uint32_t flags;
	uint32_t kind;
} YYRValue;

enum { KIND_REAL = 0, KIND_UNDEFINED = 5, KIND_OBJECT = 6, KIND_BOOL = 13 };

typedef YYRValue *(*script_fn)(void *self, void *other, YYRValue *result, int argc, YYRValue **args);
typedef YYRValue *(*virtual_get_fn)(void *obj, uint32_t slot);
typedef void (*variable_get_fn)(const YYRValue *receiver, int slot, int array_index, YYRValue *out, int, int);
typedef int (*compare_fn)(const YYRValue *a, const YYRValue *b, double epsilon, int mode);
typedef void (*free_fn)(YYRValue *value);

static script_fn orig_inside_screen;
static variable_get_fn variable_get;
static compare_fn compare_value;
static free_fn free_value;
static void **global_ptr;
static double *math_epsilon;
static uint32_t *var_xview, *var_yview, *var_wview, *var_hview, *var_x, *var_y;

// Descriptor slots are filled by the runner after load, so read them at call time
#define SLOT(desc) ((desc)[1])

static YYRValue *global_get(uint32_t *desc) {
	void *global = *global_ptr;
	virtual_get_fn get = (*(virtual_get_fn **)global)[2];
	return get(global, SLOT(desc));
}

static int self_get_real(YYRValue *receiver, uint32_t *desc, YYRValue *out) {
	out->kind = KIND_UNDEFINED;
	variable_get(receiver, (int)SLOT(desc), (int)0x80000000, out, 0, 0);
	if (out->kind != KIND_REAL || !isfinite(out->value.real)) {
		if ((0x46u >> (out->kind & 31)) & 1)
			free_value(out);
		return 0;
	}
	return 1;
}

static int compare_real(YYRValue *a, double b) {
	YYRValue bound;
	memset(&bound, 0, sizeof(bound));
	bound.value.real = b;
	bound.kind = KIND_REAL;
	return compare_value(a, &bound, *math_epsilon, 1);
}

// inside_screen(margin): xview-margin <= x < xview+wview+margin, same for y (epsilon compares)
static YYRValue *inside_screen_native(void *self, void *other, YYRValue *result, int argc, YYRValue **args) {
	if (argc != 1 || !args || !args[0] || args[0]->kind != KIND_REAL || !isfinite(args[0]->value.real) || !*global_ptr)
		return orig_inside_screen(self, other, result, argc, args);

	YYRValue *vx = global_get(var_xview), *vy = global_get(var_yview);
	YYRValue *vw = global_get(var_wview), *vh = global_get(var_hview);
	if (!vx || !vy || !vw || !vh || vx->kind != KIND_REAL || vy->kind != KIND_REAL ||
		vw->kind != KIND_REAL || vh->kind != KIND_REAL)
		return orig_inside_screen(self, other, result, argc, args);

	double margin = args[0]->value.real;
	double view_x = vx->value.real, view_y = vy->value.real, view_w = vw->value.real, view_h = vh->value.real;
	YYRValue receiver, v;
	memset(&receiver, 0, sizeof(receiver));
	receiver.value.ptr = self;
	receiver.kind = KIND_OBJECT;

	int inside = 0;
	if (!self_get_real(&receiver, var_x, &v))
		return orig_inside_screen(self, other, result, argc, args);
	if (compare_real(&v, view_x - margin) >= 1) {
		if (!self_get_real(&receiver, var_x, &v))
			return orig_inside_screen(self, other, result, argc, args);
		if (compare_real(&v, (view_x + view_w) + margin) == -1) {
			if (!self_get_real(&receiver, var_y, &v))
				return orig_inside_screen(self, other, result, argc, args);
			if (compare_real(&v, view_y - margin) >= 1) {
				if (!self_get_real(&receiver, var_y, &v))
					return orig_inside_screen(self, other, result, argc, args);
				inside = compare_real(&v, (view_y + view_h) + margin) == -1;
			}
		}
	}
	result->value.real = inside ? 1.0 : 0.0;
	result->kind = KIND_BOOL;
	return result;
}

// Installs the native scripts through their PLT GOT slots (these symbols are excluded from
// scripts/patch_plt_calls.py so their calls still go through the PLT)
void scripts_native_install(void) {
	variable_get = (variable_get_fn)so_symbol(&yoyoloader_mod, "_Z23YYGML_Variable_GetValueRK8YYRValueiiP6RValuebb");
	compare_value = (compare_fn)so_symbol(&yoyoloader_mod, "_Z12YYCompareValRK6RValueS1_db");
	free_value = (free_fn)so_symbol(&yoyoloader_mod, "_Z16FREE_RValue__PreP6RValue");
	global_ptr = (void **)so_symbol(&yoyoloader_mod, "g_pGlobal");
	math_epsilon = (double *)so_symbol(&yoyoloader_mod, "g_GMLMathEpsilon");
	var_xview = (uint32_t *)so_symbol(&yoyoloader_mod, "g_VAR_xview");
	var_yview = (uint32_t *)so_symbol(&yoyoloader_mod, "g_VAR_yview");
	var_wview = (uint32_t *)so_symbol(&yoyoloader_mod, "g_VAR_wview");
	var_hview = (uint32_t *)so_symbol(&yoyoloader_mod, "g_VAR_hview");
	var_x = (uint32_t *)so_symbol(&yoyoloader_mod, "g_VAR_x");
	var_y = (uint32_t *)so_symbol(&yoyoloader_mod, "g_VAR_y");
	if (!variable_get || !compare_value || !free_value || !global_ptr || !math_epsilon ||
		!var_xview || !var_yview || !var_wview || !var_hview || !var_x || !var_y)
		return;

	uintptr_t *slot = so_got_slot(&yoyoloader_mod, "_Z24gml_Script_inside_screenP9CInstanceS0_R8YYRValueiPPS1_");
	if (slot) {
		orig_inside_screen = (script_fn)*slot;
		*slot = (uintptr_t)&inside_screen_native;
		debugPrintf("Native inside_screen installed\n");
	}
}
