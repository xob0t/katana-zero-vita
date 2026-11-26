/*
 * rain_cheap.c
 *
 * Cheap rain (rain=1, the default): the game's rain drops are drawn by the loader instead of by obj_rain instances.
 *
 * The game's rain is one obj_rain instance per drop (room_factory_0's obj_cutscene_factory_0 spawns ~3 per tick at the
 * top of the view, 30 px/tick down; the apartment, alleyway and old motel cutscenes spawn them too). Each drop's Step
 * moves it, tests block_meeting + place_meeting(obj_outdoors_shadow) + place_meeting(obj_jumpthrough) and spawns an
 * obj_rainsplash (lit through the recolor lighting) where it lands; its Draw draws a fading trail of whitepixel
 * segments, loops over every obj_lightsource for a lit copy, and calls irandom on the game's RNG. Full rain cost
 * ~5 ms per frame in factory_0, so the port has shipped with rain off (rain=0: each drop is destroyed right after its
 * Create).
 *
 * Here the drops are destroyed right after their Create as with rain=0, so the game's state, instance ids and RNG
 * stream are those of rain=0 (the spawners' random() and the drop Create's choose() calls run as before; the drops'
 * Step/Draw/splash RNG use never happens). One exception: the first drop of a rain shower stays alive as the carrier.
 * Its Step and user event 15 never run, and its Draw draws every loader drop, so the rain is drawn at the depth the
 * drops had, and a `with (obj_rain) instance_destroy()` (the alleyway cutscene stops the rain that way) or the room
 * end removes the carrier and with it the rain. The carrier keeps the spawn position and speed the spawner gave it,
 * which the loader drops use (all drops of a spawner share them).
 *
 * Loader drops: position, speed x global.fr per frame, a landing height found at spawn from a per-room list of the
 * bounding boxes of obj_block (or obj_block_background, per the drop's draw_layer), obj_outdoors_shadow and, for half
 * of the drops as in the game, obj_jumpthrough (with children; refreshed every 30 frames). A drop is one black
 * whitepixel streak (no fading trail, no light tint); where it lands it plays one of the game's three splash sprites
 * in a fixed light grey instead of lit, and not while time is frozen (see rain_cheap_draw). Drops leave 30 px
 * outside the view, as in the game. Nothing here touches the game's RNG or variables (one xorshift of its own).
 *
 * rain=2 keeps the game's rain (each drop drawn every other frame, katana.c), rain=0 none.
 * Testing: exp_rainkeepN keeps one drop in N (default 1), exp_rainnosplash, exp_rainprof times the native work
 * ([RAIN] line with the profiler), exp_rainab draws nothing in every other report window (in-run A/B against no rain).
 */

#include <vitasdk.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "so_util.h"

#include "katana_internal.h"

#define OBJ_RAIN 839
#define SPR_WHITEPIXEL 2184
#define SPR_SPLASH 1665 // spr_rain_splash, spr_rain_splash2 (1666), spr_rain_splash3 (1667)
#define MAX_DROPS 192
#define MAX_SPLASHES 96
#define MAX_RECTS 512
#define DROP_COLOUR 0x000000
#define SPLASH_COLOUR 0x8a8682 // BGR: about the lit splashes' grey in factory_0
#define VIEW_MARGIN 30.0f      // inside_screen(30)
#define MASK_LENGTH 50.0f      // spr_rainmask: 50 px behind the drop's head

typedef void (*destroy_fn)(void *self, void *other, int argc, void **args);
typedef void (*draw_ext_fn)(void *self, int sprite, float subimg, float x, float y, float xs, float ys, float rot,
	int colour, float alpha);

typedef struct {
	float x, y, land, len, alpha;
	uint8_t jumpthrough; // stops at obj_jumpthrough (jumpthrough_collide)
	uint8_t splash;      // lands on a surface (splash), or starts inside one (none)
} drop_t;

typedef struct {
	float x, y, age, xs;
	int sprite, frames;
} splash_t;

typedef struct {
	float l, t, r, b;
	uint8_t jumpthrough;
} rect_t;

