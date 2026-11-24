/*
 * blood_drops.c
 *
 * With BLOOD SPRAY off (global.show_blood false) the game still creates the obj_blood drops of every hit and runs
 * their Step: obj_blood Create only sets visible = false. The Step's user event 0 moves the drop, animates it at
 * image_speed 0.2 * fr and runs a floor test (two block_meeting calls, ~22 us each) that sets kill = true; a killed
 * drop animates twice as fast. The drop destroys itself at Animation End, usually still in the air. The decals,
 * every random() call of the event, drawactors' drop drawing and the replay recording are already gated on
 * show_blood, so the hidden drops cost ~0.6 ms per tick in fights and show nothing, with two exceptions:
 * - obj_opto_master user event 5 counts instance_number(obj_blood) into global.particle_scaler, which scales how
 *   many sparks, dust and debris other effects spawn (and the blood emitters' own drop rate).
 * - obj_smokescreen_master Draw draws every drop that overlaps a smoke cloud, whatever show_blood is.
 *
 * Here a hidden drop (show_blood false and the drop invisible) skips its Step: it only sets image_speed to
 * BLOOD_SPEED * 0.2 * fr, so the runner still animates it and its Animation End still destroys it, after about the
 * mean lifetime of an original drop. The drop stays a real instance: instance_number, instance ids, the creators' writes to it and the
 * cutscenes' with (obj_blood) instance_destroy() all work as before, so the particle budget keeps counting it. On
 * its first skipped Step it moves far outside the room, so the smoke screen no longer finds it inside a cloud (blood
 * isn't drawn inside smoke clouds; docs/changes.md lists this). Nothing in the game deactivates instances by region.
 * With BLOOD SPRAY on nothing changes. exp_blooddrops0 keeps the original Step.
 * Debug counters print with the profiler as [BLOOD] and [PBUDGET].
 */

#include <vitasdk.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

// Hidden drops animate at BLOOD_SPEED * 0.2 * fr per tick. Airborne drops animate at 0.2 * fr and landed ones twice
// as fast. In club_4_2 fights 24-30% of the original drops landed, and their mean lifetime was 3.5% below the
// airborne one's (the [BLOOD] lifetime line, exp_blooddrops0). With 1.035 the ghosts (below) averaged 0.985 and
// 0.992 of the real drop count over two 58-s fights, so 1.025 centres it.
#define BLOOD_SPEED 1.025
#define FAR_AWAY -100000.0

typedef int (*builtin_var_fn)(void *inst, int index, gml_rvalue *value);

static int *var_show_blood, *var_fr, *var_kill, *var_particle_scaler;
static builtin_var_fn gv_visible, sv_image_speed, gv_x, sv_x, sv_y, gv_image_index, gv_image_number;
static event_func orig_blood_step, orig_blood_anim_end, orig_opto_user5;
static int (*instance_number)(void *self, void *other, int obj);
int blood_drops_installed;

static unsigned hidden_steps, hidden_moved, orig_steps;

static double builtin_get(builtin_var_fn get, void *self) {
	gml_rvalue v = {0, 0, 0};
	get(self, (int)0x80000000, &v);
	return v.kind == 0 || v.kind == 13 ? v.val : 0;
}

static void builtin_set(builtin_var_fn set, void *self, double x) {
	gml_rvalue v = {x, 0, 0};
	set(self, (int)0x80000000, &v);
}

// A real or boolean variable (read-only lookup), NULL otherwise
static gml_rvalue *read_var(void *obj, int *var) {
	gml_rvalue *v = obj && var ? gml_vget(obj, var[1]) : NULL;
	return v && (v->kind == 0 || v->kind == 13) ? v : NULL;
}

/*
 * Lifetime statistics (debugMode): every drop's ticks from its first Step to its Animation End, and for the original
 * Step also the ticks an airborne drop (0.2 * fr per tick from the same first frame) would have lived. Their ratio is
 * the change in the mean number of live drops, which is what the particle budget counts.
 */
#define DROP_SLOTS 256
static struct drop_stat {
	uint32_t id, born;
	float frame0, anim; // image_index at the first Step; airborne animation (0.2 * fr per tick) since then
	uint8_t landed;
} drops[DROP_SLOTS];
static int stats_on;
static unsigned st_died, st_landed, st_lost;
static double st_ticks, st_model_ticks;

// With the original Step (exp_blooddrops0, debugMode): a ghost per drop animates like a hidden drop would (from its
// first Step at BLOOD_SPEED * 0.2 * fr per tick until image_number), so the same run gives the mean number of live
// hidden drops next to the real count. Room starts (deaths too) clear them, as the room end destroys the drops.
#define GHOSTS 512
static struct ghost {
	float frame, frames;
} ghosts[GHOSTS];
static int ghost_count;
static double ghost_sum;
static unsigned ghost_lost;

static void ghost_add(void *self) {
	if (ghost_count >= GHOSTS) {
		ghost_lost++;
		return;
	}
	ghosts[ghost_count].frame = builtin_get(gv_image_index, self);
	ghosts[ghost_count].frames = builtin_get(gv_image_number, self);
	ghost_count++;
}

