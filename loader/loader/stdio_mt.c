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
 * Here every thread keeps its own position per FILE: a seek records it, and a read first moves the FILE to the
 * calling thread's position, all under one lock, so each thread sees the file as if it were alone.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
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

int mt_fseeko(FILE *f, off_t offset, int whence) {
	pthread_mutex_lock(&mt_lock);
	mt_slot *s = mt_find(f, 0);
	// SEEK_CUR is relative to this thread's position, not wherever another thread left the FILE
	if (s && whence == SEEK_CUR) {
		offset += s->pos;
		whence = SEEK_SET;
	}
	int ret = fseeko(f, offset, whence);
	if (!ret) {
		if (!s)
			s = mt_find(f, 1);
		s->pos = ftello(f);
	}
	pthread_mutex_unlock(&mt_lock);
	return ret;
}

int mt_fseek(FILE *f, long offset, int whence) {
	return mt_fseeko(f, (off_t)offset, whence);
}

off_t mt_ftello(FILE *f) {
	pthread_mutex_lock(&mt_lock);
	mt_slot *s = mt_find(f, 0);
	off_t pos = s ? s->pos : ftello(f);
	pthread_mutex_unlock(&mt_lock);
	return pos;
}

long mt_ftell(FILE *f) {
	return (long)mt_ftello(f);
}

size_t mt_fread(void *buf, size_t size, size_t count, FILE *f) {
	pthread_mutex_lock(&mt_lock);
	mt_slot *s = mt_find(f, 0);
	if (s && ftello(f) != s->pos)
		fseeko(f, s->pos, SEEK_SET);
	size_t ret = fread(buf, size, count, f);
	if (!s)
		s = mt_find(f, 1);
	s->pos = ftello(f);
	pthread_mutex_unlock(&mt_lock);
	return ret;
}

int mt_fgetc(FILE *f) {
	pthread_mutex_lock(&mt_lock);
	mt_slot *s = mt_find(f, 0);
	if (s && ftello(f) != s->pos)
		fseeko(f, s->pos, SEEK_SET);
	int ret = fgetc(f);
	if (s)
		s->pos = ftello(f);
	pthread_mutex_unlock(&mt_lock);
	return ret;
}

char *mt_fgets(char *str, int n, FILE *f) {
	pthread_mutex_lock(&mt_lock);
	mt_slot *s = mt_find(f, 0);
	if (s && ftello(f) != s->pos)
		fseeko(f, s->pos, SEEK_SET);
	char *ret = fgets(str, n, f);
	if (s)
		s->pos = ftello(f);
	pthread_mutex_unlock(&mt_lock);
	return ret;
}

int mt_fclose(FILE *f) {
	pthread_mutex_lock(&mt_lock);
	mt_forget(f);
	int ret = fclose(f);
	pthread_mutex_unlock(&mt_lock);
	return ret;
}
