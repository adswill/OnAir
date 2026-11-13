// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A stand-in for a user-installed mbelib, for the tests of the runtime loader only: the same five-symbol surface OnAir looks up,
// no codec. Every frame "decodes" to 160 samples of a fixed ramp.
#ifdef _WIN32
#define FAKE_API __declspec(dllexport)
#else
#define FAKE_API __attribute__((visibility("default")))
#endif
FAKE_API void mbe_initMbeParms(void* cur, void* prev, void* prevEnh) { (void)cur; (void)prev; (void)prevEnh; }
static void ramp(short* aout) { for (int i = 0; i < 160; i++) aout[i] = (short)(i * 37 + 1); }
FAKE_API void mbe_processImbe7200x4400Frame(short* aout, int* e, int* e2, char* s, char fr[8][23], char d[88], void* c, void* p, void* pe, int u)
{ (void)e; (void)e2; (void)s; (void)fr; (void)d; (void)c; (void)p; (void)pe; (void)u; ramp(aout); }
FAKE_API void mbe_processAmbe3600x2400Frame(short* aout, int* e, int* e2, char* s, char fr[4][24], char d[49], void* c, void* p, void* pe, int u)
{ (void)e; (void)e2; (void)s; (void)fr; (void)d; (void)c; (void)p; (void)pe; (void)u; ramp(aout); }
FAKE_API void mbe_processAmbe3600x2450Frame(short* aout, int* e, int* e2, char* s, char fr[4][24], char d[49], void* c, void* p, void* pe, int u)
{ (void)e; (void)e2; (void)s; (void)fr; (void)d; (void)c; (void)p; (void)pe; (void)u; ramp(aout); }
