// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Diagnostic codes: formatting and the debug switch (dect2/common/diag.h, docs/DIAGNOSTICS.md).
#include "dect2/common/diag.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace dect2::diag {

namespace {
bool envDebug() {
    const char* e = std::getenv("ONAIR_DEBUG_DIAG");
    return e && *e && !(e[0] == '0' && e[1] == 0);
}
std::atomic<void (*)(Level, const std::string&)> gTestSink{nullptr};

std::string vformat(int code, const char* fmt, va_list ap) {
    std::string out = prefix(code);
    char buf[512];
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) { va_end(ap2); return out + fmt; }   // a broken format: the text as written still says what happened
    if ((size_t)n < sizeof buf) out.append(buf, (size_t)n);
    else {
        std::vector<char> big((size_t)n + 1);
        std::vsnprintf(big.data(), big.size(), fmt, ap2);
        out.append(big.data(), (size_t)n);
    }
    va_end(ap2);
    return out;
}
} // namespace

std::atomic<bool>& debugFlag() { static std::atomic<bool> on{envDebug()}; return on; }
void setDebug(bool on) { debugFlag().store(on); }

std::string prefix(int code) {
    char b[16];
    std::snprintf(b, sizeof b, "[E%06d] ", code);
    return b;
}

void emit(Level level, int code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string line = vformat(code, fmt, ap);
    va_end(ap);
    if (auto* t = gTestSink.load()) t(level, line);
    diagEmit(level == Level::Debug ? "[v] " + line : line);
}

std::string text(int code, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vformat(code, fmt, ap);
    va_end(ap);
    return s;
}

std::string ui(int code, const std::string& translated) { return prefix(code) + translated; }

bool due(std::atomic<int64_t>& last, double seconds) {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t prev = last.load(std::memory_order_relaxed);
    if (prev != INT64_MIN && (double)(now - prev) < seconds * 1000.0) return false;
    return last.compare_exchange_strong(prev, now, std::memory_order_relaxed);
}

void setTestSink(void (*sink)(Level, const std::string&)) { gTestSink.store(sink); }

} // namespace dect2::diag
