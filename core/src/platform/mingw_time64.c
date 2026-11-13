// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* Windows (MinGW-w64) cross build only: the static FFmpeg of the Windows prefix was built against a newer MinGW runtime that names two calls
 * with a 64 suffix; the CI laptop's older runtime lacks them. Compiled straight into every executable (see the end of CMakeLists.txt), weak so
 * that a runtime that has them wins. Written against the Win32 API only, so it compiles with old and new MinGW headers alike. */
#include <windows.h>

struct onair_ts64 { long long tv_sec; long tv_nsec; };

int clock_gettime64(int clk, struct onair_ts64* t) {
    if (!t) return -1;
    if (clk == 1) {   /* CLOCK_MONOTONIC */
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        t->tv_sec = c.QuadPart / f.QuadPart;
        t->tv_nsec = (long)((c.QuadPart % f.QuadPart) * 1000000000LL / f.QuadPart);
    } else {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        unsigned long long u = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        u -= 116444736000000000ULL;   /* 1601 -> 1970 */
        t->tv_sec = (long long)(u / 10000000ULL);
        t->tv_nsec = (long)((u % 10000000ULL) * 100);
    }
    return 0;
}

int nanosleep64(const struct onair_ts64* a, struct onair_ts64* b) {
    if (b) { b->tv_sec = 0; b->tv_nsec = 0; }
    if (!a) return -1;
    unsigned long long ms = (unsigned long long)a->tv_sec * 1000ULL + ((unsigned long long)a->tv_nsec + 999999ULL) / 1000000ULL;
    Sleep((DWORD)ms);
    return 0;
}
