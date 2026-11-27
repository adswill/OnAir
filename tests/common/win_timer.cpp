// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Windows only, linked into every test program (CMakeLists.txt): 1 ms timer resolution for the whole run. Windows' default is 15.6 ms, so
// every sleep_for(1 ms) in a test's polling loop slept about 15 ms; the tests that poll an engine, a socket or a file ran 5-10 times
// longer than on macOS (time_rx 240 s against 19 s, native_kiwisdr 204 s against 22 s; docs/CI-FAST.md (4.7)). The app is not changed.
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>

namespace {
struct OneMsTimer {
    OneMsTimer() { timeBeginPeriod(1); }
    ~OneMsTimer() { timeEndPeriod(1); }
} g_oneMsTimer;
}  // namespace
#endif