static event_func orig_create, orig_step, orig_draw, orig_user15;
static destroy_fn instance_destroy;
static draw_ext_fn draw_sprite_ext;
static uint32_t *id_hash;
static void **object_hash;
static int *var_fr, *var_xview, *var_yview, *var_wview, *var_hview, *var_hsp, *var_vsp, *var_draw_layer;

static void *carrier;
static uint32_t carrier_id;
static int keep_n = 1, no_splash, prof, ab, ab_off;

static drop_t drops[MAX_DROPS];
static int ndrops;
static splash_t splashes[MAX_SPLASHES];
static int nsplashes;
static rect_t rects[MAX_RECTS];
static int nrects, rects_layer = -1, rects_room = -1;
static uint32_t rects_frame, last_frame, created;
static uint32_t rng = 0x9e3779b9;

unsigned rain_cheap_spawned, rain_cheap_kept, rain_cheap_draws, rain_cheap_frames, rain_cheap_dirty, rain_cheap_us;
static unsigned rain_cheap_carriers;

static uint32_t word(const void *p, size_t off) {
	uint32_t v;
	memcpy(&v, (const char *)p + off, 4);
	return v;
}

static float field(const void *p, size_t off) {
	float v;
	memcpy(&v, (const char *)p + off, 4);
	return v;
}

static float frand(void) {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return (rng >> 8) * (1.0f / 16777216.0f);
}

static void *hash_find(const void *h, uint32_t id) {
	if (!h || !word(h, 0))
		return NULL;
	uintptr_t node = word((void *)(uintptr_t)word(h, 0), (id & word(h, 4)) * 8);
	for (unsigned trips = 0; node && trips < 4096; trips++) {
		if (word((void *)node, 8) == id)
			return (void *)(uintptr_t)word((void *)node, 12);
		node = word((void *)node, 4);
	}
	return NULL;
}

// The carrier is alive while the runner still knows its id and it isn't marked for destruction
static int carrier_alive(void) {
	return carrier && hash_find(id_hash, carrier_id) == carrier && !(word(carrier, 0x7c) & 1);
}

static double global_num(int *var, double fallback) {
	gml_rvalue *v = var && global_obj_ptr && *global_obj_ptr ? gml_vget(*global_obj_ptr, var[1]) : NULL;
	return v && (v->kind == 0 || v->kind == 13) ? v->val : fallback;
}

static double self_num(void *self, int *var, double fallback) {
	gml_rvalue *v = var ? gml_vget(self, var[1]) : NULL;
	return v && (v->kind == 0 || v->kind == 13) ? v->val : fallback;
}

// Collision boxes that stop rain. Object indices are this game version's (OBJT parents):
// obj_block and its children, obj_block_background, obj_outdoors_shadow(2), obj_jumpthrough + obj_movingblock.
static const int block_family[] = {56, 57, 67, 79, 113, 114, 134, 151, 338, 342, 343, 384, 427, 462, 511, 630, 665,
	915, 916, 917, 918, 919, 920, 968, 969, 993, 1051};
static const int shadow_family[] = {711, 712};
static const int jumpthrough_family[] = {586, 666};

static void add_object_rects(int index, int layer, uint8_t jumpthrough) {
	void *obj = hash_find(*object_hash, (uint32_t)index);
	if (!obj || word(obj, 0x64) != (uint32_t)index)
		return;
	uintptr_t node = word(obj, 0x44);
	for (unsigned trips = 0; node && trips < 4096 && nrects < MAX_RECTS; trips++, node = word((void *)node, 0)) {
		void *p = (void *)(uintptr_t)word((void *)node, 8);
		if (!p)
			break;
		uint32_t flags = word(p, 0x7c);
		if (flags & 3) // marked or deactivated
			continue;
		if (flags & 8) { // bounding box not computed yet: skip it this time
			rain_cheap_dirty++;
			continue;
		}
		if (layer >= 0 && self_num(p, var_draw_layer, 0) != layer)
			continue;
		rect_t *r = &rects[nrects++];
		r->l = field(p, 0xec);
		r->t = field(p, 0xf0);
		r->r = field(p, 0xf4) + 1.0f;
		r->b = field(p, 0xf8) + 1.0f;
		r->jumpthrough = jumpthrough;
	}
}

