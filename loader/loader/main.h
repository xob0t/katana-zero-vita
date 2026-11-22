#ifndef __MAIN_H__
#define __MAIN_H__

#include <psp2/touch.h>
#include "config.h"
#include "so_util.h"

extern so_module yoyoloader_mod;

#if 0
#define debugPrintf sceClibPrintf
#else
int debugPrintf(char *text, ...);
void debug_flush();
#endif

int ret0();

// Debug request files in ux0:data/gms/shared, polled off the game thread (screenshot.c)
enum { DEBUG_REQUEST_SCREENSHOT, DEBUG_REQUEST_GOTO_ROOM, DEBUG_REQUEST_LEVEL_COMPLETE, DEBUG_REQUEST_CREATE_OBJECT,
	DEBUG_REQUEST_DESTROY_OBJECT, DEBUG_REQUEST_COUNT };
int debug_request_take(int id, char *text, int size);

int sceKernelChangeThreadCpuAffinityMask(SceUID thid, int cpuAffinityMask);

extern SceTouchPanelInfo panelInfoFront, panelInfoBack;

typedef enum retval_type {
	VALUE_REAL = 0,
	VALUE_STRING = 1,
	VALUE_ARRAY = 2,
	VALUE_PTR = 3,
	VALUE_VEC3 = 4,
	VALUE_UNDEFINED = 5,
	VALUE_OBJECT = 6,
	VALUE_INT32 = 7,
	VALUE_VEC4 = 8,
	VALUE_MATRIX = 9,
	VALUE_INT64 = 10,
	VALUE_ACCESSOR = 11,
	VALUE_JSNULL = 12,
	VALUE_BOOL = 13,
	VALUE_ITERATOR = 14,
	VALUE_REF = 15,
	VALUE_UNSET = 0x0ffffff
} retval_type;

typedef struct ref_t {
	void *m_thing;
	int m_refCount;
	int m_size;
} ref_t;

typedef struct retval_t {
	union {
		int v32;
		long long v64;
		double val;
		ref_t *str;
	} rvalue;

	int flags;
	retval_type kind;
} retval_t;

enum {
	TOUCH_DOWN,
	TOUCH_UP,
	TOUCH_MOVE
};

#endif
