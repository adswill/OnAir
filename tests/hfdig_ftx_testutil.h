// Shared by the FT8 / FT4 / FT2 / WSPR tests: audio with stations at known SNRs (in 2500 Hz, as WSJT-X), time offsets and frequencies,
// the receiving side's faults (dial error, sample clock error, upside down), and a run through the decoder with a known start time.
#pragma once
#include "dect2/hfdig_ftx.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ftxtest {

using namespace dect2;

constexpr double kT0 = 1767225600.0;   // 2026-01-01 00:00:00 UTC: the start of a slot of every mode
constexpr double kSigma = 0.05;          // noise per sample at 8 kHz (white, 0 .. 4 kHz)

struct Station { std::string msg; double hz; double snrDb; double dt; };

struct Gauss {
    uint64_t s;
    explicit Gauss(uint64_t seed) : s(seed * 2654435761ull + 1) {}
    double u() { s = s * 6364136223846793005ull + 1442695040888963407ull; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

// the amplitude of a tone with this SNR in 2500 Hz against kSigma
inline double ampFor(double snrDb) { return std::sqrt(2 * std::pow(10.0, snrDb / 10) * kSigma * kSigma * 2500.0 / 4000.0); }

struct Faults {
    double dialHz = 0;     // the receiver's dial is off: every tone moves by this
    double ppm = 0;        // the receiver's sample clock runs fast by this
    bool mirrored = false; // the stations are heard upside down
    double noise = 1;      // times kSigma (0: no noise)
    double startSec = 0;   // the audio begins this far into the first slot
};

// audio of `slots` slots of the mode (plus 4 s), every station in every slot
inline std::vector<float> audio(int mode, const std::vector<Station>& st, int slots, const Faults& f, uint64_t seed = 1) {
    const double P = ftxPeriod(mode), t0 = mode == 3 ? 1.0 : 0.5;
    const double len = slots * P + 4.0 - f.startSec;
    std::vector<float> a((size_t)(len * 8000), 0.f);
    const double rate = 8000.0 * (1 + f.ppm * 1e-6);   // a fast receiver clock: the signal is longer in samples and lower in frequency
    for (int k = 0; k < slots; k++)
        for (const auto& s : st) {
            const double hz = s.hz + f.dialHz;
            const double start = k * P + t0 + s.dt - f.startSec;
            if (!ftxAddTransmission(mode, s.msg, hz, ampFor(s.snrDb), rate, start, f.mirrored, a))
                printf("cannot send \"%s\"\n", s.msg.c_str());
        }
    if (f.noise > 0) {
        Gauss g(seed);
        for (auto& v : a) v += (float)(kSigma * f.noise * g.g());
    }
    return a;
}

inline std::vector<FtxDecode> decode(const std::vector<float>& a, int mode, double startUtc, bool search = false, HfdigFtxTelemetry* tel = nullptr,
                                     bool mirrored = true) {
    auto d = makeFtxDecoder();
    for (int m = 0; m < kFtxModes; m++) d->setEnabled(m, m == mode);
    d->setMirrored(mirrored);
    if (search) d->setSlotSearch(true);
    else d->setStartTime(startUtc);
    d->reset();
    for (size_t i = 0; i < a.size(); i += 800) {
        d->feedAudio(&a[i], std::min<size_t>(800, a.size() - i));
        if (search && i % 8000 == 0) d->waitIdle();   // as in real time: a slot search finds the slots before the next ones are cut
    }
    d->waitIdle();
    HfdigFtxTelemetry t;
    d->telemetry(t);
    if (tel) *tel = t;
    return t.decodes;
}

inline const FtxDecode* find(const std::vector<FtxDecode>& d, const std::string& msg) {
    for (const auto& x : d) if (x.msg == msg) return &x;
    return nullptr;
}

inline void print(const std::vector<FtxDecode>& d) {
    for (const auto& x : d)
        printf("  %.0f %-4s %4.0f dB %5.2f s %7.1f Hz %s%s (pass %d)\n", std::fmod(x.slotUtc, 3600.0), ftxModeName(x.mode), x.snrDb, x.dt, x.hz,
               x.msg.c_str(), x.mirrored ? " [mirrored]" : "", x.pass);
}

} // namespace ftxtest
