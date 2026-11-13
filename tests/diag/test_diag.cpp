// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Diagnostic codes (dect2/common/diag.h, docs/DIAGNOSTICS.md): the number rules at compile time, the "[E######] message" text, the route to
// the process log, the debug switch (environment, verbose logging, setDebug), that a debug diagnostic evaluates nothing while it is off and
// costs a few nanoseconds, and the rate limit. The lint rules themselves are checked on tests/diag/fixtures by tools/lint/diag_check.py
// --self-test (the quick gate).
#include "dect2/common/diag.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// the number rules, at compile time
static_assert(diag::validCode(482917) && diag::validCode(613587) && diag::validCode(100001), "valid codes");
static_assert(!diag::validCode(99999) && !diag::validCode(1000001), "six digits");
static_assert(!diag::validCode(482910), "last digit not 0");
static_assert(!diag::validCode(131071) && !diag::validCode(131072) && !diag::validCode(131073) && !diag::validCode(262143) && !diag::validCode(524289),
              "not within 1 of a power of two");
static_assert(!diag::validCode(131137), "more than 3 bits set");
static_assert(!diag::validCode(777777) && !diag::validCode(999999), "not all digits the same");

static std::vector<std::string> lines;                       // what reached the process log
static std::vector<diag::Level> levels;                      // the level of each diagnostic emitted
static void testSink(diag::Level l, const std::string&) { levels.push_back(l); }
static int evaluated = 0;
static int touch() { return ++evaluated; }

// one call site each (a code belongs to exactly one place)
static void debugSite(int v) { DIAG_DEBUG(424092, "The debug line of the diagnostics test carries value %d, so it shows only while debug is on.", v); }
static void debugEverySite(int v) { DIAG_DEBUG_EVERY(477947, 3600.0, "The rate-limited debug line of the diagnostics test carries value %d and shows once an hour.", v); }
static void warnEverySite(int v) { DIAG_EVERY(178948, 3600.0, "The rate-limited warning of the diagnostics test carries value %d and shows once an hour.", v); }

int main() {
    // the environment switch is read on first use
#ifdef _WIN32
    _putenv_s("ONAIR_DEBUG_DIAG", "1");
#else
    setenv("ONAIR_DEBUG_DIAG", "1", 1);
#endif
    CHECK(diag::debugEnabled(), "ONAIR_DEBUG_DIAG=1 turns debug diagnostics on");
    diag::setDebug(false);
    setVerboseLogging(false);
    CHECK(!diag::debugEnabled(), "setDebug(false) with verbose logging off turns them off");

    setDiagSink([](const std::string& s) { lines.push_back(s); });
    diag::setTestSink(testSink);

    // ---- the text
    const std::string t = DIAG_TEXT(439865, "The test file %s has %d records, so the diagnostics test reads all of them.", "a.bin", 3);
    CHECK(t == "[E439865] The test file a.bin has 3 records, so the diagnostics test reads all of them.", "DIAG_TEXT gave \"%s\"", t.c_str());
    CHECK(lines.empty(), "DIAG_TEXT logs nothing");
    CHECK(diag::prefix(100001) == "[E100001] ", "prefix");
    const std::string u = DIAG_UI(254834, "Der übersetzte Text bleibt außerhalb des Schlüssels.");
    CHECK(u == "[E254834] Der übersetzte Text bleibt außerhalb des Schlüssels.", "DIAG_UI gave \"%s\"", u.c_str());
    const std::string longArg(1500, 'x');
    const std::string lt = DIAG_TEXT(745537, "The long value %s is kept whole, so a message is never cut at 512 bytes.", longArg.c_str());
    CHECK(lt.size() == 10 + 1500 + 70, "a long message is %zu bytes, wanted %d", lt.size(), 10 + 1500 + 70);

    // ---- warning level: always, to the process log, no "[v] "
    DIAG(280152, "The test warning carries value %d, so the log sink sees exactly one line.", 7);
    CHECK(lines.size() == 1 && lines.back() == "[E280152] The test warning carries value 7, so the log sink sees exactly one line.", "DIAG line \"%s\"",
          lines.empty() ? "" : lines.back().c_str());
    CHECK(levels.size() == 1 && levels.back() == diag::Level::Warn, "DIAG is a warning");
    DIAG_ERROR(264455, "The test error carries value %d, so the log sink sees a second line.", 8);
    CHECK(lines.size() == 2 && levels.back() == diag::Level::Error, "DIAG_ERROR is an error");

    // ---- debug off: nothing logged, the arguments are not evaluated
    debugSite(touch());
    debugEverySite(touch());
    CHECK(lines.size() == 2 && evaluated == 0, "debug off: %zu lines, %d arguments evaluated", lines.size(), evaluated);

    // ---- debug on through verbose logging, then through setDebug
    setVerboseLogging(true);
    debugSite(5);
    CHECK(lines.size() == 3 && lines.back() == "[v] [E424092] The debug line of the diagnostics test carries value 5, so it shows only while debug is on.",
          "verbose logging: \"%s\"", lines.empty() ? "" : lines.back().c_str());
    CHECK(levels.back() == diag::Level::Debug, "the debug level");
    setVerboseLogging(false);
    diag::setDebug(true);
    debugSite(6);
    CHECK(lines.size() == 4, "setDebug(true) turns them on");

    // ---- the rate limits: one line per site per hour
    debugEverySite(1);
    debugEverySite(2);
    CHECK(lines.size() == 5, "DIAG_DEBUG_EVERY: %zu lines after two calls, wanted one more", lines.size());
    diag::setDebug(false);
    warnEverySite(1);
    warnEverySite(2);
    CHECK(lines.size() == 6, "DIAG_EVERY: %zu lines after two calls, wanted one more", lines.size());

    // ---- the cost while off: a relaxed load and a branch
    const int kCalls = 20000000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kCalls; i++) debugSite(i);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / kCalls;
    printf("diag: a debug diagnostic while off costs %.2f ns per call\n", ns);
    CHECK(lines.size() == 6, "nothing logged while off");
    CHECK(ns < 20.0, "a debug diagnostic while off costs %.2f ns, wanted under 20", ns);

    setDiagSink(nullptr);
    diag::setTestSink(nullptr);
    printf(fails ? "diag: FAILED\n" : "diag: ok\n");
    return fails ? 1 : 0;
}
