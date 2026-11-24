/*
 * boot_prof.c
 *
 * Startup timeline (debug mode). The log only opens once yyl.cfg is read, after libyoyo.so is loaded, so the first
 * ~9 s of a boot had no log lines at all. boot_mark() records a named process-time stamp from the very start of
 * main(); the stamps are kept in memory until boot_marks_flush() prints them as "[BOOT]" lines (and every later mark
 * is printed at once), each with the time since the previous mark.
 *
 * exp_bootprof (opt-in) also times runner startup functions: each listed function gets an entry hook that logs it
 * when one call took over BOOTFN_LOG_US ("[BOOTFN]", indented by call depth on the game thread), and the runner's
 * stdio/zlib imports get GOT-slot wrappers that count calls, bytes and time. boot_prof_report() prints the totals.
 */

#include <vitasdk.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

#include "main.h"
#include "so_util.h"
#include "hooks.h"

#define MAX_MARKS 96
#define BOOTFN_LOG_US 30000

static struct {
	const char *name;
	uint32_t us;
	int text; // name is a whole log line (boot_mark_text)
} marks[MAX_MARKS];
static int n_marks, marks_flushed;
static uint32_t last_mark_us;

static void print_mark(const char *name, uint32_t us) {
	debugPrintf("[BOOT] %-34s %8.3f s  (+%.3f)\n", name, us / 1000000.0f, (us - last_mark_us) / 1000000.0f);
	last_mark_us = us;
}

void boot_mark(const char *name) {
	uint32_t us = sceKernelGetProcessTimeLow();
	if (marks_flushed) {
		print_mark(name, us);
		return;
	}
	if (n_marks < MAX_MARKS) {
		marks[n_marks].name = name;
		marks[n_marks].us = us;
		n_marks++;
	}
}

// A whole line kept with the marks (copied), printed in order with them
void boot_mark_text(const char *line) {
	if (marks_flushed) {
		debugPrintf("%s\n", line);
		return;
	}
	char *copy = strdup(line);
	if (copy && n_marks < MAX_MARKS) {
		marks[n_marks].name = copy;
		marks[n_marks].us = 0;
		marks[n_marks].text = 1;
		n_marks++;
	}
}

void boot_marks_flush(void) {
	if (marks_flushed)
		return;
	marks_flushed = 1;
	for (int i = 0; i < n_marks; i++) {
		if (marks[i].text)
			debugPrintf("%s\n", marks[i].name);
		else
			print_mark(marks[i].name, marks[i].us);
	}
}

// ---- exp_readbench: memory card read throughput by request size and thread count ----

typedef struct {
	const char *path;
	uint64_t offset;
	uint32_t size, chunk;
	uint8_t *buf;
	uint32_t us;
} bench_job;

static int bench_read(bench_job *j) {
	uint32_t t0 = sceKernelGetProcessTimeLow();
	SceUID fd = sceIoOpen(j->path, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		sceIoLseek(fd, j->offset, SCE_SEEK_SET);
		for (uint32_t done = 0; done < j->size;) {
			uint32_t n = j->size - done < j->chunk ? j->size - done : j->chunk;
			int r = sceIoRead(fd, j->buf + (done % (16 << 20)), n);
			if (r <= 0)
				break;
			done += r;
		}
		sceIoClose(fd);
	}
	j->us = sceKernelGetProcessTimeLow() - t0;
	return 0;
}

static int bench_thread(SceSize args, void *argp) {
	bench_read(*(bench_job **)argp);
	return sceKernelExitThread(0);
}

