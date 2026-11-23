/*
 * Host test for the background save writer (loader/loader/asyncsave.c). Needs glibc (fopencookie), e.g. Linux:
 * gcc -O1 -g -Wall -pthread -Iloader/loader tests/asyncsave_test.c -o asyncsave-test && ./asyncsave-test
 * Also run it with -fsanitize=thread and with -fsanitize=address,undefined.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ASYNCSAVE_HOST 1

static int verbose;
static int debugPrintf(const char *fmt, ...) {
	if (!verbose)
		return 0;
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	return 0;
}

static int hang_depth, hang_max;
static void hang_io_begin(void) {
	int d = __atomic_add_fetch(&hang_depth, 1, __ATOMIC_SEQ_CST);
	if (d > __atomic_load_n(&hang_max, __ATOMIC_SEQ_CST))
		__atomic_store_n(&hang_max, d, __ATOMIC_SEQ_CST);
}
static void hang_io_end(void) {
	__atomic_sub_fetch(&hang_depth, 1, __ATOMIC_SEQ_CST);
}

static int exp_off;
static int exp_flag(const char *name) {
	return !strcmp(name, "asyncsave0") && exp_off;
}

static uint64_t as_now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void as_sleep_us(unsigned us) {
	usleep(us);
}

// SceIo on the host: the Vita's FAT refuses to rename over an existing file (IO_EXISTS)
static const char *fail_install_to; // rename x.new -> fail_install_to fails
static const char *fail_backup_of; // rename fail_backup_of -> x.bak fails
static int sceIoRemove(const char *path);
static int sceIoRename(const char *from, const char *to);

#include "../loader/loader/asyncsave.c"

static int sceIoRemove(const char *path) {
	if (remove(path) == 0)
		return 0;
	return errno == ENOENT ? IO_NO_ENTRY : -1;
}

static int exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static int sceIoRename(const char *from, const char *to) {
	if (!exists(from))
		return IO_NO_ENTRY;
	if (exists(to))
		return IO_EXISTS;
	if (fail_install_to && !strcmp(to, fail_install_to) && strstr(from, ".new"))
		return -1;
	if (fail_backup_of && !strcmp(from, fail_backup_of))
		return -1;
	return rename(from, to) == 0 ? 0 : -1;
}

static char dir[256];

static void path_of(char *out, const char *name) {
	sprintf(out, "%s%s", dir, name);
}

static void put(const char *name, const char *text) {
	char p[512];
	path_of(p, name);
	FILE *f = fopen(p, "wb");
	assert(f);
	fputs(text, f);
	fclose(f);
}

// Contents of a file, or "<none>"
static char got[4096];
static const char *get(const char *name) {
	char p[512];
	path_of(p, name);
	FILE *f = fopen(p, "rb");
	if (!f)
		return "<none>";
	size_t n = fread(got, 1, sizeof(got) - 1, f);
	got[n] = 0;
	fclose(f);
	return got;
}

// A commit the test holds back: records what it wrote, waits while the gate is closed
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int gate_closed, commit_started;
static char commits[32][64];
static int commit_count;

static void gate(int closed) {
	pthread_mutex_lock(&gate_lock);
	gate_closed = closed;
	pthread_cond_broadcast(&gate_cond);
	pthread_mutex_unlock(&gate_lock);
}

static int gated_commit(const char *path, const char *data, size_t len) {
	pthread_mutex_lock(&gate_lock);
	commit_started++;
	pthread_cond_broadcast(&gate_cond);
	while (gate_closed)
		pthread_cond_wait(&gate_cond, &gate_lock);
	snprintf(commits[commit_count++], sizeof(commits[0]), "%s=%.*s", as_name(path), (int)len, data);
	pthread_mutex_unlock(&gate_lock);
	return asyncsave_commit_file(path, data, len);
}

static void wait_started(int n) {
	pthread_mutex_lock(&gate_lock);
	while (commit_started < n)
		pthread_cond_wait(&gate_cond, &gate_lock);
	pthread_mutex_unlock(&gate_lock);
}

static void reset_commits(void) {
	pthread_mutex_lock(&gate_lock);
	commit_count = commit_started = 0;
	pthread_mutex_unlock(&gate_lock);
}

static void queue_text(const char *name, const char *text) {
	char p[512];
	path_of(p, name);
	char *copy = strdup(text);
	assert(asyncsave_queue(p, copy, strlen(text), gated_commit) == 0);
}

static int pending(void) {
	return __atomic_load_n(&as_pending, __ATOMIC_ACQUIRE);
}

static void test_order_and_coalescing(void) {
	reset_commits();
	gate(1);
	queue_text("A.zero", "1");
	wait_started(1); // A=1 is being written
	assert(hang_depth == 1); // the save I/O flag is up while the writer commits
	queue_text("B.zero", "1");
	queue_text("A.zero", "2");
	queue_text("B.zero", "2"); // drops the queued B=1, goes behind A=2
	queue_text("A.zero", "3"); // drops the queued A=2, goes behind B=2
	assert(pending() == 3); // A=1 running, B=2 and A=3 queued
	gate(0);
	asyncsave_wait_all();
	assert(pending() == 0 && hang_depth == 0);
	assert(commit_count == 3);
	assert(!strcmp(commits[0], "A.zero=1") && !strcmp(commits[1], "B.zero=2") && !strcmp(commits[2], "A.zero=3"));
	assert(!strcmp(get("A.zero"), "3") && !strcmp(get("B.zero"), "2"));
	assert(!strcmp(get("A.zero.new"), "<none>") && !strcmp(get("A.zero.bak"), "<none>"));
	puts("order and coalescing: ok");
}

static void test_stdio_file(void) {
	char p[512];
	path_of(p, "KatanaSave.zero");
	assert(asyncsave_is_save_file(p));
	put("KatanaSave.zero", "old contents that are longer");
	FILE *f = asyncsave_fopen_write(p);
	assert(f);
	assert(!strcmp(get("KatanaSave.zero"), "old contents that are longer")); // nothing written at open
	fputs("hello\r\n", f);
	fprintf(f, "%d\r\n", 42);
	assert(ftell(f) == 11);
	assert(fseek(f, 0, SEEK_SET) == 0);
	fputc('J', f);
	assert(fseek(f, 0, SEEK_END) == 0 && ftell(f) == 11);
	assert(fwrite("end", 1, 3, f) == 3);
	assert(fflush(f) == 0);
	assert(!strcmp(get("KatanaSave.zero"), "old contents that are longer")); // still in memory
	assert(fclose(f) == 0);
	asyncsave_wait_path(p);
	assert(!strcmp(get("KatanaSave.zero"), "Jello\r\n42\r\nend"));
	assert(!strcmp(get("KatanaSave.zero.new"), "<none>") && !strcmp(get("KatanaSave.zero.bak"), "<none>"));

	// A seek past the end leaves zeros, as a file does
	f = asyncsave_fopen_write(p);
	assert(f && fseek(f, 3, SEEK_SET) == 0 && fputc('x', f) == 'x' && fclose(f) == 0);
	asyncsave_wait_path(p);
	FILE *r = fopen(p, "rb");
	char buf[8];
	assert(fread(buf, 1, 8, r) == 4 && !memcmp(buf, "\0\0\0x", 4));
	fclose(r);

	// An empty write truncates the file
	f = asyncsave_fopen_write(p);
	assert(f && fclose(f) == 0);
	asyncsave_wait_path(p);
	assert(exists(p) && !strcmp(get("KatanaSave.zero"), ""));

	// Which files are buffered
	char q[600];
	path_of(q, "katanasave.ZERO");
	assert(asyncsave_is_save_file(q));
	path_of(q, "sub/KatanaSave.zero");
	assert(!asyncsave_is_save_file(q));
	path_of(q, "KatanaSave.zero.bak");
	assert(!asyncsave_is_save_file(q));
	path_of(q, "options.ini");
	assert(!asyncsave_is_save_file(q));
	assert(!asyncsave_is_save_file("/tmp/KatanaSave.zero"));
	assert(!strcmp(as_norm("/ux0:data/gms/x.zero"), "ux0:data/gms/x.zero"));
	puts("stdio save file: ok");
}

typedef struct {
	const char *name;
	char seen[64];
	int done;
} reader_arg;

static void *reader(void *a) {
	reader_arg *r = a;
	char p[512];
	path_of(p, r->name);
	asyncsave_wait_path(p); // what fopen_hook/stat_hook do before touching a pending file
	snprintf(r->seen, sizeof(r->seen), "%s", get(r->name));
	__atomic_store_n(&r->done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void test_read_after_write(void) {
	reset_commits();
	put("R.zero", "old");
	gate(1);
	queue_text("R.zero", "first");
	wait_started(1);
	queue_text("R.zero", "second"); // queued behind the running write
	reader_arg r = { "R.zero" };
	pthread_t t;
	pthread_create(&t, NULL, reader, &r);
	usleep(100 * 1000);
	assert(!__atomic_load_n(&r.done, __ATOMIC_SEQ_CST)); // waits for both writes
	// Other files don't wait
	reader_arg other = { "A.zero" };
	pthread_t t2;
	pthread_create(&t2, NULL, reader, &other);
	pthread_join(t2, NULL);
	assert(!strcmp(other.seen, "3"));
	gate(0);
	pthread_join(t, NULL);
	assert(!strcmp(r.seen, "second"));
	puts("read after write: ok");
}

typedef struct {
	const char *from, *to;
	int res;
} op_arg;

static void *remover(void *a) {
	op_arg *o = a;
	char p[512];
	path_of(p, o->from);
	o->res = asyncsave_remove(p);
	return NULL;
}

static void *renamer(void *a) {
	op_arg *o = a;
	char p[512], q[512];
	path_of(p, o->from);
	path_of(q, o->to);
	o->res = asyncsave_rename(p, q);
	return NULL;
}

static void test_delete_and_rename_after_write(void) {
	reset_commits();
	gate(1);
	queue_text("D.zero", "pending");
	wait_started(1);
	op_arg o = { "D.zero" };
	pthread_t t;
	pthread_create(&t, NULL, remover, &o);
	usleep(50 * 1000);
	gate(0);
	pthread_join(t, NULL);
	assert(o.res == 0 && commit_count == 1 && !strcmp(get("D.zero"), "<none>")); // the remove came last

	reset_commits();
	gate(1);
	queue_text("E.zero", "moved");
	op_arg m = { "E.zero", "F.zero" };
	pthread_create(&t, NULL, renamer, &m);
	usleep(50 * 1000);
	gate(0);
	pthread_join(t, NULL);
	assert(m.res == 0 && !strcmp(get("F.zero"), "moved") && !strcmp(get("E.zero"), "<none>"));
	puts("delete and rename after write: ok");
}

static void test_failures(void) {
	char p[512];
	path_of(p, "G.zero");
	put("G.zero", "good");
	unsigned failed = as_failed;
	// The new file can't be put in place: the old one stays, the stage file goes
	fail_install_to = p;
	assert(asyncsave_queue(p, strdup("bad"), 3, asyncsave_commit_file) == 0);
	asyncsave_wait_path(p);
	assert(as_failed == failed + 1 && hang_depth == 0);
	assert(!strcmp(get("G.zero"), "good") && !strcmp(get("G.zero.new"), "<none>"));
	fail_install_to = NULL;
	// The old file can't be moved aside: nothing changes
	fail_backup_of = p;
	assert(asyncsave_commit_file(p, "bad", 3) < 0);
	assert(!strcmp(get("G.zero"), "good") && !strcmp(get("G.zero.new"), "<none>"));
	fail_backup_of = NULL;
	// The next write goes through
	assert(asyncsave_queue(p, strdup("better"), 6, asyncsave_commit_file) == 0);
	asyncsave_wait_path(p);
	assert(!strcmp(get("G.zero"), "better") && !strcmp(get("G.zero.bak"), "<none>"));
	// A directory that doesn't exist: fails, nothing created
	char q[600];
	path_of(q, "missing/H.zero");
	assert(asyncsave_commit_file(q, "x", 1) < 0);
	puts("failures: ok");
}

static void test_flush(void) {
	reset_commits();
	gate(1);
	queue_text("I.zero", "1");
	queue_text("J.zero", "1");
	uint64_t t0 = as_now_us();
	assert(asyncsave_flush(100, "test") == 2);
	assert(as_now_us() - t0 >= 100 * 1000);
	gate(0);
	assert(asyncsave_flush(2000, "test") == 0);
	assert(!strcmp(get("I.zero"), "1") && !strcmp(get("J.zero"), "1"));
	assert(asyncsave_flush(0, "test") == 0); // nothing pending: returns at once
	puts("flush: ok");
}

static void test_recovery(void) {
	// Stopped between the renames: no file, the old one in .bak, the complete new one in .new
	put("K.zero.bak", "old");
	put("K.zero.new", "new");
	// Rollback failed: only .bak
	put("L.zero.bak", "old");
	// Stopped before the renames: the file and a .new that may be incomplete; left alone
	put("M.zero", "current");
	put("M.zero.new", "partial");
	as_recover(dir);
	assert(!strcmp(get("K.zero"), "new") && !strcmp(get("L.zero"), "old") && !strcmp(get("M.zero"), "current"));
	assert(!strcmp(get("K.zero.new"), "<none>"));
	puts("recovery: ok");
}

static void test_off(void) {
	char p[512];
	path_of(p, "N.zero");
	asyncsave_on = 0;
	assert(!asyncsave_fopen_write(p));
	assert(asyncsave_queue(p, (char *)"x", 1, asyncsave_commit_file) < 0);
	asyncsave_on = 1;
	puts("off: ok");
}

// Many writes to a few files from one thread while another thread reads them: every read sees the latest write
static void test_stress(void) {
	char p[3][512];
	for (int i = 0; i < 3; i++) {
		char name[16];
		sprintf(name, "S%d.zero", i);
		path_of(p[i], name);
	}
	for (int n = 0; n < 300; n++) {
		int i = n % 3;
		FILE *f = asyncsave_fopen_write(p[i]);
		assert(f);
		fprintf(f, "%d", n);
		assert(fclose(f) == 0);
		if (n % 7 == 0) {
			asyncsave_wait_path(p[i]);
			char name[16];
			sprintf(name, "S%d.zero", i);
			assert(atoi(get(name)) == n);
		}
	}
	asyncsave_wait_all();
	assert(atoi(get("S0.zero")) == 297 && atoi(get("S1.zero")) == 298 && atoi(get("S2.zero")) == 299);
	printf("stress: ok (%u written, %u superseded)\n", as_written, as_dropped);
}

int main(int argc, char **argv) {
	verbose = argc > 1;
	char tmpl[] = "/tmp/asyncsave-XXXXXX";
	assert(mkdtemp(tmpl));
	snprintf(dir, sizeof(dir), "%s/", tmpl);
	asyncsave_init(dir, 0);
	assert(asyncsave_on);
	test_order_and_coalescing();
	test_stdio_file();
	test_read_after_write();
	test_delete_and_rename_after_write();
	test_failures();
	test_flush();
	test_recovery();
	test_off();
	test_stress();
	assert(hang_depth == 0 && hang_max >= 1);
	char cmd[300];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpl);
	system(cmd);
	puts("all asyncsave tests passed");
	return 0;
}
