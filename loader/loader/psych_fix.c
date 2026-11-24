/*
 * psych_fix.c
 *
 * The secret Psychiatrist fight (room_psychiatrist, obj_cutscene_psych_11_battle and the obj_psychboss_* objects).
 * Exact changes only: the picture, the random numbers and every game variable stay as they were.
 *
 * Empty offscreen pass. obj_cutscene_psych_11_battle's Draw redraws a surface every frame and then draws it onto the
 * screen (global.attack_surface, 640x360):
 *
 *       surface_set_target(global.attack_surface); draw_clear_alpha(c, a);
 *       with (obj_psychboss_portal) event_user(15);
 *       with (obj_psychboss_attack_knife) if (!instance_exists(parent)) event_user(15);
 *       with (obj_psychboss_turret) if (!instance_exists(parent)) event_user(15);
 *       surface_reset_target();
 *       draw_surface_ext(...) or draw_distorted_surface(...)   // the latter draws random numbers

 *
 * Each surface_set_target / surface_reset_target pair ends the GPU scene and starts two more: the CPU then waits for
 * the GPU (part 3 of the fight, the black void, ran at 49 fps with 12 scenes per frame). In parts 2 and 3 nothing is
 * drawn between the clear and the reset (no portal, no orphaned knife or turret). Such a pass only clears the surface
 * to the colour it already holds. The event's calls are intercepted (its PLT GOT slots are swapped while it runs) so that the target switch
 * and the clear are deferred: the first user event inside the pass replays them first, and a pass that drew nothing
 * skips them when the surface is known to hold exactly that clear already (its last pass was such a clear, and no
 * free, create, resize or copy touched it since; only this object's events use global.attack_surface). The rest of
 * the event runs unchanged, including the
 * draw of the surface (with its random numbers). Part 3: 12 -> 10 scenes per frame, 49.2 -> 51.9 fps.
 * exp_psychlazy0 turns this off; exp_psychlazyverify (debug) replays every skipped pass after reading the surface back
 * and checking every pixel against the clear (exp_psychlazyfault: a negative control that paints the surface white
 * untracked before every 60th check).
 *
 * Not done: obj_psychboss_draw_eyes draws the eyes through a screen-size surface the same way, but part 3 always has
 * eyes (never an empty pass), and drawing them directly would subtract the lids (blend mode 3) from the background
 * too. obj_psychboss_effects (type -1 in part 3, every frame) fills its copy black, adds it to the screen, copies the
 * screen into it, draws a black rectangle and the copy back: not a no-op when the application surface's alpha is
 * below 1, so it stays.
 *
 * Restarts. Each death restarts the room: the fight's surfaces are freed (CleanUp) and created again, the two 1280x720
 * ones (obj_psychboss_effects and obj_psychboss_draw_eyes screencopy) ~35 ms each, the 640x360 attack surface ~25 ms
 * and even 70x70 ones ~11 ms (render target setup): the restart frame took ~143 ms. In room_psychiatrist, freed
 * surfaces are parked for 3 frames instead; a surface_create of the same size takes one back and clears it to 0
 * (what a new surface holds: exp_psychlazyverify reads new and reused surfaces back and checks it), surface_exists
 * says false for parked ids, and parked surfaces nobody took are freed. exp_psychpool0 turns this off.
 *
 * The native ground and rose petal Draws are in psych_ground.c and psych_petal.c (exp_psychground0, exp_psychpetal0).
 *
 * Measurement (debug): exp_psychab switches all of the above off in every other 2 s report window ([PSYCHAB] A/B lines
 * for work-tree ab scripts); exp_psychskip=<mask> with exp_psychskipab does the same for cuts that would be VISIBLE
 * (1: no obj_psychboss_effects Draw, 2: half the rose petals, Step and Draw), to size them (without exp_psychskipab
 * they stay on, for screenshots); exp_psychres logs the
 * effects type, eye counts and surface_create timings ([PSYCHFIX] lines).
 */
#include <vitasdk.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <kubridge.h>
#include <vitaGL.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

#define ABI __attribute__((pcs("aapcs")))
typedef int ABI (*set_target_fn)(int surf, int depth);
typedef int ABI (*reset_target_fn)(void);
typedef void ABI (*event_user_fn)(void *self, void *other, int n);
typedef gml_rvalue *ABI (*legacy_fn)(void *self, void *other, gml_rvalue *result, int argc, int funcid, gml_rvalue **argv);
typedef int ABI (*surf_int_fn)(int surf);
typedef void *ABI (*surf_get_fn)(int surf);

