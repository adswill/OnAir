// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by the JT65 / JT9 / Q65 / FST4 / MSK144 tests: audio of one mode with stations at known SNRs (in 2500 Hz), offsets and impairments, a run
// through the HF digital slot decoder with a known start time, and the real recordings of the WSJT-X samples (read from the folder named by the
// environment variable HFDIG_WSJT_SAMPLES: 8 kHz float32 files; the tests skip them when it is not set).
#pragma once
#include "hfdig_ftx_testutil.h"
#include "../../core/src/hfdig/hfdig_wsjt_int.h"
#include <cstdlib>
#include <fstream>

namespace wsjttest {

using namespace dect2;
using ftxtest::Gauss;
using ftxtest::kSigma;
using ftxtest::kT0;
using ftxtest::ampFor;

struct St { std::string msg; double hz; double snrDb; double dt; };

struct Imp {
    double dialHz = 0;      // every tone moves by this
    double ppm = 0;         // the receiver's sample clock runs fast by this
    double drift = 0;       // Hz per second at 1500 Hz: a linear drift of all tones (a Doppler rate; other frequencies drift in proportion)
    double noise = 1;       // times kSigma (0: none)
    double startSec = 0;    // the audio starts this far into the first slot
    int sub = 0;            // JT65 A B C, Q65 A .. E
    double period = 0;      // 0: the default of the mode
};

inline double t0Of(int mode, double period) {
    if (mode == ftx::kMsk144) return 0.0;
    return period > 0 && period < 20 ? 0.5 : 1.0;
}
inline double periodOf(int mode, double period) {
    if (period > 0) return period;
    return ftxPeriod(mode);
}

// slots slots of the mode with every station, plus 4 s
inline std::vector<float> audio(int mode, const std::vector<St>& st, int slots, const Imp& f, uint64_t seed = 1) {
    const double P = periodOf(mode, f.period), t0 = t0Of(mode, P);
    const double len = slots * P + 4.0 - f.startSec;
    std::vector<float> a((size_t)(len * 8000), 0.f);
    const double rate = 8000.0 * (1 + f.ppm * 1e-6);
    for (int k = 0; k < slots; k++)
        for (const auto& s : st) {
            const double hz = s.hz + f.dialHz;
            const double start = k * P + t0 + s.dt - f.startSec;
            if (!ftxAddTransmissionEx(mode, s.msg, hz, ampFor(s.snrDb), rate, start, false, f.sub, f.period, a)) printf("cannot send \"%s\"\n", s.msg.c_str());
        }
    if (f.drift != 0) {   // x'(t) = x(t + eps t^2 / 2): the frequency of a tone f becomes f (1 + eps t); cubic interpolation
        const double eps = f.drift / 1500.0;
        std::vector<float> b(a.size(), 0.f);
        for (size_t n = 0; n < b.size(); n++) {
            const double t = (double)n / 8000.0, tt = (t + 0.5 * eps * t * t) * 8000.0;
            const long i = (long)std::floor(tt);
            const double u = tt - (double)i;
            auto at = [&](long k) { return k >= 0 && k < (long)a.size() ? (double)a[(size_t)k] : 0.0; };
            const double p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
            b[n] = (float)(p1 + 0.5 * u * (p2 - p0 + u * (2 * p0 - 5 * p1 + 4 * p2 - p3 + u * (3 * (p1 - p2) + p3 - p0))));
        }
        a.swap(b);
    }
    if (f.noise > 0) {
        Gauss g(seed);
        for (auto& v : a) v += (float)(kSigma * f.noise * g.g());
    }
    return a;
}

inline std::vector<FtxDecode> decode(const std::vector<float>& a, int mode, const Imp& f, double startUtc = kT0, HfdigFtxTelemetry* tel = nullptr) {
    auto d = makeFtxDecoder();
    for (int m = 0; m < kFtxModes; m++) d->setEnabled(m, m == mode);
    d->setModeOption(mode, f.sub, f.period);
    d->setStartTime(startUtc);
    d->reset();
    for (size_t i = 0; i < a.size(); i += 800) d->feedAudio(&a[i], std::min<size_t>(800, a.size() - i));
    d->waitIdle();
    HfdigFtxTelemetry t;
    d->telemetry(t);
    if (tel) *tel = t;
    return t.decodes;
}

inline const FtxDecode* find(const std::vector<FtxDecode>& d, const std::string& msg) { return ftxtest::find(d, msg); }
inline void print(const std::vector<FtxDecode>& d) { ftxtest::print(d); }

// a real recording: <dir>/<name>.f32 (8 kHz float32); empty when the folder or the file is missing
inline std::vector<float> sample(const std::string& name) {
    const char* dir = std::getenv("HFDIG_WSJT_SAMPLES");
    std::vector<float> v;
    if (!dir) return v;
    std::ifstream in(std::string(dir) + "/" + name + ".f32", std::ios::binary);
    if (!in) return v;
    in.seekg(0, std::ios::end);
    const size_t n = (size_t)in.tellg() / 4;
    in.seekg(0);
    v.resize(n);
    in.read((char*)v.data(), (std::streamsize)(n * 4));
    return v;
}

} // namespace wsjttest
