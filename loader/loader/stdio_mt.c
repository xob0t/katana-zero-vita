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
#include <stdio.h>
#include <stdlib.h>
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
	pthread_mutex_lock(&mt_lock);
	stream->next = apk_streams;
	apk_streams = stream;
	pthread_mutex_unlock(&mt_lock);
	return 0;
}

static apk_stream **mt_apk_link(FILE *f) {
	apk_stream **link = &apk_streams;
	while (*link && (*link)->file != f)
		link = &(*link)->next;
	return link;
}

static int mt_lock_apk(FILE *f) {
	pthread_mutex_lock(&mt_lock);
	if (*mt_apk_link(f))
		return 1;
	pthread_mutex_unlock(&mt_lock);
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
			pthread_mutex_unlock(&mt_lock);
			errno = EOVERFLOW;
			return -1;
		}
		whence = SEEK_SET;
	}
	int ret = fseeko(f, offset, whence);
	if (!ret)
		ret = mt_record(f, s);
	pthread_mutex_unlock(&mt_lock);
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
	pthread_mutex_unlock(&mt_lock);
	return pos;
}

long mt_ftell(FILE *f) {
	return (long)mt_ftello(f);
}

static int mt_restore(FILE *f, mt_slot *s) {
	return s && ftello(f) != s->pos ? fseeko(f, s->pos, SEEK_SET) : 0;
}

size_t mt_fread(void *buf, size_t size, size_t count, FILE *f) {
	if (!mt_lock_apk(f))
		return fread(buf, size, count, f);
	mt_slot *s = mt_find(f, 0);
	size_t ret = 0;
	if (!mt_restore(f, s)) {
		ret = fread(buf, size, count, f);
		mt_record(f, s);
	}
	pthread_mutex_unlock(&mt_lock);
	return ret;
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
	pthread_mutex_unlock(&mt_lock);
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
	pthread_mutex_unlock(&mt_lock);
	return ret;
}

int mt_fclose(FILE *f) {
	pthread_mutex_lock(&mt_lock);
	apk_stream **link = mt_apk_link(f);
	if (*link) {
		apk_stream *stream = *link;
		*link = stream->next;
		free(stream);
		mt_forget(f);
	}
	pthread_mutex_unlock(&mt_lock);
	int ret = fclose(f);
	extern void hang_io_file_close(void *file);
	hang_io_file_close(f); // after the close: save I/O ends when the file is flushed and closed
	return ret;
}
