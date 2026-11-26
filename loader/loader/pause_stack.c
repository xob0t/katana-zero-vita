/*
 * pause_stack.c
 *
 * obj_gamepaused Draw (APK 1.1.10) draws the paused room picture over its opaque black rectangle ten times:
 *
 *   draw_rect_col(vx - 2, vy - 2, vx + vw + 2, vy + vh + 2, c_black, false);
 *   repeat (10) draw_surface_ext(screencopy, vx, vy, 1, 1, 0, c_white, 1);
 *
 * Ten full-screen blended layers (bm_normal): about 4.5 ms of GPU per paused frame. Composing them into a cached
 * surface doesn't help: VRAM is full while paused, the cache lands in main RAM, and reading it costs more than the
 * blending (pause_bg.c's history in PR #114).
 *
 * This draws the ten layers as one quad with a fragment shader that reads the screencopy texel once and applies the
 * ten blends in the shader, with the destination rounded to 8 bits after each (the render target is RGBA8):
 *   d = (0, 0, 0, 1); repeat (10) d = round8(s * s.a + d * (1 - s.a));
 * written with blending disabled. The quad covers the application surface (960x540 here; the screencopy and the view
 * are 1280x720); each fragment reads the texture at its own position on the surface, as the game's quad maps it.
 *
 * Only when: the ten calls have exactly these arguments, drawn at the view origin, the screencopy is the view's size
 * (so the quad covers the view, which the runner maps onto the whole application surface), the rectangle right before covered the view in black at alpha 1 (pause_bg.c's checks), and the
 * runner's default render state holds.
 *
 * exp_pausestackverify (debug): every 30th frame draws the ten original layers, reads the application surface back,
 * draws the shader quad over them, reads it back again and compares every byte ([PAUSESTACK] differing pixels and
 * the largest difference per channel). pauseStack=0 in yyl.cfg: the game's 10 draws. exp_pausestackab: A/B by report
 * window.
 */

#include <vitasdk.h>
#include <vitaGL.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "main.h"
#include "so_util.h"
#include "katana_internal.h"

#define SYM(name) so_symbol(&yoyoloader_mod, name)
#define KIND(v) ((v)->kind & 0xffffff)

typedef gml_rvalue RV;
typedef void *(*get_fn)(int id);
typedef double (*real_fn)(const RV *v);
typedef void (*void_fn)(void);

extern int pause_bg_in_gp(void *self);
extern int pause_bg_state_ok(void);
extern int pause_bg_camera_view(void *self, void *other, double *out);
extern uint32_t pause_bg_rect_frame, pause_bg_rect_rgba;

static builtin_fn orig_dse;
static get_fn surface_get, texture_get_surface;
static real_fn real_ex;
static void_fn graphics_flush;
static int *app_surface, *current_targets;

int pause_stack = 1; // pauseStack in yyl.cfg: 1 one shader pass (default), 0 the game's 10 draws
static int installed, ab_mode, verify;
int pause_stack_ab_off;
static uint32_t stack_frame = 0xffffffff;
static int call_idx, mode;
static RV first[8];
static unsigned n_shader, n_orig, reason[8];

static GLuint prog;
static GLint loc_tex, loc_size;
static int prog_state; // 0 not built, 1 ok, -1 failed

static const char *vsrc =
	"attribute vec2 pos;\n"
	"void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fsrc =
	"precision highp float;\n"
	"uniform sampler2D tex;\n"
	"uniform vec2 size;\n"
	"void main() {\n"
	"  vec4 s = texture2D(tex, gl_FragCoord.xy / size);\n"
	"  vec4 d = vec4(0.0, 0.0, 0.0, 1.0);\n"
	"  float k = 1.0 - s.a;\n"
	"  d = floor((s * s.a + d * k) * 255.0 + 0.5) / 255.0;\n"
	"  d = floor((s * s.a + d * k) * 255.0 + 0.5) / 255.0;\n"
	"  d = floor((s * s.a + d * k) * 255.0 + 0.5) / 255.0;\n"
	"  d = floor((s * s.a + d * k) * 255.0 + 0.5) / 255.0;\n"
	"  d = floor((s * s.a + d * k) * 255.0 + 0.5) / 255.0;\n"
	"  d = floor((s * s.a + d * k) * 255.0 + 0.5) / 255.0;\n"
	"  d = floor((s * s.a + d * k) * 255.0 + 0.5) / 255.0;\n"
	"  d = floor((s * s.a + d * k) * 255.0 + 0.5) / 255.0;\n"
	"  d = floor((s * s.a + d * k) * 255.0 + 0.5) / 255.0;\n"
	"  d = floor((s * s.a + d * k) * 255.0 + 0.5) / 255.0;\n"
	"  gl_FragColor = d;\n"
	"}\n";

static GLuint compile(GLenum type, const char *src) {
	GLuint sh = glCreateShader(type);
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[256] = "";
		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		debugPrintf("[PAUSESTACK] shader compile failed: %s\n", log);
		return 0;
	}
	return sh;
}