static void build_rects(int layer) {
	nrects = 0;
	if (layer == 1)
		add_object_rects(54, 1, 0);
	else
		for (unsigned i = 0; i < sizeof(block_family) / sizeof(*block_family); i++)
			add_object_rects(block_family[i], layer, 0);
	for (unsigned i = 0; i < sizeof(shadow_family) / sizeof(*shadow_family); i++)
		add_object_rects(shadow_family[i], -1, 0);
	for (unsigned i = 0; i < sizeof(jumpthrough_family) / sizeof(*jumpthrough_family); i++)
		add_object_rects(jumpthrough_family[i], -1, 1);
	rects_layer = layer;
	rects_room = current_room ? *current_room : -1;
	rects_frame = *frame_count;
}

// Where a falling drop stops: the first box below its head (or the start, if its 50-px mask already overlaps one).
// A drop moving sideways is tested where it crosses each box's top.
static void drop_landing(drop_t *d, float hsp, float vsp) {
	d->land = INFINITY;
	d->splash = 0;
	if (vsp <= 0)
		return;
	for (int i = 0; i < nrects; i++) {
		const rect_t *r = &rects[i];
		if (r->jumpthrough && !d->jumpthrough)
			continue;
		if (r->b < d->y - MASK_LENGTH)
			continue;
		if (r->t <= d->y) { // overlaps at the start: x must be inside now
			if (d->x >= r->l && d->x < r->r) {
				d->land = d->y;
				d->splash = 0;
				return;
			}
			continue;
		}
		float x = d->x + hsp * (r->t - d->y) / vsp;
		if (x >= r->l && x < r->r && r->t < d->land) {
			d->land = r->t;
			d->splash = 1;
		}
	}
}

static void add_splash(float x, float y) {
	if (no_splash || nsplashes >= MAX_SPLASHES)
		return;
	splash_t *s = &splashes[nsplashes++];
	int k = (int)(frand() * 3.0f);
	if (k > 2)
		k = 2;
	s->sprite = SPR_SPLASH + k;
	s->frames = k == 2 ? 5 : 4;
	s->x = x;
	s->y = y;
	s->age = 0;
	s->xs = frand() < 0.5f ? -1.0f : 1.0f;
}

// A loader drop for a game drop at x, y
static void drop_add(float x, float y) {
	rain_cheap_spawned++;
	if (keep_n <= 1 || created++ % (unsigned)keep_n == 0) {
		if (ndrops < MAX_DROPS) {
			drop_t *d = &drops[ndrops++];
			d->x = x;
			d->y = y;
			d->land = NAN; // found on the first update, once the spawner has set the speed
			d->len = 1.0f + frand() * 0.6f;
			d->alpha = 0.25f + frand() * 0.2f;
			d->jumpthrough = frand() < 0.5f; // jumpthrough_collide = choose(true, false)
			d->splash = 0;
			rain_cheap_kept++;
		}
	}
}

static void rain_cheap_create(void *self, void *other) {
	orig_create(self, other);
	int alive = carrier_alive();
	if (!alive) {
		// A new shower (or a new room): this drop carries the rain from now on
		carrier = self;
		carrier_id = word(self, 0x80);
		ndrops = nsplashes = 0;
		rects_room = -1;
		rain_cheap_carriers++;
	}
	drop_add(field(self, 0xb8), field(self, 0xbc));
	if (alive)
		instance_destroy(self, other, 0, NULL);
}

// For rain_skip.c, which doesn't create the drops this would destroy: whether a drop created now would be destroyed
// in its Create (the carrier is alive), and the Create's bookkeeping for one such drop at x, y
int rain_cheap_drop_destroyed(void) {
	return orig_create && carrier_alive();
}

void rain_cheap_phantom(float x, float y) {
	drop_add(x, y);
}

static void rain_cheap_step(void *self, void *other) {
	if (self != carrier)
		orig_step(self, other);
}

static void rain_cheap_user15(void *self, void *other) {
	if (self != carrier)
		orig_user15(self, other);
}