// Once per tick (the budget event): count the live ghosts, then animate them for this tick
static void ghost_tick(double fr) {
	ghost_sum += ghost_count;
	for (int i = 0; i < ghost_count;) {
		ghosts[i].frame += BLOOD_SPEED * 0.2 * fr;
		if (ghosts[i].frame >= ghosts[i].frames)
			ghosts[i] = ghosts[--ghost_count];
		else
			i++;
	}
}

static struct drop_stat *drop_slot(void *self, int create) {
	uint32_t id = *(uint32_t *)((char *)self + 0x80);
	struct drop_stat *free_slot = NULL;
	for (int i = 0; i < DROP_SLOTS; i++) {
		if (drops[i].id == id && id)
			return &drops[i];
		if (!free_slot && (!drops[i].id || *frame_count - drops[i].born > 3600))
			free_slot = &drops[i];
	}
	if (!create)
		return NULL;
	if (!free_slot) {
		st_lost++;
		return NULL;
	}
	if (free_slot->id)
		st_lost++; // never reached Animation End (destroyed by a cutscene or the room end)
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->id = id;
	free_slot->born = *frame_count;
	free_slot->frame0 = builtin_get(gv_image_index, self);
	if (!blood_drops_installed)
		ghost_add(self);
	return free_slot;
}

static void stats_step(void *self, double fr) {
	struct drop_stat *d = drop_slot(self, 1);
	if (!d)
		return;
	d->anim += 0.2 * fr;
	gml_rvalue *k = read_var(self, var_kill);
	if (k && k->val > 0.5)
		d->landed = 1;
}

static void blood_anim_end(void *self, void *other) {
	struct drop_stat *d = drop_slot(self, 0);
	if (d) {
		double ticks = *frame_count - d->born + 1, model = ticks;
		double left = builtin_get(gv_image_number, self) - d->frame0 - d->anim; // airborne animation still to run
		gml_rvalue *fr = read_var(*global_obj_ptr, var_fr);
		if (left > 0 && fr && fr->val > 0)
			model += ceil(left / (0.2 * fr->val));
		st_died++;
		st_landed += d->landed;
		st_ticks += ticks;
		st_model_ticks += model;
		d->id = 0;
	}
	orig_blood_anim_end(self, other);
}

// Particle budget (debugMode): obj_opto_master user event 5 sets global.particle_scaler from these counts
static const struct {
	int index;
	const char *name;
} budget_objs[] = {
	{59, "blood"}, {944, "spark"}, {943, "spark_particle"}, {865, "rubble"}, {954, "sprite_chunk"},
	{367, "dustcloud"}, {964, "staticparticle"}, {925, "smokecloud"}, {455, "fog"},
};
#define BUDGET_OBJS (sizeof(budget_objs) / sizeof(budget_objs[0]))
static double budget_sum[BUDGET_OBJS], scaler_sum;
static unsigned budget_samples, scaler_floor, lag_samples, max_dips;
static int *var_under_timing, *var_under_dips;

static void opto_user5(void *self, void *other) {
	orig_opto_user5(self, other);
	gml_rvalue *s;
	gml_rvalue *fr = read_var(*global_obj_ptr, var_fr);
	if (stats_on && !blood_drops_installed && fr)
		ghost_tick(fr->val);
	for (unsigned i = 0; i < BUDGET_OBJS; i++)
		budget_sum[i] += instance_number(self, other, budget_objs[i].index);
	if ((s = read_var(*global_obj_ptr, var_particle_scaler))) {
		scaler_sum += s->val;
		scaler_floor += s->val <= 0.2 + 1e-9;
	}
	gml_rvalue *t = read_var(self, var_under_timing), *d = read_var(self, var_under_dips);
	lag_samples += t && t->val != 0; // the lag mode (underThreshTiming counting down)
	if (d && d->val > max_dips)
		max_dips = (unsigned)d->val;
	budget_samples++;
}

static event_func orig_opto_room_start;
static void opto_room_start(void *self, void *other) {
	ghost_count = 0;
	orig_opto_room_start(self, other);
}

static void blood_step(void *self, void *other) {
	gml_rvalue *show = read_var(*global_obj_ptr, var_show_blood), *fr = read_var(*global_obj_ptr, var_fr);
	// show_blood false as the game's if() reads it (BOOL_RValue: > 0.5); Create made the drop invisible
	if (!blood_drops_installed || !show || show->val > 0.5 || !fr || builtin_get(gv_visible, self) != 0) {
		if (stats_on && fr)
			stats_step(self, fr->val);
		orig_steps++;
		orig_blood_step(self, other);
		return;
	}
	if (stats_on)
		stats_step(self, fr->val);
	hidden_steps++;
	builtin_set(sv_image_speed, self, BLOOD_SPEED * 0.2 * fr->val);
	if (builtin_get(gv_x, self) != FAR_AWAY) {
		builtin_set(sv_x, self, FAR_AWAY);
		builtin_set(sv_y, self, FAR_AWAY);
		hidden_moved++;
	}
}

