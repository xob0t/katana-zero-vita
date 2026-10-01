#ifndef __CONFIG_H__
#define __CONFIG_H__

#define DEBUG

#if 0 // Devkit mode
#define LOAD_ADDRESS 0xA8000000
#else
#define LOAD_ADDRESS 0x98000000
#endif

#define MEMORY_VITAGL_THRESHOLD_MB 12

#define DATA_PATH "ux0:data/gms"
#define GLSL_PATH DATA_PATH "/shared/glsl"
#define LAUNCH_FILE_PATH DATA_PATH "/launch.txt"

// Render resolution; smaller sizes are scaled up by the Vita display hardware (renderRes in yyl.cfg)
extern int screen_w, screen_h;
#define SCREEN_W screen_w
#define SCREEN_H screen_h

#endif
