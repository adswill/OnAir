// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A stand-in for libac4dec's run-time entry points, for test_ac4: it decodes nothing. Every frame fed gives, `lookahead` calls later, one
// frame of 1536 samples of a 1 kHz tone at half scale (48 kHz), in as many channels as the layout option asks (2.0 -> 2, 5.1 -> 6,
// none -> 2), with the frame's index in channel 1 (to check the order). Options it does not know make it refuse, like the real one.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define STUB_EXPORT __declspec(dllexport)
#else
#define STUB_EXPORT __attribute__((visibility("default")))
#endif

typedef struct {
    int lookahead, channels;
    long fed, out;
} stub;

STUB_EXPORT const char* ac4dec_version(void) { return "stub"; }

STUB_EXPORT void* ac4dec_open_options(const char* options, char* err, size_t errlen) {
    stub* s = (stub*)calloc(1, sizeof(stub));
    const char* p = options ? options : "";
    if (!s) return NULL;
    s->lookahead = 180;
    s->channels = 2;
    while (*p) {
        char tok[64];
        size_t n = 0;
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        while (*p && *p != ' ' && *p != ',' && n < sizeof(tok) - 1) tok[n++] = *p++;
        tok[n] = 0;
        if (!strncmp(tok, "lookahead=", 10)) s->lookahead = atoi(tok + 10);
        else if (!strcmp(tok, "layout=5.1")) s->channels = 6;
        else if (!strcmp(tok, "layout=2.0")) s->channels = 2;
        else if (strncmp(tok, "loudness=", 9) && strncmp(tok, "target_level=", 13) && strncmp(tok, "drc=", 4)) {
            if (err && errlen) snprintf(err, errlen, "unknown option or bad value: %s", tok);
            free(s);
            return NULL;
        }
    }
    if (s->lookahead < 8) {
        if (err && errlen) snprintf(err, errlen, "lookahead below 8");
        free(s);
        return NULL;
    }
    return s;
}

STUB_EXPORT void ac4dec_close(void* d) { free(d); }

STUB_EXPORT int ac4dec_delay(const void* d) { return ((const stub*)d)->lookahead; }

static int give(stub* s, float* out, int max_samples, int* channels, int* sample_rate, int* status) {
    const int n = max_samples < 1536 ? max_samples : 1536;
    for (int i = 0; i < n; i++) {
        const long t = s->out * 1536 + i;
        const float v = 0.5f * (float)sin(2.0 * 3.14159265358979323846 * 1000.0 * (double)t / 48000.0);
        for (int c = 0; c < s->channels; c++) out[(size_t)i * s->channels + c] = c == 1 ? (float)s->out / 1000.f : v;
    }
    s->out++;
    if (channels) *channels = s->channels;
    if (sample_rate) *sample_rate = 48000;
    if (status) *status = 0;
    return n;
}

STUB_EXPORT int ac4dec_decode_interleaved(void* d, const unsigned char* data, size_t len, float* out, int max_samples, int* channels,
                                          int* sample_rate, int* status) {
    stub* s = (stub*)d;
    (void)data; (void)len;
    if (!s || !out || max_samples <= 0) return -1;
    s->fed++;
    if (s->fed <= s->lookahead) return 0;
    return give(s, out, max_samples, channels, sample_rate, status);
}

STUB_EXPORT int ac4dec_flush_interleaved(void* d, float* out, int max_samples, int* channels, int* sample_rate, int* status) {
    stub* s = (stub*)d;
    if (!s || !out || max_samples <= 0 || s->out >= s->fed) return 0;
    return give(s, out, max_samples, channels, sample_rate, status);
}
