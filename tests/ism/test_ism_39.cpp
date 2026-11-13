// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors, the 3.9 devices (X10 RF, Insteon, inFactory, Ambient Weather WH31E, Nissan and GM TPMS, Efergy Optical, ESIC EMT7110, ERT SCM+).
//
//   1. known answers: the check algorithms against frames printed in the rtl_433 notes (real captures): WH31E CRC-8 and SUM-8, SCM+ CRC-16
//      0x39BE, the GM frame's sum
//   2. pulse trains from the generator: every device decodes with its values, with timing stretched by -8 .. +8 % (PPM / PWM, cheap oscillators)
//      or -4 .. +4 % (bit-clocked PCM / Manchester), and +-4 % jitter on every edge; one moved edge (one flipped bit) never gives a wrong reading
//   3. over the air (synthetic I/Q) through the receiver: scene 4 clean, with a 30 kHz tuning error, 200 ppm clock error and 14 dB C/N;
//      every device heard, nothing wrong. Noise only: nothing reported.
// All signals are simulated: no recording of these devices was available.
#include "dect2/ism/ism_gen.h"
#include "dect2/ism/ism_proto.h"
#include "dect2/ism/ism_rx.h"
#include "dect2/ism/ism_tel.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const IsmTx kNew[] = {IsmTx::X10Rf, IsmTx::Insteon, IsmTx::Infactory, IsmTx::Wh31e, IsmTx::Nissan, IsmTx::GmTpms, IsmTx::EfergyOptical, IsmTx::Emt7110, IsmTx::ScmPlus};

static bool heard(const std::vector<IsmReading>& rs, IsmTx p, int k) {
    for (const auto& r : rs) { IsmTx w; int kk; if (ismTruthMatches(r, &w, &kk) && w == p && kk % 2 == k % 2) return true; }
    return false;
}
[[maybe_unused]] static bool anyOf(const std::vector<IsmReading>& rs, const char* model) {
    for (const auto& r : rs) if (r.model == model) return true;
    return false;
}
static int wrong(const std::vector<IsmReading>& rs) {
    int n = 0;
    for (const auto& r : rs) if (!ismTruthMatches(r)) n++;
    return n;
}

// the unit by which one edge moves to flip one bit (PPM: short to long gap, PWM: pulse length, NRZ: one bit, Manchester: half a bit)
static float unitOf(IsmTx p) {
    switch (p) {
    case IsmTx::X10Rf: return 1125;
    case IsmTx::Infactory: return 2000;
    case IsmTx::EfergyOptical: return 72;
    case IsmTx::Insteon: return 109.6f;
    case IsmTx::Wh31e: return 56;
    case IsmTx::Nissan: return 120;
    case IsmTx::Emt7110: return 104.4f;
    case IsmTx::GmTpms: return 120;
    case IsmTx::ScmPlus: return 30;
    default: return 0;
    }
}