static void rain_cheap_update(void *self) {
	uint32_t now = *frame_count;
	uint32_t ticks = now - last_frame;
	last_frame = now;
	if (ticks > 4)
		ticks = 1;
	if (!ticks)
		return;
	double fr = global_num(var_fr, 1);
	float hsp = (float)self_num(self, var_hsp, 0), vsp = (float)self_num(self, var_vsp, 30);
	int layer = (int)self_num(self, var_draw_layer, 0);
	int room = current_room ? *current_room : -1;
	if (rects_room != room || rects_layer != layer || now - rects_frame >= 30)
		build_rects(layer);
	float step = (float)(fr * ticks);
	float xv = (float)global_num(var_xview, 0), yv = (float)global_num(var_yview, 0);
	float wv = (float)global_num(var_wview, 640), hv = (float)global_num(var_hview, 360);
	for (int i = 0; i < ndrops;) {
		drop_t *d = &drops[i];
		if (isnan(d->land))
			drop_landing(d, hsp, vsp);
		d->x += hsp * step;
		d->y += vsp * step;
		int gone = 0;
		if (d->y >= d->land) {
			if (d->splash && d->land > yv - VIEW_MARGIN && d->land < yv + hv + VIEW_MARGIN)
				add_splash(d->x - hsp * (d->y - d->land) / vsp, d->land);
			gone = 1;
		} else if (d->x < xv - VIEW_MARGIN || d->x > xv + wv + VIEW_MARGIN || d->y < yv - VIEW_MARGIN ||
			d->y > yv + hv + VIEW_MARGIN)
			gone = 1;
		if (gone)
			*d = drops[--ndrops];
		else
			i++;
	}
	for (int i = 0; i < nsplashes;) {
		splash_t *s = &splashes[i];
		s->age += 0.3f * step; // image_speed 0.3 (x fr through the lighting's frame rate)
		if (s->age >= s->frames)
			*s = splashes[--nsplashes];
		else
			i++;
	}
}

static void rain_cheap_draw(void *self, void *other) {
	if (self != carrier)
		return;
	uint64_t t0 = prof ? sceKernelGetProcessTimeWide() : 0;
	rain_cheap_update(self);
	// exp_rainab: every other report window draws nothing (in-run A/B against no rain)
	if (ab_off) {
		rain_cheap_frames++;
		if (prof)
			rain_cheap_us += (unsigned)(sceKernelGetProcessTimeWide() - t0);
		return;
	}
	// The game's trail spans the last ticks' motion (shorter in slow motion) and stays put while time is frozen
	static float trail_fr = 1;
	double fr = global_num(var_fr, 1);
	if (fr > 0)
		trail_fr = (float)fr;
	float vsp = (float)self_num(self, var_vsp, 30), hsp = (float)self_num(self, var_hsp, 0);
	float speed = sqrtf(hsp * hsp + vsp * vsp) * trail_fr;
	// The kill-flash freezes (global.fr 0) black out the scene around the characters; the game's lit splashes go dark
	// with it, a fixed-grey one would stay visible: no splashes while time stands still
	int splashes_on = fr > 0;
	// The whitepixel stretched along local +y, rotated so that +y points back along the motion (180 for falling rain)
	float angle = atan2f(-hsp, -vsp) * (180.0f / (float)M_PI);
	for (int i = 0; i < ndrops; i++) {
		const drop_t *d = &drops[i];
		if (d->y < d->land || isnan(d->land)) {
			float len = speed * d->len;
			if (len < 2.0f)
				len = 2.0f;
			draw_sprite_ext(self, SPR_WHITEPIXEL, 0, d->x, d->y, 1.0f, len, angle, DROP_COLOUR, d->alpha);
		}
	}
	for (int i = 0; splashes_on && i < nsplashes; i++) {
		const splash_t *s = &splashes[i];
		draw_sprite_ext(self, s->sprite, floorf(s->age), s->x, s->y, s->xs, 1.0f, 0, SPLASH_COLOUR, 1.0f);
	}
	rain_cheap_draws += ndrops + (splashes_on ? nsplashes : 0);
	rain_cheap_frames++;
	if (prof)
		rain_cheap_us += (unsigned)(sceKernelGetProcessTimeWide() - t0);
}

