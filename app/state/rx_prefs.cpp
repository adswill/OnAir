// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The settings store as one receiver sees it (docs/multi-sdr.md): the keys that belong to a receiver (radio, mode, frequency, gain)
// get the prefix "rx<N>_" for receiver N >= 2; receiver 1 and every shared key keep today's names, so nothing is lost.
#include "../platform/platform.h"
#include <cstring>

namespace plat {
int gPrefRx = 1;

namespace {
bool perReceiver(const char* k) {
    static const char* const kPre[] = {"freqMhz", "lna", "vga", "gain", "amp", "family", "subMode", "gLna", "gVga", "gAmp", "fFreq", "agcOn", "devName", "bw", "standard", "dtmbBw", "isdbtBw"};
    for (const char* p : kPre) if (!strncmp(k, p, strlen(p))) return true;
    return false;
}

struct RxPrefs : Prefs {
    Prefs& b() { return basePrefs(); }
    std::string key(const char* k) const { return gPrefRx > 1 && perReceiver(k) ? "rx" + std::to_string(gPrefRx) + "_" + k : std::string(k); }
    bool has(const char* k) override { return b().has(key(k).c_str()); }
    double getD(const char* k, double d) override { return b().getD(key(k).c_str(), d); }
    long getI(const char* k, long d) override { return b().getI(key(k).c_str(), d); }
    bool getB(const char* k, bool d) override { return b().getB(key(k).c_str(), d); }
    std::string getS(const char* k, const std::string& d) override { return b().getS(key(k).c_str(), d); }
    void setD(const char* k, double v) override { b().setD(key(k).c_str(), v); }
    void setI(const char* k, long v) override { b().setI(key(k).c_str(), v); }
    void setB(const char* k, bool v) override { b().setB(key(k).c_str(), v); }
    void setS(const char* k, const std::string& v) override { b().setS(key(k).c_str(), v); }
    std::vector<Channel> getChannels() override { return b().getChannels(); }
    void setChannels(const std::vector<Channel>& c) override { b().setChannels(c); }
    void flush() override { b().flush(); }
    std::string setAside() override { return b().setAside(); }
    std::vector<dect2::backup::PrefItem> dumpAll() override { return b().dumpAll(); }
};
}

Prefs& prefs() { static RxPrefs p; return p; }
}
