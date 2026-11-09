// WINMOR pilot finder (see winmor.h). Sections: 5 and 6.1 of the WINMOR protocol specification Rev 1.0.6: Pilot := Ptun & Pfsync & Pfty.
#include "dect2/xlink/winmor.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2::xlink::winmor {

using dect2::math::kPi;

namespace {
constexpr int kSym = 128;                       // samples of one 93.75 baud symbol at 12 kHz
constexpr int kWin = kSym * 20;                 // correlation window: 20 symbols
constexpr int kNeedPre = kSym * 8;              // symbols of pilot that must have passed before a trigger is taken
const double kFskTones[8] = {1312.5, 1359.375, 1406.25, 1453.125, 1546.875, 1593.75, 1640.625, 1687.5};  // the two 4FSK groups near the carrier (the text's 1358.375 is taken as a misprint of the 46.875 Hz grid)
constexpr int kFft2 = 4096;
}

void PilotFinder::reset() { b_.clear(); base_ = n_ = hold_ = 0; C_ = 0; P_ = 0; trig_ = -1; run_ = 0; }

void PilotFinder::feed(const cf32* x, size_t count) {
    for (size_t i = 0; i < count; i++) {
        b_.push_back(x[i]);
        const int64_t m = n_++;
        if (trig_ >= 0 && m >= trig_ + kSym * 34) { analyse(trig_); trig_ = -1; }
        const cf32 now = x[i];
        if (m >= kSym) { const cf32 old = b_[(size_t)(m - base_ - kSym)]; C_ += std::complex<double>(now) * std::conj(std::complex<double>(old)); }
        P_ += (double)std::norm(now);
        if (m >= kWin) { const cf32 a = b_[(size_t)(m - base_ - kWin)]; P_ -= (double)std::norm(a); if (m >= kWin + kSym) { const cf32 a2 = b_[(size_t)(m - base_ - kWin - kSym)]; C_ -= std::complex<double>(a) * std::conj(std::complex<double>(a2)); } }
        if (m < kSym || m < hold_ || trig_ >= 0 || P_ < 1e-12) continue;
        const double mag = std::abs(C_) / P_;
        const double arg = std::arg(C_);
        // an alternating carrier: consecutive symbols opposite, whatever a small tuning error adds
        (void)arg;
        if (mag > 0.75) run_++; else run_ = 0;
        if (run_ >= kNeedPre + kSym * 4 && trig_ < 0) { trig_ = m; hold_ = m + kSym * 45; run_ = 0; }
    }
    constexpr int64_t kKeep = kSym * 60;
    if (n_ - base_ > kKeep * 2) { const int64_t cut = n_ - kKeep - base_; b_.erase(b_.begin(), b_.begin() + (ptrdiff_t)cut); base_ += cut; }
}

