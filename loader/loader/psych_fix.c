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
 * to the colour it already holds. The calls are redirected (bl veneers at the call sites) so that the target switch
 * and the clear are deferred: the first user event inside the pass replays them first, and a pass that drew nothing
 * skips them when the surface is known to hold exactly that clear already (its last pass was such a clear, and no
 * set_target, copy, free, create or resize touched it since). The rest of the event runs unchanged, including the
 * draw of the surface (with its random numbers). Part 3: 12 -> 10 scenes per frame, 49.2 -> 51.9 fps.
 * exp_psychlazy0 turns this off; exp_psychlazyverify (debug) replays every skipped pass after reading the surface back
 * and checking every pixel against the clear.
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
 * (1: no obj_psychboss_effects Draw, 2: half the rose petals, Step and Draw), to size them; exp_psychres logs the
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
typedef int ABI (*set_target_ext_fn)(int index, int surf, int depth);
typedef int ABI (*reset_target_fn)(void);
typedef void ABI (*event_user_fn)(void *self, void *other, int n);
typedef gml_rvalue *ABI (*legacy_fn)(void *self, void *other, gml_rvalue *result, int argc, int funcid, gml_rvalue **argv);
typedef int ABI (*surf_int_fn)(int surf);
typedef void *ABI (*surf_get_fn)(int surf);

static set_target_fn real_set_target;
static reset_target_fn real_reset_target;
static event_user_fn real_event_user;
static legacy_fn real_legacy;
static set_target_ext_fn orig_set_target_ext;
static surf_int_fn surf_width, surf_height, surf_texture;
static surf_get_fn surf_get;
static int fid_draw_clear_alpha = -1;

static int lazy_verify, research;
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