void blood_drops_report(int frames) {
	if (!orig_blood_step)
		return;
	if (hidden_steps || orig_steps)
		debugPrintf("[BLOOD] %s: drop steps hidden %.1f (new %.1f) original %.1f per tick\n",
			blood_drops_installed ? "hidden drops skip Step" : "original (exp_blooddrops0)", (float)hidden_steps / frames,
			(float)hidden_moved / frames, (float)orig_steps / frames);
	if (st_died)
		debugPrintf("[BLOOD] lifetime: %u drops died, %u landed (%.1f%%), mean %.2f ticks, airborne model %.2f "
			"(x%.3f), %u never reached Animation End\n", st_died, st_landed, 100.0f * st_landed / st_died,
			st_ticks / st_died, st_model_ticks / st_died, st_ticks > 0 ? st_model_ticks / st_ticks : 0, st_lost);
	if (budget_samples) {
		char line[512];
		int len = snprintf(line, sizeof(line), "[PBUDGET] %u samples in %d ticks, scaler %.4f (floor %.1f%%), lag mode "
			"%.1f%%, dips %u, mean count", budget_samples, frames, scaler_sum / budget_samples,
			100.0f * scaler_floor / budget_samples, 100.0f * lag_samples / budget_samples, max_dips);
		for (unsigned i = 0; i < BUDGET_OBJS && len < (int)sizeof(line); i++)
			len += snprintf(line + len, sizeof(line) - len, " %s %.2f", budget_objs[i].name,
				budget_sum[i] / budget_samples);
		if (stats_on && !blood_drops_installed && len < (int)sizeof(line))
			len += snprintf(line + len, sizeof(line) - len, " | hidden-drop ghosts %.2f (lost %u)",
				ghost_sum / budget_samples, ghost_lost);
		debugPrintf("%s\n", line);
	}
	hidden_steps = hidden_moved = orig_steps = 0;
	st_died = st_landed = st_lost = 0;
	st_ticks = st_model_ticks = 0;
	memset(budget_sum, 0, sizeof(budget_sum));
	scaler_sum = ghost_sum = 0;
	ghost_lost = 0;
	budget_samples = scaler_floor = lag_samples = max_dips = 0;
}

void blood_drops_install(gml_func_entry *table) {
	var_show_blood = (int *)so_symbol(&yoyoloader_mod, "g_VAR_show_blood");
	var_fr = (int *)so_symbol(&yoyoloader_mod, "g_VAR_fr");
	var_kill = (int *)so_symbol(&yoyoloader_mod, "g_VAR_kill");
	var_particle_scaler = (int *)so_symbol(&yoyoloader_mod, "g_VAR_particle_scaler");
#define BD_RESOLVE(var, sym) var = (builtin_var_fn)so_symbol(&yoyoloader_mod, sym)
	BD_RESOLVE(gv_visible, "_Z10GV_VisibleP9CInstanceiP6RValue");
	BD_RESOLVE(sv_image_speed, "_Z13SV_ImageSpeedP9CInstanceiP6RValue");
	BD_RESOLVE(gv_x, "_Z4GV_XP9CInstanceiP6RValue");
	BD_RESOLVE(sv_x, "_Z4SV_XP9CInstanceiP6RValue");
	BD_RESOLVE(sv_y, "_Z4SV_YP9CInstanceiP6RValue");
	BD_RESOLVE(gv_image_index, "_Z13GV_ImageIndexP9CInstanceiP6RValue");
	BD_RESOLVE(gv_image_number, "_Z14GV_ImageNumberP9CInstanceiP6RValue");
#undef BD_RESOLVE
	instance_number = (void *)so_symbol(&yoyoloader_mod, "_Z21YYGML_instance_numberP9CInstanceS0_i");
	int skip = !exp_flag("blooddrops0");
	int ready = var_show_blood && var_fr && global_obj_ptr && gv_visible && sv_image_speed && gv_x && sv_x && sv_y;
	// debugMode: the Step is wrapped either way for the counters and lifetime statistics
	if (ready && (skip || debugMode) &&
			(orig_blood_step = gml_replace(table, "gml_Object_obj_blood_Step_0", blood_step))) {
		blood_drops_installed = skip;
		if (debugMode && var_kill && gv_image_index && gv_image_number &&
				(orig_blood_anim_end = gml_replace(table, "gml_Object_obj_blood_Other_7", blood_anim_end)))
			stats_on = 1;
	}
	var_under_timing = (int *)so_symbol(&yoyoloader_mod, "g_VAR_underThreshTiming");
	var_under_dips = (int *)so_symbol(&yoyoloader_mod, "g_VAR_underThreshDips");
	if (debugMode && instance_number && var_particle_scaler && global_obj_ptr)
		orig_opto_user5 = gml_replace(table, "gml_Object_obj_opto_master_Other_15", opto_user5);
	if (stats_on && !blood_drops_installed && orig_opto_user5)
		orig_opto_room_start = gml_replace(table, "gml_Object_obj_opto_master_Other_4", opto_room_start);
	debugPrintf("hidden blood drops: %s\n", blood_drops_installed ? "Step skipped" :
		!skip ? "original Step (exp_blooddrops0)" : "NOT installed");
}