void PilotFinder::analyse(int64_t m) {
    if (m - base_ < 8 * kSym) return;
    // frequency of the pilot: y^2 removes the BPSK and leaves a tone at twice the carrier
    static Fft fft(kFft2);
    std::vector<cf32> v(kFft2);
    const int avail = (int)std::min<int64_t>(kFft2, m - base_);
    std::fill(v.begin(), v.end(), cf32(0, 0));
    for (int i = 0; i < avail; i++) { const cf32 y = b_[(size_t)(m - base_ - avail + i)]; v[(size_t)i] = y * y; }
    fft.forward(v.data());
    int kBest = -1; double best = 0;
    for (int k = (int)(2600.0 / kRate * kFft2); k <= (int)(3400.0 / kRate * kFft2); k++) { const double e = std::norm(v[(size_t)k]); if (e > best) { best = e; kBest = k; } }
    if (kBest < 0) return;
    double avg = 0; int cnt = 0;
    for (int k = 0; k < kFft2 / 2; k++) { avg += std::norm(v[(size_t)k]); cnt++; }
    avg /= cnt;
    if (best < 30 * avg) return;
    const double f2 = (double)kBest * kRate / kFft2;
    const double off = f2 / 2 - 1500.0;
    if (std::fabs(off) > 200) return;

    // symbol timing: the sample offset that maximises the alternating-phase consistency over the last 8 symbols
    const int64_t c0 = m - 8 * kSym;
    int bestT = 0; double bm = -1e30;
    const double w = -2 * kPi * (1500.0 + off) / kRate;
    auto rot = [&](int64_t idx, int64_t r) { const std::complex<double> s(b_[(size_t)(idx - base_)]); return s * std::polar(1.0, w * (double)(idx - r)); };
    auto symAt = [&](int64_t start) { std::complex<double> a(0, 0); for (int j = 20; j < kSym - 20; j++) a += rot(start + j, start); return a; };
    for (int t = 0; t < kSym; t += 4) {
        double me = 0;
        std::complex<double> prev = symAt(c0 + t);
        const double base2 = 2 * kPi * (1500.0 + off) * kSym / kRate;
        for (int s = 1; s < 7; s++) { std::complex<double> a = symAt(c0 + t + s * kSym); me += std::real(a * std::conj(prev) * std::polar(1.0, -base2)) < 0 ? 1 : -1; prev = a; }
        if (me > bm) { bm = me; bestT = t; }
    }
    if (bm < 4) return;                         // not an alternating carrier (a plain tone, for instance)
    // walk forward symbol by symbol until the alternation stops (the frame sync symbol)
    int64_t pos = c0 + bestT;
    const double base2 = 2 * kPi * (1500.0 + off) * kSym / kRate;
    std::complex<double> prev = symAt(pos);
    int count = 1;
    int64_t syncAt = -1;
    for (int s = 0; s < 40; s++) {
        const int64_t p = pos + (s + 1) * kSym;
        if (p + kSym >= n_) return;
        std::complex<double> a = symAt(p);
        const double dot = std::real(a * std::conj(prev) * std::polar(1.0, -base2));
        if (dot > 0) { syncAt = p; break; }
        count++;
        prev = a;
    }
    if (syncAt < 0) return;
    Pilot pl;
    pl.offsetHz = off;
    pl.symbols = count;
    pl.metric = (float)bm;
    // four 4FSK symbols of 256 samples after the frame sync symbol: the strongest of the eight candidate tones in each (raw)
    const int64_t f0 = syncAt + kSym;
    for (int s = 0; s < 4; s++) {
        const int64_t st = f0 + s * 256;
        if (st + 256 > n_) break;
        double bt = -1; int bi = -1;
        for (int t = 0; t < 8; t++) {
            std::complex<double> a(0, 0);
            const double wt = -2 * kPi * (kFskTones[t] + off) / kRate;
            for (int j = 0; j < 256; j++) a += std::complex<double>(b_[(size_t)(st + j - base_)]) * std::polar(1.0, wt * j);
            const double e = std::norm(a);
            if (e > bt) { bt = e; bi = t; }
        }
        pl.tones[(size_t)s] = bi;
    }
    if (pl.tones[3] < 0) return;
    if (cb_) cb_(pl);
}

std::vector<cf32> txPilotFrame(int pilotSymbols, const std::array<int, 4>& tones, double offsetHz, int extra) {
    std::vector<cf32> out;
    const double w = 2 * kPi * (1500.0 + offsetHz) / kRate;
    double ph = 0.4;
    double level = 1.0;
    auto emit = [&](double fHz, int nSamples, double amp) { const double wn = 2 * kPi * (fHz + offsetHz) / kRate; for (int j = 0; j < nSamples; j++) { const double e = amp * std::sin(kPi * (j + 0.5) / nSamples); out.push_back(cf32((float)(e * std::cos(ph)), (float)(e * std::sin(ph)))); ph += wn; } };
    (void)w;
    for (int s = 0; s < pilotSymbols; s++) { emit(1500.0, kSym, level); level = -level; }
    // frame sync symbol: same phase as the last alternating one (no further inversion)
    level = -level;
    emit(1500.0, kSym, level);
    for (int s = 0; s < 4; s++) emit(kFskTones[tones[(size_t)s] & 7], 256, 1.0);
    for (int s = 0; s < extra; s++) emit(kFskTones[(unsigned)(s * 3 + 2) % 8], 256, 1.0);
    return out;
}

} // namespace dect2::xlink::winmor