void boot_readbench(const char *path) {
	if (!exp_flag("readbench"))
		return;
	uint8_t *buf = malloc(4 * (16 << 20));
	if (!buf)
		return;
	uint64_t off = 4 << 20;
	static const uint32_t chunks[] = {64 << 10, 256 << 10, 1 << 20, 4 << 20, 16 << 20};
	char line[512];
	int len = snprintf(line, sizeof(line), "[READBENCH] %s, 8 MB each, one thread:", path);
	for (int i = 0; i < 5; i++, off += 9 << 20) {
		bench_job j = {path, off, 8 << 20, chunks[i], buf, 0};
		bench_read(&j);
		len += snprintf(line + len, sizeof(line) - len, " %uK %.1f MB/s", chunks[i] >> 10, 8.0f * 1000000 / j.us);
	}
	boot_mark_text(line);
	for (int threads = 2; threads <= 4; threads += 2) {
		bench_job jobs[4];
		bench_job *ptrs[4];
		SceUID th[4];
		uint32_t t0 = sceKernelGetProcessTimeLow();
		for (int i = 0; i < threads; i++, off += 9 << 20) {
			jobs[i] = (bench_job){path, off, 8 << 20, 1 << 20, buf + i * (16 << 20), 0};
			ptrs[i] = &jobs[i];
			th[i] = sceKernelCreateThread("readbench", bench_thread, 0x10000100, 0x4000, 0, 0x70000, NULL);
			sceKernelStartThread(th[i], sizeof(bench_job *), &ptrs[i]);
		}
		for (int i = 0; i < threads; i++) {
			sceKernelWaitThreadEnd(th[i], NULL, NULL);
			sceKernelDeleteThread(th[i]);
		}
		uint32_t us = sceKernelGetProcessTimeLow() - t0;
		snprintf(line, sizeof(line), "[READBENCH] %d threads x 8 MB (1M reads): %.1f MB/s total", threads,
			threads * 8.0f * 1000000 / us);
		boot_mark_text(line);
	}
	free(buf);
}

// ---- exp_bootprof: runner function timers ----

typedef uint64_t (*fn8)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

