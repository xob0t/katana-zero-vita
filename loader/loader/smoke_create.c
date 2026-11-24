/* Target libyoyo SHA256 ab7014c19c449858c8ea53c75b2c7cd3d78642d6aad60297aa0d7db456bd4701 (Katana ZERO 1.1.10).
 *
 * The two warm-up loops of obj_smokecloud's Create, natively (exact).
 *
 * A smoke vial bursts into 131 clouds in one frame (create_smokescreen); each cloud's Create runs
 *   repeat (random(100)) { if (offset < 0) vsp += 0.002 * global.fr; if (offset > 0) vsp -= 0.002 * global.fr;
 *                          offset += vsp * global.fr; }
 * and the same for hoffset / hsp, about 100 iterations per cloud in YYC: every iteration looks the variables up
 * again (hash lookups), compares through YYCompareVal and does the arithmetic through YYRValue operators. The
 * Create events took 34.6 ms of the 48 ms burst frame on the Vita.
 *
 * The loop entries (after the repeat count is taken and found >= 1) branch to a stub that calls loop_native. It
 * runs the loop in doubles when every value involved is a plain real and writes what the original leaves behind:
 *  - offset / vsp (hoffset / hsp) with the values the original computes, operation for operation: the comparison
 *    with 0 as YYCompareVal does it for two reals (difference within g_GMLMathEpsilon = equal), 0.002 * fr
 *    (operator*=, rhs * lhs), vsp + that (operator+=, rhs + lhs; kind and flags untouched), vsp - that
 *    (operator-=), and offset + vsp * fr; a NaN anywhere leaves everything to the original;
 *  - the event's scratch RValue (fp - 0x98) as the last iteration leaves it (vsp * fr, vsp's flags and kind), and
 *    the outgoing argument slot [sp] the loop stores YYCompareVal's flag (1) in;
 * then continues at the loop exit. The loops draw no random numbers (random(100) is called before the stub) and
 * the variables they look up are all set earlier in the same event, so the lookups the original repeats change
 * nothing. Anything else (a kind that isn't a plain real, fr not a real, the code not as expected) runs the
 * original loop.
 *
 * Debug flags: exp_smokecreate0 (the original loops), exp_smokecreateverify (loop_native predicts, the original
 * loop runs, and a stub at each loop exit compares the variables, the scratch RValue and the argument slot with
 * the prediction), exp_smokecreatebreak (negative control for verify: the prediction runs one iteration too few),
 * exp_smokecreateab (the native on every other smoke burst of the create_object=smoke debug request). Report line
 * [SMOKECREATE].
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <kubridge.h>
#include "main.h"
#include "so_util.h"

#pragma GCC optimize("no-fast-math")

extern int debugMode;
extern void *katana_rx_alloc(size_t size);

#define AAPCS __attribute__((pcs("aapcs")))
typedef struct { double val; uint32_t flags, kind; } RV;
typedef RV *AAPCS (*ref_fn)(void *self, int id);

static int *var_ids[4]; // offset, vsp, hoffset, hsp
static const double *epsilon;
static int sc_verify, sc_break;
int smoke_create_ab, smoke_create_ab_off;
unsigned smoke_create_native, smoke_create_original;
static unsigned sc_iterations, sc_verify_ok, sc_verify_bad;

static uint32_t word(const void *p, size_t off) { uint32_t v; memcpy(&v, (const uint8_t *)p + off, 4); return v; }

// The variable's RValue, as the loop's GetYYVarRefL calls (vtable +0xc) return it
static RV *var_ref(void *self, int which) {
	ref_fn ref = (ref_fn)(uintptr_t)word((void *)(uintptr_t)word(self, 0), 0xc);
	return ref(self, var_ids[which][1]);
}

// YYCompareVal(a, 0.0) for a real a: -1, 0, 1, or 2 for NaN (the runner's other path)
static int compare_zero(double a, double eps) {
	double d = a - 0.0;
	if (d != d)
		return 2;
	return fabs(d) <= eps ? 0 : (d < 0 ? -1 : 1);
}

typedef struct {
	int ok;
	double pos, vel, tmp;
	uint32_t tmp_flags, tmp_kind;
} Result;

static int simulate(const RV *pos, const RV *vel, const RV *fr, int count, Result *out) {
	if (pos->kind != 0 || vel->kind != 0 || (fr->kind & 0xffffff) != 0)
		return 0;
	double p = pos->val, v = vel->val, f = fr->val, eps = *epsilon, tmp = 0;
	static const uint64_t k_bits = 0x3f60624dd2f1a9fcull; // 0.002, as the event stores it
	double k;
	memcpy(&k, &k_bits, 8);
	for (int i = 0; i < count; i++) {
		int c = compare_zero(p, eps);
		if (c == 2)
			return 0;
		if (c == -1)
			v = f * k + v;
		if (c == 1)
			v = v - f * k;
		tmp = f * v;
		p = tmp + p;
	}
	out->ok = 1;
	out->pos = p;
	out->vel = v;
	out->tmp = tmp;
	out->tmp_flags = vel->flags;
	out->tmp_kind = vel->kind;
	return 1;
}

// exp_smokecreateverify: the prediction for the loop now running in the original
static struct { void *self; int which; Result r; } pending;

// Called by the entry stubs: orig_sp is the event's sp (self at +0x24, the global.fr RValue pointer at +0x20, the
// argument slot at +0), fp its frame pointer (scratch RValue at fp - 0x98). 1 = done, continue at the loop exit.
static int AAPCS loop_native(uint8_t *orig_sp, int count, uint8_t *fp, int which) {
	void *self = (void *)(uintptr_t)word(orig_sp, 0x24);
	const RV *fr = (const RV *)(uintptr_t)word(orig_sp, 0x20);
	if (count < 1 || !self || !fr || (smoke_create_ab && smoke_create_ab_off)) {
		smoke_create_original++;
		return 0;
	}
	RV *pos = var_ref(self, which * 2), *vel = var_ref(self, which * 2 + 1);
	Result r;
	if (!pos || !vel || !simulate(pos, vel, fr, sc_verify && sc_break ? count - 1 : count, &r)) {
		smoke_create_original++;
		return 0;
	}
	if (sc_verify) {
		pending.self = self;
		pending.which = which;
		pending.r = r;
		smoke_create_original++;
		return 0;
	}
	pos->val = r.pos;
	vel->val = r.vel;
	RV *scratch = (RV *)(fp - 0x98);
	scratch->val = r.tmp;
	scratch->flags = r.tmp_flags;
	scratch->kind = r.tmp_kind;
	uint32_t one = 1;
	memcpy(orig_sp, &one, 4);
	smoke_create_native++;
	sc_iterations += (unsigned)count;
	return 1;
}

// Called by the exit stubs (verify only)
static void AAPCS loop_check(uint8_t *orig_sp, uint8_t *fp, int which) {
	void *self = (void *)(uintptr_t)word(orig_sp, 0x24);
	if (!pending.self || pending.self != self || pending.which != which)
		return;
	pending.self = NULL;
	RV *pos = var_ref(self, which * 2), *vel = var_ref(self, which * 2 + 1);
	const RV *scratch = (const RV *)(fp - 0x98);
	const Result *r = &pending.r;
	int ok = pos && vel && !memcmp(&pos->val, &r->pos, 8) && !memcmp(&vel->val, &r->vel, 8) && pos->kind == 0 &&
		vel->kind == 0 && !memcmp(&scratch->val, &r->tmp, 8) && scratch->flags == r->tmp_flags &&
		scratch->kind == r->tmp_kind && word(orig_sp, 0) == 1;
	if (ok) {
		sc_verify_ok++;
	} else {
		if (sc_verify_bad < 8)
			debugPrintf("[SMOKECREATE] mismatch: cloud %u loop %d pos %.17g/%.17g vel %.17g/%.17g tmp %.17g/%.17g "
				"%08x/%08x %08x/%08x arg %u\n", word(self, 0x80), which, pos ? pos->val : 0, r->pos,
				vel ? vel->val : 0, r->vel, scratch->val, r->tmp, scratch->flags, r->tmp_flags, scratch->kind,
				r->tmp_kind, word(orig_sp, 0));
		sc_verify_bad++;
	}
}

// ---- Patching ----
#define CREATE 0x48f8948
static const struct { uint32_t entry, entry_ins, exit, exit_ins; } sites[2] = {
	{ 0x48fa448, 0xe59d7024, 0x48fa7b8, 0xe3a00039 }, // offset / vsp: ldr r7, [sp, #0x24] / mov r0, #0x39
	{ 0x48fa7e0, 0xe2807001, 0x48faabc, 0xe59d0024 }, // hoffset / hsp: add r7, r0, #1 / ldr r0, [sp, #0x24]
};

// bl from site to an 8-byte veneer near it that jumps to target
static int branch_to(uintptr_t site, uintptr_t target) {
	uint32_t *veneer = (uint32_t *)so_alloc_near(&yoyoloader_mod, site, 8);
	int32_t off = veneer ? (int32_t)((uintptr_t)veneer - (site + 8)) : 0;
	if (!veneer || off < -(1 << 25) || off >= (1 << 25))
		return 0;
	uint32_t code[2] = { 0xe51ff004, (uint32_t)target }; // ldr pc, [pc, #-4]
	kuKernelCpuUnrestrictedMemcpy(veneer, code, sizeof(code));
	kuKernelFlushCaches(veneer, sizeof(code));
	uint32_t bl = 0xeb000000 | (((uint32_t)off >> 2) & 0xffffff);
	kuKernelCpuUnrestrictedMemcpy((void *)site, &bl, 4);
	kuKernelFlushCaches((void *)site, 4);
	return 1;
}

int smoke_create_install(void) {
	static int installed;
	if (installed)
		return 0;
	uintptr_t create = so_symbol(&yoyoloader_mod, "_Z34gml_Object_obj_smokecloud_Create_0P9CInstanceS0_");
	if (!create)
		return 0;
	uintptr_t bias = create - CREATE;
	// The loops as read: FNV-1a over every instruction from the first repeat's random(100) setup to past the second
	// loop's exit, bl instructions left out (patch_plt_calls.py retargets them)
	uint32_t h = 0x811c9dc5u;
	for (uintptr_t a = 0x48fa418; a < 0x48faaf8; a += 4) {
		uint32_t w = word((void *)(bias + a), 0);
		if ((w & 0x0f000000) == 0x0b000000)
			continue;
		for (int b = 0; b < 4; b++)
			h = (h ^ ((w >> (8 * b)) & 0xff)) * 16777619u;
	}
	if (h != 0x2701598eu) {
		debugPrintf("[SMOKECREATE] loop code differs (hash %08x)\n", h);
		return 0;
	}
	static const char *const names[4] = { "g_VAR_offset", "g_VAR_vsp", "g_VAR_hoffset", "g_VAR_hsp" };
	for (int i = 0; i < 4; i++)
		if (!(var_ids[i] = (int *)so_symbol(&yoyoloader_mod, names[i])))
			return 0;
	epsilon = (const double *)so_symbol(&yoyoloader_mod, "g_GMLMathEpsilon");
	if (!epsilon)
		return 0;
	if (debugMode) {
		sc_verify = exp_flag("smokecreateverify");
		sc_break = exp_flag("smokecreatebreak");
		smoke_create_ab = exp_flag("smokecreateab");
	}
	uint32_t *rx = katana_rx_alloc(2 * 4 * 15 + (sc_verify ? 2 * 4 * 11 : 0));
	if (!rx)
		return 0;
	for (int i = 0; i < 2; i++) {
		if (word((void *)(bias + sites[i].entry), 0) != sites[i].entry_ins ||
				word((void *)(bias + sites[i].exit), 0) != sites[i].exit_ins)
			return 0;
	}
	for (int i = 0; i < 2; i++) {
		uint32_t *stub = rx + 15 * i;
		uint32_t code[15] = {
			0xe92d500f,                 // push {r0-r3, ip, lr}
			0xe1a01000,                 // mov r1, r0 (the repeat count)
			0xe28d0018,                 // add r0, sp, #24 (the event's sp)
			0xe1a0200b,                 // mov r2, fp
			0xe3a03000 | (uint32_t)i,   // mov r3, #which
			0xe59fc01c,                 // ldr ip, [pc, #28] (loop_native)
			0xe12fff3c,                 // blx ip
			0xe3500000,                 // cmp r0, #0
			0xe8bd500f,                 // pop {r0-r3, ip, lr}
			0x1a000001,                 // bne exit
			sites[i].entry_ins,         // the instruction the bl replaced
			0xe12fff1e,                 // bx lr (back into the original loop)
			0xe51ff004,                 // exit: ldr pc, [pc, #-4]
			(uint32_t)(bias + sites[i].exit),
			(uint32_t)(uintptr_t)loop_native,
		};
		kuKernelCpuUnrestrictedMemcpy(stub, code, sizeof(code));
		kuKernelFlushCaches(stub, sizeof(code));
	}
	if (sc_verify)
		for (int i = 0; i < 2; i++) {
			uint32_t *stub = rx + 30 + 11 * i;
			uint32_t code[11] = {
				0xe92d500f,                 // push {r0-r3, ip, lr}
				0xe28d0018,                 // add r0, sp, #24
				0xe1a0100b,                 // mov r1, fp
				0xe3a02000 | (uint32_t)i,   // mov r2, #which
				0xe59fc010,                 // ldr ip, [pc, #16] (loop_check)
				0xe12fff3c,                 // blx ip
				0xe8bd500f,                 // pop {r0-r3, ip, lr}
				sites[i].exit_ins,          // the instruction the bl replaced
				0xe12fff1e,                 // bx lr
				0xe1a00000,                 // nop
				(uint32_t)(uintptr_t)loop_check,
			};
			kuKernelCpuUnrestrictedMemcpy(stub, code, sizeof(code));
			kuKernelFlushCaches(stub, sizeof(code));
		}
	for (int i = 0; i < 2; i++)
		if (!branch_to(bias + sites[i].entry, (uintptr_t)(rx + 15 * i)) ||
				(sc_verify && !branch_to(bias + sites[i].exit, (uintptr_t)(rx + 30 + 11 * i))))
			return 0; // (a partial install leaves patched entries working: each stub is complete)
	installed = 1;
	return 1;
}

void smoke_create_report(int frames) {
	(void)frames;
	if (!smoke_create_native && !smoke_create_original)
		return;
	debugPrintf("[SMOKECREATE] cloud Create loops: %u native (%u iterations), %u original | verify ok %u mismatch %u\n",
		smoke_create_native, sc_iterations, smoke_create_original, sc_verify_ok, sc_verify_bad);
	smoke_create_native = smoke_create_original = sc_iterations = 0;
}
