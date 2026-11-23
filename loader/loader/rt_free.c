/*
 * rt_free.c
 *
 * Deferred deletes of render-target textures (GameMaker surfaces).
 *
 * vitaGL frees a texture's memory at once in glDeleteTextures, or in glDeleteFramebuffers when the texture is still
 * attached, unless the texture was *sampled* in the last FRAME_PURGE_FREQ (4) frames (gpu_free_texture_data,
 * utils/gpu_utils.h). Being drawn into doesn't count as use. A surface the game cleared or drew into and then freed
 * without having drawn it recently (room end, room restart) loses its VRAM while the GPU still has that frame's
 * writes into it queued. The heap hands the block out again, or keeps its free-list links in it, and the late GPU
 * writes overwrite them.
 *
 * Seen in a playtest (2026-11-23, restarting room_mansion_bikechase after deaths): the vitaGL Garbage Collector
 * thread took a data abort in sceClibMspaceFree. The VRAM free chunk next to a live 140x140 surface had its
 * free-list links overwritten with 0xff000000 (opaque black, a surface clear).
 *
 * Textures the runner attached as an FBO colour target are deleted RT_FREE_DELAY swaps later instead, when the GPU
 * is done with every frame that could have written them. Until then the texture name stays reserved
 * (glGenTextures can't hand it out again), so nothing else sees the difference. Other textures (texture pages) are
 * deleted at once, as before: the loader's texture-flush drain relies on that.
 *
 * Opt-out: exp_rtfree0. Debug mode logs [RTFREE] totals when they change (at most every 2 s).
 */
#include <stdint.h>
#include <string.h>
#include <vitasdk.h>
#include <vitaGL.h>

extern int debugMode;
extern int exp_flag(const char *name);
extern int debugPrintf(char *text, ...);

#define RT_FREE_TEXTURES 16384 // vitaGL TEXTURES_NUM
#define RT_FREE_DELAY 6        // swaps; vitaGL's own deferred frees wait FRAME_PURGE_FREQ (4) collector cycles
#define RT_FREE_QUEUE 512

static uint32_t rt_attached[RT_FREE_TEXTURES / 32]; // texture names attached as a colour target since their creation
static struct {
	GLuint tex;
	uint32_t due; // swap count at which it is deleted
} rt_queue[RT_FREE_QUEUE];
static unsigned rt_head, rt_count; // ring buffer, oldest at rt_head
static uint32_t rt_swaps;
static unsigned rt_deferred, rt_forced; // totals: deferred deletes, deletes run early because the queue was full
static int rt_off = -1;

static int rt_enabled(void) {
	if (rt_off < 0)
		rt_off = exp_flag("rtfree0");
	return !rt_off;
}

static void rt_delete_oldest(void) {
	GLuint tex = rt_queue[rt_head].tex;
	rt_head = (rt_head + 1) % RT_FREE_QUEUE;
	rt_count--;
	glDeleteTextures(1, &tex);
}

void rt_free_framebuffer_texture(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
	if (texture > 0 && texture < RT_FREE_TEXTURES && attachment == GL_COLOR_ATTACHMENT0)
		rt_attached[texture >> 5] |= 1u << (texture & 31);
	glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

void rt_free_delete_textures(GLsizei n, const GLuint *textures) {
	if (n <= 0 || !textures || !rt_enabled()) {
		glDeleteTextures(n, textures);
		return;
	}
	for (GLsizei i = 0; i < n; i++) {
		GLuint tex = textures[i];
		if (tex > 0 && tex < RT_FREE_TEXTURES && (rt_attached[tex >> 5] & (1u << (tex & 31)))) {
			rt_attached[tex >> 5] &= ~(1u << (tex & 31));
			if (rt_count == RT_FREE_QUEUE) {
				rt_delete_oldest();
				rt_forced++;
			}
			unsigned slot = (rt_head + rt_count) % RT_FREE_QUEUE;
			rt_queue[slot].tex = tex;
			rt_queue[slot].due = rt_swaps + RT_FREE_DELAY;
			rt_count++;
			rt_deferred++;
		} else
			glDeleteTextures(1, &tex);
	}
}

// After every vglSwapBuffers of the game's frames, on the game thread (the runner's GL thread)
void rt_free_swapped(void) {
	rt_swaps++;
	while (rt_count && (int32_t)(rt_swaps - rt_queue[rt_head].due) >= 0)
		rt_delete_oldest();
	if (debugMode) {
		static unsigned logged_deferred = 0, logged_forced = 0;
		static uint32_t last_log;
		if ((rt_deferred != logged_deferred || rt_forced != logged_forced) && rt_swaps - last_log >= 120) {
			logged_deferred = rt_deferred;
			logged_forced = rt_forced;
			last_log = rt_swaps;
			debugPrintf("[RTFREE] render-target texture deletes deferred: %u (pending %u, run early %u)\n",
				rt_deferred, rt_count, rt_forced);
		}
	}
}
