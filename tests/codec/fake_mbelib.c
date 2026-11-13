// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* Two stand-ins for the user-installed mbelib, no codec in either (tests/codec/test_mbe_abi.cpp). FAKE_NEO: the frame function has
 * mbelib-neo's arguments (short*, mbe_process_result*, fr, d, cur, prev, enh) and the library exports mbe_versionString; otherwise
 * mbelib 1.3's (short*, int*, int*, char*, fr, d, cur, prev, enh, int). Each checks that it was called with its own layout: the frame
 * bits it gets must be the test pattern, else it writes a marker that the test sees as a wrong call. */
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

EXPORT void mbe_initMbeParms(void* cur, void* prev, void* enh) {
    memset(cur, 0, 64); memset(prev, 0, 64); memset(enh, 0, 64);
}

#ifdef FAKE_NEO
typedef struct { int c0, prot, c4, total; unsigned flags; } result_t;
EXPORT const char* mbe_versionString(void) { return "2.6.2-fake"; }
EXPORT int mbe_processAmbe3600x2450Frame(short* out, result_t* r, const char fr[4][24], char d[49], void* cur, void* prev, void* enh) {
    (void)d; (void)cur; (void)prev; (void)enh;
    if (r) { r->c0 = 0; r->prot = 0; r->c4 = 0; r->total = 0; r->flags = 0; }
    const short v = (fr[0][0] == 1 && fr[3][23] == 1) ? 2000 : -1;   /* the frame bits where neo has them */
    for (int i = 0; i < 160; i++) out[i] = v;
    return 0;
}
#else
EXPORT void mbe_processAmbe3600x2450Frame(short* out, int* e1, int* e2, char* errStr, char fr[4][24], char d[49], void* cur, void* prev, void* enh, int uvq) {
    (void)d; (void)cur; (void)prev; (void)enh;
    *e1 = 0; *e2 = 0; errStr[0] = 0;
    const short v = (fr[0][0] == 1 && fr[3][23] == 1 && uvq == 3) ? 1000 : -1;
    for (int i = 0; i < 160; i++) out[i] = v;
}
#endif
