// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A fake TETRAPOL speech codec plug-in for the voice-chain test (tests/tetrapol/test_tetrapol_voice.cpp): no codec. It exports the ABI of
// core/include/dect2/codec/speech_codec.h, accepts only the 16-byte TETRAPOL plug-in frame (tetrapol/tetrapol_voice.h) and answers every
// frame with 20 ms of a 1 kHz tone at 8 kHz; a frame flagged damaged gives a quieter tone, so a test can tell them apart.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef _WIN32
#define TONE_API __declspec(dllexport)
#else
#define TONE_API __attribute__((visibility("default")))
#endif

static const int16_t kSine8[8] = {0, 5657, 8000, 5657, 0, -5657, -8000, -5657};   // 1 kHz at 8 kHz

TONE_API void* onair_speech_open(void) { return malloc(1); }
TONE_API void onair_speech_close(void* s) { free(s); }
TONE_API int onair_speech_sample_rate(void) { return 8000; }
TONE_API int onair_speech_decode(void* s, const uint8_t* coded, size_t n, int16_t* pcm, size_t max) {
    (void)s;
    if (n != 16 || max < 160 || (coded[15] & 0xfe)) return -1;
    const int div = (coded[15] & 1) ? 4 : 1;
    for (int i = 0; i < 160; i++) pcm[i] = (int16_t)(kSine8[i % 8] / div);
    return 160;
}
