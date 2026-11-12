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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static char *nfx_slot_path(const char *slot) {
	static char path[512];
	char dir[512];
	sprintf(dir, "%snetflix/", data_path);
	recursive_mkdir(dir);
	sprintf(path, "%s%s.sav", dir, slot);
	return path;
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

static char *nfx_read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = size >= 0 ? malloc(size + 1) : NULL;
	if (buf && fread(buf, 1, size, f) != (size_t)size) {
		free(buf);
		buf = NULL;
	}
	if (buf)
		buf[size] = 0;
	fclose(f);
	return buf;
}

static char *nfx_read_slot(const char *slot) {
	char *path = nfx_slot_path(slot);
	char *data = nfx_read_file(path);
	if (!data) {
		// A crash between nfx_write_slot's remove and rename leaves only the new save in the temp file
		char tmp[520];
		snprintf(tmp, sizeof(tmp), "%s.tmp", path);
		data = nfx_read_file(tmp);
	}
	return data;
}

static int nfx_write_slot(const char *slot, const char *data) {
	// Write to a temp file first so a crash mid-save can't corrupt the existing slot
	char tmp[512];
	char *path = nfx_slot_path(slot);
	sprintf(tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return -1;
	size_t len = strlen(data);
	int ok = fwrite(data, 1, len, f) == len;
	fclose(f);
	if (!ok)
		return -1;
	sceIoRemove(path);
	return sceIoRename(tmp, path) < 0 ? -1 : 0;
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
		char *data = nfx_read_slot(arg0);
		debugPrintf("Nfxa: read slot %s (%s)\n", arg0, data ? "found" : "empty");
		nfx_send_event("Nfx_cloudRead", data ? "string" : NULL, data, "error", 0.0);
		free(data);
		return nfx_return_double(1.0);
	} else if (!strcmp(m, "NfxaCloudWrite")) {
		if (!arg0 || !arg1)
			return nfx_return_double(0.0);
		int err = nfx_write_slot(arg0, arg1);
		debugPrintf("Nfxa: write slot %s (%d bytes) = %d\n", arg0, strlen(arg1), err);
		nfx_send_event("Nfx_cloudWrite", NULL, NULL, "error", (double)err);
		return nfx_return_double(1.0);
	} else if (!strcmp(m, "NfxaCloudDelete")) {
		if (!arg0)
			return nfx_return_double(0.0);
		sceIoRemove(nfx_slot_path(arg0));
		nfx_send_event("Nfx_cloudDelete", NULL, NULL, "error", 0.0);
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
