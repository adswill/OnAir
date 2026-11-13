// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* A fake speech codec plug-in for tests/codec/test_codec_guard.cpp: no codec in it. Built five times (FAKE_MODE_ok, _crash, _hang,
 * _garbage, _abi) as onair-codec-fake<mode>. It shows that a plug-in that crashes, never returns, reports nonsense or speaks another
 * interface version cannot take OnAir down. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define EXPORT __declspec(dllexport)
#else
#include <unistd.h>
#define EXPORT __attribute__((visibility("default")))
#endif

static int gState;

EXPORT void* onair_speech_open(void) { return &gState; }
EXPORT void onair_speech_close(void* s) { (void)s; }
EXPORT int onair_speech_sample_rate(void) {
#ifdef FAKE_MODE_garbage
    return -12345;   /* a rate no codec has */
#else
    return 8000;
#endif
}

#ifdef FAKE_MODE_abi
EXPORT int onair_speech_abi(void) { return 99; }
#else
EXPORT int onair_speech_abi(void) { return 1; }
#endif

EXPORT int onair_speech_decode(void* s, const uint8_t* coded, size_t n, int16_t* pcm, size_t max) {
    (void)s; (void)coded; (void)n;
#if defined(FAKE_MODE_crash)
    volatile int* p = (volatile int*)(uintptr_t)16;
    *p = 1;   /* a write to an address nobody owns */
    return 0;
#elif defined(FAKE_MODE_hang)
    for (;;) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
    }
#elif defined(FAKE_MODE_garbage)
    (void)max;
    pcm[0] = 1;
    return 1 << 30;   /* more samples than it was given room for */
#else
    for (size_t i = 0; i < 160 && i < max; i++) pcm[i] = (int16_t)i;
    return 160;
#endif
}