static const struct {
	const char *sym, *label;
} timed_syms[] = {
	{"_Z14RunnerLoadGamev", "RunnerLoadGame"},
	{"_Z13RunnerLoadWadv", "RunnerLoadWad"},
	{"_ZN8LoadSave14ReadBundleFileEPKcPj", "LoadSave::ReadBundleFile"},
	{"_ZN8LoadSave9_ReadFileEPKcPj", "LoadSave::_ReadFile"},
	{"_Z18Code_Function_FindPKcPi", "Code_Function_Find"},
	{"_Z11Script_FindPKc", "Script_Find"},
	{"_Z24Extension_Function_GetIdPKc", "Extension_Function_GetId"},
	{"_Z18Code_Variable_FindPKc", "Code_Variable_Find"},
	{"_Z25InsertToInstanceVarLookupPKci", "InsertToInstanceVarLookup"},
	{"_Z10Code_PatchPhjS_", "Code_Patch"},
	{"_ZN8CLangMan13Language_LoadEPhjS0_", "Language_Load"},
	{"_ZN13MemoryManager7ReAllocEPvjPKcib", "MemoryManager::ReAlloc"},
	{"_Z16F_TexturePreloadR6RValueP9CInstanceS2_iPS_", "texture_prefetch"},
	{"_Z18F_TexturegroupLoadR6RValueP9CInstanceS2_iPS_", "texturegroup_load"},
	{"_Z17F_Audio_LoadGroupR6RValueP9CInstanceS2_iPS_", "audio_group_load"},
	{"zip_open", "zip_open"},
	{"zip_fopen", "zip_fopen"},
	{"zip_fread", "zip_fread"},
	{"_Z12LoadGameDataPvj", "LoadGameData"},
	{"_Z11Header_LoadPhjS_", "Header_Load"},
	{"_Z9Code_LoadPhjS_", "Code_Load"},
	{"_Z11Sprite_LoadPhiS_", "Sprite_Load"},
	{"_Z12Texture_LoadPhjS_", "Texture_Load"},
	{"_Z9Font_LoadPhjS_", "Font_Load"},
	{"_Z9Room_LoadPhjS_", "Room_Load"},
	{"_Z11Object_LoadPhjS_", "Object_Load"},
	{"_Z11Script_LoadPhjS_", "Script_Load"},
	{"_Z12Tileset_LoadPhjS_", "Tileset_Load"},
	{"_Z13Sequence_LoadPhjS_", "Sequence_Load"},
	{"_Z11Shader_LoadPhjS_", "Shader_Load(chunk)"},
	{"_Z11Shader_LoadP6Shader", "Shader_Load(Shader)"},
	{"_Z19EmbeddedImages_LoadPhjS_", "EmbeddedImages_Load"},
	{"_Z21TextureGroupInfo_LoadPhjS_", "TextureGroupInfo_Load"},
	{"_Z14Extension_LoadPhjS_", "Extension_Load"},
	{"_Z14DebugInfo_LoadPhjS_", "DebugInfo_Load"},
	{"_Z9Path_LoadPhjS_", "Path_Load"},
	{"_Z13TimeLine_LoadPhjS_", "TimeLine_Load"},
	{"_Z11Option_LoadPhjS_", "Option_Load"},
	{"_Z9Tags_LoadPhjS_", "Tags_Load"},
	{"_Z9FEAT_LoadPhjS_", "FEAT_Load"},
	{"_Z20ParticleSystems_LoadPhjS_", "ParticleSystems_Load"},
	{"_Z14AnimCurve_LoadPhjS_", "AnimCurve_Load"},
	{"_Z9VARI_LoadPhjS_", "VARI_Load"},
	{"_Z9FUNC_LoadPhjS_", "FUNC_Load"},
	{"_Z15EffectDefs_LoadPhjS_", "EffectDefs_Load"},
	{"_Z19Audio_WADLoadAssetsPKh", "Audio_WADLoadAssets"},
	{"_Z19Audio_WADLoadGroupsPKh", "Audio_WADLoadGroups"},
	{"_Z30Audio_WADLoadAudioDataForGroupPKhS0_i", "Audio_WADLoadAudioDataForGroup"},
	{"_Z18Audio_PrepareGroupi", "Audio_PrepareGroup"},
	{"_Z13GetResolutionPhj", "GetResolution"},
	{"_Z19CreateDesktopWindowii", "CreateDesktopWindow"},
	{"_Z12InitGraphicsv", "InitGraphics"},
	{"_Z7IO_Initv", "IO_Init"},
	{"_Z10DrawSplashv", "DrawSplash"},
	{"_Z19Part_CreateTexturesv", "Part_CreateTextures"},
	{"_ZN2VM4InitEv", "VM::Init"},
	{"_Z9DoTheWorkv", "DoTheWork"},
	{"_ZN10JobManager4InitEv", "JobManager::Init"},
	{"_ZN18TextureLoadManager4InitEv", "TextureLoadManager::Init"},
	{"_Z15GR_Texture_Initv", "GR_Texture_Init"},
	{"_Z17InitYoYoFunctionsv", "InitYoYoFunctions"},
	{"_Z17InitGameFunctionsv", "InitGameFunctions"},
	{"_Z20Variable_Global_Initv", "Variable_Global_Init"},
	{"_Z11PrepareGamev", "PrepareGame"},
	{"_Z10Audio_Initv", "Audio_Init"},
	{"_Z12Game_Preparev", "Game_Prepare"},
	{"_Z14Script_Preparev", "Script_Prepare"},
	{"_Z21Code_Constant_Preparev", "Code_Constant_Prepare"},
	{"_Z16TimeLine_Preparev", "TimeLine_Prepare"},
	{"_Z14Object_Preparev", "Object_Prepare"},
	{"_Z12Room_Preparev", "Room_Prepare"},
	{"_Z16Sequence_Preparev", "Sequence_Prepare"},
	{"_Z9Run_Startv", "Run_Start"},
	{"_Z14CreateColPairsv", "CreateColPairs"},
	{"_Z20Extension_Initializev", "Extension_Initialize"},
	{"_Z9StartRoomib", "StartRoom"},
	{"_Z18GR_Texture_Preloadi", "GR_Texture_Preload"},
	{"_Z19LoadTextureFromQOIFP7Texture10eMipEnable", "LoadTextureFromQOIF"},
	{"_Z13MainLoop_Initv", "MainLoop_Init"},
};
#define N_TIMED (sizeof(timed_syms) / sizeof(*timed_syms))

static fn8 orig_fn[N_TIMED];
static uint64_t fn_total_us[N_TIMED];
static uint32_t fn_calls[N_TIMED];
static SceUID game_thread;
static int game_depth;
static volatile int game_stack[16]; // timed functions running on the game thread, innermost last

