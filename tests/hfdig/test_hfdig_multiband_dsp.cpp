// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Multi-band monitor (hfdig_multiband.h) on synthetic wideband IQ:
//  1. an RTL-SDR-like capture (960 ksps) centred on 14.085 MHz: FT8 on 14.074, FT4 on 14.080, FT2 on 14.084 and WSPR on 14.0956 MHz at
//     the same time; every mode is decoded on its own dial frequency, from one shared slot clock (one combined list, dialHz per decode)
//  2. a wide capture (10 Msps) centred on 16.087 MHz: FT8 on 20 m (14.074) and on 17 m (18.100) at once; a 40 m selection that the
//     capture does not hold is reported as not covered and produces nothing
// Receive only: the signals are made from the test generator of the decoders (ftxAddTransmission), turned into the analytic signal and
// interpolated up to the capture rate.
#include "dect2/dsp/fftutil.h"
#include "dect2/hfdig/hfdig_multiband.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "dect2/common/mathconst.h"
using namespace dect2;
#include "../common/test_util.h"

using dect2::math::kPi;
constexpr double kT0 = 1767225600.0;   // a multiple of 120 s

struct Rng {
    uint64_t s = 88172645463325252ull;
    double u() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double)(s >> 11) / 9007199254740992.0; }
    double g() { return (u() + u() + u() + u() - 2.0) * 1.7320508; }
};

struct Layer {
    std::vector<cf32> a;
    double offsetHz = 0;                       // dial - capture centre
    std::complex<double> rot{1, 0}, step{1, 0};
    int factor = 1;                            // capture rate / audio rate
};

static Layer makeLayer(int mode, const std::vector<std::pair<std::string, double>>& st, double offsetHz, double rate, int audioRate, double secs) {
    const double P = ftxPeriod(mode), t0 = mode == 3 ? 1.0 : 0.5;
    std::vector<float> au((size_t)(secs * audioRate), 0.f);
    const double sigma = 0.05, amp = std::sqrt(2 * std::pow(10.0, 3.0 / 10) * sigma * sigma * 2500.0 / 4000.0);   // +3 dB
    for (int k = 0; (k + 1) * P <= secs; k++)
        for (const auto& s : st)
            if (!ftxAddTransmission(mode, s.first, s.second, amp, audioRate, k * P + t0, false, au)) printf("cannot send %s\n", s.first.c_str());
    size_t N = 1;
    while (N < au.size()) N <<= 1;
    std::vector<cf32> sp(N);
    for (size_t i = 0; i < N; i++) sp[i] = cf32(i < au.size() ? au[i] : 0.f, 0.f);
    Fft f((int)N);
    f.forward(sp.data());
    for (size_t i = 1; i < N / 2; i++) sp[i] *= 2.f;
    for (size_t i = N / 2 + 1; i < N; i++) sp[i] = cf32(0, 0);
    f.inverse(sp.data());
    for (auto& v : sp) v /= (float)N;
    Layer L; L.a = std::move(sp); L.offsetHz = offsetHz; L.factor = (int)std::lround(rate / audioRate);
    L.step = std::polar(1.0, 2 * kPi * offsetHz / rate);
    return L;
}

static std::vector<std::pair<std::string, double>> stationsOf(int mode) {
    std::vector<std::pair<std::string, double>> r;
    for (const auto& s : ftxTestStations(mode)) r.push_back({s.msg, s.hz});
    return r;
}

