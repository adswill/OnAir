// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// thread_name.h: the name is set on the thread, kept short on Linux, and namedThread names its thread
#include "dect2/common/thread_name.h"
#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main() {
    CHECK(dect2::clipThreadName("onair-short") == "onair-short");
    CHECK(dect2::clipThreadName("onair-a-very-long-thread-name", 15) == "onair-a-very-lo");
    CHECK(dect2::clipThreadName(nullptr).empty());
#if !defined(_WIN32)
    std::string seen;
    std::thread t = dect2::namedThread("onair-test", [&seen](int) { seen = dect2::currentThreadName(); }, 1);
    t.join();
    CHECK(seen == "onair-test");
    std::string longSeen;
    std::thread u = dect2::namedThread("onair-this-name-is-way-too-long-for-linux-and-macos-limits-both-xxxxxxxxxxxxxxxxxxxxxxxx", [&] { longSeen = dect2::currentThreadName(); });
    u.join();
    CHECK(!longSeen.empty() && longSeen.size() <= dect2::kThreadNameMax);   // cut, never dropped
    CHECK(longSeen.rfind("onair-this-na", 0) == 0);
    dect2::setThreadName("onair-main");
    CHECK(dect2::currentThreadName() == "onair-main");
    dect2::setThreadName("");   // ignored
    CHECK(dect2::currentThreadName() == "onair-main");
#endif
    // the scheduling class by name: savers are background, uploads utility, the rest (the sample path) untouched
    CHECK(dect2::threadNameClass("onair-results") == 2);
    CHECK(dect2::threadNameClass("onair-mapsend") == 1);
    CHECK(dect2::threadNameClass("onair-analysis") == 0);
    CHECK(dect2::threadNameClass("onair-src") == 0);
    CHECK(dect2::threadNameClass(nullptr) == 0);
    {
        bool ran = false;
        std::thread b = dect2::namedThread("onair-results", [&] { ran = true; });
        b.join();
        CHECK(ran);
    }
    if (!fails) std::printf("thread_name OK\n");
    return fails ? 1 : 0;
}