static set_target_fn real_set_target;
static reset_target_fn real_reset_target;
static event_user_fn real_event_user;
static legacy_fn real_legacy;
static surf_int_fn surf_width, surf_height, surf_texture;
static surf_get_fn surf_get;
static int fid_draw_clear_alpha = -1, fid_instance_exists = -1;
static int lazy_ok; // the battle Draw's interception is in place (psych_fix_install)

static int lazy_verify, lazy_fault, research;
static unsigned lazy_faults; // negative control: passes painted white
static int ab, ab_off; // exp_psychab: everything off in every other report window

// ---------------------------------------------------------------------------------------------------------------
// Deferred surface passes

typedef struct {
	const char *name;
	int active, flushed;              // inside a deferred pass; target switch and clear replayed
	int surf, depth;                  // the deferred surface_set_target
	int have_clear;                   // the deferred draw_clear_alpha
	void *cself, *cother;
	gml_rvalue cargs[2];
	int known;                        // the surface holds exactly known_args' clear
	int known_surf;
	void *known_rs;                   // RSurface* and texture when it was cleared
	int known_tex;
	gml_rvalue known_args[2];
	uint32_t known_pixel;             // verify: the first pixel right after that clear
	unsigned passes, skips, flushes, clears, events, verified, mismatches;
} lazy_pass;

static lazy_pass battle = {"battle"};
static lazy_pass *const passes[] = {&battle};

static void forget_surface(int surf) {
	for (unsigned i = 0; i < sizeof(passes) / sizeof(passes[0]); i++)
		if (passes[i]->known && passes[i]->known_surf == surf)
			passes[i]->known = 0;
}

static void lazy_flush(lazy_pass *p) {
	if (!p->active || p->flushed)
		return;
	p->flushed = 1;
	p->flushes++;
	real_set_target(p->surf, p->depth);
	if (p->have_clear) {
		gml_rvalue result = {0, 0, 5};
		gml_rvalue *argv[2] = {&p->cargs[0], &p->cargs[1]};
		real_legacy(p->cself, p->cother, &result, 2, fid_draw_clear_alpha, argv);
	}
}

static int in_battle_draw, battle_pass_done; // inside the battle Draw (GOT slots swapped); its pass already ran

static int ABI lazy_set_target(lazy_pass *p, int surf, int depth) {
	if (!in_battle_draw || battle_pass_done || p->active) { // only the Draw's first target switch is deferred
		lazy_flush(p);
		return real_set_target(surf, depth);
	}
	battle_pass_done = 1;
	p->active = 1;
	p->flushed = 0;
	p->have_clear = 0;
	p->surf = surf;
	p->depth = depth;
	p->passes++;
	return 1;
}

static gml_rvalue *ABI lazy_clear(lazy_pass *p, void *self, void *other, gml_rvalue *result, int argc, int funcid,
		gml_rvalue **argv) {
	if (!p->active || p->flushed)
		return real_legacy(self, other, result, argc, funcid, argv);
	if (funcid == fid_instance_exists) // the knife / turret loops' parent test: draws nothing
		return real_legacy(self, other, result, argc, funcid, argv);
	if (p->have_clear || argc != 2 || funcid != fid_draw_clear_alpha ||
			(argv[0]->kind & 0xffffff) != 0 || (argv[1]->kind & 0xffffff) != 0 ||
			((1u << (result->kind & 31)) & 0x46)) {
		lazy_flush(p);
		return real_legacy(self, other, result, argc, funcid, argv);
	}
	p->have_clear = 1;
	p->cself = self;
	p->cother = other;
	p->cargs[0] = *argv[0];
	p->cargs[1] = *argv[1];
	// What YYGML_CallLegacyFunction leaves in the result before calling the builtin (draw_clear_alpha writes none)
	uint32_t lo = 0;
	memcpy(&result->val, &lo, 4);
	result->flags = 0;
	result->kind = 5;
	return result;
}

static void ABI lazy_event(lazy_pass *p, void *self, void *other, int n) {
	if (in_battle_draw)
		p->events++; // every user event the battle Draw runs, in its pass or not
	lazy_flush(p);
	real_event_user(self, other, n);
}

static uint32_t read_pixel_check(int w, int h, uint32_t ref, int *bad) {
	uint32_t *px = malloc((size_t)w * h * 4);
	uint32_t first = 0;
	*bad = 0;
	if (!px)
		return 0;
	// End the open scene and wait for the GPU: glReadPixels only does that when the read target is the scene's
	glFinish();
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	first = px[0];
	for (int i = 0; i < w * h; i++)
		if (px[i] != ref) {
			(*bad)++;
		}
	free(px);
	return first;
}

