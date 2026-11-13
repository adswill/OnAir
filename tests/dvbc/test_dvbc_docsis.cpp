// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DOCSIS management messages through a transport stream on PID 0x1FFE: split over packets, stuffing, lost packets, other PIDs and subscriber frames (simulated).
#include "dect2/dvbc/docsis_down.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static uint16_t x25(const uint8_t* d, size_t n) { uint16_t c = 0xFFFF; for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); } return (uint16_t)~c; }
static std::vector<uint8_t> macFrame(uint8_t fc, const std::vector<uint8_t>& payload) { std::vector<uint8_t> f(6 + payload.size()); f[0] = fc; f[1] = 0; f[2] = (uint8_t)(payload.size() >> 8); f[3] = (uint8_t)payload.size(); const uint16_t h = x25(f.data(), 4); f[4] = (uint8_t)(h >> 8); f[5] = (uint8_t)h; memcpy(f.data() + 6, payload.data(), payload.size()); return f; }
static std::vector<uint8_t> mgmt(int type, const std::vector<uint8_t>& body, uint8_t fc = 0xC2) { std::vector<uint8_t> m(20 + body.size(), 0); m[0] = 1; m[6] = 0x00; m[7] = 0x11; m[8] = 0x22; m[9] = 0x33; m[10] = 0x44; m[11] = 0x55; m[17] = 1; m[18] = (uint8_t)type; memcpy(m.data() + 20, body.data(), body.size()); return macFrame(fc, m); }
struct Ts { int cc = 0; std::vector<uint8_t> out; void put(const std::vector<uint8_t>& s, std::mt19937& r) {
    size_t pos = 0;
    while (pos < s.size()) { uint8_t p[188]; memset(p, 0xFF, 188); p[0] = 0x47; p[1] = 0x5F; p[2] = 0xFE; p[3] = 0x10 | (cc++ & 15);
        bool first = pos == 0; p[1] = first ? 0x5F : 0x1F;
        int off = 4; if (first) { p[off++] = 0; }
        const size_t k = std::min<size_t>(188 - off, s.size() - pos); memcpy(p + off, s.data() + pos, k); pos += k;
        out.insert(out.end(), p, p + 188); if (r() % 5 == 0) { uint8_t o[188]; memset(o, 0, 188); o[0] = 0x47; o[1] = 0x1F; o[2] = 0xFF; out.insert(out.end(), o, o + 188); } }
} };
int main() {
    std::mt19937 r(2);
    for (int loss : {0, 1}) {
        Ts ts;
        std::vector<uint8_t> ucd = {3, 7, 4, 2, 1, 1, 32, 2, 4, 0x02, 0x0F, 0x58, 0x00, 4, 3, 1, 1, 1};
        std::vector<uint8_t> mdd = {1, 1, 1, 0, 1, 15, 1, 1, 9, 2, 4, 0x25, 0x51, 0xE3, 0x00, 3, 1, 0x01, 4, 1, 1};
        for (int i = 0; i < 400; i++) {
            ts.put(mgmt(1, {0, 0, (uint8_t)(i >> 8), (uint8_t)i}, 0xC0), r);
            ts.put(mgmt(2, ucd), r);
            ts.put(mgmt(3, {3, 7, 2, 0, 0, 0, 0, 0}), r);
            ts.put(mgmt(33, mdd), r);
            std::vector<uint8_t> data(300 + r() % 500, 0xAB);
            ts.put(macFrame(0x00, data), r);
        }
        std::vector<uint8_t>& v = ts.out;
        if (loss) { for (int k = 0; k < 120; k++) { size_t a = (r() % (v.size() / 188)) * 188; v[a + 1] |= 0x80; } }
        DocsisDownstream d;
        size_t chunk = 7 * 188; for (size_t o = 0; o < v.size(); o += chunk) d.feed(v.data() + o, std::min(chunk, v.size() - o) / 188);
        const auto& s = d.status();
        printf("loss %d: frames %llu mgmt %llu data %llu sync %llu hcsBad %llu lost %llu, UCD %zu, DS %zu, MAC %s\n", loss, (unsigned long long)s.framesTotal, (unsigned long long)s.framesMgmt, (unsigned long long)s.framesData, (unsigned long long)s.timingSync, (unsigned long long)s.hcsBad, (unsigned long long)s.lost, s.upstream.size(), s.downstream.size(), s.lastMac.c_str());
        CHECK(s.present, "not found");
        if (!loss) { CHECK(s.framesMgmt == 1600 && s.framesData == 400, "frame counts"); CHECK(s.timingSync == 400, "sync count"); }
        else CHECK(s.framesMgmt > 1200, "with loss: %llu", (unsigned long long)s.framesMgmt);
        CHECK(s.upstream.size() == 1 && s.upstream.begin()->second.channelId == 3, "UCD");
        if (!s.upstream.empty()) { const auto& u = s.upstream.begin()->second; CHECK(u.changeCount == 7 && u.miniSlotTicks == 4 && u.dsChannelId == 2, "UCD header"); CHECK(u.symbolRateMsym == 5.12, "symbol rate %.2f", u.symbolRateMsym); CHECK(std::abs(u.frequencyMhz - 34.56) < 0.001, "frequency %.3f", u.frequencyMhz); CHECK(u.bursts == 1, "bursts %d", u.bursts); CHECK(u.maps >= 300, "maps %llu", (unsigned long long)u.maps); }
        CHECK(s.downstream.size() == 1 && s.downstream[0].channelId == 9 && std::abs(s.downstream[0].frequencyMhz - 626.12352) < 0.001 && s.downstream[0].primaryCapable, "MDD");
        CHECK(s.lastTimestamp != 0, "timestamp");
    }
    printf(fails ? "docsis FAILED\n" : "docsis passed\n");
    return fails ? 1 : 0;
}