static int is_readfile(int i) {
	static int readfile = -2;
	if (readfile == -2) {
		readfile = -1;
		for (int k = 0; k < (int)N_TIMED; k++)
			if (!strcmp(timed_syms[k].label, "LoadSave::_ReadFile"))
				readfile = k;
	}
	return i == readfile;
}

static uint64_t timed_call(int i, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
	uint32_t h) {
	int on_game = sceKernelGetThreadId() == game_thread;
	int depth = on_game ? game_depth++ : -1;
	if (on_game && depth < 16)
		game_stack[depth] = i;
	uint32_t t0 = sceKernelGetProcessTimeLow();
	uint64_t r = orig_fn[i](a, b, c, d, e, f, g, h);
	uint32_t dt = sceKernelGetProcessTimeLow() - t0;
	if (on_game)
		game_depth--;
	fn_total_us[i] += dt;
	fn_calls[i]++;
	if (dt >= BOOTFN_LOG_US || is_readfile(i))
		debugPrintf("[BOOTFN] %*s%s %.1f ms (%.3f -> %.3f s)%s %s\n", depth < 0 ? 0 : depth * 2, "", timed_syms[i].label,
			dt / 1000.0f, t0 / 1000000.0f, (t0 + dt) / 1000000.0f, on_game ? "" : " [other thread]",
			is_readfile(i) && a ? (const char *)a : "");
	return r;
}

#define W(i) \
	static uint64_t w##i(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, \
		uint32_t h) { return timed_call(i, a, b, c, d, e, f, g, h); }
#define W10(n) W(n##0) W(n##1) W(n##2) W(n##3) W(n##4) W(n##5) W(n##6) W(n##7) W(n##8) W(n##9)
W(0) W(1) W(2) W(3) W(4) W(5) W(6) W(7) W(8) W(9)
W10(1) W10(2) W10(3) W10(4) W10(5) W10(6) W10(7) W10(8)
#define R(i) w##i
#define R10(n) R(n##0), R(n##1), R(n##2), R(n##3), R(n##4), R(n##5), R(n##6), R(n##7), R(n##8), R(n##9)
static void *const wrappers[] = {R(0), R(1), R(2), R(3), R(4), R(5), R(6), R(7), R(8), R(9), R10(1), R10(2), R10(3),
	R10(4), R10(5), R10(6), R10(7), R10(8)};
_Static_assert(N_TIMED <= sizeof(wrappers) / sizeof(*wrappers), "more timed functions than wrappers");

// ---- exp_bootprof: runner stdio/zlib import counters (GOT slots) ----

typedef struct {
	uint32_t calls;
	uint64_t us, bytes;
} io_stat;
static io_stat st_fread, st_fseek, st_fopen, st_read, st_open, st_lseek, st_crc, st_inflate, st_strcmp;

static size_t (*o_fread)(void *, size_t, size_t, FILE *);
static int (*o_fseeko)(FILE *, off_t, int);
static int (*o_fseek)(FILE *, long, int);
static FILE *(*o_fopen)(const char *, const char *);
static int (*o_read)(int, void *, size_t);
static int (*o_open)(const char *, int, int);
static off_t (*o_lseek)(int, off_t, int);
static unsigned long (*o_crc32)(unsigned long, const unsigned char *, unsigned);
static int (*o_inflate)(void *, int);
static int (*o_strcmp)(const char *, const char *);

#define STAT_BEGIN uint32_t _t0 = sceKernelGetProcessTimeLow()
#define STAT_END(s, n) do { (s).calls++; (s).us += sceKernelGetProcessTimeLow() - _t0; (s).bytes += (n); } while (0)

