/*
 * netflix.c
 *
 * Offline replacement for the NetflixIntegration Android extension (Nfxa* functions)
 * used by Netflix builds of GameMaker games (e.g. Katana ZERO).
 *
 * Cloud save slots are stored as plain files in the game data folder and every
 * asynchronous Netflix callback is answered immediately with a success result.
 */

#include <vitasdk.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "asyncsave.h"
#include "file_replace.h"
#include "main.h"
#include "so_util.h"

#define NETFLIX_EVENT_SOCIAL 70
#define NETFLIX_PLAYER_ID "vita"
#define NETFLIX_ACCESS_GAINED 1.0

extern char data_path[256];
extern double jni_double;
extern void recursive_mkdir(char *dir);

static int (*nfx_CreateDsMap)(int n, ...);
static void (*nfx_CreateAsynEventWithDSMap)(int dsMap, int event_type);

typedef struct {
	char *module_name;
	char *method_name;
	int argc;
	double *double_array;
	void **object_array;
} nfx_ext_func;

typedef struct {
	char save[512], backup[520], legacy[520], stage[520];
} nfx_slot_paths;

// Save I/O is infrequent. One lock also covers slot names that alias on FAT.
static pthread_mutex_t nfx_slot_lock = PTHREAD_MUTEX_INITIALIZER;

static void nfx_slot_dir(char *dir, size_t size) {
	snprintf(dir, size, "%snetflix/", data_path);
}

static int nfx_paths_of(nfx_slot_paths *paths, const char *save) {
	int len = snprintf(paths->save, sizeof(paths->save), "%s", save);
	if (len < 0 || (size_t)len >= sizeof(paths->save))
		return -1;
	snprintf(paths->backup, sizeof(paths->backup), "%s.bak", paths->save);
	snprintf(paths->legacy, sizeof(paths->legacy), "%s.tmp", paths->save);
	snprintf(paths->stage, sizeof(paths->stage), "%s.new", paths->save);
	return 0;
}

// The slot's file names. mkdir 0 doesn't touch the card (the async queue: the writer thread creates the folder).
static int nfx_slot_names(nfx_slot_paths *paths, const char *slot, int mkdir) {
	char dir[sizeof(data_path) + sizeof("netflix/")], save[sizeof(paths->save)];
	nfx_slot_dir(dir, sizeof(dir));
	if (mkdir)
		recursive_mkdir(dir);
	int len = snprintf(save, sizeof(save), "%s%s.sav", dir, slot);
	if (len < 0 || (size_t)len >= sizeof(save))
		return -1;
	return nfx_paths_of(paths, save);
}

static int nfx_slot_path(nfx_slot_paths *paths, const char *slot) {
	return nfx_slot_names(paths, slot, 1);
}

// CreateDsMap takes (key, double, string) triplets; a non-NULL string wins over the double
static void nfx_send_event(const char *type, const char *str_key, const char *str_val, const char *num_key, double num_val) {
	int map;
	if (str_key && num_key)
		map = nfx_CreateDsMap(3, "type", 0.0, type, str_key, 0.0, str_val, num_key, num_val, NULL);
	else if (str_key)
		map = nfx_CreateDsMap(2, "type", 0.0, type, str_key, 0.0, str_val);
	else if (num_key)
		map = nfx_CreateDsMap(2, "type", 0.0, type, num_key, num_val, NULL);
	else
		map = nfx_CreateDsMap(1, "type", 0.0, type);
	nfx_CreateAsynEventWithDSMap(map, NETFLIX_EVENT_SOCIAL);
}

static void *nfx_return_double(double val) {
	jni_double = val;
	return &jni_double;
}

// Return 1 for a missing file, or -1 for a read failure.
static int nfx_read_file(const char *path, char **data) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return errno == ENOENT ? 1 : -1;
	long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
	char *buf = size >= 0 && fseek(f, 0, SEEK_SET) == 0 ? malloc((size_t)size + 1) : NULL;
	if (buf && fread(buf, 1, size, f) != (size_t)size) {
		free(buf);
		buf = NULL;
	}
	if (fclose(f) != 0 || !buf) {
		free(buf);
		return -1;
	}
	buf[size] = 0;
	*data = buf;
	return 0;
}

static int nfx_read_slot(const char *slot, char **data) {
	nfx_slot_paths paths;
	pthread_mutex_lock(&nfx_slot_lock);
	int res = nfx_slot_path(&paths, slot);
	if (!res) {
		res = nfx_read_file(paths.save, data);
		if (res == 1) {
			// No .sav: a write stopped between its two renames. .sav only becomes .bak after the
			// complete .new was written, synced and closed, so with a .bak present .new is the newer
			// save. Without a .bak, .new may be incomplete and is never read.
			res = nfx_read_file(paths.backup, data);
			char *staged = NULL;
			if (res == 0 && nfx_read_file(paths.stage, &staged) == 0) {
				free(*data);
				*data = staged;
			}
		}
		// Older loaders can leave only .tmp
		if (res == 1)
			res = nfx_read_file(paths.legacy, data);
	}
	pthread_mutex_unlock(&nfx_slot_lock);
	return res < 0 ? -1 : 0;
}

