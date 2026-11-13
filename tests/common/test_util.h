// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared boilerplate for the stand-alone test programs: a failure counter and a CHECK macro that prints the
// failing line and a printf-style message. Include it once per test (it defines a file-local `fails`),
// call testSummary() or return `fails ? 1 : 0` at the end of main.
#pragma once
#include <cstdio>

static int fails = 0;
// CHECK(cond, "printf format", args...): counts and reports a failed condition with its source line.
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
// Prints "<name>: all passed" or "<name>: N FAILED" and returns the process exit code.
inline int testSummary(const char* name) {
    printf(fails ? "%s: %d FAILED\n" : "%s: all passed\n", name, fails);
    return fails ? 1 : 0;
}
