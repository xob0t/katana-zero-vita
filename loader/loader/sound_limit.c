/*
 * sound_limit.c
 *
 * Caps how many copies of one sound asset play at the same time (sameSoundLimit in yyl.cfg, default 3).
 *
 * A laser death dissolves the body into chunks that each play the same positional sound: ~45 overlapping
 * copies. Mixing them held the runner's audio lock for ~30 ms per mixer pass, and every audio_* call on the
 * game thread waited for it (frames of 60+ ms). With the cap, further non-looping copies of a sound that is
 * already playing sameSoundLimit times are skipped (audio_play_sound* return -1, as for a failed play).
 */

#include <stdint.h>
#include <string.h>

#include "main.h"
#include "so_util.h"

typedef struct {
	double val;
	uint32_t flags;
	uint32_t kind;
} rvalue;

typedef void (*builtin_fn)(rvalue *result, void *self, void *other, int argc, rvalue *args);

typedef struct {
	const char *name;
	builtin_fn func;
	int argc;
} registry_entry;

#define SOUND_SLOTS 4096
#define SAME_SOUND_MAX 8

int same_sound_limit = 3;
uint32_t sound_limit_skipped;

static builtin_fn orig_play_sound, orig_play_sound_at, orig_play_sound_on, audio_is_playing_fn;
static rvalue sound_handles[SOUND_SLOTS][SAME_SOUND_MAX];

static int sound_index(const rvalue *v) {
	int index;
	if (v->kind == 15)
		memcpy(&index, &v->val, 4); // asset reference: id in the low word
	else if (v->kind == 0)
		index = (int)v->val;
	else
		return -1;
	return index >= 0 && index < SOUND_SLOTS ? index : -1;
}

static int handle_playing(rvalue *handle) {
	rvalue r = {0};
	audio_is_playing_fn(&r, NULL, NULL, 1, handle);
	return r.val > 0.5;
}

// Slot for a new copy of the sound, or -1 when it already plays same_sound_limit times
static int free_slot(int index) {
	int slot = -1;
	for (int i = 0; i < same_sound_limit; i++) {
		rvalue *h = &sound_handles[index][i];
		if (h->kind == 0 && h->val == 0.0) {
			if (slot < 0)
				slot = i;
		} else if (!handle_playing(h)) {
			memset(h, 0, sizeof(*h));
			if (slot < 0)
				slot = i;
		}
	}
	return slot;
}

static void limited_play(builtin_fn orig, int sound_arg, int loop_arg, rvalue *result, void *self, void *other,
		int argc, rvalue *args) {
	int index = argc > sound_arg ? sound_index(&args[sound_arg]) : -1;
	int looping = argc > loop_arg && args[loop_arg].val > 0.5;
	int slot = -1;
	if (index >= 0 && !looping) {
		slot = free_slot(index);
		if (slot < 0) {
			sound_limit_skipped++;
			result->kind = 0;
			result->val = -1.0;
			return;
		}
	}
	orig(result, self, other, argc, args);
	if (slot >= 0)
		sound_handles[index][slot] = *result;
}

// audio_play_sound(sound, priority, loop)
static void play_sound(rvalue *r, void *s, void *o, int argc, rvalue *a) {
	limited_play(orig_play_sound, 0, 2, r, s, o, argc, a);
}

// audio_play_sound_at(sound, x, y, z, falloff_ref, falloff_max, falloff_factor, loop, priority)
static void play_sound_at(rvalue *r, void *s, void *o, int argc, rvalue *a) {
	limited_play(orig_play_sound_at, 0, 7, r, s, o, argc, a);
}

// audio_play_sound_on(emitter, sound, loop, priority)
static void play_sound_on(rvalue *r, void *s, void *o, int argc, rvalue *a) {
	limited_play(orig_play_sound_on, 1, 2, r, s, o, argc, a);
}

// Needs the runner's function registry: call once it is filled (katana_late_install)
void sound_limit_install(void) {
	registry_entry **functions = (registry_entry **)so_symbol(&yoyoloader_mod, "the_functions");
	int *count = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	if (same_sound_limit <= 0 || !functions || !*functions || !count)
		return;
	if (same_sound_limit > SAME_SOUND_MAX)
		same_sound_limit = SAME_SOUND_MAX;
	for (int i = 0; i < *count; i++) {
		registry_entry *f = &(*functions)[i];
		if (f->name && !strcmp(f->name, "audio_is_playing"))
			audio_is_playing_fn = f->func;
	}
	if (!audio_is_playing_fn)
		return;
	for (int i = 0; i < *count; i++) {
		registry_entry *f = &(*functions)[i];
		if (!f->name)
			continue;
		if (!strcmp(f->name, "audio_play_sound")) {
			orig_play_sound = f->func;
			f->func = play_sound;
		} else if (!strcmp(f->name, "audio_play_sound_at")) {
			orig_play_sound_at = f->func;
			f->func = play_sound_at;
		} else if (!strcmp(f->name, "audio_play_sound_on")) {
			orig_play_sound_on = f->func;
			f->func = play_sound_on;
		}
	}
	debugPrintf("same-sound limit: %d\n", same_sound_limit);
}
