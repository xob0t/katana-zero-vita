/*
 * stdio_mt.c
 *
 * Thread-safe seek/read for FILEs shared between runner threads.
 *
 * The runner reads game.apk through libzip, which keeps one FILE for the whole archive and refills each open
 * entry's buffer with fseeko(apk, entry_pos) + fread(apk). The OGG streaming threads load songs this way while
 * the game thread loads other assets at a room change; when their seek/read pairs interleave, a thread reads
 * from the other thread's position ("Failed to read vorbis info for assets/song_*.ogg", "ov_read returned -3":
 * the scene has no music).
 *
 * Each thread keeps its own position on the registered APK streams. A seek records it, and a read first moves
 * the FILE to the calling thread's position, all under one lock. Other streams use ordinary stdio semantics.
 */

#include <errno.h>
#include <pthread.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>

#define MT_SLOTS 64

typedef struct {
	pthread_t thread;
	FILE *file;
	off_t pos;
	int used;
	unsigned age;
} mt_slot;

static pthread_mutex_t mt_lock = PTHREAD_MUTEX_INITIALIZER;
// The thread holding mt_lock (exp_bootwatch's stall report reads it)
volatile int mt_lock_owner;
static inline void mt_lock_take(void) {
	pthread_mutex_lock(&mt_lock);
	mt_lock_owner = sceKernelGetThreadId();
}
static inline void mt_lock_drop(void) {
	mt_lock_owner = 0;
	pthread_mutex_unlock(&mt_lock);
}
static mt_slot mt_slots[MT_SLOTS];
static unsigned mt_clock;

typedef struct apk_stream {
	FILE *file;
	struct apk_stream *next;
} apk_stream;

static apk_stream *apk_streams;

int mt_register_apk(FILE *f) {
	apk_stream *stream = malloc(sizeof(*stream));
	if (!stream)
		return -1;
	stream->file = f;
	mt_lock_take();
	stream->next = apk_streams;
	apk_streams = stream;
	mt_lock_drop();
	return 0;
}

static apk_stream **mt_apk_link(FILE *f) {
	apk_stream **link = &apk_streams;
	while (*link && (*link)->file != f)
		link = &(*link)->next;
	return link;
}

static int mt_lock_apk(FILE *f) {
	mt_lock_take();
	if (*mt_apk_link(f))
		return 1;
	mt_lock_drop();
	return 0;
}

static mt_slot *mt_find(FILE *f, int create) {
	pthread_t self = pthread_self();
	mt_slot *oldest = &mt_slots[0];
	for (int i = 0; i < MT_SLOTS; i++) {
		mt_slot *s = &mt_slots[i];
		if (s->used && s->file == f && pthread_equal(s->thread, self)) {
			s->age = ++mt_clock;
			return s;
		}
		if (!s->used || (oldest->used && s->age < oldest->age))
			oldest = s;
	}
	if (!create)
		return NULL;
	oldest->used = 1;
	oldest->thread = self;
	oldest->file = f;
	oldest->age = ++mt_clock;
	return oldest;
}

static void mt_forget(FILE *f) {
	for (int i = 0; i < MT_SLOTS; i++)
		if (mt_slots[i].used && mt_slots[i].file == f)
			mt_slots[i].used = 0;
}

static int mt_record(FILE *f, mt_slot *s) {
	off_t pos = ftello(f);
	if (pos < 0) {
		if (s)
			s->used = 0;
		return -1;
	}
	if (!s)
		s = mt_find(f, 1);
	s->pos = pos;
	return 0;
}

int mt_fseeko(FILE *f, off_t offset, int whence) {
	if (!mt_lock_apk(f))
		return fseeko(f, offset, whence);
	mt_slot *s = mt_find(f, 0);
	// SEEK_CUR is relative to this thread's position, not wherever another thread left the FILE
	if (s && whence == SEEK_CUR) {
		if (__builtin_add_overflow(offset, s->pos, &offset)) {
			mt_lock_drop();
			errno = EOVERFLOW;
			return -1;
		}
		whence = SEEK_SET;
	}
	int ret = fseeko(f, offset, whence);
	if (!ret)
		ret = mt_record(f, s);
	mt_lock_drop();
	return ret;
}

int mt_fseek(FILE *f, long offset, int whence) {
	return mt_fseeko(f, (off_t)offset, whence);
}

off_t mt_ftello(FILE *f) {
	if (!mt_lock_apk(f))
		return ftello(f);
	mt_slot *s = mt_find(f, 0);
	off_t pos = s ? s->pos : ftello(f);
	mt_lock_drop();
	return pos;
}

long mt_ftell(FILE *f) {
	return (long)mt_ftello(f);
}

static int mt_restore(FILE *f, mt_slot *s) {
	return s && ftello(f) != s->pos ? fseeko(f, s->pos, SEEK_SET) : 0;
}

long apk_prefetch_serve(uint64_t pos, void *buf, size_t n);
int apk_prefetch_wait(uint64_t pos, size_t n);

size_t mt_fread(void *buf, size_t size, size_t count, FILE *f) {
	for (;;) {
		if (!mt_lock_apk(f))
			return fread(buf, size, count, f);
		mt_slot *s = mt_find(f, 0);
		size_t ret = 0;
		if (!mt_restore(f, s)) {
			// Bytes the prefetcher already read (apk_prefetch.c): copied, then the FILE moves to where fread would leave it
			off_t pos = size == 1 && count ? ftello(f) : -1;
			long got = pos >= 0 ? apk_prefetch_serve((uint64_t)pos, buf, count) : 0;
			if (got < 0) {
				mt_lock_drop();
				if (apk_prefetch_wait((uint64_t)pos, count))
					continue;
				if (!mt_lock_apk(f))
					return fread(buf, size, count, f);
				s = mt_find(f, 0);
				if (mt_restore(f, s)) {
					mt_lock_drop();
					return 0;
				}
				got = 0;
			}
			if (got > 0 && fseeko(f, pos + got, SEEK_SET)) {
				// Can't move the FILE past the copied bytes: read them again from the card
				fseeko(f, pos, SEEK_SET);
				got = 0;
			}
			ret = got;
			if ((size_t)got < count)
				ret += fread((char *)buf + got, size, count - got, f);
			mt_record(f, s);
		}
		mt_lock_drop();
		return ret;
	}
}

int mt_fgetc(FILE *f) {
	if (!mt_lock_apk(f))
		return fgetc(f);
	mt_slot *s = mt_find(f, 0);
	int ret = EOF;
	if (!mt_restore(f, s)) {
		ret = fgetc(f);
		mt_record(f, s);
	}
	mt_lock_drop();
	return ret;
}

char *mt_fgets(char *str, int n, FILE *f) {
	if (!mt_lock_apk(f))
		return fgets(str, n, f);
	mt_slot *s = mt_find(f, 0);
	char *ret = NULL;
	if (!mt_restore(f, s)) {
		ret = fgets(str, n, f);
		mt_record(f, s);
	}
	mt_lock_drop();
	return ret;
}

int mt_fclose(FILE *f) {
	mt_lock_take();
	apk_stream **link = mt_apk_link(f);
	if (*link) {
		apk_stream *stream = *link;
		*link = stream->next;
		free(stream);
		mt_forget(f);
	}
	mt_lock_drop();
	int ret = fclose(f);
	extern void hang_io_file_close(void *file);
	hang_io_file_close(f); // after the close: save I/O ends when the file is flushed and closed
	return ret;
}