static int nfx_commit_slot(const nfx_slot_paths *paths, const char *data, size_t len) {
	FILE *f = fopen(paths->stage, "wb");
	if (!f)
		return -1;
	int ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
	if (fclose(f) != 0)
		ok = 0;
	if (!ok || replace_file(paths->stage, paths->save, paths->backup) < 0) {
		remove_if_present(paths->stage);
		return -1;
	}
	remove_if_present(paths->legacy);
	return 0;
}

static int nfx_write_slot(const char *slot, const char *data) {
	nfx_slot_paths paths;
	pthread_mutex_lock(&nfx_slot_lock);
	int res = nfx_slot_path(&paths, slot);
	if (!res)
		res = nfx_commit_slot(&paths, data, strlen(data));
	pthread_mutex_unlock(&nfx_slot_lock);
	return res;
}

// Writer thread (asyncsave.c): commits a queued slot under the slot lock, as nfx_write_slot does
static int nfx_commit_queued(const char *save, const char *data, size_t len) {
	nfx_slot_paths paths;
	char dir[sizeof(data_path) + sizeof("netflix/")];
	nfx_slot_dir(dir, sizeof(dir));
	pthread_mutex_lock(&nfx_slot_lock);
	recursive_mkdir(dir);
	int res = nfx_paths_of(&paths, save);
	if (!res)
		res = nfx_commit_slot(&paths, data, len);
	pthread_mutex_unlock(&nfx_slot_lock);
	return res;
}

// Hands the write to the background writer. -1: it couldn't be queued, write it now.
static int nfx_queue_slot(const char *slot, const char *data) {
	nfx_slot_paths paths;
	if (!asyncsave_on || nfx_slot_names(&paths, slot, 0) < 0)
		return -1;
	size_t len = strlen(data);
	char *copy = malloc(len + 1);
	if (!copy)
		return -1;
	memcpy(copy, data, len + 1);
	if (asyncsave_queue(paths.save, copy, len, nfx_commit_queued) < 0) {
		free(copy);
		return -1;
	}
	return 0;
}

// Reads and deletes see the slot as the game last wrote it: they wait for its pending write
static void nfx_wait_slot(const char *slot) {
	nfx_slot_paths paths;
	if (nfx_slot_names(&paths, slot, 0) == 0)
		asyncsave_wait_path(paths.save);
}

static int nfx_delete_slot(const char *slot) {
	nfx_slot_paths paths;
	pthread_mutex_lock(&nfx_slot_lock);
	// Remove recovery files first so a partial delete cannot resurrect one.
	int ok = nfx_slot_path(&paths, slot) == 0 &&
		remove_if_present(paths.stage) >= 0 && remove_if_present(paths.legacy) >= 0 &&
		remove_if_present(paths.backup) >= 0 && remove_if_present(paths.save) >= 0;
	pthread_mutex_unlock(&nfx_slot_lock);
	return ok ? 0 : -1;
}

static const char *nfx_locale() {
	int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
	switch (lang) {
	case SCE_SYSTEM_PARAM_LANG_JAPANESE: return "ja";
	case SCE_SYSTEM_PARAM_LANG_FRENCH: return "fr";
	case SCE_SYSTEM_PARAM_LANG_SPANISH: return "es";
	case SCE_SYSTEM_PARAM_LANG_GERMAN: return "de";
	case SCE_SYSTEM_PARAM_LANG_ITALIAN: return "it";
	case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT:
	case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR: return "pt";
	case SCE_SYSTEM_PARAM_LANG_RUSSIAN: return "ru";
	case SCE_SYSTEM_PARAM_LANG_KOREAN: return "ko";
	case SCE_SYSTEM_PARAM_LANG_CHINESE_T: return "zh-Hant";
	case SCE_SYSTEM_PARAM_LANG_CHINESE_S: return "zh-Hans";
	case SCE_SYSTEM_PARAM_LANG_POLISH: return "pl";
	case SCE_SYSTEM_PARAM_LANG_TURKISH: return "tr";
	default: return "en";
	}
}

int netflix_is_extension(const char *module_name) {
	return !strcmp(module_name, "NetflixIntegration") || !strcmp(module_name, "NetflixIntegrationAndroid");
}

