/*
 * runner_inflate.c
 *
 * Reads a deflated apk entry with the card read, the inflate and the CRC check running at the same time. prepare_apk.py
 * deflates libyoyo.so (80 MB -> 26 MB) and game.droid (75 MB -> ~51 MB). The memory card reads ~11 MB/s however the reads
 * are sized or spread over threads, so the stored 80 MB runner took ~6.3 s to read; minizip's own path (16 KB reads,
 * inflate and CRC one after the other on one thread) is slower still.
 *
 * A worker reads the compressed bytes in 1 MB blocks into an 8 MB ring, the calling thread inflates whatever has arrived,
 * and a second worker runs the CRC-32 over what has been inflated; the result must match the entry's CRC and size, as
 * minizip's unzCloseCurrentFile checks. Used for the runner (main.c) and by the apk prefetcher (apk_prefetch.c).
 */

#include <vitasdk.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define BLOCK (1u << 20)
#define RING 8
#define READ_PIECE (256u << 10)

typedef struct {
	const char *path;
	uint64_t offset;
	uint32_t comp_size, size;
	uint8_t *ring, *out;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	uint32_t read_done;  // compressed bytes read
	uint32_t consumed;   // compressed bytes inflated: blocks before consumed / BLOCK are free again
	uint32_t inflated;   // output bytes ready for the CRC
	int read_failed, inflate_over;
	uint32_t crc;
	int cpu_mask;
} inflate_job;

// Both workers are pthreads: pthread-embedded's lock and condition variable need the waiting thread to be one of its own
static void *reader_thread(void *arg) {
	inflate_job *j = arg;
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), j->cpu_mask);
	extern void boot_watch_thread(const char *label, void *start);
	boot_watch_thread("inflate reader", reader_thread);
	SceUID fd = sceIoOpen(j->path, SCE_O_RDONLY, 0);
	int ok = fd >= 0 && sceIoLseek(fd, j->offset, SCE_SEEK_SET) == (SceOff)j->offset;
	uint32_t done = 0;
	while (ok && done < j->comp_size) {
		uint32_t block = done / BLOCK;
		pthread_mutex_lock(&j->lock);
		while (block >= j->consumed / BLOCK + RING && !j->inflate_over)
			pthread_cond_wait(&j->cond, &j->lock);
		int over = j->inflate_over;
		pthread_mutex_unlock(&j->lock);
		if (over)
			break;
		uint32_t n = j->comp_size - done < BLOCK ? j->comp_size - done : BLOCK;
		uint8_t *dst = j->ring + (block % RING) * BLOCK;
		uint32_t got = 0;
		while (got < n) {
			// 256 KB requests: no slower, and the game thread's small file accesses don't wait behind 1 MB reads
			// (vglInit's folders, asyncsave's saves; game.droid is inflated while the loader sets up)
			int r = sceIoRead(fd, dst + got, n - got < READ_PIECE ? n - got : READ_PIECE);
			if (r <= 0)
				break;
			got += r;
		}
		if (got != n) {
			ok = 0;
			break;
		}
		done += n;
		pthread_mutex_lock(&j->lock);
		j->read_done = done;
		pthread_cond_broadcast(&j->cond);
		pthread_mutex_unlock(&j->lock);
	}
	if (fd >= 0)
		sceIoClose(fd);
	pthread_mutex_lock(&j->lock);
	j->read_failed = !ok;
	pthread_cond_broadcast(&j->cond);
	pthread_mutex_unlock(&j->lock);
	return NULL;
}

static void *crc_thread(void *arg) {
	inflate_job *j = arg;
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), j->cpu_mask);
	uLong crc = crc32(0, Z_NULL, 0);
	uint32_t done = 0;
	for (;;) {
		pthread_mutex_lock(&j->lock);
		while (j->inflated == done && !j->inflate_over)
			pthread_cond_wait(&j->cond, &j->lock);
		uint32_t upto = j->inflated;
		int over = j->inflate_over;
		pthread_mutex_unlock(&j->lock);
		if (upto > done) {
			crc = crc32(crc, j->out + done, upto - done);
			done = upto;
		} else if (over) {
			break;
		}
	}
	j->crc = (uint32_t)crc;
	return NULL;
}

// Inflates the raw deflate stream of comp_size bytes at offset in path into out (size bytes). Returns 0 when the stream
// ends exactly at size bytes with the expected CRC-32. cpu_mask: cores for the two workers.
int runner_inflate(const char *path, uint64_t offset, uint32_t comp_size, uint32_t size, uint32_t expected_crc,
	uint8_t *out, int cpu_mask) {
	inflate_job job;
	memset(&job, 0, sizeof(job));
	job.path = path;
	job.offset = offset;
	job.comp_size = comp_size;
	job.size = size;
	job.out = out;
	job.cpu_mask = cpu_mask;
	job.ring = malloc(RING * BLOCK);
	if (!job.ring || !comp_size) {
		free(job.ring);
		return -1;
	}
	pthread_mutex_init(&job.lock, NULL);
	pthread_cond_init(&job.cond, NULL);
	pthread_t reader, crc;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	int started_reader = !pthread_create(&reader, &attr, reader_thread, &job);
	int started_crc = started_reader && !pthread_create(&crc, &attr, crc_thread, &job);
	pthread_attr_destroy(&attr);
	int res = -1;
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	int zr = started_crc ? inflateInit2(&zs, -MAX_WBITS) : Z_MEM_ERROR;
	int inited = zr == Z_OK;
	zs.next_out = out;
	zs.avail_out = size;
	uint32_t fed = 0;
	while (zr == Z_OK) {
		pthread_mutex_lock(&job.lock);
		while (job.read_done == fed && !job.read_failed && fed < comp_size)
			pthread_cond_wait(&job.cond, &job.lock);
		uint32_t avail = job.read_done;
		pthread_mutex_unlock(&job.lock);
		if (avail == fed) {
			zr = Z_DATA_ERROR; // input ended (or the read failed) before the stream did
			break;
		}
		// Within one ring block at a time
		uint32_t block_end = (fed / BLOCK + 1) * BLOCK;
		if (avail > block_end)
			avail = block_end;
		zs.next_in = job.ring + (fed / BLOCK % RING) * BLOCK + fed % BLOCK;
		zs.avail_in = avail - fed;
		zr = inflate(&zs, Z_NO_FLUSH);
		fed = avail - zs.avail_in;
		pthread_mutex_lock(&job.lock);
		job.consumed = fed;
		job.inflated = size - zs.avail_out;
		pthread_cond_broadcast(&job.cond);
		pthread_mutex_unlock(&job.lock);
		if (zr == Z_BUF_ERROR && zs.avail_out)
			zr = Z_OK; // needed more input
	}
	uint32_t total = zs.total_out;
	if (inited)
		inflateEnd(&zs);
	pthread_mutex_lock(&job.lock);
	job.inflate_over = 1;
	pthread_cond_broadcast(&job.cond);
	pthread_mutex_unlock(&job.lock);
	if (started_crc) {
		pthread_join(crc, NULL);
		res = zr == Z_STREAM_END && total == size && job.crc == expected_crc ? 0 : -1;
	}
	if (started_reader)
		pthread_join(reader, NULL);
	pthread_cond_destroy(&job.cond);
	pthread_mutex_destroy(&job.lock);
	free(job.ring);
	return res;
}