static int build_program(void) {
	if (prog_state)
		return prog_state > 0;
	prog_state = -1;
	GLint old = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &old);
	GLuint v = compile(GL_VERTEX_SHADER, vsrc), f = compile(GL_FRAGMENT_SHADER, fsrc);
	if (!v || !f)
		return 0;
	prog = glCreateProgram();
	glAttachShader(prog, v);
	glAttachShader(prog, f);
	glBindAttribLocation(prog, 0, "pos");
	glLinkProgram(prog);
	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		debugPrintf("[PAUSESTACK] shader link failed\n");
		return 0;
	}
	loc_tex = glGetUniformLocation(prog, "tex");
	loc_size = glGetUniformLocation(prog, "size");
	glUseProgram(prog);
	glUniform1i(loc_tex, 0);
	glUseProgram(old);
	prog_state = 1;
	debugPrintf("[PAUSESTACK] shader ready\n");
	return 1;
}

static const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};

// One quad with the shader over the current target (the application surface), GL state put back as found
static void draw_quad(GLuint tex, int w, int h) {
	GLint old_prog = 0, old_active = 0, old_tex = 0, old_abuf = 0, old_en = 0;
	GLboolean blend = glIsEnabled(GL_BLEND);
	glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_abuf);
	glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &old_en);
	glUseProgram(prog);
	glUniform2f(loc_size, (float)w, (float)h);
	glBindTexture(GL_TEXTURE_2D, tex);
	if (blend)
		glDisable(GL_BLEND);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if (!old_en)
		glDisableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, old_abuf);
	if (blend)
		glEnable(GL_BLEND);
	glBindTexture(GL_TEXTURE_2D, old_tex);
	glActiveTexture(old_active);
	glUseProgram(old_prog);
}

static uint32_t word(const void *p, unsigned off) {
	return *(const uint32_t *)((const uint8_t *)p + off);
}

static int fail(int r) {
	reason[r]++;
	return 0;
}

static int surface_id(const RV *v, int *id) {
	if (KIND(v) == 0) {
		if (!isfinite(v->val) || v->val != floor(v->val) || v->val < 0 || v->val > 1e6)
			return 0;
		*id = (int)v->val;
		return 1;
	}
	if (KIND(v) == 15) {
		*id = (int)real_ex(v);
		return *id >= 0;
	}
	return 0;
}

// --- verify ---
#define VW 1280
#define VH 720
#define BAND 48
static uint32_t band[VW * BAND];
static uint32_t *orig_px; // memblock (not newlib: big heap blocks there must stay free for texture pages)
static unsigned v_frames, v_bad_frames, v_bad_px, v_max[4], v_tick;
static unsigned v_hist[4];

static void readback(uint32_t *dst, int w, int h, int compare) {
	graphics_flush();
	for (int y0 = 0; y0 < h; y0 += BAND) {
		int n = h - y0 < BAND ? h - y0 : BAND;
		glReadPixels(0, y0, w, n, GL_RGBA, GL_UNSIGNED_BYTE, compare ? band : dst + y0 * w);
		if (!compare)
			continue;
		for (int i = 0; i < w * n; i++) {
			uint32_t a = dst[y0 * w + i], b = band[i];
			if (a == b)
				continue;
			v_bad_px++;
			for (int c = 0; c < 4; c++) {
				int d = abs((int)((a >> (8 * c)) & 0xff) - (int)((b >> (8 * c)) & 0xff));
				if ((unsigned)d > v_max[c])
					v_max[c] = d;
				v_hist[d > 3 ? 3 : d]++;
			}
		}
	}
}

static int try_shader(RV *result, void *self, void *other, int argc, RV *args) {
	if (ab_mode && pause_stack_ab_off)
		return fail(0);
	int src;
	if (argc != 8 || !surface_id(&args[0], &src))
		return fail(1);
	for (int i = 1; i < 8; i++)
		if (KIND(&args[i]) != 0 || !isfinite(args[i].val))
			return fail(1);
	if (args[3].val != 1.0 || args[4].val != 1.0 || args[5].val != 0.0 || args[6].val != 16777215.0 ||
			args[7].val != 1.0)
		return fail(1);
	if (pause_bg_rect_frame != *frame_count || pause_bg_rect_rgba != 0xff000000u || !pause_bg_state_ok() ||
			current_targets[0] != *app_surface)
		return fail(2);
	double view[4];
	if (!pause_bg_camera_view(self, other, view) || args[1].val != view[0] || args[2].val != view[1])
		return fail(3);
	void *surf = surface_get(src), *app = surface_get(*app_surface);
	if (!surf || !app)
		return fail(4);
	void *tex = texture_get_surface((int)word(surf, 4)), *app_tex = texture_get_surface((int)word(app, 4));
	if (!tex || !app_tex || (int)word(tex, 0x1c) == -1)
		return fail(4);
	extern int pause_bg_surface_size(int id, int *w, int *h);
	int sw, sh, aw, ah;
	int ok5 = pause_bg_surface_size(src, &sw, &sh) && pause_bg_surface_size(*app_surface, &aw, &ah);
	if (!ok5 || view[2] != sw || view[3] != sh) {
		if (!reason[5])
			debugPrintf("[PAUSESTACK] sizes: screencopy %dx%d, application surface %dx%d (id %d), view %gx%g\n",
				ok5 ? sw : -1, ok5 ? sh : -1, ok5 ? aw : -1, ok5 ? ah : -1, *app_surface, view[2], view[3]);
		return fail(5);
	}
	if (!build_program())
		return fail(6);
	const GLuint name = word(tex, 0x1c);
	if (verify && v_tick++ % 30 == 15 && orig_px && aw <= VW && ah <= VH) {
		for (int i = 0; i < 10; i++)
			orig_dse(result, self, other, argc, args);
		readback(orig_px, aw, ah, 0);
		draw_quad(name, aw, ah);
		unsigned before = v_bad_px;
		readback(orig_px, aw, ah, 1);
		v_frames++;
		if (v_bad_px != before)
			v_bad_frames++;
	} else {
		graphics_flush();
		draw_quad(name, aw, ah);
	}
	return 1;
}

