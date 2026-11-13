// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDES: ASM bursts (GMSK 9.6 kbit/s, AIS framing) decode with the payload sent; VDE-TER bursts of 25 / 50 / 100 kHz are detected, their bandwidth, band,
// level and modulation (pi/4-QPSK, 8PSK, 16QAM) reported; offsets, noise and clock errors; noise alone gives nothing.
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/vdes/vdes_gen.h"
#include "dect2/vdes/vdes_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static VdesTelemetry feedAll(const std::vector<cf32>& x, double rate, double dial = 161.9e6) {
    VdesReceiver r;
    r.configure(rate);
    r.setSignalOffset(0);
    r.setDialHz(dial);
    VdesTelemetry last;
    uint64_t seq = 0;
    for (size_t i = 0; i < x.size(); i += 16384) {
        r.feed(x.data() + i, std::min<size_t>(16384, x.size() - i));
        VdesTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    // flush: one more quarter second of nothing
    std::vector<cf32> z(16384, cf32());
    for (int k = 0; k < 20; k++) {
        r.feed(z.data(), z.size());
        VdesTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    return last;
}

static ais::Bits payload(std::mt19937& rng, size_t n) {
    ais::Bits b(n);
    for (auto& v : b) v = (uint8_t)(rng() & 1);
    return b;
}

static std::string hexOf(const ais::Bits& bits) {
    static const char* hx = "0123456789ABCDEF";
    std::string h;
    for (size_t i = 0; i < bits.size(); i += 8) {
        int v = 0;
        for (size_t k = 0; k < 8; k++) v = (v << 1) | (i + k < bits.size() ? bits[i + k] : 0);
        h += hx[v >> 4]; h += hx[v & 15];
    }
    return h;
}

// ASM: twelve bursts on the two channels
static void asmTest(double rate, double snr, double cfo, double ppm) {
    std::mt19937 rng(7);
    std::vector<AisBurstSpec> sp;
    for (int i = 0; i < 12; i++) {
        AisBurstSpec b;
        b.payload = payload(rng, 88 + (size_t)(rng() % 150));
        b.channel = i & 1 ? 'B' : 'A';
        b.startSec = 0.3 + 0.17 * i;
        sp.push_back(b);
    }
    VdesRenderConfig rc;
    rc.rate = rate; rc.durationSec = 2.6; rc.snrDb = snr; rc.cfoHz = cfo; rc.sroPpm = ppm;
    const VdesTelemetry t = feedAll(renderVdes(sp, {}, rc), rate);
    int got = 0, wrongCh = 0;
    for (const AisBurstSpec& b : sp) {
        const std::string h = hexOf(b.payload);
        bool f = false;
        for (const VdesAsm& a : t.asms) if (a.hex == h && a.bits == (int)b.payload.size()) { f = true; if (a.channel != b.channel) wrongCh++; }
        got += f;
    }
    printf("ASM %.2f Msps SNR %4.1f cfo %+5.0f ppm %+4.0f: %d of 12 decoded, %zu listed, %llu bad, SNR %.1f dB\n", rate / 1e6, snr, cfo, ppm, got, t.asms.size(), (unsigned long long)t.blocksBad, t.snrDb);
    CHECK(got >= (snr < 12 ? 9 : 12), "ASM: %d of 12 frames", got);
    CHECK(wrongCh == 0, "ASM: %d on the wrong channel", wrongCh);
    CHECK(t.asms.size() == (size_t)t.blocksOk && t.blocksOk >= (uint64_t)got, "ASM count %zu / %llu", t.asms.size(), (unsigned long long)t.blocksOk);
}

// VDE-TER: nine kinds of burst one after the other
static void vdeTest(double rate, double snr, double cfo, double ppm, int band = 1, double dial = 161.9e6) {
    std::vector<VdeBurstSpec> sp;
    double t0 = 0.2;
    int idx = 0;
    for (int bw : {1, 2, 4}) for (int mod = 1; mod <= 3; mod++) {
        VdeBurstSpec b;
        b.band = band; b.nSub = bw; b.firstSub = bw == 4 ? 0 : (idx % (5 - bw)); b.mod = mod; b.startSec = t0; b.durSec = 0.03; b.seed = 40 + (uint32_t)idx; b.cfoHz = cfo;
        sp.push_back(b);
        t0 += 0.15; idx++;
    }
    VdesRenderConfig rc;
    rc.rate = rate; rc.durationSec = t0 + 0.3; rc.snrDb = snr; rc.sroPpm = ppm; rc.dialHz = dial;
    const VdesTelemetry t = feedAll(renderVdes({}, sp, rc), rate, dial);
    int found = 0, bwOk = 0, modOk = 0;
    for (const VdeBurstSpec& b : sp) {
        const VdeBurst* m = nullptr;
        for (const VdeBurst& v : t.bursts) if (std::fabs(v.timeSec - b.startSec) < 0.02) m = &v;
        if (!m) { printf("    missing burst %d kHz mod %d\n", b.nSub * 25, b.mod); continue; }
        found++;
        bwOk += m->bwKhz == b.nSub * 25 && m->band == band;
        const bool mo = m->mod == b.mod;
        modOk += mo;
        if (!mo || m->bwKhz != b.nSub * 25 || getenv("VDES_DEBUG")) printf("    %d kHz %s: got %d kHz %s (cv %.2f psi %+.2f, %d sym), SNR %.1f\n", b.nSub * 25, vdeModName(b.mod), m->bwKhz, vdeModName(m->mod), m->cv, m->psi, m->nSym, m->snrDb);
        CHECK(std::fabs(m->cfoHz - cfo) < 900.0, "cfo %.0f vs %.0f", m->cfoHz, cfo);
        CHECK(std::fabs(m->durMs - 30.0) < 6.0, "duration %.1f ms", m->durMs);
    }
    printf("VDE-TER %.2f Msps SNR %4.1f cfo %+5.0f ppm %+4.0f: %d of 9 found, %d bandwidths, %d modulations; %zu bursts logged\n", rate / 1e6, snr, cfo, ppm, found, bwOk, modOk, t.bursts.size());
    CHECK(found == 9, "VDE-TER: %d of 9 bursts found", found);
    CHECK(bwOk == found, "VDE-TER: %d of %d bandwidths right", bwOk, found);
    CHECK(modOk >= found - (snr < 14 ? 2 : 0), "VDE-TER: %d of %d modulations right", modOk, found);
    CHECK(t.bursts.size() == 9, "VDE-TER: %zu bursts logged (9 sent)", t.bursts.size());
    CHECK(t.state >= 1, "state %d", t.state);
}

// The channels are ITU-R M.2092's: ASM 1 = 161.950 (channel 2027), ASM 2 = 162.000 (2028). Bursts placed with the AIS renderer at absolute
// frequencies (not through kVdesAsm*): the pair decodes on the right channels; real AIS on 161.975 / 162.025, even off frequency, is not ASM.
static void channelPlanTest() {
    CHECK(kVdesAsm1Hz == 161.950e6 && kVdesAsm2Hz == 162.000e6, "ASM channels %.0f / %.0f Hz", kVdesAsm1Hz, kVdesAsm2Hz);
    const double dial = 161.9e6, rate = 1e6;
    std::mt19937 rng(11);
    std::vector<AisBurstSpec> sp;
    for (int i = 0; i < 8; i++) { AisBurstSpec b; b.payload = payload(rng, 168); b.channel = i & 1 ? 'B' : 'A'; b.startSec = 0.3 + 0.2 * i; sp.push_back(b); }
    const auto render = [&](double midHz, double cfo) {
        AisRenderConfig ac; ac.rate = rate; ac.durationSec = 2.2; ac.snrDb = 30; ac.cfoHz = cfo; ac.tuneOffsetHz = -(midHz - dial);
        return renderAisBursts(sp, ac);
    };
    for (double cfo : {0.0, -1500.0, 1500.0}) {
        const VdesTelemetry t = feedAll(render(161.975e6, cfo), rate, dial);
        int ok = 0;
        for (const AisBurstSpec& b : sp) for (const VdesAsm& a : t.asms) if (a.hex == hexOf(b.payload) && a.channel == b.channel) { ok++; break; }
        printf("ASM pair 161.950 / 162.000, cfo %+5.0f: %d of 8 on the right channel\n", cfo, ok);
        CHECK(ok == 8, "ASM pair: %d of 8 at cfo %.0f", ok, cfo);
        const VdesTelemetry w = feedAll(render(162.000e6, cfo), rate, dial);
        printf("AIS 161.975 / 162.025, cfo %+5.0f: %zu listed as ASM\n", cfo, w.asms.size());
        CHECK(w.asms.empty(), "AIS channels: %zu frames listed as ASM at cfo %.0f", w.asms.size(), cfo);
    }
}

int main() {
    channelPlanTest();
    asmTest(1e6, 30, 0, 0);
    for (double snr : {14.0, 20.0, 30.0}) for (double cfo : {-1200.0, 900.0}) asmTest(1e6, snr, cfo, 0);
    asmTest(1e6, 22, 0, 40);
    asmTest(2e6, 22, 600, -40);
    vdeTest(1e6, 30, 0, 0);
    for (double snr : {18.0, 26.0}) for (double cfo : {-300.0, 250.0}) vdeTest(1e6, snr, cfo, 0);
    vdeTest(1e6, 24, 0, 25);
    vdeTest(1e6, 24, 150, -25);
    vdeTest(2e6, 24, -200, 0);
    vdeTest(1e6, 26, 100, 0, 0, 157.3e6);      // the 157.2 MHz band, tuned just above it
    // levels: half the amplitude is 6 dB less
    {
        std::vector<VdeBurstSpec> sp;
        for (int i = 0; i < 2; i++) { VdeBurstSpec b; b.nSub = 2; b.firstSub = 1; b.mod = 1; b.startSec = 0.3 + 0.2 * i; b.level = i ? 0.5 : 1.0; b.seed = 3 + (uint32_t)i; sp.push_back(b); }
        VdesRenderConfig rc; rc.rate = 1e6; rc.durationSec = 0.9; rc.snrDb = 40;
        const VdesTelemetry t = feedAll(renderVdes({}, sp, rc), 1e6);
        CHECK(t.bursts.size() == 2, "level test: %zu bursts", t.bursts.size());
        if (t.bursts.size() == 2) {
            const float d = t.bursts[1].rssiDb - t.bursts[0].rssiDb;
            printf("levels: %.1f and %.1f dBFS, difference %.1f dB\n", t.bursts[1].rssiDb, t.bursts[0].rssiDb, d);
            CHECK(std::fabs(d - 6.0) < 1.2, "level difference %.1f dB", d);
        }
    }
    // an ASM that is an AIS position report (type 1): the receiver reads the position out of it (#303), a payload that is not AIS gets none
    {
        ais::Bits pos;
        ais::putU(pos, 0, 6, 1); ais::putU(pos, 6, 2, 0); ais::putU(pos, 8, 30, 244123456); ais::putU(pos, 38, 4, 0); ais::putS(pos, 42, 8, -128);
        ais::putU(pos, 50, 10, 125); ais::putU(pos, 60, 1, 1);
        ais::putS(pos, 61, 28, (int32_t)std::lround(5.25 * 600000.0)); ais::putS(pos, 89, 27, (int32_t)std::lround(52.5 * 600000.0));
        ais::putU(pos, 116, 12, 800); ais::putU(pos, 128, 9, 80); ais::putU(pos, 137, 6, 10); ais::putU(pos, 143, 25, 0);
        std::mt19937 rng(11);
        std::vector<AisBurstSpec> sp;
        AisBurstSpec a; a.payload = pos; a.channel = 'A'; a.startSec = 0.3; sp.push_back(a);
        AisBurstSpec b; b.payload = payload(rng, 120); b.payload[0] = 1; b.payload[1] = 1; b.channel = 'B'; b.startSec = 0.7; sp.push_back(b);   // type 48+: not AIS
        VdesRenderConfig rc; rc.rate = 1e6; rc.durationSec = 1.4; rc.snrDb = 30;
        const VdesTelemetry t = feedAll(renderVdes(sp, {}, rc), 1e6);
        int withPos = 0;
        for (const VdesAsm& f : t.asms) if (f.hasPos) {
            withPos++;
            CHECK(f.aisMmsi == 244123456 && std::fabs(f.lat - 52.5) < 1e-4 && std::fabs(f.lon - 5.25) < 1e-4 && std::fabs(f.sogKt - 12.5) < 0.05 && std::fabs(f.cogDeg - 80) < 0.05, "position %.5f %.5f", f.lat, f.lon);
        }
        CHECK(withPos == 1, "%d ASM frames with a position", withPos);
    }
    // noise alone, and the generator's silence: no ASM, no burst
    {
        genutil::NoiseSource ns(5);
        std::vector<cf32> x((size_t)(6e6));
        ns.add(x.data(), x.size(), 0.1f);
        const VdesTelemetry t = feedAll(x, 1e6);
        printf("noise: %llu ASM, %llu bursts, state %d\n", (unsigned long long)t.blocksOk, (unsigned long long)t.vdeBursts, t.state);
        CHECK(t.blocksOk == 0 && t.vdeBursts == 0 && t.state == 0 && t.asms.empty() && t.bursts.empty(), "noise gave output");
    }
    CHECK(vdesTuning().stdMode == 65 && modeTuningById("vdes") && modeTuningById("vdes")->stdMode == 65, "tuning table");
    printf(fails ? "FAILED: %d\n" : "vdes rx ok\n", fails);
    return fails ? 1 : 0;
}