static size_t w_fread(void *p, size_t sz, size_t n, FILE *f) {
	STAT_BEGIN;
	size_t r = o_fread(p, sz, n, f);
	STAT_END(st_fread, (uint64_t)r * sz);
	return r;
}
static int w_fseeko(FILE *f, off_t o, int w) {
	STAT_BEGIN;
	int r = o_fseeko(f, o, w);
	STAT_END(st_fseek, 0);
	return r;
}
static int w_fseek(FILE *f, long o, int w) {
	STAT_BEGIN;
	int r = o_fseek(f, o, w);
	STAT_END(st_fseek, 0);
	return r;
}
static FILE *w_fopen(const char *p, const char *m) {
	STAT_BEGIN;
	FILE *r = o_fopen(p, m);
	STAT_END(st_fopen, 0);
	return r;
}
static int w_read(int fd, void *p, size_t n) {
	STAT_BEGIN;
	int r = o_read(fd, p, n);
	STAT_END(st_read, r > 0 ? r : 0);
	return r;
}
static int w_open(const char *p, int fl, int m) {
	STAT_BEGIN;
	int r = o_open(p, fl, m);
	STAT_END(st_open, 0);
	return r;
}
static off_t w_lseek(int fd, off_t o, int w) {
	STAT_BEGIN;
	off_t r = o_lseek(fd, o, w);
	STAT_END(st_lseek, 0);
	return r;
}
static unsigned long w_crc32(unsigned long c, const unsigned char *p, unsigned n) {
	STAT_BEGIN;
	unsigned long r = o_crc32(c, p, n);
	STAT_END(st_crc, n);
	return r;
}
static int w_inflate(void *s, int fl) {
	STAT_BEGIN;
	int r = o_inflate(s, fl);
	STAT_END(st_inflate, 0);
	return r;
}

// Counted only: timing ~45 million calls would distort everything else
static int w_strcmp(const char *a, const char *b) {
	st_strcmp.calls++;
	return o_strcmp(a, b);
}

static void got_wrap(const char *sym, void *wrapper, void **orig) {
	uintptr_t *slot = so_got_slot(&yoyoloader_mod, sym);
	if (!slot || !*slot)
		return;
	*orig = (void *)*slot;
	*slot = (uintptr_t)wrapper;
}

static int bootprof_on;

// ---- exp_bootprof: game-thread sampler ----
// Every 2 ms, notes the innermost timed function on the game thread, the builtin the runner called last (g_pFunction)
// and the current object; boot_prof_report() prints the most frequent combinations. Stops after the third report.

#define SAMPLE_SLOTS 512
static struct {
	uint32_t fn, builtin, object, count;
} samples[SAMPLE_SLOTS];
static volatile int sampler_stop;
static uint32_t samples_total;
static void *volatile *g_pfunction;
static int *g_current_object;

static int sampler_thread(SceSize args, void *argp) {
	while (!sampler_stop) {
		sceKernelDelayThread(2000);
		int d = game_depth;
		uint32_t fn = d > 0 && d <= 16 ? (uint32_t)game_stack[d - 1] + 1 : 0;
		uint32_t b = g_pfunction ? (uint32_t)*g_pfunction : 0;
		uint32_t o = g_current_object ? (uint32_t)*g_current_object : 0xffffffff;
		uint32_t h = (fn * 2654435761u ^ b ^ o * 40503u) % SAMPLE_SLOTS;
		for (int k = 0; k < SAMPLE_SLOTS; k++, h = (h + 1) % SAMPLE_SLOTS) {
			if (!samples[h].count) {
				samples[h].fn = fn;
				samples[h].builtin = b;
				samples[h].object = o;
			}
			if (samples[h].fn == fn && samples[h].builtin == b && samples[h].object == o) {
				samples[h].count++;
				break;
			}
		}
		samples_total++;
	}
	return 0;
}

