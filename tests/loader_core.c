/*
 * From the repo root, with host GCC:
 * gcc -D__BEGIN_DECLS= -D__END_DECLS= -Iloader/loader -pthread tests/loader_core.c \
 *     loader/loader/so_elf.c loader/loader/stdio_mt.c -o loader-core-test
 * Run with the path to libyoyo.so from the ARMv7 Katana ZERO 1.1.10 APK.
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "so_elf.h"
#include "yyl_config.h"

// stdio_mt.c's mt_fclose reports closes to the freeze locator (hang_watch.c), which isn't part of this host test
void hang_io_file_close(void *file) { (void)file; }
// mt_fread asks the startup prefetcher (apk_prefetch.c) first; here nothing is prefetched
long apk_prefetch_serve(uint64_t pos, void *buf, size_t n) { (void)pos; (void)buf; (void)n; return 0; }
int apk_prefetch_wait(uint64_t pos, size_t n) { (void)pos; (void)n; return 0; }

int mt_register_apk(FILE *f);
int mt_fseek(FILE *f, long offset, int whence);
long mt_ftell(FILE *f);
size_t mt_fread(void *buf, size_t size, size_t count, FILE *f);
int mt_fclose(FILE *f);

static int sceIoRemove(const char *path);
static int sceIoRename(const char *from, const char *to);

#include "file_replace.h"

static int files[3]; // stage, save, backup
static int fail_install, fail_rollback;

static int sceIoRemove(const char *path) {
	int *file = &files[*path - '0'];
	if (!*file)
		return IO_NO_ENTRY;
	*file = 0;
	return 0;
}

static int sceIoRename(const char *from, const char *to) {
	int *src = &files[*from - '0'], *dst = &files[*to - '0'];
	if (!*src)
		return IO_NO_ENTRY;
	if (*dst)
		return IO_EXISTS;
	if ((*from == '0' && *to == '1' && fail_install) ||
		(*from == '2' && *to == '1' && fail_rollback))
		return -1;
	*dst = *src;
	*src = 0;
	return 0;
}

static void test_replace(void) {
	files[0] = 2;
	files[1] = 1;
	assert(replace_file("0", "1", "2") == 0);
	assert(!files[0] && files[1] == 2 && !files[2]);

	files[0] = 3;
	fail_install = 1;
	assert(replace_file("0", "1", "2") < 0);
	assert(files[0] == 3 && files[1] == 2 && !files[2]);

	fail_rollback = 1;
	assert(replace_file("0", "1", "2") < 0);
	assert(files[0] == 3 && !files[1] && files[2] == 2);

	fail_install = fail_rollback = 0;
	assert(replace_file("0", "1", "2") == 0);
	assert(!files[0] && files[1] == 3 && !files[2]);
}

static void test_config(void) {
	FILE *f = tmpfile();
	assert(f);
	fputs("# comment\n platTarget = 2\r\n"
	      "platTarget=1oops\nplatTarget=\nplatTarget=2 1\n"
	      "platTarget=99999999999999999999999\nplatTarget=2147483648\n"
	      "platTarget=-2147483649\n", f);
	for (int i = 0; i < 128; i++)
		fputc(' ', f);
	fputs("platTarget=1\nend = -2147483648", f);
	rewind(f);
	char key[YYL_KEY_SIZE];
	int value;
	assert(yyl_read_setting(f, key, &value) && !strcmp(key, "platTarget") && value == 2);
	assert(yyl_read_setting(f, key, &value) && !strcmp(key, "end") && value == INT_MIN);
	assert(!yyl_read_setting(f, key, &value));
	fclose(f);
}

static void *read_apk_start(void *arg) {
	FILE *f = arg;
	char byte;
	assert(mt_fseek(f, 0, SEEK_SET) == 0);
	assert(mt_fread(&byte, 1, 1, f) == 1 && byte == 'a');
	return NULL;
}

static void test_stdio(void) {
	FILE *f = tmpfile();
	assert(f);
	assert(mt_fseek(f, 0, SEEK_SET) == 0);
	assert(fwrite("abc", 1, 3, f) == 3);
	assert(mt_ftell(f) == 3);
	assert(mt_register_apk(f) == 0);
	assert(mt_fseek(f, 2, SEEK_SET) == 0);
	pthread_t worker;
	assert(pthread_create(&worker, NULL, read_apk_start, f) == 0);
	assert(pthread_join(worker, NULL) == 0);
	assert(mt_ftell(f) == 2);
	char byte;
	assert(mt_fread(&byte, 1, 1, f) == 1 && byte == 'c');
	assert(mt_fclose(f) == 0);
}

static int validate(void *buffer, size_t size, so_elf *elf) {
	return so_elf_validate(buffer, size, 0x99000000, elf);
}

static void test_elf(const char *path) {
	FILE *f = fopen(path, "rb");
	assert(f && fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	assert(size > 0 && fseek(f, 0, SEEK_SET) == 0);
	unsigned char *data = malloc(size);
	assert(data && fread(data, 1, size, f) == (size_t)size);
	fclose(f);
	so_elf elf;
	assert(validate(data, size, &elf));
	Elf32_Ehdr *h = (void *)data;
	Elf32_Shdr *s = (void *)(data + h->e_shoff);
	const size_t cuts[] = {
		0, sizeof(*h) - 1, h->e_phoff + h->e_phnum * sizeof(Elf32_Phdr) - 1,
		h->e_shoff + h->e_shnum * sizeof(Elf32_Shdr) - 1,
		s[h->e_shstrndx].sh_offset + s[h->e_shstrndx].sh_size - 1,
	};
	uint32_t *gnu_hash = (void *)(data + elf.tables[SO_GNU_HASH]->sh_offset);
	Elf32_Rel *rel = (void *)(data + elf.tables[SO_RELDYN]->sh_offset);
	Elf32_Dyn *dyn = (void *)(data + elf.tables[SO_DYNAMIC]->sh_offset);
	size_t dyn_count = elf.dynamic_count;
	for (size_t i = 0; i < sizeof(cuts) / sizeof(*cuts); i++)
		assert(!validate(data, cuts[i], &elf));

	uint32_t saved = gnu_hash[2];
	gnu_hash[2] = UINT32_MAX;
	assert(!validate(data, size, &elf));
	gnu_hash[2] = saved;

	saved = rel->r_offset;
	rel->r_offset = UINT32_MAX - 3;
	assert(!validate(data, size, &elf));
	rel->r_offset = 4; // mapped but read-only
	assert(!validate(data, size, &elf));
	rel->r_offset = saved;

	saved = rel->r_info;
	rel->r_info = ELF32_R_INFO(0xffffff, ELF32_R_TYPE(saved));
	assert(!validate(data, size, &elf));
	rel->r_info = saved;

	for (size_t i = 0; i < dyn_count; i++) {
		if (dyn[i].d_tag == DT_REL) {
			saved = dyn[i].d_un.d_ptr;
			dyn[i].d_un.d_ptr = UINT32_MAX;
			assert(!validate(data, size, &elf));
			dyn[i].d_un.d_ptr = saved;
			break;
		}
	}
	assert(validate(data, size, &elf));
	free(data);
}

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: %s <Katana ZERO 1.1.10 ARMv7 libyoyo.so>\n", argv[0]);
		return 2;
	}
	test_replace();
	test_config();
	test_stdio();
	test_elf(argv[1]);
	puts("Loader core tests passed");
	return 0;
}
