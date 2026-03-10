/* config.h -- Build configuration for TaDOOM (PrBoom on Tanmatsu/ESP32-P4) */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#define PACKAGE "tanmatsu-tadoom"
#define VERSION "0.1.0"
#define PACKAGE_NAME "TaDOOM"
#define PACKAGE_STRING "TaDOOM 0.1.0"
#define PACKAGE_TARNAME "tanmatsu-tadoom"
#define PACKAGE_VERSION "0.1.0"

/* Base path on SD card for all game data */
#define TADOOM_BASE_PATH "/sd/apps/at.cavac.tadoom"
#define DOOMWADDIR TADOOM_BASE_PATH

/* Available C library features */
#define HAVE_UNISTD_H 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_STRLWR 1

/* Disable features not available on ESP32 */
/* #undef GL_DOOM */
/* #undef USE_SDL */
/* #undef HAVE_MIXER */
/* #undef HAVE_LIBSDL_MIXER */
/* #undef USE_SDL_NET */
/* #undef HAVE_NET */
/* #undef HAVE_LIBSDL_NET */
/* #undef I386 */
/* #undef I386_ASM */
/* #undef HAVE_SCHED_H */
/* #undef HAVE_SCHED_SETAFFINITY */
/* #undef SECURE_UID */
/* #undef SYS_SIGLIST_DECLARED */

/* We handle music ourselves via TinySoundFont */
#define HAVE_OWN_MUSIC 1

/* Packed structures for binary data reading */
#define PACKEDATTR __attribute__((packed))

/* String compatibility */
#define stricmp strcasecmp
#define strnicmp strncasecmp

/* Disable double-buffering (we blit directly) */
#define DISABLE_DOUBLEBUFFER 1

#endif /* __CONFIG_H__ */