static void w_dse(RV *result, void *self, void *other, int argc, RV *args) {
	if (!installed || !pause_bg_in_gp(self)) {
		orig_dse(result, self, other, argc, args);
		return;
	}
	if (stack_frame != *frame_count) {
		stack_frame = *frame_count;
		call_idx = 0;
		mode = 0;
	}
	call_idx++;
	if (call_idx == 1) {
		mode = try_shader(result, self, other, argc, args);
		if (mode) {
			memcpy(first, args, sizeof(first));
			n_shader++;
			return;
		}
		n_orig++;
	} else if (mode && call_idx <= 10 && argc == 8 && !memcmp(args, first, sizeof(first)))
		return;
	orig_dse(result, self, other, argc, args);
}

void pause_stack_report(int frames) {
	if (!installed)
		return;
	if (n_shader || n_orig)
		debugPrintf("[PAUSESTACK] shader %.2f original %.2f per frame%s | reasons %u %u %u %u %u %u %u\n",
			(float)n_shader / frames, (float)n_orig / frames, ab_mode && pause_stack_ab_off ? " (A/B: original this window)" : "",
			reason[0], reason[1], reason[2], reason[3], reason[4], reason[5], reason[6]);
	if (verify && v_frames)
		debugPrintf("[PAUSESTACK] verify: %u frames, %u with differences, %u differing pixels, max diff R %u G %u B %u A %u, channel diffs 1:%u 2:%u 3+:%u\n",
			v_frames, v_bad_frames, v_bad_px, v_max[0], v_max[1], v_max[2], v_max[3], v_hist[1], v_hist[2], v_hist[3]);
	n_shader = n_orig = 0;
	if (ab_mode)
		pause_stack_ab_off = !pause_stack_ab_off;
}

// katana_late_install: the runner's function registry is filled by then
void pause_stack_late_install(void) {
	if (!pause_stack) {
		debugPrintf("pause stack: the game's 10 draws (pauseStack=0)\n");
		return;
	}
	runner_function **functions = (runner_function **)SYM("the_functions");
	int *count = (int *)SYM("the_numb");
	runner_function *f_dse = NULL;
	for (int i = 0; functions && *functions && count && i < *count; i++)
		if ((*functions)[i].name && !strcmp((*functions)[i].name, "draw_surface_ext"))
			f_dse = &(*functions)[i];
	surface_get = (get_fn)SYM("_Z14GR_Surface_Geti");
	texture_get_surface = (get_fn)SYM("_Z22GR_Texture_Get_Surfacei");
	real_ex = (real_fn)SYM("_Z14REAL_RValue_ExPK6RValue");
	graphics_flush = (void_fn)SYM("_ZN8Graphics5FlushEv");
	app_surface = (int *)SYM("g_ApplicationSurface");
	current_targets = (int *)SYM("currenttargets");
	if (!f_dse || !surface_get || !texture_get_surface || !real_ex || !graphics_flush || !app_surface ||
			!current_targets || !frame_count) {
		debugPrintf("pause stack: NOT installed (runner symbols)\n");
		return;
	}
	verify = debugMode && exp_flag("pausestackverify");
	ab_mode = debugMode && exp_flag("pausestackab");
	if (verify) {
		SceUID b = sceKernelAllocMemBlock("pausestack verify", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * 1024 * 1024, NULL);
		if (b < 0 || sceKernelGetMemBlockBase(b, (void **)&orig_px) < 0)
			orig_px = NULL;
	}
	orig_dse = builtin_swap((void **)&f_dse->func, w_dse);
	// (recorded: virtual_capture.c, installed before, checks this entry)
	installed = 1;
	debugPrintf("pause stack: one shader pass for the 10 screencopy layers%s%s\n", verify ? " (verify)" : "",
		ab_mode ? ", A/B by report window" : "");
}
