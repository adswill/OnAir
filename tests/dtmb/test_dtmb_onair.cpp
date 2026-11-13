// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DTMB as it is on the air (issue #58): two single carrier transmitters, PN595 with a fixed phase, 4QAM rate 0.8, interleaver mode 2, recorded at
// 10 Msps, were tracked with a clean header fit (C/N 18 dB) but their system information never decoded. The PN595 header of this receiver started
// one chip late: on the air the header is a 1 and then the nine zeros of the register, and the system information follows chip 594 of the old
// sequence. The header fit hardly notices (594 of 595 chips agree), but the body was taken one symbol late.
//
// The test signal here is the transmitter model with every frame header replaced by the on-air PN595 written out in this file (not taken from
// the receiver's tables, so that the two cannot drift together again), at the parameters of the recordings: 10 Msps, 8 MHz, a carrier offset of
// 230 Hz, a clock 0.4 ppm off, a short echo, C/N 18 dB, 8 bit samples.
#include "dect2/dtmb/dtmb_defs.h"
#include "dect2/common/stable_random.h"
#include "dect2/dtmb/dtmb_gen.h"
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/dtmb/dtmb_tx.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

// PN595 on the air: x^10 + x^3 + 1, the chips 1 0 0 0 0 0 0 0 0 0 first; chip value +1 for a 0, -1 for a 1
static std::vector<int8_t> onAirPn595() {
    std::vector<int> b = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    while (b.size() < 595) b.push_back(b[b.size() - 10] ^ b[b.size() - 3]);
    std::vector<int8_t> c(595);
    for (int i = 0; i < 595; i++) c[(size_t)i] = b[(size_t)i] ? -1 : 1;
    return c;
}

int main() {
    const std::vector<int8_t> pn = onAirPn595();
    {
        const auto& c = pnChips(Header::Pn595);
        int bad = 0;
        for (int i = 0; i < 595; i++) bad += c[(size_t)i] != pn[(size_t)i];
        CHECK(bad == 0, "the receiver's PN595 differs from the on-air one in %d chips", bad);
    }

    const double rate = 10e6, seconds = 0.8, cfoHz = 230, ppm = -0.4, cnDb = 18;
    const uint32_t seed = 58;
    TxConfig tc;
    tc.header = Header::Pn595; tc.carriers = 1; tc.phaseRotate = false;
    tc.profile.map = Mapping::Qam4; tc.profile.rate = Rate::R08; tc.profile.mode2 = true;
    FrameTx tx(tc, testPacketSource(seed));
    const int Lf = frameLength(Header::Pn595);
    const float a = std::sqrt(0.5f);   // PN595 is not boosted: a header symbol has the power of a body symbol

    // symbol stream: the transmitter's frames with the on-air header, and an echo 12 dB down, two symbols late
    std::vector<cf32> sym;   // sym[i] is symbol base + i
    long base = 0;
    std::vector<cf32> frame((size_t)Lf), prev2(2, cf32(0, 0));
    const cf32 echo = std::polar(0.25f, 0.7f);
    auto moreSymbols = [&]() {
        tx.nextFrame(frame.data());
        for (int n = 0; n < 595; n++) frame[(size_t)n] = cf32(a * pn[(size_t)n], a * pn[(size_t)n]);
        for (int n = 0; n < Lf; n++) {
            const cf32 s = frame[(size_t)n];
            sym.push_back(s + echo * prev2[0]);
            prev2[0] = prev2[1]; prev2[1] = s;
        }
    };

    // square-root raised cosine at 10 Msps: 512 phases, 24 symbols on each side, Hann taper
    constexpr int H = 24, P = 512;
    std::vector<float> table((size_t)P * 2 * H);
    for (int ph = 0; ph < P; ph++)
        for (int j = 0; j < 2 * H; j++) {
            const double d = (double)ph / P + (H - 1 - j);
            const double w = std::fabs(d) < H ? 0.5 + 0.5 * std::cos(3.14159265358979 * d / H) : 0.0;
            table[(size_t)ph * 2 * H + (size_t)j] = (float)(srrcPulse(d) * w);
        }
    const double step = kSymbolRate / rate * (1.0 + ppm * 1e-6);
    double t = 1234.37;   // the recording starts somewhere inside a frame
    std::mt19937 rng(7);
    dect2::normal_distribution<float> gauss(0.f, 1.f);
    const float sigma = (float)std::sqrt(0.5 * rate / kSymbolRate * std::pow(10.0, -cnDb / 10.0));
    const double dph = 2.0 * 3.14159265358979 * cfoHz / rate;

    DtmbReceiver rx;
    rx.configure(rate, 8);
    rx.setDecoderThreads(0);
    uint64_t good = 0, wrong = 0;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++) { uint32_t num = 0xFFFFFFFFu; if (checkTestPacket(p + i * 188, seed, &num)) good++; else wrong++; }
    });
    std::vector<std::string> log;
    rx.setLogCallback([&](const std::string& s) { log.push_back(s); });

    const long total = (long)(seconds * rate);
    std::vector<cf32> chunk(16384);
    long n0 = 0;
    while (n0 < total) {
        for (size_t i = 0; i < chunk.size(); i++, n0++) {
            const long k0 = (long)std::floor(t);
            const int ph = std::min(P - 1, (int)((t - (double)k0) * P));
            while (base + (long)sym.size() <= k0 + H) moreSymbols();
            const cf32* s = &sym[(size_t)(k0 - H + 1 - base)];
            const float* w = &table[(size_t)ph * 2 * H];
            cf32 v(0, 0);
            for (int j = 0; j < 2 * H; j++) v += s[j] * w[j];
            v = v * std::polar(1.f, (float)std::fmod(dph * (double)n0, 2.0 * 3.14159265358979)) + cf32{sigma * gauss(rng), sigma * gauss(rng)};
            // 8 bit samples, rms about 12 as in the recordings
            const float re = std::max(-127.f, std::min(127.f, std::round(v.real() * 12.f)));
            const float im = std::max(-127.f, std::min(127.f, std::round(v.imag() * 12.f)));
            chunk[i] = cf32(re / 128.f, im / 128.f);
            t += step;
        }
        rx.feed(chunk.data(), chunk.size());
        const long keep = (long)std::floor(t) - H - 16 - base;
        if (keep > 65536) { sym.erase(sym.begin(), sym.begin() + keep); base += keep; }
    }
    rx.flush();

    DtmbTelemetry tel;
    rx.telemetry(tel, 0);
    for (const auto& s : log) printf("  [log] %s\n", s.c_str());
    printf("%s\n", dtmbSummary(tel).c_str());
    printf("packets: %llu good, %llu wrong\n", (unsigned long long)good, (unsigned long long)wrong);
    CHECK(tel.siOk && tel.siIndex == 10, "system information %d (index %d), expected index 10", (int)tel.siOk, tel.siIndex);
    CHECK(tel.carriers == 1 && tel.header == 1, "carriers %d header %d", tel.carriers, tel.header);
    CHECK(tel.mapping == 1 && tel.rate == 2 && tel.interleaver == 2, "mapping %d rate %d interleaver %d", tel.mapping, tel.rate, tel.interleaver);
    // 0.8 s: about 1380 frames, 510 of them fill the de-interleaver, then four packets a frame
    CHECK(good >= 2000, "only %llu good packets", (unsigned long long)good);
    CHECK(wrong == 0, "%llu wrong packets", (unsigned long long)wrong);
    for (const auto& s : log) CHECK(s.find("swapped") == std::string::npos, "the receiver had to try I and Q swapped");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all DTMB on-air PN595 checks passed\n");
    return 0;
}