static int same_args(const gml_rvalue *a, const gml_rvalue *b) {
	return a[0].val == b[0].val && a[1].val == b[1].val;
}

static int ABI lazy_reset(lazy_pass *p) {
	if (!p->active)
		return real_reset_target();
	if (p->flushed) {
		p->active = 0;
		p->known = 0;
		return real_reset_target();
	}
	int surf = p->surf;
	if (p->have_clear && p->known && p->known_surf == surf && same_args(p->known_args, p->cargs) &&
			surf_get(surf) == p->known_rs && surf_texture(surf) == p->known_tex) {
		p->skips++;
		if (lazy_verify) { // replay the pass as the original does, checking what the surface held first
			p->active = 0;
			if (lazy_fault && p->skips % 60 == 1) {
				// Negative control: paint the surface white behind the tracking's back (no forget)
				gml_rvalue r = {0, 0, 5}, white[2] = {{16777215.0, 0, 0}, {1.0, 0, 0}};
				gml_rvalue *wargv[2] = {&white[0], &white[1]};
				real_set_target(surf, -1);
				real_legacy(p->cself, p->cother, &r, 2, fid_draw_clear_alpha, wargv);
				int b0;
				uint32_t painted = read_pixel_check(1, 1, 0, &b0);
				real_reset_target();
				if (lazy_faults++ < 3)
					debugPrintf("[PSYCHFIX] fault: surface %d painted, pixel now %08x\n", surf, (unsigned)painted);
			}
			real_set_target(surf, p->depth);
			int bad;
			uint32_t first = read_pixel_check(surf_width(surf), surf_height(surf), p->known_pixel, &bad);
			static unsigned shown;
			if (shown++ < 3)
				debugPrintf("[PSYCHFIX] verify: surface %d %dx%d first pixel %08x, clear %08x\n", surf, surf_width(surf),
					surf_height(surf), (unsigned)first, (unsigned)p->known_pixel);
			p->verified++;
			if (bad) {
				if (p->mismatches++ < 5)
					debugPrintf("[PSYCHFIX] %s verify: surface %d had %d pixels other than the clear\n", p->name, surf, bad);
			}
			gml_rvalue result = {0, 0, 5};
			gml_rvalue *argv[2] = {&p->cargs[0], &p->cargs[1]};
			real_legacy(p->cself, p->cother, &result, 2, fid_draw_clear_alpha, argv);
			int r = real_reset_target();
			p->known = 1; // the set_target above made it forget; it holds the clear again
			p->known_surf = surf;
			p->known_rs = surf_get(surf);
			p->known_tex = surf_texture(surf);
			return r;
		}
		p->active = 0;
		return 1;
	}
	// Not known to hold the clear yet: do the pass now (nothing was drawn, so only the switch and the clear)
	lazy_flush(p);
	p->active = 0;
	p->clears++;
	if (p->have_clear && lazy_verify) {
		int bad;
		p->known_pixel = read_pixel_check(1, 1, 0, &bad);
	}
	int r = real_reset_target();
	p->known = p->have_clear;
	p->known_surf = surf;
	p->known_rs = surf_get(surf);
	p->known_tex = surf_texture(surf);
	p->known_args[0] = p->cargs[0];
	p->known_args[1] = p->cargs[1];
	return r;
}

static int ABI battle_set_target(int s, int d) { return lazy_set_target(&battle, s, d); }
static gml_rvalue *ABI battle_clear(void *s, void *o, gml_rvalue *r, int c, int f, gml_rvalue **a) {
	return lazy_clear(&battle, s, o, r, c, f, a);
}
static void ABI battle_event(void *s, void *o, int n) { lazy_event(&battle, s, o, n); }
static int ABI battle_reset(void) { return lazy_reset(&battle); }


// ---------------------------------------------------------------------------------------------------------------
// Interception: while the battle Draw runs, the PLT's GOT slots of the four runner calls point at the wrappers above
// (every call in that event goes through the PLT: the callees are out of bl range, so patch_plt_calls leaves them).
// No veneers (the code cave near the event can be full) and no entry hooks (virtual_capture.c hooks
// surface_set_target_ext / surface_reset_target and checks their prologues).