void rain_cheap_report(int frames) {
	if (!rain_cheap_frames && !rain_cheap_spawned)
		return;
	char timing[48] = "";
	if (prof)
		snprintf(timing, sizeof(timing), ", native %.3f ms/frame",
			rain_cheap_frames ? rain_cheap_us / 1000.0f / rain_cheap_frames : 0.0f);
	debugPrintf("[RAIN] cheap: %.2f drops created/tick, %.2f kept, %.1f draws/frame (%u carriers, %u dirty boxes, %d "
		"boxes)%s%s\n", (float)rain_cheap_spawned / frames, (float)rain_cheap_kept / frames,
		rain_cheap_frames ? (float)rain_cheap_draws / rain_cheap_frames : 0.0f, rain_cheap_carriers, rain_cheap_dirty,
		nrects, timing, ab ? (ab_off ? " | A/B: off" : " | A/B: on") : "");
	if (ab)
		ab_off = !ab_off;
	rain_cheap_spawned = rain_cheap_kept = rain_cheap_draws = rain_cheap_frames = rain_cheap_dirty = 0;
	rain_cheap_carriers = 0;
	rain_cheap_us = 0;
}

#define VAR(name) (int *)so_symbol(&yoyoloader_mod, "g_VAR_" name)

// Returns 1 if installed; the caller falls back to no rain otherwise
int rain_cheap_install(gml_func_entry *table, void *destroy) {
	instance_destroy = destroy;
	draw_sprite_ext = (draw_ext_fn)so_symbol(&yoyoloader_mod, "_Z21YYGML_draw_sprite_extP9CInstanceiffffffif");
	id_hash = (uint32_t *)so_symbol(&yoyoloader_mod, "_ZN9CInstance14ms_ID2InstanceE");
	object_hash = (void **)so_symbol(&yoyoloader_mod, "g_ObjectHash");
	var_fr = VAR("fr");
	var_xview = VAR("xview");
	var_yview = VAR("yview");
	var_wview = VAR("wview");
	var_hview = VAR("hview");
	var_hsp = VAR("hsp");
	var_vsp = VAR("vsp");
	var_draw_layer = VAR("draw_layer");
	if (!instance_destroy || !draw_sprite_ext || !id_hash || !object_hash || !frame_count || !global_obj_ptr ||
		!var_fr || !var_xview || !var_yview || !var_wview || !var_hview || !var_hsp || !var_vsp || !var_draw_layer) {
		debugPrintf("cheap rain: symbols missing, rain off\n");
		return 0;
	}
	for (int n = 2; n <= 8; n++) {
		char name[16];
		snprintf(name, sizeof(name), "rainkeep%d", n);
		if (exp_flag(name))
			keep_n = n;
	}
	no_splash = exp_flag("rainnosplash");
	prof = exp_flag("rainprof");
	ab = exp_flag("rainab");
	// All four, or none: a carrier whose Step ran would fall and draw its trail
	static const char *const names[4] = {"gml_Object_obj_rain_Create_0", "gml_Object_obj_rain_Step_0",
		"gml_Object_obj_rain_Other_25", "gml_Object_obj_rain_Draw_0"};
	event_func wrappers[4] = {rain_cheap_create, rain_cheap_step, rain_cheap_user15, rain_cheap_draw}, origs[4];
	int ok = 1;
	for (int i = 0; i < 4; i++)
		ok &= (origs[i] = gml_replace(table, names[i], wrappers[i])) != NULL;
	if (!ok) {
		for (int i = 0; i < 4; i++)
			if (origs[i])
				gml_replace(table, names[i], origs[i]);
		debugPrintf("cheap rain: obj_rain events missing, rain off\n");
		return 0;
	}
	orig_create = origs[0];
	orig_step = origs[1];
	orig_user15 = origs[2];
	orig_draw = origs[3];
	debugPrintf("cheap rain: installed (one drop in %d%s)\n", keep_n, no_splash ? ", no splashes" : "");
	return 1;
}