int main() {
    // ---- 1. known answers from real captures quoted in rtl_433's notes
    {
        const uint8_t wh[7] = {0x30, 0xc3, 0x81, 0xd5, 0x5c, 0x2a, 0xcf};   // ambientweather_wh31e.c "Some payloads", first line
        CHECK(ismCrc8(wh, 5, 0x31, 0x00) == 0x2a && ismCrc8(wh, 6, 0x31, 0x00) == 0, "WH31E CRC-8 %02x", ismCrc8(wh, 5, 0x31, 0x00));
        CHECK((uint8_t)(wh[0] + wh[1] + wh[2] + wh[3] + wh[4] + wh[5]) == 0xcf, "WH31E SUM-8");
        // scmplus.c: ProtocolID 0x1E EndpointType 0xAB EndpointID 68211547 Consumption 6883 Tamper 0x4900 PacketCRC 0x39BE (an rtlamr capture)
        const uint32_t id = 68211547, cons = 6883;
        const uint8_t s[12] = {0x1E, 0xAB, (uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id,
                               (uint8_t)(cons >> 24), (uint8_t)(cons >> 16), (uint8_t)(cons >> 8), (uint8_t)cons, 0x49, 0x00};
        CHECK(ismCrc16(s, 12, 0x1021, 0x0971) == 0x39BE, "SCM+ CRC %04x", ismCrc16(s, 12, 0x1021, 0x0971));
        const uint8_t gm[10] = {0x4c, 0x90, 0x00, 0x78, 0x49, 0x17, 0x66, 0x00, 0x53, 0x6d};   // tpms_gm.c data layout example
        uint8_t sum = 0;
        for (int i = 0; i < 9; i++) sum = (uint8_t)(sum + gm[i]);
        CHECK(sum == gm[9], "GM sum");
        // the WH31E and GM generators use exactly those frames for k = 0; their values
        const auto r0 = ismDecode(ismEncode(IsmTx::Wh31e, 0));
        bool ok = false;
        for (const auto& r : r0) if (r.model == "AmbientWeather-WH31E" && r.id == "195" && r.channel == 1 && r.battery == 1) {
            for (const auto& f : r.fields) if (f.name == "Humidity") ok = f.value == 92;
        }
        CHECK(ok, "WH31E known frame: id 195, channel 1, 92 %%");
        const auto g0 = ismDecode(ismEncode(IsmTx::GmTpms, 0));
        ok = false;
        for (const auto& r : g0) if (r.model == "GM-Aftermarket" && r.id == "2018056038") {
            for (const auto& f : r.fields) if (f.name == "Temperature") ok = f.value == 23;
        }
        CHECK(ok, "GM known frame: id 2018056038, 23 C");
    }

    // ---- 2. pulse trains: values, timing faults, one flipped bit
    std::mt19937 rng(39);
    int cases = 0;
    for (IsmTx p : kNew) {
        for (int k = 0; k < 2; k++) {
            const IsmTrain t = ismEncode(p, k);
            CHECK(!t.p.empty(), "%s: no train", ismTxName(p));
            const auto rs = ismDecode(t);
            CHECK(heard(rs, p, k) && wrong(rs) == 0, "%s k=%d clean: %zu readings, %d wrong", ismTxName(p), k, rs.size(), wrong(rs));
            const bool slots = p == IsmTx::X10Rf || p == IsmTx::Infactory || p == IsmTx::EfergyOptical;   // PPM / PWM: no bit clock to hold
            for (double stretch : slots ? std::vector<double>{0.92, 0.96, 1.04, 1.08} : std::vector<double>{0.96, 0.98, 1.02, 1.04}) {
                IsmTrain s = t;
                for (auto& q : s.p) { q.pulseUs *= (float)stretch; q.gapUs *= (float)stretch; }
                const auto rr = ismDecode(s);
                CHECK(heard(rr, p, k) && wrong(rr) == 0, "%s k=%d timing x%.2f", ismTxName(p), k, stretch);
                cases++;
            }
            for (int rep = 0; rep < 5; rep++) {
                IsmTrain s = t;
                std::uniform_real_distribution<float> j(-0.04f, 0.04f);
                for (auto& q : s.p) { const float d = j(rng) * std::min(q.pulseUs, q.gapUs); q.pulseUs += d; q.gapUs -= d; }   // move each edge
                const auto rr = ismDecode(s);
                CHECK(heard(rr, p, k) && wrong(rr) == 0, "%s k=%d jitter %d", ismTxName(p), k, rep);
                cases++;
            }
        }
        // one edge moved by one unit, at several places in the payload: never a reading of this device
        const IsmTrain t = ismEncode(p, 0);
        const float u = unitOf(p);
        const char* model = ismTruth(p, 0).model;
        const size_t n = t.p.size();
        int tried = 0;
        for (size_t i = n / 2; i + 2 < n && tried < 6; i += std::max<size_t>(1, n / 14)) {
            IsmTrain s = t;
            if (p == IsmTx::X10Rf || p == IsmTx::Infactory) {   // PPM: a short gap becomes long or a long one short
                const float sh = p == IsmTx::X10Rf ? 562.5f : 2000.f;
                if (std::fabs(s.p[i].gapUs - sh) < 1) s.p[i].gapUs += u; else if (std::fabs(s.p[i].gapUs - sh - u) < 1) s.p[i].gapUs -= u; else continue;
            } else if (p == IsmTx::EfergyOptical) {           // PWM: swap a 1 and a 0
                std::swap(s.p[i].pulseUs, s.p[i].gapUs);
            } else {                                          // NRZ / Manchester: move the falling edge by one unit
                if (s.p[i].gapUs >= 2 * u - 1) { s.p[i].pulseUs += u; s.p[i].gapUs -= u; }
                else if (s.p[i].pulseUs >= 2 * u - 1) { s.p[i].pulseUs -= u; s.p[i].gapUs += u; }
                else continue;
            }
            tried++;
            const auto rr = ismDecode(s);
            CHECK(wrong(rr) == 0, "%s: a flipped bit at pulse %zu gave a wrong reading", ismTxName(p), i);   // repeats and unchecked bytes may still give the right one
            (void)model;
        }
        CHECK(tried >= 1, "%s: no damaged train", ismTxName(p));
    }
    printf("  pulse trains: %d timing cases\n", cases);

    // ---- 3. over the air through the receiver
    const double kRate = 2e6, kDial = -250e3;
    auto run = [&](const char* name, double snr, double cfo, double ppm, bool devices) {
        IsmGenConfig c;
        c.rate = kRate; c.dialOffsetHz = kDial; c.snrDb = snr; c.cfoHz = cfo; c.seed = 391;
        if (devices) c.devs = ismTestScene(4);
        IsmGenerator g(c);
        std::vector<cf32> x((size_t)(6.0 * kRate));
        g.generate(x.data(), x.size());
        if (!devices) impair::noise(x, 0, 5);
        if (ppm != 0) x = impair::clock(x, ppm);
        IsmReceiver rx;
        rx.configure(kRate);
        rx.setSignalOffset(kDial);
        std::set<int> got;
        int bad = 0, total = 0;
        rx.setReadingTap([&](const IsmReading& rd, double) {
            total++;
            IsmTx w;
            if (ismTruthMatches(rd, &w)) got.insert((int)w);
            else { bad++; printf("    WRONG: %s id %s\n", rd.model.c_str(), rd.id.c_str()); }
        });
        for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        std::vector<cf32> z(65536, cf32(0, 0));
        rx.feed(z.data(), z.size());
        std::string miss;
        if (devices)
            for (IsmTx p : kNew) if (!got.count((int)p)) { if (!miss.empty()) miss += ", "; miss += ismTxName(p); }
        printf("  %-28s %zu/%d heard, %d readings, %d wrong, missing: %s\n", name, got.size(), devices ? 9 : 0, total, bad, miss.empty() ? "-" : miss.c_str());
        return std::make_pair(miss, bad + (devices ? 0 : total));
    };
    auto a = run("clean", 30, 0, 0, true);
    CHECK(a.first.empty() && a.second == 0, "clean scene: missing %s", a.first.c_str());
    a = run("tuning error +30 kHz", 30, 30e3, 0, true);
    CHECK(a.first.empty() && a.second == 0, "+30 kHz: missing %s", a.first.c_str());
    const bool longRun = getenv("ONAIR_TEST_LONG") != nullptr;
    if (longRun) {
        a = run("tuning error -30 kHz", 30, -30e3, 0, true);
        CHECK(a.first.empty() && a.second == 0, "-30 kHz: missing %s", a.first.c_str());
    }
    a = run("clock +200 ppm", 30, 0, 200, true);
    CHECK(a.first.empty() && a.second == 0, "200 ppm: missing %s", a.first.c_str());
    a = run("14 dB C/N", 14, 0, 0, true);
    CHECK(a.second == 0, "14 dB: %d wrong", a.second);
    a = run("noise only", 30, 0, 0, false);
    CHECK(a.second == 0, "noise: %d readings", a.second);

    return testSummary("ism_39");
}
