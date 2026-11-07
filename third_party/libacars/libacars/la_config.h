// Build configuration for the vendored libacars subset (OnAir): no zlib, libxml2 or jansson
#ifndef _CONFIG_H
#define _CONFIG_H
#if defined(__APPLE__) || defined(__linux__)
#define HAVE_STRSEP 1
#define HAVE_MEMMEM 1
#define HAVE_UNISTD_H 1
#endif
#define HAVE_SYS_TIME_H 1
#define LFIND_NMEMB_SIZE_SIZE_T 1
#endif
