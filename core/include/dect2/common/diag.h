// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Diagnostic codes (docs/DIAGNOSTICS.md). Every failure or notable condition carries a unique six-digit code next to a specific message, shown
// as "[E482917] message", so a user can quote it and a developer finds the exact line with one search.
//
//   DIAG(482917, "The DAB+ recording %s could not be written, so the recording stops.", path);   // always shown (warning level)
//   DIAG_ERROR(...)                                                                               // always shown (error level)
//   DIAG_DEBUG(613587, "...");      // only when debug diagnostics are on; costs one relaxed load and a branch when off, nothing is formatted
//   DIAG_EVERY(718243, 10.0, "..."); // at most once per 10 s from this call site (rare conditions on a busy path)
//   DIAG_DEBUG_EVERY(...)            // the same at debug level: nothing is evaluated while debug diagnostics are off
//   err = DIAG_TEXT(357821, "...");  // the same "[E357821] ..." text, returned (error out-parameters, status shown in the interface), not logged
//
// Rules (tools/lint/diag_check.py checks them): the code is a plain six-digit decimal literal at the call site, unique in the tree and never
// reused; the message is one string literal (printf placeholders are fine), a full sentence, unique in the tree. `tools/lint/diag_check.py --new 5`
// prints unused codes.
//
// Debug diagnostics are on when verbose logging is (View > Verbose logging on the desktop, Settings > Verbose logging on Android), when the
// environment variable ONAIR_DEBUG_DIAG is set to anything but 0 or empty, or after diag::setDebug(true).
//
// Include this header from .cpp files only, never from a header: core/src/dvbs/dvbs_sf.cpp (third-party code) has its own identifier DIAG.
#pragma once
#include "dect2/common/verbose_log.h"
#include <atomic>
#include <cstdint>
#include <string>

#if defined(__clang__)
#define ONAIR_DIAG_PRINTF(f, a) __attribute__((format(printf, f, a)))
#elif defined(__GNUC__)
#define ONAIR_DIAG_PRINTF(f, a) __attribute__((format(gnu_printf, f, a)))
#else
#define ONAIR_DIAG_PRINTF(f, a)
#endif

namespace dect2::diag {

enum class Level : uint8_t { Debug, Info, Warn, Error };

// The rules for a code (docs/DIAGNOSTICS.md): 100000 to 999999, last digit not 0, not within 1 of a power of two, more than 3 bits set, not all
// digits the same.
constexpr bool validCode(long long c) {
    if (c < 100000 || c > 999999 || c % 10 == 0) return false;
    for (int k = 16; k <= 20; k++) {
        const long long p = 1LL << k;
        if (c >= p - 1 && c <= p + 1) return false;
    }
    int bits = 0;
    for (long long v = c; v; v >>= 1) bits += (int)(v & 1);
    if (bits <= 3) return false;
    const long long d = c % 10;
    bool same = true;
    for (long long v = c; v; v /= 10) same = same && (v % 10 == d);
    return !same;
}

// A compile-time check of the literal at a call site: a code that breaks the rules, or one that is not a constant, does not compile.
template <long long C> struct Code {
    static_assert(validCode(C), "diagnostic code: six digits 100000-999999, last digit not 0, not within 1 of a power of two, more than 3 bits set, not all digits the same");
    static constexpr int value = (int)C;
};

// The debug switch: one relaxed atomic load each.
std::atomic<bool>& debugFlag();
inline bool debugEnabled() { return debugFlag().load(std::memory_order_relaxed) || verboseLogging(); }
void setDebug(bool on);

// "[E482917] " for code 482917
std::string prefix(int code);
// Formats "[E<code>] <text>" and sends it to the process log (diagEmit: the app's log, else stderr). A debug line starts with "[v] " like the
// other verbose lines.
void emit(Level level, int code, const char* fmt, ...) ONAIR_DIAG_PRINTF(3, 4);
// The same text, returned and not logged.
std::string text(int code, const char* fmt, ...) ONAIR_DIAG_PRINTF(2, 3);
// A message already translated by the interface (tr()): the code goes in front, outside the translation key.
std::string ui(int code, const std::string& translated);
// For DIAG_EVERY: true when `seconds` have passed since the last true at this site (`last` holds steady-clock milliseconds).
bool due(std::atomic<int64_t>& last, double seconds);

// Tests: every line emitted goes here too while set (an empty function stops it).
void setTestSink(void (*sink)(Level, const std::string&));

} // namespace dect2::diag

// `"" fmt` makes a message that is not a string literal a compile error; Code<code> does the same for the code.
#define DIAG(code, fmt, ...) ::dect2::diag::emit(::dect2::diag::Level::Warn, ::dect2::diag::Code<code>::value, "" fmt __VA_OPT__(,) __VA_ARGS__)
#define DIAG_ERROR(code, fmt, ...) ::dect2::diag::emit(::dect2::diag::Level::Error, ::dect2::diag::Code<code>::value, "" fmt __VA_OPT__(,) __VA_ARGS__)
#define DIAG_DEBUG(code, fmt, ...) \
    do { \
        if (::dect2::diag::debugEnabled()) [[unlikely]] \
            ::dect2::diag::emit(::dect2::diag::Level::Debug, ::dect2::diag::Code<code>::value, "" fmt __VA_OPT__(,) __VA_ARGS__); \
    } while (0)
#define DIAG_EVERY(code, seconds, fmt, ...) \
    do { \
        static std::atomic<int64_t> onairDiagLast_{INT64_MIN}; \
        if (::dect2::diag::due(onairDiagLast_, (seconds))) \
            ::dect2::diag::emit(::dect2::diag::Level::Warn, ::dect2::diag::Code<code>::value, "" fmt __VA_OPT__(,) __VA_ARGS__); \
    } while (0)
// A debug line at most once per `seconds` from this call site: nothing at all (not even the clock read) while debug diagnostics are off.
#define DIAG_DEBUG_EVERY(code, seconds, fmt, ...) \
    do { \
        if (::dect2::diag::debugEnabled()) [[unlikely]] { \
            static std::atomic<int64_t> onairDiagLast_{INT64_MIN}; \
            if (::dect2::diag::due(onairDiagLast_, (seconds))) \
                ::dect2::diag::emit(::dect2::diag::Level::Debug, ::dect2::diag::Code<code>::value, "" fmt __VA_OPT__(,) __VA_ARGS__); \
        } \
    } while (0)
#define DIAG_TEXT(code, fmt, ...) ::dect2::diag::text(::dect2::diag::Code<code>::value, "" fmt __VA_OPT__(,) __VA_ARGS__)
#define DIAG_UI(code, translated) ::dect2::diag::ui(::dect2::diag::Code<code>::value, (translated))
