// The long form of a test: ONAIR_TEST_LONG=1 (set by the nightly Full CI and on release tags) runs the whole sweep, the default run a
// shorter one that still checks every assertion (fewer seconds of signal, fewer sweep points). docs/CI-SPEED-3.9.md.
//   const bool L = testLong();
//   const int seconds = testLongPick(2, 10);     // 2 s by default, 10 s in the long run
#pragma once
#include <cstdlib>

inline bool testLong() {
    static const bool on = [] { const char* e = std::getenv("ONAIR_TEST_LONG"); return e && *e && *e != '0'; }();
    return on;
}
template <class T> inline T testLongPick(T quick, T full) { return testLong() ? full : quick; }
