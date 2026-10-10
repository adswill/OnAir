// The faults of tests/impair.h applied to the GNSS test stream chunk by chunk (gnsstest::Options::hook): the stream is far too long to hold whole.
// - a frequency offset: impair::shift on chunks whose length holds a whole number of its cycles, so that the phase runs on across chunks
// - a sample clock error: impair::clock on windows of exactly L input samples that give exactly M output samples (ppm = (M / L - 1) * 1e6), so the
//   sample times run on across the windows; the windows overlap by the interpolator's taps
// - low C/N0: impair::noise with its own seed for every chunk
// - a start in the middle: the first `skip` samples are thrown away (impair::skip)
#pragma once
#include "dect2/gnss_testkit.h"
#include "impair.h"
#include <deque>
#include <memory>

namespace gnssfault {
using dect2::cf32;

struct Faults {
    double shiftHz = 0;          // must make shiftHz * chunk / rate a whole number
    int clockPpm = 0;            // whole ppm (windows of 1e6 samples)
    double noiseSnrDb = 1e9;     // extra white noise: the SNR over the band against the chunk's own power (0 dB: the noise power doubles)
    size_t skip = 0;             // samples dropped at the start
};

inline std::function<size_t(std::vector<cf32>&, size_t)> hook(const Faults& f, double rate) {
    struct State {
        std::vector<cf32> in;    // input not yet consumed by the clock interpolator (starts H samples before the next window)
        size_t skipped = 0;
        uint32_t seed = 7;
    };
    auto st = std::make_shared<State>();
    const int H = 16;
    const size_t L = 1000000, M = (size_t)((int64_t)L + f.clockPpm);
    return [f, rate, st, H, L, M](std::vector<cf32>& b, size_t) -> size_t {
        if (f.shiftHz != 0) impair::shift(b, f.shiftHz, rate);
        if (f.noiseSnrDb < 1e8) impair::noise(b, f.noiseSnrDb, st->seed++);
        if (st->skipped < f.skip) {
            const size_t n = std::min(f.skip - st->skipped, b.size());
            impair::skip(b, n);
            st->skipped += n;
        }
        if (f.clockPpm == 0) return b.size();
        if (st->in.empty()) st->in.assign((size_t)H, cf32(0.f, 0.f));      // the first window starts with H zeros of history
        st->in.insert(st->in.end(), b.begin(), b.end());
        b.clear();
        // a window: H before, L samples, H + 2 after
        while (st->in.size() >= L + 2 * H + 2) {
            std::vector<cf32> w(st->in.begin(), st->in.begin() + (long)(L + 2 * H + 2));
            std::vector<cf32> y = impair::clock(w, (double)f.clockPpm);
            if (y.size() > M) y.resize(M);
            b.insert(b.end(), y.begin(), y.end());
            st->in.erase(st->in.begin(), st->in.begin() + (long)L);
        }
        return b.size();
    };
}

} // namespace gnssfault