void *netflix_call(void *ext) {
	nfx_ext_func *f = (nfx_ext_func *)ext;
	const char *m = f->method_name;
	char **str_args = (char **)f->object_array;
	const char *arg0 = (str_args && f->argc > 0) ? str_args[0] : NULL;
	const char *arg1 = (str_args && f->argc > 1) ? str_args[1] : NULL;

	if (!strcmp(m, "NfxaInit")) {
		// The real SDK reports the signed-in profile right after init
		nfx_send_event("Nfx_onPlayerAccessChanged", "current", NETFLIX_PLAYER_ID, "eventType", NETFLIX_ACCESS_GAINED);
		return nfx_return_double(1.0);
	} else if (!strcmp(m, "NfxaCloudRead")) {
		if (!arg0)
			return nfx_return_double(0.0);
		char *data = NULL;
		nfx_wait_slot(arg0);
		int err = nfx_read_slot(arg0, &data);
		debugPrintf("Nfxa: read slot %s (%s)\n", arg0, err ? "failed" : data ? "found" : "empty");
		nfx_send_event("Nfx_cloudRead", data ? "string" : NULL, data, "error", (double)err);
		free(data);
		return nfx_return_double(1.0);
	} else if (!strcmp(m, "NfxaCloudWrite")) {
		if (!arg0 || !arg1)
			return nfx_return_double(0.0);
		// In the background (asyncsave.c): the game gets the answer a successful write gave. A write that fails
		// later is logged and retried once; the next checkpoint writes the whole save again.
		int queued = nfx_queue_slot(arg0, arg1) == 0, err = 0;
		if (!queued) {
			extern void hang_io_begin(void), hang_io_end(void);
			hang_io_begin(); // save I/O: no freeze-locator dump meanwhile
			err = nfx_write_slot(arg0, arg1);
			hang_io_end();
		}
		debugPrintf("Nfxa: write slot %s (%u bytes) = %d%s\n", arg0, (unsigned)strlen(arg1), err,
			queued ? " (queued)" : ""); // the Vita printf has no %zu
		nfx_send_event("Nfx_cloudWrite", NULL, NULL, "error", (double)err);
		return nfx_return_double(1.0);
	} else if (!strcmp(m, "NfxaCloudDelete")) {
		if (!arg0)
			return nfx_return_double(0.0);
		nfx_wait_slot(arg0);
		int err = nfx_delete_slot(arg0);
		debugPrintf("Nfxa: delete slot %s = %d\n", arg0, err);
		nfx_send_event("Nfx_cloudDelete", NULL, NULL, "error", (double)err);
		return nfx_return_double(1.0);
	} else if (!strcmp(m, "NfxaCloudResolve")) {
		nfx_send_event("Nfx_cloudResolve", NULL, NULL, "error", 0.0);
		return nfx_return_double(1.0);
	} else if (!strcmp(m, "NfxaCloudConflictExists")) {
		return nfx_return_double(0.0);
	} else if (!strcmp(m, "NfxaCloudConflictGetLocal") || !strcmp(m, "NfxaCloudConflictGetRemote")) {
		return "";
	} else if (!strcmp(m, "NfxaGetPlayerID")) {
		return NETFLIX_PLAYER_ID;
	} else if (!strcmp(m, "NfxaGetGameHandle")) {
		return "Vita";
	} else if (!strcmp(m, "NfxaGetLocale")) {
		return (void *)nfx_locale();
	} else if (!strcmp(m, "NfxaShowUI")) {
		nfx_send_event("Nfx_onPlayerAccessRequest", "playerID", NETFLIX_PLAYER_ID, NULL, 0.0);
		return NULL;
	} else if (!strcmp(m, "NfxaSubmitScoreNow")) {
		nfx_send_event("Nfx_submitScoreNow", NULL, NULL, "error", -1.0);
		return NULL;
	} else if (!strcmp(m, "NfxaGetAggregatedStat")) {
		nfx_send_event("Nfx_getAggregatedStat", NULL, NULL, "error", -1.0);
		return NULL;
	} else if (!strncmp(m, "NfxaLeaderboard", 15)) {
		// No online services: report an error so any waiting UI gives up
		const char *type = "Nfx_leaderboardInfo";
		if (strstr(m, "CurrentPlayerEntry"))
			type = "Nfx_leaderboardCurrentPlayerEntry";
		else if (strstr(m, "AroundCurrentPlayer"))
			type = "Nfx_leaderboardEntriesAroundCurrentPlayer";
		else if (strstr(m, "Entries"))
			type = "Nfx_leaderboardTopEntries";
		nfx_send_event(type, NULL, NULL, "error", -1.0);
		return NULL;
	}

	// Menus, language, telemetry and crash reporting have no effect offline
	return NULL;
}

void patch_netflix() {
	nfx_CreateDsMap = (void *)so_symbol(&yoyoloader_mod, "_Z11CreateDsMapiz");
	nfx_CreateAsynEventWithDSMap = (void *)so_symbol(&yoyoloader_mod, "_Z24CreateAsynEventWithDSMapii");
}
