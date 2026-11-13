// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A stand-in for a user-installed HDC-patched FAAD2, for the tests of the runtime loader only: the same entry points OnAir looks up, no codec.
// Built four times from this file (FAKE_VARIANT, CMakeLists.txt):
//   0  a good library: any packet "decodes" to 2048 stereo frames at 44.1 kHz, L = first byte * 100, R = packet size * 10; the first packet after
//      NeAACDecInitHDC is marked by L + 7 (so a test can see that the decoder state was restarted); a packet that starts with 0xEE is a decode error
//   1  an unknown version string (3.0.1)
//   2  a plain FAAD2: no NeAACDecInitHDC
//   3  a different frame info layout: the answers land in the wrong fields
#ifdef _WIN32
#define FAKE_API __declspec(dllexport)
#else
#define FAKE_API __attribute__((visibility("default")))
#endif
#include <stdlib.h>
#include <string.h>

#ifndef FAKE_VARIANT
#define FAKE_VARIANT 0
#endif

typedef struct { unsigned long bytesconsumed, samples; unsigned char channels, error; unsigned long samplerate; unsigned char tail[200]; } Info;
typedef struct { int fresh; short pcm[4096]; } Dec;

FAKE_API int NeAACDecGetVersion(const char** id, const char** cp) {
#if FAKE_VARIANT == 1
    *id = "3.0.1";
#else
    *id = "2.11.2";
#endif
    *cp = "fake";
    return 0;
}
FAKE_API const char* NeAACDecGetErrorMessage(unsigned char e) { return e ? "fake decode error" : "no error"; }
#if FAKE_VARIANT != 2
FAKE_API void NeAACDecInitHDC(void** h) { Dec* d = (Dec*)calloc(1, sizeof(Dec)); if (d) d->fresh = 1; *h = d; }
#endif
FAKE_API void NeAACDecClose(void* h) { free(h); }
FAKE_API void* NeAACDecDecode(void* h, Info* info, unsigned char* buf, unsigned long size) {
    Dec* d = (Dec*)h;
    memset(info, 0, sizeof(Info));
    info->bytesconsumed = size;
#if FAKE_VARIANT == 3
    info->samples = 0x7FFFFFFFul; info->channels = 200; info->error = 99; info->samplerate = 0x12345678ul;   // what a shifted layout looks like
    return 0;
#endif
    if (size <= 1) { info->error = 1; return 0; }
    if (buf[0] == 0xEE) { info->error = 20; return 0; }
    info->samples = 4096; info->channels = 2; info->samplerate = 44100; info->error = 0;
    for (int i = 0; i < 2048; i++) {
        d->pcm[2 * i] = (short)(buf[0] * 100 + (d->fresh ? 7 : 0));
        d->pcm[2 * i + 1] = (short)(size * 10);
    }
    d->fresh = 0;
    return d->pcm;
}