// Every set_target, from anywhere: the surface may be drawn on now
static int ABI set_target_ext_hook(int index, int surf, int depth) {
	forget_surface(surf);
	return orig_set_target_ext(index, surf, depth);
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

static int ABI lazy_set_target(lazy_pass *p, int surf, int depth) {
	if (ab_off)
		return real_set_target(surf, depth);
	if (p->active) { // not expected: run it as it comes
		lazy_flush(p);
		return real_set_target(surf, depth);
	}
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
	if (!p->active || p->flushed || p->have_clear || argc != 2 || funcid != fid_draw_clear_alpha ||
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
	p->events++;
	lazy_flush(p);
	real_event_user(self, other, n);
}

static uint32_t read_pixel_check(int w, int h, uint32_t ref, int *bad) {
	uint32_t *px = malloc((size_t)w * h * 4);
	uint32_t first = 0;
	*bad = 0;
	if (!px)
		return 0;
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
			real_set_target(surf, p->depth);
			int bad;
			read_pixel_check(surf_width(surf), surf_height(surf), p->known_pixel, &bad);
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
// Call-site redirection

static uintptr_t bl_target(uintptr_t site) {
	uint32_t ins = *(uint32_t *)site;
	if ((ins >> 24) != 0xeb)
		return 0;
	return site + 8 + (uintptr_t)((int32_t)(ins << 8) >> 6);
}

typedef struct { uint32_t site, plt; void *wrapper; } site_patch;

// The function a PLT entry stands for: the runner build may call it directly (PLT-patched lib)
static uintptr_t plt_direct(uint32_t plt) {
	switch (plt) {
	case 0x4ab19f0: return (uintptr_t)real_set_target;
	case 0x4ab1a40: return (uintptr_t)real_reset_target;
	case 0x4ab0ff0: return (uintptr_t)real_legacy;
	case 0x4ab13d0: return (uintptr_t)real_event_user;
	}
	return 0;
}

#define MAX_SITES 16
// Checks every site and allocates every veneer first; patches only if all of them are fine
static int patch_sites(uintptr_t bias, const site_patch *s, int n) {
	uint32_t *veneer[MAX_SITES];
	uint32_t bl[MAX_SITES];
	if (n > MAX_SITES)
		return 0;
	for (int i = 0; i < n; i++) {
		uintptr_t site = bias + s[i].site, t = bl_target(site);
		if (!t || (t != bias + s[i].plt && t != plt_direct(s[i].plt))) {
			debugPrintf("[PSYCHFIX] call site %x: unexpected target %x\n", s[i].site, (unsigned)(t - bias));
			return 0;
		}
		veneer[i] = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
		int64_t delta = (int64_t)(uintptr_t)veneer[i] - (int64_t)site - 8;
		if (!veneer[i] || delta < -(1 << 25) || delta >= (1 << 25)) {
			debugPrintf("[PSYCHFIX] call site %x: no veneer space in range (%x)\n", s[i].site, (unsigned)(uintptr_t)veneer[i]);
			return 0;
		}
		bl[i] = 0xeb000000 | (((uint32_t)delta >> 2) & 0xffffff);
	}
	for (int i = 0; i < n; i++) {
		uint32_t code[2] = {0xe51ff004, (uint32_t)s[i].wrapper}; // ldr pc, [pc, #-4]; .word wrapper
		kuKernelCpuUnrestrictedMemcpy(veneer[i], code, sizeof(code));
		kuKernelFlushCaches(veneer[i], sizeof(code));
		kuKernelCpuUnrestrictedMemcpy((void *)(bias + s[i].site), &bl[i], 4);
		kuKernelFlushCaches((void *)(bias + s[i].site), 4);
	}
	return 1;
}

// ELF addresses in APK 1.1.10
enum {
	PLT_SET_TARGET = 0x4ab19f0, PLT_RESET_TARGET = 0x4ab1a40, PLT_LEGACY = 0x4ab0ff0, PLT_EVENT_USER = 0x4ab13d0,
	BATTLE_DRAW = 0x3bd05e0,
};

static int lazy_install(void) {
	uintptr_t battle_draw = so_symbol(&yoyoloader_mod, "_Z46gml_Object_obj_cutscene_psych_11_battle_Draw_0P9CInstanceS0_");
	if (!battle_draw) {
		debugPrintf("[PSYCHFIX] battle Draw not found\n");
		return 0;
	}
	uintptr_t bias = battle_draw - BATTLE_DRAW;
	static const site_patch battle_sites[] = {
		{0x3bd0898, PLT_SET_TARGET, battle_set_target}, // battle line 6
		{0x3bd0ae0, PLT_LEGACY, battle_clear},          // line 7: draw_clear_alpha
		{0x3bd0b64, PLT_EVENT_USER, battle_event},      // line 32: portal
		{0x3bd0d1c, PLT_EVENT_USER, battle_event},      // line 39: knife
		{0x3bd0ed0, PLT_EVENT_USER, battle_event},      // line 47: turret
		{0x3bd0f1c, PLT_RESET_TARGET, battle_reset},    // line 52
	};
	return patch_sites(bias, battle_sites, sizeof(battle_sites) / sizeof(battle_sites[0]));
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
static builtin_fn bi_instance_number;
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
		debugPrintf("[PSYCHFIX] %s pass: %u passes, %u skipped, %u cleared, %u drawn (%u user events)%s", p->name, p->passes,
			p->skips, p->clears, p->flushes, p->events, lazy_verify ? "" : "\n");
		if (lazy_verify)
			debugPrintf(" | verify %u checked, %u mismatches total\n", p->verified, p->mismatches);
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
	research = debugMode && exp_flag("psychres");
	skip_ab = debugMode && exp_flag("psychskipab");
	if (skip_ab) {
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
	real_set_target = (set_target_fn)so_symbol(&yoyoloader_mod, "_Z24YYGML_surface_set_targetii");
	real_reset_target = (reset_target_fn)so_symbol(&yoyoloader_mod, "_Z26YYGML_surface_reset_targetv");
	real_event_user = (event_user_fn)so_symbol(&yoyoloader_mod, "_Z16YYGML_event_userP9CInstanceS0_i");
	real_legacy = (legacy_fn)so_symbol(&yoyoloader_mod, "_Z24YYGML_CallLegacyFunctionP9CInstanceS0_R8YYRValueiiPPS1_");
	surf_width = (surf_int_fn)so_symbol(&yoyoloader_mod, "_Z20GR_Surface_Get_Widthi");
	surf_height = (surf_int_fn)so_symbol(&yoyoloader_mod, "_Z21GR_Surface_Get_Heighti");
	surf_texture = (surf_int_fn)so_symbol(&yoyoloader_mod, "_Z22GR_Surface_Get_Texturei");
	surf_get = (surf_get_fn)so_symbol(&yoyoloader_mod, "_Z14GR_Surface_Geti");
	room_find = (int (*)(const char *))so_symbol(&yoyoloader_mod, "_Z9Room_FindPKc");
	const int *fclear = (const int *)so_symbol(&yoyoloader_mod, "g_FUNC_draw_clear_alpha");
	uintptr_t set_ext = so_symbol(&yoyoloader_mod, "_Z28YYGML_surface_set_target_extiii");
	if (fclear)
		fid_draw_clear_alpha = fclear[1];
	lazy_verify = debugMode && exp_flag("psychlazyverify");
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
		f_copy_part && bi_set_target && bi_reset_target && bi_draw_clear_alpha && set_ext;
	if (!base_ok) {
		debugPrintf("psychiatrist fight fixes: NOT installed (symbols)\n");
		return;
	}
	// Anything that may change a surface makes the passes forget what it holds
	orig_set_target_ext = (set_target_ext_fn)entry_hook(set_ext, (void *)set_target_ext_hook);
	if (!orig_set_target_ext) {
		debugPrintf("psychiatrist fight fixes: NOT installed (set_target hook)\n");
		return;
	}
	orig_surface_free = f_free->func; f_free->func = surface_free_pool;
	orig_surface_create = f_create->func; f_create->func = surface_create_pool;
	orig_surface_exists = f_exists->func; f_exists->func = surface_exists_pool;
	orig_surface_resize = f_resize->func; f_resize->func = surface_resize_forget;
	orig_surface_copy = f_copy->func; f_copy->func = surface_copy_forget;
	orig_surface_copy_part = f_copy_part->func; f_copy_part->func = surface_copy_part_forget;

	int lazy = exp_flag("psychlazy0") ? 0 : lazy_install();
	pool_on = !exp_flag("psychpool0") && room_find;
	debugPrintf("psychiatrist fight: empty attack surface passes %s, restart surface reuse %s%s\n",
		lazy ? "skipped" : "NOT skipped", pool_on ? "on" : "off", lazy_verify ? " (verify)" : "");
}