// Feeds the capture and returns the decodes (the decoders run on threads of their own: waits for them)
static std::vector<FtxDecode> run(HfdigMultiBand& mb, HfdigFtx& main, std::vector<Layer>& L, double rate, double secs, double& cpu, size_t minDecodes, int needMode, double needDial) {
    Rng rng;
    const size_t total = (size_t)(secs * rate), blk = 240000;
    std::vector<cf32> x(blk);
    size_t n = 0;
    cpu = 0;
    while (n < total) {
        for (size_t i = 0; i < blk; i++, n++) {
            std::complex<double> v(0.7 * rng.g(), 0.7 * rng.g());
            for (Layer& l : L) {
                const size_t i0 = n / (size_t)l.factor;
                const float fr = (float)(n % (size_t)l.factor) / (float)l.factor;
                const cf32 a = i0 + 1 < l.a.size() ? l.a[i0] * (1 - fr) + l.a[i0 + 1] * fr : cf32(0, 0);
                v += std::complex<double>(a.real(), a.imag()) * l.rot;
                l.rot *= l.step;
                if ((n & 1023) == 0) l.rot /= std::abs(l.rot);
            }
            x[i] = cf32((float)v.real(), (float)v.imag());
        }
        const auto t0 = std::chrono::steady_clock::now();
        mb.feed(x.data(), x.size());
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    for (int w = 0; w < 120; w++) {
        HfdigFtxTelemetry ft;
        main.telemetry(ft);
        int have = 0;
        for (const auto& x : ft.decodes) if (x.mode == needMode && std::fabs(x.dialHz - needDial) < 1) have++;
        if (ft.decodes.size() >= minDecodes && ft.pending == 0 && have > 0) break;   // the slot decoders run on threads of their own
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    HfdigFtxTelemetry ft;
    main.telemetry(ft);
    return ft.decodes;
}

static int count(const std::vector<FtxDecode>& d, int mode, double dial) {
    int n = 0;
    for (const auto& x : d) if (x.mode == mode && std::fabs(x.dialHz - dial) < 1) n++;
    return n;
}

int main() {
    // ---------------------------------------------------------------- 1. 20 m: FT8, FT4, FT2 and WSPR on a 960 ksps capture
    {
        const double rate = 960000, centre = 14085000, secs = 124;   // past the end of the first 2-minute WSPR slot
        std::vector<Layer> L;
        L.push_back(makeLayer(0, {{"CQ LY2ABC KO24", 600}, {"LY2ABC OH3XYZ -10", 1400}, {"CQ DL5QRP JO62", 2300}}, 14074000 - centre, rate, 48000, secs));
        L.push_back(makeLayer(1, {{"CQ SM4DDD JO89", 900}, {"SM4DDD LY2ABC R-05", 1800}}, 14080000 - centre, rate, 48000, secs));
        L.push_back(makeLayer(2, stationsOf(2), 14084000 - centre, rate, 48000, secs));
        L.push_back(makeLayer(3, stationsOf(3), 14095600 - centre, rate, 48000, secs));
        auto main = makeFtxDecoder();
        main->setStartTime(kT0);
        HfdigMultiBand mb(*main);
        mb.configure(rate, centre, {{5, 0}, {5, 1}, {5, 2}, {5, 3}, {3, 0}});
        const MbStatus st = mb.status();
        CHECK(st.bands == 1 && st.active.size() == 4 && st.notCovered.size() == 1, "one band, 4 selections in, the 40 m one out (%d, %zu, %zu)", st.bands, st.active.size(), st.notCovered.size());
        CHECK(st.channels == 4, "four slot decoders (one per dial frequency): %d", st.channels);
        double cpu;
        const auto d = run(mb, *main, L, rate, secs, cpu, 12, 3, 14095600);
        const int n8 = count(d, 0, 14074000), n4 = count(d, 1, 14080000), n2 = count(d, 2, 14084000), nw = count(d, 3, 14095600);
        printf("20 m on %.0f ksps: FT8 %d, FT4 %d, FT2 %d, WSPR %d, cpu %.1f s for %.0f s (%.0f %% of one core)\n", rate / 1e3, n8, n4, n2, nw, cpu, secs, cpu / secs * 100);
        for (const auto& x : d) printf("   %s %.4f MHz %+.0f dB dt %+.1f %.0f Hz  %s\n", ftxModeName(x.mode), x.dialHz / 1e6, x.snrDb, x.dt, x.hz, x.msg.c_str());
        CHECK(n8 >= 6, "FT8 on 14.074: %d (3 stations, 8 slots)", n8);
        CHECK(n4 >= 4, "FT4 on 14.080: %d", n4);
        CHECK(n2 >= 1, "FT2 on 14.084: %d", n2);
        CHECK(nw >= 1, "WSPR on 14.0956: %d", nw);
        CHECK(count(d, 0, 7074000) == 0, "nothing for the 40 m selection");
        for (const auto& x : d) CHECK(x.dialHz > 14.0e6 && x.dialHz < 14.35e6, "every decode carries its true dial frequency (%.4f)", x.dialHz / 1e6);
    }
    // ---------------------------------------------------------------- 2. 20 m + 17 m on a 10 Msps capture
    {
        const double rate = 10000000, centre = 16087000, secs = 17;
        std::vector<Layer> L;
        L.push_back(makeLayer(0, {{"CQ LY2ABC KO24", 700}, {"LY2ABC OH3XYZ -10", 1500}}, 14074000 - centre, rate, 50000, secs));
        L.push_back(makeLayer(0, {{"CQ DL5QRP JO62", 900}, {"DL5QRP SM4DDD R-07", 2000}}, 18100000 - centre, rate, 50000, secs));
        auto main = makeFtxDecoder();
        main->setStartTime(kT0);
        HfdigMultiBand mb(*main);
        mb.configure(rate, centre, {{5, 0}, {6, 0}, {3, 0}});
        const MbStatus st = mb.status();
        CHECK(st.bands == 2 && st.active.size() == 2 && st.notCovered.size() == 1, "two bands in, 40 m out (%d, %zu, %zu)", st.bands, st.active.size(), st.notCovered.size());
        double cpu;
        const auto d = run(mb, *main, L, rate, secs, cpu, 4, 0, 18100000);
        const int a = count(d, 0, 14074000), b = count(d, 0, 18100000);
        printf("20 m + 17 m on %.0f Msps: %d + %d decodes, cpu %.1f s for %.0f s (%.0f %% of one core)\n", rate / 1e6, a, b, cpu, secs, cpu / secs * 100);
        for (const auto& x : d) printf("   %s %.4f MHz %+.0f dB %.0f Hz  %s\n", ftxModeName(x.mode), x.dialHz / 1e6, x.snrDb, x.hz, x.msg.c_str());
        CHECK(a >= 2, "20 m decodes: %d", a);
        CHECK(b >= 2, "17 m decodes: %d", b);
        CHECK(count(d, 0, 7074000) == 0, "nothing for 40 m");
    }
    // ---------------------------------------------------------------- 3. through the receiver: dial 14.077 MHz with the dial 20 kHz below the
    // centre of the capture (setSignalOffset), FT8 on 14.074 and FT4 on 14.080 selected: the receiver finds the centre from dial - offset
    {
        const double rate = 960000, dial = 14077000, off = -20000, centre = dial - off, secs = 34;
        std::vector<Layer> L;
        L.push_back(makeLayer(0, {{"CQ LY2ABC KO24", 600}, {"LY2ABC OH3XYZ -10", 1400}}, 14074000 - centre, rate, 48000, secs));
        L.push_back(makeLayer(1, {{"CQ SM4DDD JO89", 900}, {"SM4DDD LY2ABC R-05", 1800}}, 14080000 - centre, rate, 48000, secs));
        HfdigReceiver r;
        r.setSilent(true);
        r.setDecoders(8);
        r.ftx().setStartTime(kT0);
        r.setDialHz(dial);
        r.configure(rate);
        r.setSignalOffset(off);
        r.setMultiBand(true, {{5, 0}, {5, 1}});
        Rng rng;
        std::vector<cf32> x(240000);
        size_t n = 0;
        HfdigTelemetry tel; uint64_t seq = 0;
        while (n < (size_t)(secs * rate)) {
            for (size_t i = 0; i < x.size(); i++, n++) {
                std::complex<double> v(0.7 * rng.g(), 0.7 * rng.g());
                for (Layer& l : L) {
                    const size_t i0 = n / (size_t)l.factor;
                    const float fr = (float)(n % (size_t)l.factor) / (float)l.factor;
                    const cf32 a = i0 + 1 < l.a.size() ? l.a[i0] * (1 - fr) + l.a[i0 + 1] * fr : cf32(0, 0);
                    v += std::complex<double>(a.real(), a.imag()) * l.rot;
                    l.rot *= l.step;
                    if ((n & 1023) == 0) l.rot /= std::abs(l.rot);
                }
                x[i] = cf32((float)v.real(), (float)v.imag());
            }
            r.feed(x.data(), x.size());
            HfdigTelemetry tt;
            if (r.telemetry(tt, seq)) { seq = tt.seq; tel = std::move(tt); }
        }
        for (int w = 0; w < 80; w++) {
            HfdigFtxTelemetry ft;
            r.ftx().telemetry(ft);
            if (count(ft.decodes, 0, 14074000) >= 4 && count(ft.decodes, 1, 14080000) >= 4 && ft.pending == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        HfdigFtxTelemetry ft;
        r.ftx().telemetry(ft);
        const int a = count(ft.decodes, 0, 14074000), b = count(ft.decodes, 1, 14080000);
        printf("receiver, offset %.0f Hz: FT8 %d on 14.074, FT4 %d on 14.080; monitor on %d, centre %.4f MHz, %d dial frequencies\n", off, a, b, (int)tel.multiBandOn, tel.multiBand.centreHz / 1e6, tel.multiBand.channels);
        CHECK(tel.multiBandOn && std::fabs(tel.multiBand.centreHz - centre) < 1, "the receiver builds the monitor around dial - offset (%.1f)", tel.multiBand.centreHz);
        CHECK(a >= 2 && b >= 2, "decodes through the receiver: FT8 %d, FT4 %d", a, b);
        CHECK(count(ft.decodes, 0, dial) == 0 && count(ft.decodes, 1, dial) == 0, "none stamped with the tuned dial (the main decoder is silent while the monitor runs)");
    }
    return testSummary("hfdig_multiband_dsp");
}