static uintptr_t *got_slot[4], got_saved[4], got_mine[4];
static event_func orig_battle_draw;
static builtin_fn bi_instance_number;
static unsigned portal_frames;
static int lazy_on;

static void battle_draw(void *self, void *other) {
	if (!lazy_on || ab_off) {
		orig_battle_draw(self, other);
		return;
	}
	for (int i = 0; i < 4; i++) {
		got_saved[i] = *got_slot[i];
		*got_slot[i] = got_mine[i];
	}
	// The wrappers call whatever the slots held (a hook installed after this file's, e.g. a profiler, stays in)
	real_set_target = (set_target_fn)got_saved[0];
	real_reset_target = (reset_target_fn)got_saved[1];
	real_event_user = (event_user_fn)got_saved[2];
	real_legacy = (legacy_fn)got_saved[3];
	if (research && bi_instance_number) { // research: frames with a portal, whose pass can't be skipped
		gml_rvalue r = {0, 0, 5}, arg;
		uint32_t ref[4] = {828, 0x1000000, 0, 15}; // obj_psychboss_portal
		memcpy(&arg, ref, sizeof(arg));
		bi_instance_number(&r, self, other, 1, &arg);
		if ((r.kind & 0xffffff) == 0 && r.val > 0)
			portal_frames++;
	}
	in_battle_draw = 1;
	battle_pass_done = 0;
	orig_battle_draw(self, other);
	in_battle_draw = 0;
	for (int i = 0; i < 4; i++)
		*got_slot[i] = got_saved[i];
	if (battle.active) { // a pass the event didn't close (not expected): replay it, unbalanced as the original
		lazy_flush(&battle);
		battle.active = 0;
		battle.known = 0;
	}
}

static uintptr_t bl_target(uintptr_t site) {
	uint32_t ins = *(uint32_t *)site;
	if ((ins >> 24) != 0xeb)
		return 0;
	return site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
}

// ELF addresses in APK 1.1.10: the battle Draw, its calls and the PLT entries they go through
enum {
	PLT_SET_TARGET = 0x4ab19f0, PLT_RESET_TARGET = 0x4ab1a40, PLT_LEGACY = 0x4ab0ff0, PLT_EVENT_USER = 0x4ab13d0,
	BATTLE_DRAW = 0x3bd05e0,
};

