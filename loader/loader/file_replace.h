#ifndef FILE_REPLACE_H
#define FILE_REPLACE_H

// SceIo encodes filesystem errno values in the low bits of the return code.
#define IO_NO_ENTRY ((int)0x80010002)
#define IO_EXISTS ((int)0x80010011)

static inline int remove_if_present(const char *path) {
	int res = sceIoRemove(path);
	return res == IO_NO_ENTRY ? 0 : res;
}

// Vita filesystems can refuse to rename over an existing file. Callers must
// serialize access to these paths and close the complete staged file first.
static inline int replace_file(const char *stage, const char *path, const char *backup) {
	int res = sceIoRename(stage, path);
	if (res == IO_EXISTS) {
		res = remove_if_present(backup);
		if (res >= 0)
			res = sceIoRename(path, backup);
		if (res < 0)
			return res;
		res = sceIoRename(stage, path);
		if (res < 0) {
			// If rollback also fails, leave the old file available for recovery.
			sceIoRename(backup, path);
			return res;
		}
	}
	if (res >= 0)
		remove_if_present(backup);
	return res;
}

#endif
