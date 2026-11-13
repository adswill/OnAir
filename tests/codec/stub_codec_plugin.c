// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A stand-in speech codec plug-in for the tests of the plug-in loader and of the voice chains (no codec): the four exported functions of the
// ABI in core/include/dect2/codec/speech_codec.h. Every coded frame "decodes" to 160 samples: a fixed ramp whose first sample carries the
// first coded byte, so a test can see which frame produced which audio.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef _WIN32
#define STUB_API __declspec(dllexport)
#else
#define STUB_API __attribute__((visibility("default")))
#endif
STUB_API void* onair_speech_open(void) { return malloc(1); }
STUB_API void onair_speech_close(void* s) { free(s); }
STUB_API int onair_speech_sample_rate(void) { return 8000; }
STUB_API int onair_speech_decode(void* s, const uint8_t* coded, size_t n, int16_t* pcm, size_t max) {
    (void)s;
    if (!n || max < 160) return -1;
    for (int i = 0; i < 160; i++) pcm[i] = (int16_t)(i * 37 + 1);
    pcm[0] = (int16_t)(1000 + coded[0]);
    return 160;
}
