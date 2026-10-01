/*
 * glprof.c
 *
 * Debug-mode GL call profiler: wraps selected GL entrypoints handed to the runner
 * through dlsym and reports per-frame call counts and CPU time spent inside them.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <string.h>

#include "main.h"

enum {
	P_DRAW_ARRAYS,
	P_DRAW_ELEMENTS,
	P_BIND_FRAMEBUFFER,
	P_USE_PROGRAM,
	P_CLEAR,
	P_TEX_IMAGE,
	P_TEX_SUB_IMAGE,
	P_COMPILE_SHADER,
	P_LINK_PROGRAM,
	P_UNIFORM,
	P_BUFFER_DATA,
	P_BIND_TEXTURE,
	P_COUNT
};

static const char *prof_names[P_COUNT] = {
	"DrawArrays", "DrawElements", "BindFramebuffer", "UseProgram", "Clear",
	"TexImage2D", "TexSubImage2D", "CompileShader", "LinkProgram", "Uniform*",
	"BufferData", "BindTexture"
};

static uint32_t prof_calls[P_COUNT];
static uint32_t prof_time[P_COUNT];

#define PROF_BEGIN uint32_t prof_start = sceKernelGetProcessTimeLow()
#define PROF_END(id) prof_time[id] += sceKernelGetProcessTimeLow() - prof_start; prof_calls[id]++

#define PROF_VOID(id, name, params, args) \
	static void (*orig_##name) params; \
	static void prof_##name params { PROF_BEGIN; orig_##name args; PROF_END(id); }

PROF_VOID(P_DRAW_ARRAYS, glDrawArrays, (GLenum m, GLint f, GLsizei c), (m, f, c))
PROF_VOID(P_DRAW_ELEMENTS, glDrawElements, (GLenum m, GLsizei c, GLenum t, const void *i), (m, c, t, i))
static void (*orig_glBindFramebuffer)(GLenum t, GLuint f);
static GLuint cur_fb;
static void prof_glBindFramebuffer(GLenum t, GLuint f) {
	PROF_BEGIN;
	orig_glBindFramebuffer(t, f);
	cur_fb = f;
	PROF_END(P_BIND_FRAMEBUFFER);
}
PROF_VOID(P_USE_PROGRAM, glUseProgram, (GLuint p), (p))
// Per-frame ordinal breakdown of glClear to find which render target switch stalls
#define CLEAR_SLOTS 16
static uint32_t clear_time[CLEAR_SLOTS];
static uint32_t clear_calls[CLEAR_SLOTS];
static GLuint clear_fb[CLEAR_SLOTS];
static GLbitfield clear_mask[CLEAR_SLOTS];
static int clear_ordinal;

static void (*orig_glClear)(GLbitfield m);
static void prof_glClear(GLbitfield m) {
	PROF_BEGIN;
	orig_glClear(m);
	uint32_t t = sceKernelGetProcessTimeLow() - prof_start;
	prof_time[P_CLEAR] += t;
	prof_calls[P_CLEAR]++;
	if (clear_ordinal < CLEAR_SLOTS) {
		clear_time[clear_ordinal] += t;
		clear_calls[clear_ordinal]++;
		clear_fb[clear_ordinal] = cur_fb;
		clear_mask[clear_ordinal] = m;
	}
	clear_ordinal++;
}

void glprof_frame(void) {
	clear_ordinal = 0;
}
extern int vgl_forced_gc_allocs, vgl_failed_allocs;
static void (*orig_glTexImage2D)(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *d);
static void prof_glTexImage2D(GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *d) {
	PROF_BEGIN;
	orig_glTexImage2D(t, l, i, w, h, b, f, ty, d);
	uint32_t us = sceKernelGetProcessTimeLow() - prof_start;
	prof_time[P_TEX_IMAGE] += us;
	prof_calls[P_TEX_IMAGE]++;
	if (us > 5000)
		debugPrintf("[TEXIMG] %dx%d lvl %d ifmt %04X fmt %04X type %04X data %p: %.1fms (forced gc %d, failed %d)\n", w, h, l, i, f, ty, d, us / 1000.0f,
			vgl_forced_gc_allocs, vgl_failed_allocs);
}
PROF_VOID(P_TEX_SUB_IMAGE, glTexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *d), (t, l, x, y, w, h, f, ty, d))
PROF_VOID(P_COMPILE_SHADER, glCompileShader, (GLuint s), (s))
PROF_VOID(P_LINK_PROGRAM, glLinkProgram, (GLuint p), (p))
PROF_VOID(P_BUFFER_DATA, glBufferData, (GLenum t, GLsizeiptr s, const void *d, GLenum u), (t, s, d, u))
PROF_VOID(P_BIND_TEXTURE, glBindTexture, (GLenum t, GLuint tex), (t, tex))
PROF_VOID(P_UNIFORM, glUniform1f, (GLint l, GLfloat a), (l, a))
PROF_VOID(P_UNIFORM, glUniform1i, (GLint l, GLint a), (l, a))
PROF_VOID(P_UNIFORM, glUniform2f, (GLint l, GLfloat a, GLfloat b), (l, a, b))
PROF_VOID(P_UNIFORM, glUniform4f, (GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d), (l, a, b, c, d))
PROF_VOID(P_UNIFORM, glUniform4fv, (GLint l, GLsizei c, const GLfloat *v), (l, c, v))
PROF_VOID(P_UNIFORM, glUniformMatrix4fv, (GLint l, GLsizei c, GLboolean t, const GLfloat *v), (l, c, t, v))

#define PROF_ENTRY(name) { #name, (void **)&orig_##name, (void *)prof_##name }

static struct {
	const char *symbol;
	void **orig;
	void *wrapper;
} prof_table[] = {
	PROF_ENTRY(glDrawArrays),
	PROF_ENTRY(glDrawElements),
	PROF_ENTRY(glBindFramebuffer),
	PROF_ENTRY(glUseProgram),
	PROF_ENTRY(glClear),
	PROF_ENTRY(glTexImage2D),
	PROF_ENTRY(glTexSubImage2D),
	PROF_ENTRY(glCompileShader),
	PROF_ENTRY(glLinkProgram),
	PROF_ENTRY(glBufferData),
	PROF_ENTRY(glBindTexture),
	PROF_ENTRY(glUniform1f),
	PROF_ENTRY(glUniform1i),
	PROF_ENTRY(glUniform2f),
	PROF_ENTRY(glUniform4f),
	PROF_ENTRY(glUniform4fv),
	PROF_ENTRY(glUniformMatrix4fv),
};

// Returns a profiling wrapper around func if symbol is tracked, otherwise func itself
void *glprof_wrap(const char *symbol, void *func) {
	if (!func)
		return func;
	for (int i = 0; i < sizeof(prof_table) / sizeof(*prof_table); i++) {
		if (!strcmp(symbol, prof_table[i].symbol)) {
			*prof_table[i].orig = func;
			return prof_table[i].wrapper;
		}
	}
	return func;
}

// Prints per-frame averages over the given number of frames and resets the counters
void glprof_report(int frames) {
	uint32_t total = 0;
	for (int i = 0; i < P_COUNT; i++)
		total += prof_time[i];
	debugPrintf("[GLPROF] total GL %.2fms/frame\n", total / 1000.0f / frames);
	for (int i = 0; i < P_COUNT; i++) {
		if (prof_calls[i])
			debugPrintf("[GLPROF]   %-16s %7.1f calls %6.2fms\n", prof_names[i], (float)prof_calls[i] / frames, prof_time[i] / 1000.0f / frames);
	}
	for (int i = 0; i < CLEAR_SLOTS; i++) {
		if (clear_calls[i])
			debugPrintf("[GLPROF]   clear#%-2d fb %3u mask %04X %5.1f%% %6.2fms\n", i, clear_fb[i], clear_mask[i],
				100.0f * clear_calls[i] / frames, clear_time[i] / 1000.0f / frames);
	}
	memset(clear_time, 0, sizeof(clear_time));
	memset(clear_calls, 0, sizeof(clear_calls));
	memset(prof_calls, 0, sizeof(prof_calls));
	memset(prof_time, 0, sizeof(prof_time));
}
