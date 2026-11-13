// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AIS frame layer under mutation: libFuzzer entry point (clang -fsanitize=fuzzer, -DONAIR_LIBFUZZER) or, by default, a deterministic run of seeded
// mutations of valid frames and sentences (ONAIR_FUZZ_ITERS, default 200000). Meant to run under DECT2_SANITIZE=ON: any crash or UB report fails it.
#include "dect2/ais/ais_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
using namespace dect2::ais;

static unsigned long long sink = 0;

static void exercise(const uint8_t* d, size_t n) {
    Bits bits(n * 8);
    for (size_t i = 0; i < bits.size(); i++) bits[i] = (d[i / 8] >> (7 - i % 8)) & 1;
    AisMsg m;
    const bool ok = decodeMessage(bits, m);
    sink += ok + m.mmsi + m.name.size() + m.destination.size() + (acceptableAis(m, ok) ? 1 : 0);
    int fill = 0;
    const std::string a = armour(bits, fill);
    sink += unarmour(a, fill).size() + unarmour(a, (int)(d[0] % 9) - 2).size();
    for (const std::string& s : toNmea(bits, 'A', (int)n)) {
        std::string c; int f = 0, cn = 0, no = 0;
        sink += parseNmea(s, c, f, cn, no);
    }
    Bits ds;
    sink += destuff(bits, ds) ? ds.size() : 0;
    sink += stuff(bits).size() + nrziDecode(bits).size() + nrziEncode(bits).size() + fcsOk(bits);
    Bits r = bits; reverseOctets(r);
    sink += hdlcFrames(bits).good.size();
    sink += burstLineBits(bits).size();
    std::string txt((const char*)d, n);
    std::string c; int f = 0, cn = 0, no = 0;
    sink += parseNmea(txt, c, f, cn, no);
    const size_t l = d[n > 1 ? 1 : 0] % 40;
    sink += getU(bits, d[0], l) + (uint32_t)getS(bits, d[0], l) + getText(bits, d[0] % 64, d[n > 1 ? 1 : 0]).size();
}

#ifdef ONAIR_LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) { if (n) exercise(d, n); return 0; }
#else
static Bits sample(std::mt19937& g, int type) {
    Bits b;
    putU(b, 0, 6, (uint32_t)type); putU(b, 8, 30, 227000000u + g() % 1000000);
    for (size_t i = 38; i < 168; i++) putU(b, i, 1, g() & 1);
    if (type == 5) b.resize(424);
    for (size_t i = 168; i < b.size(); i++) b[i] = g() & 1;
    return b;
}
int main() {
    const char* e = getenv("ONAIR_FUZZ_ITERS");
    const long iters = e ? atol(e) : 200000;
    std::mt19937 g(12345);
    std::vector<std::vector<uint8_t>> seeds;
    const int types[] = {1, 2, 3, 4, 5, 6, 8, 9, 14, 18, 19, 21, 24, 27};
    for (int t : types) {
        Bits p = sample(g, t);
        Bits fr = p; reverseOctets(fr); appendFcs(fr);
        Bits line = burstLineBits(p);
        seeds.push_back(line);
        seeds.push_back(nrziDecode(line));
        seeds.push_back(stuff(fr));
        std::vector<uint8_t> by((p.size() + 7) / 8);
        for (size_t i = 0; i < p.size(); i++) by[i / 8] |= (uint8_t)(p[i] << (7 - i % 8));
        seeds.push_back(by);
        int fill; const std::string a = armour(p, fill);
        char s[200]; snprintf(s, sizeof s, "!AIVDM,1,1,,A,%s,%d*00", a.c_str(), fill);
        seeds.push_back(std::vector<uint8_t>(s, s + strlen(s)));
        for (const std::string& n : toNmea(p, 'B', 3)) seeds.push_back(std::vector<uint8_t>(n.begin(), n.end()));
    }
    for (long it = 0; it < iters; it++) {
        std::vector<uint8_t> v = seeds[g() % seeds.size()];
        const int k = 1 + (int)(g() % 6);
        for (int j = 0; j < k; j++) {
            switch (g() % 6) {
            case 0: if (!v.empty()) v[g() % v.size()] ^= (uint8_t)(1u << (g() % 8)); break;
            case 1: if (!v.empty()) v[g() % v.size()] = (uint8_t)g(); break;
            case 2: if (!v.empty()) v.erase(v.begin() + (long)(g() % v.size())); break;
            case 3: v.insert(v.begin() + (long)(g() % (v.size() + 1)), (uint8_t)g()); break;
            case 4: if (v.size() > 2) v.resize(g() % v.size()); break;
            default: if (!v.empty()) { size_t a = g() % v.size(); size_t l = g() % 24; for (size_t q = a; q < v.size() && q < a + l; q++) v[q] = (uint8_t)(g() & 1); } break;
            }
        }
        if (v.empty()) v.push_back(0);
        exercise(v.data(), v.size());
    }
    printf("fuzz_ais_proto: %ld iterations ok (%llu)\n", iters, sink);
    return 0;
}
#endif