static void sampler_report(void) {
	const char *const *base = NULL;
	int numb = 0;
	const char *const **functions = (const char *const **)so_symbol(&yoyoloader_mod, "the_functions");
	int *n = (int *)so_symbol(&yoyoloader_mod, "the_numb");
	if (functions && *functions && n) {
		base = *functions;
		numb = *n;
	}
	const char *(*object_name)(int) = (void *)so_symbol(&yoyoloader_mod, "_Z11Object_Namei");
	debugPrintf("[BOOTSAMPLE] %u samples (2 ms each); top combinations of timed function | last builtin | object:\n",
		samples_total);
	static uint8_t shown[SAMPLE_SLOTS];
	memset(shown, 0, sizeof(shown));
	for (int r = 0; r < 25; r++) {
		int best = -1;
		for (int k = 0; k < SAMPLE_SLOTS; k++)
			if (!shown[k] && samples[k].count && (best < 0 || samples[k].count > samples[best].count))
				best = k;
		if (best < 0)
			break;
		shown[best] = 1;
		uintptr_t b = samples[best].builtin, bb = (uintptr_t)base;
		const char *bname = bb && b >= bb && b < bb + (uintptr_t)numb * 12 && (b - bb) % 12 == 0 ?
			*(const char *const *)b : NULL;
		int obj = (int)samples[best].object;
		debugPrintf("[BOOTSAMPLE] %6.1f ms  %-26s | %-28s | %s\n", samples[best].count * 2.0f,
			samples[best].fn ? timed_syms[samples[best].fn - 1].label : "-", bname ? bname : (b ? "?" : "-"),
			obj >= 0 && obj < 100000 && object_name ? object_name(obj) : "-");
	}
}

void boot_prof_install(void) {
	if (!exp_flag("bootprof"))
		return;
	bootprof_on = 1;
	game_thread = sceKernelGetThreadId();
	int ok = 0;
	for (int i = 0; i < (int)N_TIMED; i++) {
		uintptr_t entry = so_symbol(&yoyoloader_mod, timed_syms[i].sym);
		if (!entry)
			continue;
		orig_fn[i] = (fn8)entry_hook(entry, wrappers[i]);
		ok += orig_fn[i] != NULL;
	}
	got_wrap("fread", w_fread, (void **)&o_fread);
	got_wrap("fseeko", w_fseeko, (void **)&o_fseeko);
	got_wrap("fseek", w_fseek, (void **)&o_fseek);
	got_wrap("fopen", w_fopen, (void **)&o_fopen);
	got_wrap("read", w_read, (void **)&o_read);
	got_wrap("open", w_open, (void **)&o_open);
	got_wrap("lseek", w_lseek, (void **)&o_lseek);
	got_wrap("crc32", w_crc32, (void **)&o_crc32);
	got_wrap("inflate", w_inflate, (void **)&o_inflate);
	got_wrap("strcmp", w_strcmp, (void **)&o_strcmp);
	so_flush_caches(&yoyoloader_mod);
	g_pfunction = (void *volatile *)so_symbol(&yoyoloader_mod, "g_pFunction");
	g_current_object = (int *)so_symbol(&yoyoloader_mod, "Current_Object");
	SceUID th = sceKernelCreateThread("boot sampler", sampler_thread, 0x10000100, 0x4000, 0, 0x70000, NULL);
	if (th >= 0)
		sceKernelStartThread(th, 0, NULL);
	debugPrintf("[BOOTFN] boot profiler: %d of %d runner functions timed (calls over %d ms are logged)\n", ok,
		(int)N_TIMED, BOOTFN_LOG_US / 1000);
}

static void print_stat(const char *name, io_stat *s) {
	if (s->calls)
		debugPrintf("[BOOTIO] %-8s %7u calls %9.1f ms %9.2f MB\n", name, s->calls, s->us / 1000.0f,
			s->bytes / 1048576.0f);
}

void boot_prof_report(const char *when) {
	static int reports;
	if (!bootprof_on || reports >= 3)
		return;
	if (++reports == 3)
		sampler_stop = 1;
	debugPrintf("[BOOTFN] totals at %s:\n", when);
	for (int i = 0; i < (int)N_TIMED; i++)
		if (fn_calls[i] && fn_total_us[i] >= 1000)
			debugPrintf("[BOOTFN]   %-32s %6u calls %9.1f ms\n", timed_syms[i].label, fn_calls[i],
				fn_total_us[i] / 1000.0f);
	print_stat("fread", &st_fread);
	print_stat("fseek", &st_fseek);
	print_stat("fopen", &st_fopen);
	print_stat("read", &st_read);
	print_stat("open", &st_open);
	print_stat("lseek", &st_lseek);
	print_stat("crc32", &st_crc);
	print_stat("inflate", &st_inflate);
	print_stat("strcmp", &st_strcmp);
	sampler_report();
}