static int lazy_install(gml_func_entry *table) {
	uintptr_t battle_draw_fn = so_symbol(&yoyoloader_mod, "_Z46gml_Object_obj_cutscene_psych_11_battle_Draw_0P9CInstanceS0_");
	if (!battle_draw_fn)
		return 0;
	uintptr_t bias = battle_draw_fn - BATTLE_DRAW;
	static const uint32_t sites[][2] = {
		{0x3bd0898, PLT_SET_TARGET}, {0x3bd0ae0, PLT_LEGACY}, {0x3bd0b64, PLT_EVENT_USER}, {0x3bd0d1c, PLT_EVENT_USER},
		{0x3bd0ed0, PLT_EVENT_USER}, {0x3bd0f1c, PLT_RESET_TARGET},
	};
	for (unsigned i = 0; i < sizeof(sites) / sizeof(sites[0]); i++)
		if (bl_target(bias + sites[i][0]) != bias + sites[i][1]) {
			debugPrintf("[PSYCHFIX] battle Draw call %x doesn't go through the PLT\n", sites[i][0]);
			return 0;
		}
	static const char *const names[4] = {"_Z24YYGML_surface_set_targetii", "_Z26YYGML_surface_reset_targetv",
		"_Z16YYGML_event_userP9CInstanceS0_i", "_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_"};
	for (int i = 0; i < 4; i++)
		if (!(got_slot[i] = so_got_slot(&yoyoloader_mod, names[i])) || !*got_slot[i])
			return 0;
	// The wrappers call what the slots hold now (the runner's functions, or whatever hooked them before)
	real_set_target = (set_target_fn)*got_slot[0];
	real_reset_target = (reset_target_fn)*got_slot[1];
	real_event_user = (event_user_fn)*got_slot[2];
	real_legacy = (legacy_fn)*got_slot[3];
	got_mine[0] = (uintptr_t)battle_set_target;
	got_mine[1] = (uintptr_t)battle_reset;
	got_mine[2] = (uintptr_t)battle_event;
	got_mine[3] = (uintptr_t)battle_clear;
	event_func o = gml_replace(table, "gml_Object_obj_cutscene_psych_11_battle_Draw_0", battle_draw);
	if (!o)
		return 0;
	orig_battle_draw = o;
	return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// Surface pool across restarts

#define POOL_MAX 8
#define POOL_FRAMES 3
static int pool_on;
static builtin_fn orig_surface_create, orig_surface_free, orig_surface_exists, orig_surface_resize, orig_surface_copy,
	orig_surface_copy_part, bi_set_target, bi_reset_target, bi_draw_clear_alpha;
static struct { int id, w, h; uint32_t frame; } pool[POOL_MAX];
static int pool_n;
static unsigned pool_parked, pool_reused, pool_expired, pool_checked, pool_mismatches;
static int psych_room = -2;
static int (*room_find)(const char *name);

static int surf_arg(const gml_rvalue *v) {
	uint32_t k = v->kind & 0xffffff;
	if (k == 0)
		return (int)v->val;
	if (k == 15 || k == 7) {
		int id;
		memcpy(&id, &v->val, 4);
		return id;
	}
	return -1;
}

// A surface reference as surface_create returns it: id in the low word, 0x08000004 above it, kind 15
static gml_rvalue surf_ref(int id) {
	gml_rvalue v;
	uint32_t words[2] = {(uint32_t)id, 0x08000004};
	memcpy(&v.val, words, sizeof(words));
	v.flags = 0;
	v.kind = 15;
	return v;
}

static int pooled(int id) {
	for (int i = 0; i < pool_n; i++)
		if (pool[i].id == id)
			return i;
	return -1;
}

static void pool_drop(int i) {
	gml_rvalue result = {0, 0, 5}, arg = surf_ref(pool[i].id);
	pool[i] = pool[--pool_n];
	orig_surface_free(&result, NULL, NULL, 1, &arg);
}

static int in_psych_room(void) {
	if (psych_room == -2)
		psych_room = room_find ? room_find("room_psychiatrist") : -1;
	return psych_room >= 0 && current_room && *current_room == psych_room;
}

static void surface_free_pool(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	int id = argc >= 1 ? surf_arg(&args[0]) : -1;
	forget_surface(id);
	if (pool_on && !ab_off && id >= 0 && pooled(id) < 0 && in_psych_room() && surf_get(id)) {
		int w = surf_width(id), h = surf_height(id);
		if (w > 0 && h > 0) {
			if (pool_n == POOL_MAX)
				pool_drop(0);
			pool[pool_n].id = id;
			pool[pool_n].w = w;
			pool[pool_n].h = h;
			pool[pool_n].frame = frame_count ? *frame_count : 0;
			pool_n++;
			pool_parked++;
			return; // F_SurfaceFree leaves the result as it is
		}
	}
	orig_surface_free(result, self, other, argc, args);
}

// Verify: every pixel of a surface the game just got is 0
static void check_new_surface(int id, void *self, void *other, int reused) {
	gml_rvalue r = {0, 0, 5}, sa = surf_ref(id);
	bi_set_target(&r, self, other, 1, &sa);
	int bad;
	read_pixel_check(surf_width(id), surf_height(id), 0, &bad);
	bi_reset_target(&r, self, other, 0, NULL);
	forget_surface(id);
	pool_checked++;
	if (bad && pool_mismatches++ < 5)
		debugPrintf("[PSYCHFIX] verify: %s surface %d has %d pixels that aren't 0\n", reused ? "reused" : "new", id, bad);
}

static void surface_create_pool(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	uint32_t t0 = sceKernelGetProcessTimeLow();
	int timed = in_psych_room();
	if (pool_n && !ab_off && argc == 2 && (args[0].kind & 0xffffff) == 0 && (args[1].kind & 0xffffff) == 0) {
		int w = (int)args[0].val, h = (int)args[1].val;
		for (int i = pool_n - 1; i >= 0; i--) {
			if (pool[i].w != w || pool[i].h != h)
				continue;
			int id = pool[i].id;
			pool[i] = pool[--pool_n];
			if (!surf_get(id))
				continue; // gone meanwhile (not expected): create a new one
			// A new surface holds zeroes (vitaGL clears the texture memory): clear the reused one to (0, 0, 0, 0)
			gml_rvalue r = {0, 0, 5}, sa = surf_ref(id), ca[2] = {{0, 0, 0}, {0, 0, 0}};
			bi_set_target(&r, self, other, 1, &sa);
			bi_draw_clear_alpha(&r, self, other, 2, ca);
			bi_reset_target(&r, self, other, 0, NULL);
			forget_surface(id);
			pool_reused++;
			*result = surf_ref(id); // as F_SurfaceCreate writes it
			if (lazy_verify)
				check_new_surface(id, self, other, 1);
			if (research || ab)
				debugPrintf("[PSYCHFIX] surface_create %dx%d: reused %d, %.2f ms\n", w, h, id,
					(sceKernelGetProcessTimeLow() - t0) / 1000.0f);
			return;
		}
	}
	orig_surface_create(result, self, other, argc, args);
	forget_surface(surf_arg(result));
	if (timed && lazy_verify)
		check_new_surface(surf_arg(result), self, other, 0);
	if (timed && (research || ab))
		debugPrintf("[PSYCHFIX] surface_create %.0fx%.0f: new %d, %.2f ms\n", argc > 0 ? args[0].val : 0.0,
			argc > 1 ? args[1].val : 0.0, surf_arg(result), (sceKernelGetProcessTimeLow() - t0) / 1000.0f);
}

static void surface_exists_pool(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (pool_n && argc >= 1 && pooled(surf_arg(&args[0])) >= 0) {
		result->val = 0.0; // F_SurfaceExists returns a real
		result->kind = 0;
		return;
	}
	orig_surface_exists(result, self, other, argc, args);
}

static void surface_resize_forget(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (argc >= 1)
		forget_surface(surf_arg(&args[0]));
	orig_surface_resize(result, self, other, argc, args);
}

static void surface_copy_forget(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (argc >= 1)
		forget_surface(surf_arg(&args[0]));
	orig_surface_copy(result, self, other, argc, args);
}

static void surface_copy_part_forget(gml_rvalue *result, void *self, void *other, int argc, gml_rvalue *args) {
	if (argc >= 1)
		forget_surface(surf_arg(&args[0]));
	orig_surface_copy_part(result, self, other, argc, args);
}

// Once per frame after the swap: free parked surfaces nobody took
void psych_fix_frame(void) {
	uint32_t now = frame_count ? *frame_count : 0;
	for (int i = pool_n - 1; i >= 0; i--)
		if (now - pool[i].frame >= POOL_FRAMES || !in_psych_room()) {
			pool_drop(i);
			pool_expired++;
		}
}

// ---------------------------------------------------------------------------------------------------------------
// Research (exp_psychres)

static event_func orig_effects_draw;
static int *var_type, *var_update_timer;
static unsigned fx_types[12], fx_draws, fx_refresh;
// exp_psychskip=<mask> with exp_psychskipab (measurements of VISIBLE cuts, research only): every other report window
// 1 skips the effects object's Draw, 2 skips half the rose petals (Step and Draw), [PSYCHSKIP] A/B lines
int psych_skip_mask, psych_skip_on;
static int skip_ab;
static void effects_draw_res(void *self, void *other) {
	if (psych_skip_on && (psych_skip_mask & 1))
		return;
	gml_rvalue *t = var_type ? gml_vget(self, var_type[1]) : NULL;
	gml_rvalue *u = var_update_timer ? gml_vget(self, var_update_timer[1]) : NULL;
	fx_draws++;
	if (t && t->kind == 0) {
		int k = (int)t->val + 1;
		fx_types[k >= 0 && k < 12 ? k : 11]++;
	}
	if (u && u->kind == 0 && u->val <= 1.0)
		fx_refresh++;
	orig_effects_draw(self, other);
}

// Eyes Draw: how often a pass has no obj_psychboss_eye to draw (instance_number through the builtin)
static event_func orig_eyes_draw;
static unsigned eyes_draws, eyes_empty;
static void eyes_draw_res(void *self, void *other) {
	eyes_draws++;
	if (bi_instance_number) {
		gml_rvalue r = {0, 0, 5}, arg;
		uint32_t ref[4] = {821, 0x1000000, 0, 15}; // obj_psychboss_eye
		memcpy(&arg, ref, sizeof(arg));
		bi_instance_number(&r, self, other, 1, &arg);
		if ((r.kind & 0xffffff) == 0 && r.val == 0.0)
			eyes_empty++;
	}
	orig_eyes_draw(self, other);
}

void psych_fix_report(int frames) {
	if (!debugMode || frames <= 0)
		return;
	extern void psych_ground_report(int frames, int ab_off);
	psych_ground_report(frames, ab && !ab_off); // the next window's state (toggled below)
	extern void psych_petal_report(int frames, int ab_off);
	psych_petal_report(frames, ab && !ab_off);
	if (skip_ab) {
		debugPrintf("[PSYCHSKIP] mask %d A/B: %s\n", psych_skip_mask, psych_skip_on ? "on" : "off");
		psych_skip_on = !psych_skip_on;
	}
	if (ab) { // the state of the window just reported, then the other one
		debugPrintf("[PSYCHAB] A/B: %s\n", ab_off ? "off" : "on");
		ab_off = !ab_off;
	}
	for (unsigned i = 0; i < sizeof(passes) / sizeof(passes[0]); i++) {
		lazy_pass *p = passes[i];
		if (!p->passes)
			continue;
		debugPrintf("[PSYCHFIX] %s pass: %u passes, %u skipped, %u cleared, %u drawn (%u user events, %u frames with a "
			"portal)%s", p->name, p->passes, p->skips, p->clears, p->flushes, p->events, portal_frames, lazy_verify ? "" : "\n");
		portal_frames = 0;
		if (lazy_verify)
			debugPrintf(" | verify %u checked, %u mismatches total (%u faults injected)\n", p->verified, p->mismatches,
				lazy_faults);
		p->passes = p->skips = p->clears = p->flushes = p->events = p->verified = 0;
	}
	if (pool_parked || pool_reused || pool_expired) {
		debugPrintf("[PSYCHFIX] surface pool: %u parked, %u reused, %u freed after %d frames | verify: %u new surfaces "
			"checked, %u not 0 (total)\n", pool_parked, pool_reused, pool_expired, POOL_FRAMES, pool_checked, pool_mismatches);
		pool_parked = pool_reused = pool_expired = 0;
	}
	if (research && fx_draws) {
		debugPrintf("[PSYCHFIX] effects Draw %u, refresh %u, types -1:%u 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u 8:%u other %u\n",
			fx_draws, fx_refresh, fx_types[0], fx_types[1], fx_types[2], fx_types[3], fx_types[4], fx_types[5], fx_types[6],
			fx_types[7], fx_types[8], fx_types[9], fx_types[10] + fx_types[11]);
		fx_draws = fx_refresh = 0;
		memset(fx_types, 0, sizeof(fx_types));
	}
	if (research && eyes_draws) {
		debugPrintf("[PSYCHFIX] eyes Draw %u, with no eye %u\n", eyes_draws, eyes_empty);
		eyes_draws = eyes_empty = 0;
	}
}

// ---------------------------------------------------------------------------------------------------------------

// patch_performance (before the runner starts): event table hooks
void psych_fix_install(gml_func_entry *table) {
	extern int psych_ground_install(gml_func_entry *table);
	int ground = !exp_flag("psychground0") && psych_ground_install(table);
	extern int psych_petal_install(gml_func_entry *table);
	int petal = !exp_flag("psychpetal0") && psych_petal_install(table);
	debugPrintf("psychiatrist fight: native ground Draw %s, native rose petal Draw %s\n", ground ? "installed" : "NOT installed",
		petal ? "installed" : "NOT installed");
	lazy_ok = !exp_flag("psychlazy0") && lazy_install(table);
	research = debugMode && exp_flag("psychres");
	skip_ab = debugMode && exp_flag("psychskipab");
	if (skip_ab || (debugMode && exp_flag("psychskip"))) { // without psychskipab: always skipped (screenshots)
		psych_skip_on = !skip_ab;
		FILE *f = fopen("ux0:data/gms/shared/exp_psychskip", "r");
		char t[8] = {0};
		if (f) {
			fread(t, 1, sizeof(t) - 1, f);
			fclose(f);
		}
		psych_skip_mask = atoi(t);
		research = 1;
	}
	if (research) {
		var_type = (int *)so_symbol(&yoyoloader_mod, "g_VAR_type");
		var_update_timer = (int *)so_symbol(&yoyoloader_mod, "g_VAR_update_timer");
		event_func o = gml_replace(table, "gml_Object_obj_psychboss_effects_Draw_0", effects_draw_res);
		if (o)
			orig_effects_draw = o;
		o = gml_replace(table, "gml_Object_obj_psychboss_draw_eyes_Draw_0", eyes_draw_res);
		if (o)
			orig_eyes_draw = o;
	}
}

// katana_late_install (the runner's function registry is filled by then)
void psych_fix_late_install(void) {
	extern void psych_ground_late_install(void);
	psych_ground_late_install();
	if (!lazy_ok) { // else lazy_install took them from the GOT slots
		real_set_target = (set_target_fn)so_symbol(&yoyoloader_mod, "_Z24YYGML_surface_set_targetii");
		real_reset_target = (reset_target_fn)so_symbol(&yoyoloader_mod, "_Z26YYGML_surface_reset_targetv");
		real_event_user = (event_user_fn)so_symbol(&yoyoloader_mod, "_Z16YYGML_event_userP9CInstanceS0_i");
		real_legacy = (legacy_fn)so_symbol(&yoyoloader_mod, "_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
	}
	surf_width = (surf_int_fn)so_symbol(&yoyoloader_mod, "_Z20GR_Surface_Get_Widthi");
	surf_height = (surf_int_fn)so_symbol(&yoyoloader_mod, "_Z21GR_Surface_Get_Heighti");
	surf_texture = (surf_int_fn)so_symbol(&yoyoloader_mod, "_Z22GR_Surface_Get_Texturei");
	surf_get = (surf_get_fn)so_symbol(&yoyoloader_mod, "_Z14GR_Surface_Geti");
	room_find = (int (*)(const char *))so_symbol(&yoyoloader_mod, "_Z9Room_FindPKc");
	const int *fclear = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_draw_clear_alpha");
	const int *fexists = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_instance_exists");
	if (fclear)
		fid_draw_clear_alpha = fclear[1];
	if (fexists)
		fid_instance_exists = fexists[1];
	lazy_verify = debugMode && exp_flag("psychlazyverify");
	lazy_fault = lazy_verify && exp_flag("psychlazyfault");
	ab = debugMode && exp_flag("psychab");

	runner_function **functions = (runner_function **)so_symbol(&yoyoloader_mod, "the_functions");
	int *count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	runner_function *f_create = NULL, *f_free = NULL, *f_exists = NULL, *f_resize = NULL, *f_copy = NULL, *f_copy_part = NULL;
	for (int i = 0; functions && *functions && count && i < *count; i++) {
		runner_function *f = &(*functions)[i];
		if (!f->name)
			continue;
		if (!strcmp(f->name, "surface_create")) f_create = f;
		else if (!strcmp(f->name, "surface_free")) f_free = f;
		else if (!strcmp(f->name, "surface_exists")) f_exists = f;
		else if (!strcmp(f->name, "surface_resize")) f_resize = f;
		else if (!strcmp(f->name, "surface_copy")) f_copy = f;
		else if (!strcmp(f->name, "surface_copy_part")) f_copy_part = f;
		else if (!strcmp(f->name, "surface_set_target")) bi_set_target = f->func;
		else if (!strcmp(f->name, "surface_reset_target")) bi_reset_target = f->func;
		else if (!strcmp(f->name, "draw_clear_alpha")) bi_draw_clear_alpha = f->func;
		else if (!strcmp(f->name, "instance_number")) bi_instance_number = f->func;
	}
	int base_ok = real_set_target && real_reset_target && real_event_user && real_legacy && surf_width && surf_height &&
		surf_texture && surf_get && fid_draw_clear_alpha >= 0 && f_create && f_free && f_exists && f_resize && f_copy &&
		f_copy_part && bi_set_target && bi_reset_target && bi_draw_clear_alpha;
	if (!base_ok) {
		debugPrintf("psychiatrist fight fixes: NOT installed (symbols)\n");
		return;
	}
	// Builtins that free, create, resize or copy into a surface make the pass forget what it holds. Nothing else can
	// draw into the attack surface: only the battle object's Create, Draw and CleanUp read global.attack_surface (GOT
	// xrefs in APK 1.1.10), and its Draw's target switches go through the wrappers. No entry hook on the runner's
	// surface_set_target_ext: virtual_capture.c hooks it and checks its prologue.
	orig_surface_free = f_free->func; f_free->func = surface_free_pool;
	orig_surface_create = f_create->func; f_create->func = surface_create_pool;
	orig_surface_exists = f_exists->func; f_exists->func = surface_exists_pool;
	orig_surface_resize = f_resize->func; f_resize->func = surface_resize_forget;
	orig_surface_copy = f_copy->func; f_copy->func = surface_copy_forget;
	orig_surface_copy_part = f_copy_part->func; f_copy_part->func = surface_copy_part_forget;

	lazy_on = lazy_ok && fid_instance_exists >= 0;
	pool_on = !exp_flag("psychpool0") && room_find;
	debugPrintf("psychiatrist fight: empty attack surface passes %s, restart surface reuse %s%s\n",
		lazy_on ? "skipped" : "NOT skipped", pool_on ? "on" : "off", lazy_verify ? " (verify)" : "");
}
