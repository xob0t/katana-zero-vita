/*
 * wobble_rest.c
 *
 * Every enemy owns an obj_wobblefx, a spring that offsets the enemy's sprite when it is hit. Its Step runs every tick
 * for every enemy in the room (bunker2_3: 18, 0.2 ms per tick), and almost all of the time the spring is at rest:
 *
 *   motion_add(point_direction(x, y, 1, 1), 0.1);
 *   friction = 0.05;
 *   if (point_distance(x, y, 1, 1) < 0.1 && speed < 1) { speed = 0; x = 1; y = 1; }
 *   if (!instance_exists(parent)) instance_destroy();
 *
 * At rest (x = y = 1, no motion, friction already 0.05) the Step pushes the spring by 0.1 toward the point it is on,
 * then sets it back to the same rest state. The object has no sprite or mask, so it isn't in the collision tree. With
 * the parent alive, a Step at rest changes nothing and is skipped; any other state runs the original. The test reads
 * the instance's motion fields as raw floats (the getters' sources): x, y, direction, speed, friction, hspeed, vspeed.
 * exp_wobblerest0 runs every Step; exp_wobbleverify (debug) runs it after a skip as well and counts field changes.
 * Counters print with the profiler as [WOBBLEREST].
 */

#include <vitasdk.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

// CInstance float fields (APK 1.1.10 runner: GV_X, GV_Y, GV_Direction, GV_Speed, GV_Friction, GV_Hspeed, GV_Vspeed)
enum { F_X = 0xb8, F_Y = 0xbc, F_DIRECTION = 0xd0, F_SPEED = 0xd4, F_FRICTION = 0xd8, F_HSPEED = 0xe4, F_VSPEED = 0xe8 };
static const unsigned fields[] = {F_X, F_Y, F_DIRECTION, F_SPEED, F_FRICTION, F_HSPEED, F_VSPEED};
#define NFIELDS (sizeof(fields) / sizeof(*fields))

static int *var_parent;
static builtin_fn instance_exists_fn;
static event_func orig_wobble_step;
static int verify;
static unsigned rest_skips, rest_runs, verify_checks, verify_mismatches;

static uint32_t field(void *self, unsigned offset) {
	uint32_t v;
	memcpy(&v, (char *)self + offset, 4);
	return v;
}

static int parent_exists(void *self, void *other) {
	gml_rvalue *parent = gml_vget(self, var_parent[1]);
	if (!parent)
		return 0;
	gml_rvalue arg = *parent, result = {0, 0, 5};
	instance_exists_fn(&result, self, other, 1, &arg);
	unsigned kind = result.kind & 0xffffff;
	return (kind == 0 || kind == 13) && result.val > 0.5;
}

static int at_rest(void *self, void *other) {
	float friction = 0.05f;
	uint32_t friction_bits;
	memcpy(&friction_bits, &friction, 4);
	return field(self, F_X) == 0x3f800000 && field(self, F_Y) == 0x3f800000 && field(self, F_DIRECTION) == 0 &&
		field(self, F_SPEED) == 0 && field(self, F_FRICTION) == friction_bits &&
		field(self, F_HSPEED) == 0 && field(self, F_VSPEED) == 0 && // +0.0 only: speed = 0 writes positive zeros
		parent_exists(self, other);
}

static void wobble_step(void *self, void *other) {
	if (!at_rest(self, other)) {
		rest_runs++;
		orig_wobble_step(self, other);
		return;
	}
	rest_skips++;
	if (!verify)
		return;
	uint32_t before[NFIELDS];
	for (unsigned i = 0; i < NFIELDS; i++)
		before[i] = field(self, fields[i]);
	orig_wobble_step(self, other);
	verify_checks++;
	for (unsigned i = 0; i < NFIELDS; i++) {
		if (field(self, fields[i]) != before[i]) {
			if (verify_mismatches++ < 20)
				debugPrintf("[WOBBLEREST] verify: field +0x%x %08x -> %08x\n", fields[i], (unsigned)before[i],
					(unsigned)field(self, fields[i]));
			break;
		}
	}
}

void wobble_rest_report(int frames) {
	if (!orig_wobble_step)
		return;
	if (rest_skips || rest_runs)
		debugPrintf("[WOBBLEREST] wobble steps skipped %.1f run %.1f per tick\n", (float)rest_skips / frames,
			(float)rest_runs / frames);
	if (verify)
		debugPrintf("[WOBBLEREST] verify: %u skips checked, %u mismatches\n", verify_checks, verify_mismatches);
	rest_skips = rest_runs = 0;
}

void wobble_rest_install(gml_func_entry *table) {
	if (exp_flag("wobblerest0")) {
		debugPrintf("wobble springs at rest: stepped (exp_wobblerest0)\n");
		return;
	}
	verify = debugMode && exp_flag("wobbleverify");
	var_parent = (int *)so_symbol(&yoyoloader_mod, "g_VAR_parent");
	instance_exists_fn = (builtin_fn)so_symbol(&yoyoloader_mod, "_Z16F_InstanceExistsR6RValueP9CInstanceS2_iPS_");
	if (var_parent && instance_exists_fn)
		orig_wobble_step = gml_replace(table, "gml_Object_obj_wobblefx_Step_0", wobble_step);
	debugPrintf("wobble springs at rest: %s\n", orig_wobble_step ? "skipped" : "NOT hooked");
}
