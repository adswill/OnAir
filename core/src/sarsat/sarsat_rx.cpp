// COSPAS-SARSAT 406 MHz receiver (see sarsat_rx.h).
//
// Burst finder: the stream is mixed to the dial offset, averaged down to 32 kHz and cut into blocks of 1024 samples (32 ms); a Hann-windowed FFT of each block
// is searched for one line that stands out from the median bin (the 160 ms unmodulated preamble) and that was not already there a moment ago (radio DC spikes
// and carriers do not count). A burst runs from the first block with a line until 6 blocks without one. Its carrier frequency is the median of the first lines.
// Demodulator: the burst is rotated to zero frequency; each bit is two half-bit vector sums, and the angle between them (Manchester phase modulation of
// +-1.1 rad turns the carrier phase by +2.2 rad between the halves of a bit, whatever the carrier phase is) gives the bit. Bit timing, bit rate (+-1 %) and
// polarity are found by sliding the 24 sync bits over the burst; the best candidates are decoded and the one the BCH check repairs with the fewest changes wins.
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/sarsat/sarsat_tel.h"
#include "dect2/sarsat/sgb_rx.h"
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <ctime>
#include <mutex>

namespace dect2 {

using dect2::math::kPi;
using sarsat::kFs;
using sarsat::kBitRate;

ModeTuning sarsatTuning() {
    ModeTuning t;
    t.stdMode = 94;
    t.id = "sarsat";
    t.name = "COSPAS-SARSAT 406 MHz";
    t.minMhz = 406.0;
    t.maxMhz = 406.1;
    t.defMhz = sarsat::kDefaultMhz;
    t.sampleRate = 256000;
    t.basebandHz = 16000;
    t.bandwidthMhz = 0.1;
    t.minSampleRate = 128000;
    t.tuneOffsetHz = 20000;
    return t;
}

namespace sarsat {

namespace {

using cd = std::complex<double>;

struct Cand {
    int matches = 0;
    double conf = 0;
    double t0 = 0, r = 0;
    bool inv = false, test = false;
};

struct Slicer {
    std::vector<cd> P;   // prefix sums of the rotated burst
    size_t n = 0;
    bool coherent = true;   // false: the 3.9 differential message bits
    double costPerBit = kRepairCostPerBit;
    bool soft(double b0, double spb, double& d) const {
        const double bm = b0 + spb * 0.5, b1 = b0 + spb;
        if (b0 < 0 || b1 > (double)n) return false;
        const size_t i0 = (size_t)std::lround(b0), im = (size_t)std::lround(bm), i1 = std::min(n, (size_t)std::lround(b1));
        const cd h1 = P[im] - P[i0], h2 = P[i1] - P[im];
        const double m = std::abs(h1) * std::abs(h2);
        if (m <= 1e-12) { d = 0; return true; }
        d = (h1 * std::conj(h2)).imag() / m;
        return true;
    }
    // 4.0: coherent bits. The +-1.1 rad Manchester modulation leaves cos(1.1) of the carrier in every bit: the sum of a bit's two half-bit vectors is
    // that carrier whatever the bit is. Its average over 21 bits (the preamble's bare carrier before the sync included) is the phase reference, and
    // the bit is the projection of the difference of the halves on it: coherent detection of the data, against the product of the two noisy halves
    // that the differential slicer forms. Scaled to a log-likelihood ratio from the burst's own spread, so that bursts of different strength add right.
    std::vector<double> coherentBits(double t0, double spb, int count, bool llr = true) const {
        constexpr int kPre = 20, kW = 10;
        std::vector<cd> car, dif;
        int first = -kPre;
        while (first < 0 && t0 + first * spb < 0) first++;
        for (int k = first; k < count; k++) {
            const double b0 = t0 + k * spb, b1 = b0 + spb;
            if (b1 > (double)n) break;
            const size_t i0 = (size_t)std::lround(b0), im = (size_t)std::lround(b0 + spb * 0.5), i1 = std::min(n, (size_t)std::lround(b1));
            const cd h1 = P[im] - P[i0], h2 = P[i1] - P[im];
            car.push_back(h1 + h2); dif.push_back(h1 - h2);
        }
        const int off = -first, m = (int)car.size();
        std::vector<double> y;
        for (int k = off; k < m; k++) {
            cd ref(0, 0);
            for (int j = std::max(0, k - kW); j <= std::min(m - 1, k + kW); j++) ref += car[(size_t)j];
            const double a = std::abs(ref);
            y.push_back(a > 1e-12 ? (dif[(size_t)k] * std::conj(ref)).imag() / a : 0.0);
        }
        if (y.empty() || !llr) return y;
        double m1 = 0, m2 = 0;
        for (double v : y) { m1 += std::fabs(v); m2 += v * v; }
        m1 /= (double)y.size(); m2 /= (double)y.size();
        const double var = std::max(m2 - m1 * m1, 1e-3 * m2 + 1e-30);
        for (double& v : y) v *= 2 * m1 / var;
        return y;
    }
    // the first `count` bits starting at t0 with bit period spb; stops at the end of the burst
    std::vector<double> bits(double t0, double spb, int count) const {
        std::vector<double> v;
        for (int k = 0; k < count; k++) {
            double d;
            if (!soft(t0 + k * spb, spb, d)) break;
            v.push_back(d);
        }
        return v;
    }
};

Cand syncScore(const Slicer& s, double t0, double r) {
    Cand c;
    c.t0 = t0; c.r = r;
    const double spb = kFs / kBitRate * (1 + r);
    const std::vector<double> d = s.bits(t0, spb, kSyncBits);
    if (d.size() < (size_t)kSyncBits) return c;
    for (int test = 0; test < 2; test++) {
        const std::vector<uint8_t> ref = syncBits(test != 0);
        for (int inv = 0; inv < 2; inv++) {
            int m = 0;
            double conf = 0;
            for (int k = 0; k < kSyncBits; k++) {
                const int want = (ref[(size_t)k] ^ inv) ? 1 : -1;
                const int got = d[(size_t)k] > 0 ? 1 : -1;
                if (want == got) m++;
                conf += want * d[(size_t)k];
            }
            if (m > c.matches || (m == c.matches && conf > c.conf)) { c.matches = m; c.conf = conf; c.inv = inv != 0; c.test = test != 0; }
        }
    }
    return c;
}

// a real burst starts with 160 ms of bare carrier: the two halves of the 20 bit periods before the sync point the same way (cosine of the angle near 1);
// data and noise do not do that. This keeps a burst that was caught in the middle, or noise, from passing as a message.
bool preambleOk(const Slicer& s, double t0, double spb) {
    double cs = 0;
    int cnt = 0;
    for (int k = 1; k <= 20; k++) {
        const double b0 = t0 - k * spb;
        if (b0 < 0) break;
        const size_t i0 = (size_t)std::lround(b0), im = (size_t)std::lround(b0 + spb * 0.5), i1 = (size_t)std::lround(b0 + spb);
        const cd h1 = s.P[im] - s.P[i0], h2 = s.P[i1] - s.P[im];
        const double m = std::abs(h1) * std::abs(h2);
        if (m > 1e-12) { cs += (h1 * std::conj(h2)).real() / m; cnt++; }
    }
    return cnt >= 16 && cs / cnt >= 0.35;
}

// 4.0: what the BCH repair cost in soft terms. A bit the noise flipped usually had a small soft value; a decoder that lands on a wrong codeword
// (4 or more errors in BCH-1) flips bits of ordinary confidence. Sum of |soft| over the repaired bits of the message over the mean |soft|.
template <class T> float repairCost(const std::vector<T>& d, const std::vector<int>& fixedBits, int nb) {
    double mean = 0;
    int cnt = 0;
    for (int k = kSyncBits; k < nb && k < (int)d.size(); k++) { mean += std::fabs((double)d[(size_t)k]); cnt++; }
    if (cnt == 0 || mean <= 0) return 0;
    mean /= cnt;
    double c = 0;
    for (int b : fixedBits) if (b - 1 < (int)d.size()) c += std::fabs((double)d[(size_t)(b - 1)]);
    return (float)(c / mean);
}

// 4.0: soft-decision (Chase) decoding of the coherent bits. The 7 least reliable bits of the BCH-1 part are flipped in all 128 combinations, each word is
// repaired by the BCH codes, and the codeword closest to the soft values (the sum of |soft| over the bits it changes) wins. This repairs up to 3 more bits
// than the hard decoder when they are the doubtful ones, and gives a margin: how much closer the winner is than the next different codeword, in units of
// the mean |soft|. A word of noise or a burst with too many errors has codewords at nearly the same distance (margin near 0).
template <class T> bool chaseDecode(const std::vector<T>& y, const std::vector<uint8_t>& hard, Beacon& best, std::vector<uint8_t>& bestBits, float& margin) {
    const int n0 = (int)std::min(y.size(), hard.size());
    if (n0 < kShortBits) return false;
    double mean = 0;
    for (int k = kSyncBits; k < n0; k++) mean += std::fabs((double)y[(size_t)k]);
    mean /= std::max(1, n0 - kSyncBits);
    if (mean <= 0) return false;
    constexpr int kFirst = 24, kLast = 105, kL = 7;   // the BCH-1 code word: bits 25 - 106 (index 24 - 105); bit 107 on is PDF-2 with its own code
    std::vector<int> idx;
    for (int k = kFirst; k <= kLast && k < n0; k++) idx.push_back(k);
    std::partial_sort(idx.begin(), idx.begin() + kL, idx.end(), [&](int a, int b) { return std::fabs((double)y[(size_t)a]) < std::fabs((double)y[(size_t)b]); });
    double d1 = 1e300, d2 = 1e300;
    for (int pat = 0; pat < (1 << kL); pat++) {
        std::vector<uint8_t> b = hard;
        for (int i = 0; i < kL; i++) if (pat >> i & 1) b[(size_t)idx[(size_t)i]] ^= 1;
        Beacon t;
        if (!decode(b, t) || t.country.empty()) continue;
        const int nb = std::min(n0, t.longMsg ? kLongBits : kShortBits);
        double dist = 0;
        for (int k = kSyncBits; k < nb; k++) if ((b[(size_t)k] & 1) != (hard[(size_t)k] & 1)) dist += std::fabs((double)y[(size_t)k]);
        if (!bestBits.empty() && std::equal(b.begin() + kFirst, b.begin() + kLast + 1, bestBits.begin() + kFirst)) continue;   // the same codeword again
        if (dist < d1) { d2 = d1; d1 = dist; bestBits = b; best = t; }
        else if (dist < d2) d2 = dist;
    }
    if (bestBits.empty()) return false;
    margin = d2 > 1e299 ? 99.f : (float)((d2 - d1) / mean);
    int flips = 0;
    const int nb = std::min(n0, best.longMsg ? kLongBits : kShortBits);
    for (int k = 0; k < nb; k++) flips += (bestBits[(size_t)k] & 1) != (hard[(size_t)k] & 1);
    best.corrected = flips;
    return true;
}

bool tryDecode(const Slicer& s, const Cand& c, Beacon& out, int& matches) {
    const double spb = kFs / kBitRate * (1 + c.r);
    const std::vector<double> d = s.coherent ? s.coherentBits(c.t0, spb, kLongBits) : s.bits(c.t0, spb, kLongBits);
    if (d.size() < (size_t)kShortBits) return false;
    std::vector<uint8_t> b(d.size());
    for (size_t k = 0; k < d.size(); k++) b[k] = (uint8_t)((d[k] > 0) != c.inv);
    if (!preambleOk(s, c.t0, spb)) return false;
    // the sync bits must be the ones found
    const std::vector<uint8_t> ref = syncBits(c.test);
    matches = 0;
    for (int k = 0; k < kSyncBits; k++) matches += b[(size_t)k] == ref[(size_t)k];
    if (matches < 23) return false;
    Beacon o;
    const std::vector<uint8_t> received = b;
    if (s.coherent) {
        std::vector<uint8_t> cb;
        float margin = 0;
        if (!chaseDecode(d, received, o, cb, margin)) return false;
        b = cb;
        o.repairMargin = margin;
        if (s.costPerBit < 1e8 && o.corrected > 0 && margin < kRepairMargin) return false;
    } else if (!decode(b, o)) return false;
    // BCH-1 repairs any 82-bit word within 3 bits of a codeword (4.4 % of random words), and this search tries thousands of timings/rates/polarities.
    // So a candidate that needed the full repair must at least have a perfect sync word, a country code that exists and, for a long message,
    // an intact second check; otherwise it is noise or a second-generation burst (issue #404).
    if (o.country.empty()) return false;
    if (o.corrected >= 3 && matches < kSyncBits) return false;
    if (o.corrected >= 2 && o.longMsg && !o.bch2Ok) return false;
    o.selfTest = c.test;
    o.rawHex = bitsToHex(b, 1, o.longMsg ? kLongBits : kShortBits);
    o.rxHex = bitsToHex(received, 1, o.longMsg ? kLongBits : kShortBits);
    for (int i = 0; i < (o.longMsg ? kLongBits : kShortBits) && i < (int)b.size() && i < (int)received.size(); i++) if ((b[(size_t)i] & 1) != (received[(size_t)i] & 1)) o.fixedBits.push_back(i + 1);
    o.repairCost = repairCost(d, o.fixedBits, o.longMsg ? kLongBits : kShortBits);
    if (s.coherent && o.corrected > 0 && o.repairCost > s.costPerBit * o.corrected) return false;
    out = o;
    return true;
}

}  // namespace

bool demodBurst2(const std::vector<cf32>& x, double carrierHz, Beacon& out, SoftBurst* failed, bool coherent, double costPerBit);
bool demodBurst(const std::vector<cf32>& x, double carrierHz, Beacon& out) { return demodBurst(x, carrierHz, out, nullptr, true); }

bool decodeCombined(const std::vector<float>& soft, Beacon& out, double costPerBit) {
    if (soft.size() < (size_t)kShortBits) return false;
    std::vector<uint8_t> b(soft.size());
    for (size_t k = 0; k < soft.size(); k++) b[k] = soft[k] > 0 ? 1 : 0;
    int best = 0;
    bool test = false;
    for (int t = 0; t < 2; t++) {
        const std::vector<uint8_t> ref = syncBits(t != 0);
        int m = 0;
        for (int k = 0; k < kSyncBits; k++) m += b[(size_t)k] == ref[(size_t)k];
        if (m > best) { best = m; test = t != 0; }
    }
    if (best < kSyncBits - 1) return false;
    const std::vector<uint8_t> received = b;
    Beacon o;
    {
        std::vector<uint8_t> cb;
        float margin = 0;
        if (!chaseDecode(soft, received, o, cb, margin)) return false;
        b = cb;
        o.repairMargin = margin;
        if (costPerBit < 1e8 && o.corrected > 0 && margin < kRepairMargin) return false;
    }
    if (o.country.empty() || (costPerBit < 1e8 && o.corrected > 6)) return false;
    if (o.longMsg && o.corrected >= 2 && !o.bch2Ok) return false;
    if (o.corrected >= 2 && best < kSyncBits) return false;
    o.selfTest = test;
    const int nb = o.longMsg ? kLongBits : kShortBits;
    o.rawHex = bitsToHex(b, 1, nb);
    o.rxHex = bitsToHex(received, 1, nb);
    for (int i = 0; i < nb && i < (int)b.size(); i++) if ((b[(size_t)i] & 1) != (received[(size_t)i] & 1)) o.fixedBits.push_back(i + 1);
    o.repairCost = repairCost(soft, o.fixedBits, nb);
    if (o.corrected > 0 && o.repairCost > costPerBit * o.corrected) return false;
    out = o;
    return true;
}

bool demodBurst(const std::vector<cf32>& x, double carrierHz, Beacon& out, SoftBurst* failed, bool coherent) { return demodBurst2(x, carrierHz, out, failed, coherent, kRepairCostPerBit); }

bool demodBurst2(const std::vector<cf32>& x, double carrierHz, Beacon& out, SoftBurst* failed, bool coherent, double costPerBit) {
    const size_t n = x.size();
    if (n < (size_t)(kFs * 0.4)) return false;
    Slicer s;
    s.coherent = coherent;
    s.costPerBit = costPerBit;
    s.n = n;
    s.P.assign(n + 1, cd(0, 0));
    cd lo(1, 0);
    const double w = -2 * kPi * carrierHz / kFs;
    const cd st(std::cos(w), std::sin(w));
    for (size_t i = 0; i < n; i++) {
        s.P[i + 1] = s.P[i] + cd(x[i].real(), x[i].imag()) * lo;
        lo *= st;
        if ((i & 1023) == 1023) lo /= std::abs(lo);
    }
    const double spb0 = kFs / kBitRate;
    const double rates[] = {-0.01, -0.0075, -0.005, -0.0025, 0.0, 0.0025, 0.005, 0.0075, 0.01};
    std::vector<Cand> cands;
    for (double r : rates) {
        const double spb = spb0 * (1 + r);
        for (double t0 = 0; t0 + kSyncBits * spb < (double)n; t0 += 4) {
            const Cand c = syncScore(s, t0, r);
            if (c.matches >= (coherent ? 19 : 22)) cands.push_back(c);
        }
    }
    if (cands.empty()) return false;
    // 4.0 (coherent): the differential sync search only proposes timings (19 of 24 is enough to be looked at); the sync word is then checked on the coherent bits
    if (coherent) std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.conf > b.conf; });
    else std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.matches != b.matches ? a.matches > b.matches : a.conf > b.conf; });
    if (cands.size() > (coherent ? 16u : 10u)) cands.resize(coherent ? 16 : 10);
    bool found = false;
    int bestCorr = 99;
    double bestConf = -1;
    Cand softC;               // for repeat combining: the candidate with the best sync correlation that has the bare-carrier preamble before it
    double softConf = -1;
    std::vector<std::pair<double, Cand>> softList;   // coherent: every timing that passed the sync and preamble tests, with its message quality
    for (const Cand& c0 : cands) {
        for (double dt = -4; dt <= 4; dt += 1) {
            for (double dr = -0.0025; dr <= 0.0026; dr += 0.0025) {
                Cand c = syncScore(s, c0.t0 + dt, c0.r + dr);
                if (coherent) {
                    // the sync word, polarity and test flag read from the coherent bits; the message quality (mean |projection|) picks the timing kept for combining
                    const double spb = kFs / kBitRate * (1 + c.r);
                    const std::vector<double> y = s.coherentBits(c.t0, spb, kLongBits, false);
                    if (y.size() < (size_t)kShortBits) continue;
                    Cand cc = c;
                    cc.matches = 0; cc.conf = 0;
                    for (int test = 0; test < 2; test++) {
                        const std::vector<uint8_t> ref = syncBits(test != 0);
                        for (int inv = 0; inv < 2; inv++) {
                            int m = 0;
                            double conf = 0;
                            for (int k = 0; k < kSyncBits; k++) {
                                const int want = (ref[(size_t)k] ^ inv) ? 1 : -1;
                                if ((y[(size_t)k] > 0 ? 1 : -1) == want) m++;
                                conf += want * y[(size_t)k];
                            }
                            if (m > cc.matches || (m == cc.matches && conf > cc.conf)) { cc.matches = m; cc.conf = conf; cc.inv = inv != 0; cc.test = test != 0; }
                        }
                    }
                    c = cc;
                    if (failed && c.matches >= 20) {
                        double q = 0;
                        for (double v : y) q += std::fabs(v);
                        if (preambleOk(s, c.t0, spb)) {
                            softList.emplace_back(q, c);
                            if (q > softConf) { softConf = q; softC = c; }
                        }
                    }
                } else if (failed && c.matches >= 22) {
                    // the timing and bit rate that make the whole message look most like +-2.2 rad steps (the 24 sync bits alone do not tell a
                    // 1 % rate error, which slips the end of a long message by more than a bit)
                    const double spb = kFs / kBitRate * (1 + c.r);
                    const std::vector<double> d = s.bits(c.t0, spb, kLongBits);
                    double q = 0;
                    for (double v : d) q += std::fabs(v);   // the differential values: bounded, comparable between candidates
                    if (d.size() >= (size_t)kShortBits && q > softConf && preambleOk(s, c.t0, spb)) { softConf = q; softC = c; }
                }
                if (c.matches < 23) continue;
                Beacon o;
                int m = 0;
                if (!tryDecode(s, c, o, m)) continue;
                if (o.corrected < bestCorr || (o.corrected == bestCorr && c.conf > bestConf)) { bestCorr = o.corrected; bestConf = c.conf; out = o; found = true; }
            }
        }
    }
    if (!found && failed && softConf > 0) {
        const double spb = kFs / kBitRate * (1 + softC.r);
        const std::vector<double> d = s.coherent ? s.coherentBits(softC.t0, spb, kLongBits) : s.bits(softC.t0, spb, kLongBits);
        if (d.size() >= (size_t)kShortBits) {
            failed->soft.resize(d.size());
            for (size_t k = 0; k < d.size(); k++) failed->soft[k] = (float)(softC.inv ? -d[k] : d[k]);
            failed->test = softC.test;
            failed->conf = (float)(softC.conf / kSyncBits);
            // up to 5 more timings, best quality first, each at least a quarter bit or one rate step from those taken, or of the other polarity
            std::sort(softList.begin(), softList.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::vector<Cand> taken{softC};
            for (const auto& e : softList) {
                if (failed->alt.size() >= 5) break;
                const Cand& c = e.second;
                bool near = false;
                for (const Cand& t : taken) if (t.inv == c.inv && std::fabs(t.t0 - c.t0) < 0.25 * spb && std::fabs(t.r - c.r) < 0.002) near = true;
                if (near) continue;
                const std::vector<double> v = s.coherentBits(c.t0, kFs / kBitRate * (1 + c.r), kLongBits);
                if (v.size() < (size_t)kShortBits) continue;
                std::vector<float> f(v.size());
                for (size_t k = 0; k < v.size(); k++) f[k] = (float)(c.inv ? -v[k] : v[k]);
                failed->alt.push_back(std::move(f));
                taken.push_back(c);
            }
        }
    }
    return found;
}

}  // namespace sarsat

struct SarsatReceiver::Impl {
    std::mutex mu;
    SarsatTelemetry tel;
    std::function<void(const std::string&)> log;
    double fs = 0, offHz = 0;
    bool rateSet = false;
    // whole-capture search: child receivers 90 kHz apart
    bool wide = false, kidsDirty = true;
    std::vector<std::unique_ptr<SarsatReceiver>> kids;
    std::vector<double> kidShift;              // each child's centre from the dial
    std::vector<uint64_t> kidSeq;
    std::vector<SarsatEntry> pending;          // decoded in some channel, waiting 1.5 s for a better copy of the same burst
    double wideSpan = 0;

    // mixer and the average down to 32 kHz
    double loRe = 1, loIm = 0, stRe = 1, stIm = 0;
    int loCount = 0;
    double accRe = 0, accIm = 0;
    int accN = 0;
    uint64_t nIn = 0, k32 = 0;
    double binEnd = 0;

    // second-generation beacons (C/S T.018): a second mixer (406.050 MHz to zero) and an average down to 153.6 kHz, needs an input rate of 160 kHz or more
    sarsat::SgbDetector sgb;
    double sRe = 1, sIm = 0, sStRe = 1, sStIm = 0, sAccRe = 0, sAccIm = 0, sBinEnd = 0;
    int sCount = 0, sAccN = 0;
    uint64_t sIn = 0, sK = 0, sOut = 0;
    bool sgbOn = false;
    std::vector<cf32> sBuf;

    // the 32 kHz stream
    static constexpr double kDet = 128000.0;   // detection rate: +-46 kHz searched, the 100 kHz transponder passband
    static constexpr size_t kRing = 262144, kBlock = 4096;
    std::vector<cf32> ring = std::vector<cf32>(kRing);
    uint64_t n32 = 0;
    std::vector<float> win = std::vector<float>(kBlock);
    Fft fft{(int)kBlock};
    std::vector<float> bg = std::vector<float>(kBlock, 0.f);
    int warm = 0;

    // burst state
    bool active = false;
    int miss = 0, dets = 0;
    double falseBank = 0, bankT = 0, falseLimit = 1.0;   // seconds of search spent on bursts that did not look like a beacon (see finishBurst)
    uint64_t falseSkipped = 0;
    uint64_t firstBlockStart = 0, lastDetEnd = 0;   // sample counts (at kDet): first block of a burst, end of its last block with a line
    std::vector<float> linePow;                       // power of the line in each detecting block of the burst
    std::vector<double> peaks;
    float peakSnr = -200, peakLevel = -200;
    double lastDecodeSec = -1e9, lastPublish = 0;

    // repeat combining: the soft bits of bursts that did not decode, kept for the beacon's next bursts
    struct Kept { double t = 0, carrier = 0; std::vector<std::vector<float>> cands; };   // cands[0]: best-looking timing
    std::vector<Kept> kept;
    struct Pending { double t = 0, carrier = 0; std::string hexId; };   // repaired decodes waiting for a second burst with the same identification
    std::vector<Pending> pending;
    uint64_t heldBursts = 0;
    std::vector<std::pair<double, double>> expect;   // (carrier, time) of beacons heard: their next bursts are looked for with a lower line threshold
    bool combineOn = true;
    bool fullWindow = true;   // false: the 3.9 burst end (6 blocks without a line)
    bool coherentOn = true;   // false: the 3.9 differential message bits
    double costPerBit = sarsat::kRepairCostPerBit;
    std::vector<std::vector<float>> last4;   // the last 4 block spectra (weak burst detection)
    size_t last4i = 0;
    int last4n = 0;
    bool weakBurst = false;                  // the burst in progress was found by the 4-block sum
    double perMin = 47.5, perMax = 52.5;

    Impl() {
        for (size_t i = 0; i < kBlock; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)kBlock));
    }

    void resetState() {
        loRe = 1; loIm = 0;
        const double w = -2 * kPi * offHz / (fs > 0 ? fs : 1);
        stRe = std::cos(w); stIm = std::sin(w);
        accRe = accIm = 0; accN = 0; nIn = 0; k32 = 0;
        binEnd = fs > 0 ? fs / kDet : 0;
        sgbOn = fs >= 160000.0;
        sRe = 1; sIm = 0; sAccRe = sAccIm = 0; sAccN = 0; sIn = 0; sK = 0; sOut = 0; sCount = 0;
        sBinEnd = fs > 0 ? fs / sarsat::kSgbFs : 0;
        { const double w2 = -2 * kPi * (offHz + sarsat::kSgbOffsetHz) / (fs > 0 ? fs : 1); sStRe = std::cos(w2); sStIm = std::sin(w2); }
        sgb.reset(); sBuf.clear();
        n32 = 0;
        std::fill(bg.begin(), bg.end(), 0.f);
        warm = 0; active = false; miss = 0; dets = 0; peaks.clear();
        lastDecodeSec = -1e9; lastPublish = 0;
        kept.clear(); expect.clear(); pending.clear(); last4.clear(); last4i = 0; last4n = 0; weakBurst = false;
    }

    // A failed burst at time t on `carrier`: add it to the kept bursts of the same beacon (same carrier, at the repeat interval or a multiple of it),
    // newest first, one more at a time, and decode each sum. The timing of each burst is the one of its candidates whose soft bits correlate best with
    // the new burst's (a cheap match: the same message correlates by 0.4 to 0.8 at the levels where combining is needed, a wrong timing or another
    // message by about 0 +- 0.08); a kept burst joins only above 0.3. Returns the number of bursts in the sum that decoded (0: none; the burst is kept).
    static double corr(const std::vector<float>& a, const std::vector<float>& b) {
        double ab = 0, aa = 0, bb = 0;
        for (size_t k = 0; k < a.size() && k < b.size(); k++) { ab += (double)a[k] * b[k]; aa += (double)a[k] * a[k]; bb += (double)b[k] * b[k]; }
        return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0.0;
    }
    int combine(double t, double carrier, const std::vector<std::vector<float>>& mine, sarsat::Beacon& b) {
        const double keepFor = 4 * perMax + 2;
        kept.erase(std::remove_if(kept.begin(), kept.end(), [&](const Kept& k) { return t - k.t > keepFor; }), kept.end());
        auto periodic = [&](double dt) {
            for (int m = 1; m <= 4; m++) if (dt >= m * perMin - 0.5 && dt <= m * perMax + 0.5) return true;
            return false;
        };
        std::vector<size_t> match;
        for (size_t i = 0; i < kept.size(); i++)
            if (std::fabs(kept[i].carrier - carrier) <= 150.0 && periodic(t - kept[i].t)) match.push_back(i);
        std::sort(match.begin(), match.end(), [&](size_t a, size_t c) { return kept[a].t > kept[c].t; });
        if (match.size() > 3) match.resize(3);
        constexpr double kMinCorr = 0.3;
        // the new burst's timing: the candidate with the largest summed best correlation to the kept bursts
        size_t bi = 0;
        double bestTot = -1;
        for (size_t i = 0; i < mine.size(); i++) {
            double tot = 0;
            for (size_t m : match) {
                double best = 0;
                for (const auto& kc : kept[m].cands) best = std::max(best, corr(mine[i], kc));
                if (best >= kMinCorr) tot += best;
            }
            if (tot > bestTot) { bestTot = tot; bi = i; }
        }
        if (!mine.empty() && !match.empty()) {
            std::vector<float> sum = mine[bi];
            std::vector<size_t> used;
            for (size_t m : match) {
                size_t bj = 0;
                double best = -1;
                for (size_t j = 0; j < kept[m].cands.size(); j++) { const double c = corr(mine[bi], kept[m].cands[j]); if (c > best) { best = c; bj = j; } }
                if (best < kMinCorr) continue;
                const std::vector<float>& o = kept[m].cands[bj];
                for (size_t k = 0; k < sum.size(); k++) sum[k] += k < o.size() ? o[k] : 0.f;
                used.push_back(m);
                if (sarsat::decodeCombined(sum, b, costPerBit)) {
                    std::sort(used.rbegin(), used.rend());
                    for (size_t u : used) kept.erase(kept.begin() + (long)u);
                    return (int)used.size() + 1;
                }
            }
        }
        Kept k;
        k.t = t; k.carrier = carrier; k.cands = mine;
        kept.push_back(std::move(k));
        if (kept.size() > 16) kept.erase(kept.begin());
        return 0;
    }

    double timeSec() const { return (double)n32 / kDet; }

    void publish() {
        tel.timeSec = timeSec();
        tel.state = rateSet ? (timeSec() - lastDecodeSec < 300 ? 2 : 1) : 0;
        tel.seq++;
    }

    void finishBurst() {
        active = false;
        const bool weak = weakBurst;
        weakBurst = false;
        if (dets >= 4 && (peakSnr >= 10.f || weak)) {
            // Only a burst that looks like a beacon is counted when it does not decode. A beacon holds one steady line for 0.3 s or more, and after
            // its 160 ms unmodulated preamble the phase-modulated data lowers the carrier line (about 7 dB). A radio with no antenna shows short
            // blips, wandering tones and steady tones: none of those is a burst. A burst that decodes always counts.
            bool plausible = lastDetEnd > firstBlockStart && (double)(lastDetEnd - firstBlockStart) >= 0.30 * kDet && linePow.size() >= 8;
            if (plausible) {
                const auto mm = std::minmax_element(peaks.begin(), peaks.end());
                plausible = *mm.second - *mm.first <= 600.0;
            }
            if (plausible) {
                std::vector<float> pre(linePow.begin(), linePow.begin() + 5), post(linePow.begin() + 5, linePow.end());
                std::nth_element(pre.begin(), pre.begin() + 2, pre.end());
                std::nth_element(post.begin(), post.begin() + (long)(post.size() / 2), post.end());
                plausible = post[post.size() / 2] <= 0.7f * pre[2];
            }
            // The search below costs a few hundred ms of CPU per burst. A band with many carriers (radiosondes, pagers, a noisy site) makes the
            // detector fire all the time, and every such burst that does not look like a beacon would run it: a falseBank of seconds of search,
            // paid back at 0.15 s per second of signal, stops that. A burst that looks like a beacon is always searched.
            {
                const double now = (double)n32 / kDet;
                falseBank = std::max(0.0, falseBank - 0.15 * (now - bankT)); bankT = now;
            }
            const bool skipSearch = !plausible && falseBank > falseLimit;
            if (skipSearch) { falseSkipped++; peaks.clear(); linePow.clear(); dets = 0; peakSnr = -200; peakLevel = -200; publish(); return; }
            const auto wall0 = std::chrono::steady_clock::now();
            const uint64_t start = firstBlockStart >= 8192 ? firstBlockStart - 8192 : 0;
            const uint64_t from = n32 > kRing - 4096 && start < n32 - (kRing - 4096) ? n32 - (kRing - 4096) : start;
            std::vector<cf32> x;
            x.reserve((size_t)(n32 - from));
            for (uint64_t i = from; i < n32; i++) x.push_back(ring[(size_t)(i & (kRing - 1))]);
            std::vector<double> pk(peaks.begin(), peaks.begin() + std::min<size_t>(4, peaks.size()));
            std::sort(pk.begin(), pk.end());
            const double carrier = pk[pk.size() / 2];
            // mix the carrier to 0 and average 4:1 down to the 32 kHz the demodulator works at
            std::vector<cf32> y;
            y.reserve(x.size() / 4 + 1);
            {
                const double w = -2 * kPi * carrier / kDet;
                double ph = 0;
                cf32 acc(0, 0);
                for (size_t i = 0; i < x.size(); i++) {
                    acc += x[i] * cf32((float)std::cos(ph), (float)std::sin(ph));
                    ph += w;
                    if ((i & 3) == 3) { y.push_back(acc * 0.25f); acc = cf32(0, 0); }
                }
            }
            sarsat::Beacon b;
            sarsat::SoftBurst sb;
            bool got = sarsat::demodBurst2(y, 0.0, b, combineOn ? &sb : nullptr, coherentOn, costPerBit);
            int combined = 0;
            const double tBurst = (double)firstBlockStart / kDet;
            if (combineOn) {   // the carriers where a beacon's next burst is expected
                expect.erase(std::remove_if(expect.begin(), expect.end(), [&](const std::pair<double, double>& e) { return tBurst - e.second > 4 * perMax + 2 || std::fabs(e.first - carrier) <= 60.0; }), expect.end());
            }
            if (got && combineOn) {   // the beacon decoded: its kept bursts are not needed any more
                kept.erase(std::remove_if(kept.begin(), kept.end(), [&](const Kept& k) { return std::fabs(k.carrier - carrier) <= 150.0 && tBurst - k.t < 4 * perMax + 2; }), kept.end());
            } else if (combineOn && !sb.soft.empty()) {
                std::vector<std::vector<float>> mine{sb.soft};
                for (auto& a : sb.alt) mine.push_back(std::move(a));
                combined = combine(tBurst, carrier, mine, b);
                got = combined > 0;
            }
            if (combineOn && (got || !sb.soft.empty())) { expect.emplace_back(carrier, tBurst); if (expect.size() > 8) expect.erase(expect.begin()); }
            if (got || plausible || combined) tel.bursts++;
            // 4.0 confirmation by repeat: a message that needed more than one repaired bit (or a long message whose second check failed) is held until a
            // later burst of the same beacon (same carrier, within 4 repeat intervals) decodes to the same identification. BCH-1 can land on a wrong
            // codeword when a weak burst has 4 or more errors, and the soft decoding and the timing search give it many chances; two such errors
            // giving the same wrong identification do not happen. A message repaired by at most one bit with its checks intact is reported at once.
            bool held = false;
            int confirmed = 0;
            if (got && coherentOn) {
                const bool sure = b.corrected == 0 || (b.corrected == 1 && (!b.longMsg || b.bch2Ok));
                pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Pending& q) { return tBurst - q.t > 4 * perMax + 2; }), pending.end());
                auto same = std::find_if(pending.begin(), pending.end(), [&](const Pending& q) { return q.hexId == b.hexId && std::fabs(q.carrier - carrier) <= 150.0; });
                if (same != pending.end()) { confirmed = 1; pending.erase(same); }
                else if (!sure) {
                    Pending q; q.t = tBurst; q.carrier = carrier; q.hexId = b.hexId;
                    pending.push_back(q);
                    if (pending.size() > 16) pending.erase(pending.begin());
                    held = true;
                }
                if (sure && same == pending.end()) pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const Pending& q) { return std::fabs(q.carrier - carrier) <= 150.0; }), pending.end());
            }
            if (got && held) heldBursts++;
            if (got && !held) {
                SarsatEntry e;
                e.wallTime = (int64_t)std::time(nullptr);
                e.timeSec = timeSec();
                e.b = b;
                e.snrDb = peakSnr; e.levelDb = peakLevel; e.cfoHz = (float)carrier;
                e.combined = combined;
                e.confirmedByRepeat = confirmed != 0;
                tel.log.insert(tel.log.begin(), e);
                if (tel.log.size() > kSarsatLogMax) tel.log.pop_back();
                tel.blocksOk++;
                tel.dataValid = true;
                lastDecodeSec = timeSec();
                if (log) {
                    char line[256];
                    snprintf(line, sizeof line, "COSPAS-SARSAT %s %s %s%s%s", b.hexId.c_str(), b.beaconType.c_str(), b.country.c_str(), b.hasPos ? " (position)" : "",
                             combined ? (" (" + std::to_string(combined) + " bursts combined)").c_str() : "");
                    log(line);
                }
            } else if (plausible) {
                tel.blocksBad++;
            }
            if (!plausible) falseBank += std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
            tel.cfoHz = carrier; tel.snrDb = peakSnr; tel.levelDb = peakLevel;
        }
        peaks.clear(); linePow.clear(); dets = 0; peakSnr = -200; peakLevel = -200;
        publish();
    }

    void block() {   // the last kBlock samples of the ring
        const uint64_t base = n32 - kBlock;
        std::vector<cf32> buf(kBlock);
        double pw = 0;
        for (size_t i = 0; i < kBlock; i++) {
            const cf32 v = ring[(size_t)((base + i) & (kRing - 1))];
            pw += (double)v.real() * v.real() + (double)v.imag() * v.imag();
            buf[i] = v * win[i];
        }
        fft.forward(buf.data());
        std::vector<float> p(kBlock);
        for (size_t k = 0; k < kBlock; k++) p[k] = buf[k].real() * buf[k].real() + buf[k].imag() * buf[k].imag();
        const size_t kMax = (size_t)(46000.0 / kDet * (double)kBlock);   // keep to +-46 kHz: the edges are attenuated by the average
        auto allowed = [&](size_t k) { return k <= kMax || k >= kBlock - kMax; };
        std::vector<float> sel;
        sel.reserve(kBlock);
        for (size_t k = 0; k < kBlock; k++) if (allowed(k)) sel.push_back(p[k]);
        std::nth_element(sel.begin(), sel.begin() + (long)(sel.size() / 2), sel.end());
        const float med = std::max(sel[sel.size() / 2], 1e-20f);
        size_t pk = 0;
        float pmax = 0;
        for (size_t k = 0; k < kBlock; k++) if (allowed(k) && p[k] > pmax && p[k] > 6.f * bg[k]) { pmax = p[k]; pk = k; }
        bool det = warm >= 12 && pmax >= 30.f * med;
        // 4.0: the 160 ms preamble spans 4 to 5 blocks. The sum of the last 4 spectra finds a line about 4 dB weaker than one block does at the same false
        // alarm rate (the threshold on the sum, 8.5 x its median, is about 7.5 x the mean noise of a block: about 5e-10 of noise bins exceed it)
        bool weakDet = false;
        double fW = 0;
        float qW = 0, snrW = -200;
        if (fullWindow) {
            if (last4.empty()) last4.assign(4, std::vector<float>(kBlock, 0.f));
            last4[last4i] = p; last4i = (last4i + 1) % 4;
            if (last4n < 4) last4n++;
            if (!active && !det && warm >= 12 && last4n == 4) {
                std::vector<float> q(kBlock);
                for (size_t k = 0; k < kBlock; k++) q[k] = last4[0][k] + last4[1][k] + last4[2][k] + last4[3][k];
                std::vector<float> sq;
                sq.reserve(kBlock);
                for (size_t k = 0; k < kBlock; k++) if (allowed(k)) sq.push_back(q[k]);
                std::nth_element(sq.begin(), sq.begin() + (long)(sq.size() / 2), sq.end());
                const float med4 = std::max(sq[sq.size() / 2], 1e-20f);
                size_t qk = 0;
                for (size_t k = 0; k < kBlock; k++) if (allowed(k) && q[k] > qW && q[k] > 24.f * bg[k]) { qW = q[k]; qk = k; }
                bool hit = qW >= 8.5f * med4;
                // 4.0 a-priori: near the carrier of a beacon heard in the last minutes (decoded or kept for combining) its next burst is expected,
                // so +-3 bins there take a line at 5.5 x the median (about 3e-6 of noise bins: one false start in tens of minutes, each refused by the checks)
                if (!hit && combineOn)
                    for (const auto& ex : expect) {
                        const long c0 = std::lround(ex.first / kDet * (double)kBlock);
                        for (long dk = -3; dk <= 3; dk++) {
                            const size_t k = (size_t)((c0 + dk + (long)kBlock) % (long)kBlock);
                            if (allowed(k) && q[k] >= 5.5f * med4 && q[k] > 24.f * bg[k] && q[k] > (hit ? q[qk] : 0.f)) { qk = k; hit = true; }
                        }
                    }
                if (hit) {
                    qW = q[qk];
                    weakDet = true;
                    const size_t a = (qk + kBlock - 1) % kBlock, c = (qk + 1) % kBlock;
                    const double la = std::log((double)q[a] + 1e-30), lb = std::log((double)q[qk] + 1e-30), lc = std::log((double)q[c] + 1e-30);
                    const double den = la - 2 * lb + lc;
                    const double delta = std::fabs(den) > 1e-9 ? 0.5 * (la - lc) / den : 0.0;
                    fW = ((double)(qk < kBlock / 2 ? (long)qk : (long)qk - (long)kBlock) + std::max(-1.0, std::min(1.0, delta))) * kDet / (double)kBlock;
                    const float nm = med4 / 3.672f;   // the median of a sum of 4 exponentials is 3.672 x its unit mean
                    snrW = 10.f * std::log10(std::max(1e-3f, (q[a] + q[qk] + q[c] - 3 * nm) / (1.5f * nm)));
                }
            }
        }
        float snr = -200;
        double f = 0;
        if (det) {
            const size_t a = (pk + kBlock - 1) % kBlock, c = (pk + 1) % kBlock;
            const double la = std::log((double)p[a] + 1e-30), lb = std::log((double)p[pk] + 1e-30), lc = std::log((double)p[c] + 1e-30);
            const double den = la - 2 * lb + lc;
            const double delta = std::fabs(den) > 1e-9 ? 0.5 * (la - lc) / den : 0.0;
            const double bin = (double)(pk < kBlock / 2 ? (long)pk : (long)pk - (long)kBlock) + std::max(-1.0, std::min(1.0, delta));
            f = bin * kDet / (double)kBlock;
            const float nm = med / 0.6931f;
            const float s3 = p[a] + p[pk] + p[c];
            snr = 10.f * std::log10(std::max(1e-3f, (s3 - 3 * nm) / (1.5f * nm)));
        }
        if (det) {
            if (!active) { active = true; firstBlockStart = base; miss = 0; dets = 0; peakSnr = -200; peakLevel = -200; peaks.clear(); linePow.clear(); }
            miss = 0; dets++; lastDetEnd = n32; linePow.push_back(pmax);
            peaks.push_back(f);
            peakSnr = std::max(peakSnr, snr);
            peakLevel = std::max(peakLevel, 10.f * std::log10((float)(pw / kBlock) + 1e-12f));
        } else if (weakDet) {
            active = true; weakBurst = true; firstBlockStart = base >= 3 * kBlock ? base - 3 * kBlock : 0; miss = 0; dets = 4; lastDetEnd = n32;
            peaks.assign(1, fW); linePow.assign(1, qW / 4); peakSnr = snrW; peakLevel = 10.f * std::log10((float)(pw / kBlock) + 1e-12f);
            last4n = 0;
        } else if (active) {
            miss++;
        }
        // 4.0: a weak burst shows its line only in the 160 ms preamble (the data lowers it by about 7 dB), so 6 blocks without a line came 200 ms after the
        // preamble and cut the 360 ms message short: the burst now runs at least 0.62 s from its first line, which holds a long message after a late first line
        const bool longEnough = !fullWindow || n32 - firstBlockStart >= (uint64_t)(0.62 * kDet);
        if (active && ((miss >= 6 && longEnough) || n32 - firstBlockStart > (uint64_t)(0.9 * kDet))) finishBurst();
        if (!active && !det) {
            for (size_t k = 0; k < kBlock; k++) bg[k] = warm == 0 ? std::min(p[k], 4.f * med) : bg[k] * 0.98f + std::min(p[k], 4.f * med) * 0.02f;   // a carrier present during the warm-up must not be learnt as background
            if (warm < 1000) warm++;
        }
        tel.levelDb = active ? tel.levelDb : 10.f * std::log10((float)(pw / kBlock) + 1e-12f);
    }

    void sample32(double re, double im) {
        ring[(size_t)(n32 & (kRing - 1))] = cf32((float)re, (float)im);
        n32++;
        if (n32 % kBlock == 0) block();
    }
};

SarsatReceiver::SarsatReceiver() : p_(std::make_unique<Impl>()) {}
SarsatReceiver::~SarsatReceiver() = default;

void SarsatReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = inputRateHz;
    p_->kidsDirty = true;
    p_->rateSet = inputRateHz >= sarsatTuning().minSampleRate - 1;
    p_->resetState();
    p_->tel.inputRate = inputRateHz;
    p_->tel.state = p_->rateSet ? 1 : 0;
    p_->tel.seq++;
}
void SarsatReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (hz == p_->offHz) return;
    p_->offHz = hz;
    p_->kidsDirty = true;
    const double w = -2 * kPi * hz / (p_->fs > 0 ? p_->fs : 1);
    p_->stRe = std::cos(w); p_->stIm = std::sin(w);
    const double w2 = -2 * kPi * (hz + sarsat::kSgbOffsetHz) / (p_->fs > 0 ? p_->fs : 1);
    p_->sStRe = std::cos(w2); p_->sStIm = std::sin(w2);
}
bool SarsatReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rateSet;
}
void SarsatReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetState();
    p_->kidsDirty = true;
    const uint64_t seq = p_->tel.seq;
    p_->tel = SarsatTelemetry();
    p_->tel.seq = seq + 1;
    p_->tel.inputRate = p_->fs;
    p_->tel.state = p_->rateSet ? 1 : 0;
}
void SarsatReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.log.clear();
    p_->tel.dataValid = false;
    p_->tel.blocksOk = p_->tel.blocksBad = p_->tel.bursts = 0;
    p_->tel.seq++;
}
void SarsatReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->log = std::move(cb);
}
bool SarsatReceiver::telemetry(SarsatTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

void SarsatReceiver::setFalseBurstBudget(double seconds) { std::lock_guard<std::mutex> lk(p_->mu); p_->falseLimit = seconds; }
uint64_t SarsatReceiver::falseBurstsSkipped() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->falseSkipped; }
void SarsatReceiver::setRepeatCombining(bool on, double periodMinSec, double periodMaxSec) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->combineOn = on;
    p_->perMin = std::max(1.0, periodMinSec); p_->perMax = std::max(p_->perMin, periodMaxSec);
    if (!on) p_->kept.clear();
    for (auto& k : p_->kids) k->setRepeatCombining(on, periodMinSec, periodMaxSec);
}
void SarsatReceiver::setWeakBurstPath(bool fullWindow, bool coherentBits) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fullWindow = fullWindow;
    p_->coherentOn = coherentBits;
    for (auto& k : p_->kids) k->setWeakBurstPath(fullWindow, coherentBits);
}
void SarsatReceiver::setRepairCostLimit(double perBit) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->costPerBit = perBit;
    for (auto& k : p_->kids) k->setRepairCostLimit(perBit);
}
void SarsatReceiver::setWideSearch(bool on) { std::lock_guard<std::mutex> lk(p_->mu); if (on != p_->wide) { p_->wide = on; p_->kidsDirty = true; } }
bool SarsatReceiver::wideSearch() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->wide; }
double SarsatReceiver::wideSpanHz() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->wide ? p_->wideSpan : 0; }

void SarsatReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (!m.rateSet) { m.tel.state = 0; return; }
    if (m.wide) {
        if (m.kidsDirty) {
            m.kidsDirty = false;
            m.kids.clear(); m.kidShift.clear(); m.kidSeq.clear(); m.pending.clear();
            const double span = std::min(0.85 * m.fs, 1.1e6);
            const int k = std::max(1, (int)std::ceil(span / 90e3));
            m.wideSpan = k * 90e3;
            for (int i = 0; i < k; i++) {
                const double d = (i - (k - 1) / 2.0) * 90e3;
                auto r = std::make_unique<SarsatReceiver>();
                r->configure(m.fs);
                r->setSignalOffset(m.offHz + d);
                r->setRepeatCombining(m.combineOn, m.perMin, m.perMax);
                r->setWeakBurstPath(m.fullWindow, m.coherentOn);
                r->setRepairCostLimit(m.costPerBit);
                m.kids.push_back(std::move(r)); m.kidShift.push_back(d); m.kidSeq.push_back(0);
            }
            m.tel.wideSpanHz = m.wideSpan; m.tel.wideChannels = k;
        }
        bool changed = false;
        for (size_t i = 0; i < m.kids.size(); i++) {
            m.kids[i]->feed(x, n);
            SarsatTelemetry kt;
            const uint64_t oldBurst = m.kidSeq[i];
            if (!m.kids[i]->telemetry(kt, 0)) continue;
            m.tel.levelDb = kt.levelDb; m.tel.timeSec = kt.timeSec;
            // new entries of this child: those past the count it had (log is newest first)
            const uint64_t total = kt.blocksOk;
            for (uint64_t j = 0; j < total - std::min(total, oldBurst) && j < kt.log.size(); j++) {
                SarsatEntry e = kt.log[(size_t)j];
                e.cfoHz += (float)m.kidShift[i];
                // one burst is heard in several channels: next to it (the overlap) and as weaker aliases further away, which can even pass
                // the BCH check with corrections. Keep the strongest copy (then the one with fewer corrections) of each burst time.
                bool merged = false;
                for (auto& o : m.pending)
                    if (std::fabs(o.timeSec - e.timeSec) < 0.4) {
                        if (e.snrDb > o.snrDb + 0.5f || (std::fabs(e.snrDb - o.snrDb) <= 0.5f && e.b.corrected < o.b.corrected)) o = e;
                        merged = true; break;
                    }
                if (!merged) m.pending.push_back(e);
            }
            m.kidSeq[i] = total;
            m.tel.blocksBad = std::max(m.tel.blocksBad, kt.blocksBad);
        }
        m.n32 += (uint64_t)((double)n / std::max(1.0, m.fs) * Impl::kDet);
        for (size_t q = 0; q < m.pending.size();) {
            if (m.timeSec() - m.pending[q].timeSec < 1.5) { q++; continue; }
            const SarsatEntry e = m.pending[q];
            m.pending.erase(m.pending.begin() + (long)q);
            m.tel.log.insert(m.tel.log.begin(), e);
            if (m.tel.log.size() > kSarsatLogMax) m.tel.log.pop_back();
            m.tel.blocksOk++; m.tel.bursts++; m.tel.dataValid = true; m.tel.cfoHz = e.cfoHz; m.tel.snrDb = e.snrDb;
            m.lastDecodeSec = e.timeSec;
            changed = true;
            if (m.log) {
                char line[256];
                snprintf(line, sizeof line, "COSPAS-SARSAT %s %s %s at %+.1f kHz", e.b.hexId.c_str(), e.b.beaconType.c_str(), e.b.country.c_str(), e.cfoHz / 1e3);
                m.log(line);
            }
        }
        if (changed || m.timeSec() - m.lastPublish >= 0.25) { m.lastPublish = m.timeSec(); m.publish(); }
        return;
    }
    if (!m.kids.empty()) { m.kids.clear(); m.tel.wideSpanHz = 0; m.tel.wideChannels = 0; }
    const double binW = m.fs / Impl::kDet;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.accRe += re * m.loRe - im * m.loIm;
        m.accIm += re * m.loIm + im * m.loRe;
        m.accN++;
        const double nr = m.loRe * m.stRe - m.loIm * m.stIm, ni = m.loRe * m.stIm + m.loIm * m.stRe;
        m.loRe = nr; m.loIm = ni;
        if (++m.loCount >= 4096) { m.loCount = 0; const double a = 1.0 / std::sqrt(m.loRe * m.loRe + m.loIm * m.loIm); m.loRe *= a; m.loIm *= a; }
        m.nIn++;
        if ((double)m.nIn >= m.binEnd) {
            const double inv = 1.0 / (double)m.accN;
            m.sample32(m.accRe * inv, m.accIm * inv);
            m.accRe = m.accIm = 0; m.accN = 0;
            m.k32++;
            m.binEnd = (double)(m.k32 + 1) * binW;
        }
        if (m.sgbOn) {
            m.sAccRe += re * m.sRe - im * m.sIm;
            m.sAccIm += re * m.sIm + im * m.sRe;
            m.sAccN++;
            const double qr = m.sRe * m.sStRe - m.sIm * m.sStIm, qi = m.sRe * m.sStIm + m.sIm * m.sStRe;
            m.sRe = qr; m.sIm = qi;
            if (++m.sCount >= 4096) { m.sCount = 0; const double a = 1.0 / std::sqrt(m.sRe * m.sRe + m.sIm * m.sIm); m.sRe *= a; m.sIm *= a; }
            m.sIn++;
            if ((double)m.sIn >= m.sBinEnd) {
                const double inv = 1.0 / (double)m.sAccN;
                m.sBuf.push_back(cf32((float)(m.sAccRe * inv), (float)(m.sAccIm * inv)));
                m.sAccRe = m.sAccIm = 0; m.sAccN = 0;
                m.sK++;
                m.sBinEnd = (double)(m.sK + 1) * (m.fs / sarsat::kSgbFs);
            }
        }
    }
    if (m.sgbOn && !m.sBuf.empty()) {
        m.sOut += m.sBuf.size();
        m.sgb.push(m.sBuf.data(), m.sBuf.size());
        m.sBuf.clear();
        for (const sarsat::SgbHit& h : m.sgb.take()) {
            SarsatEntry e;
            e.wallTime = (int64_t)std::time(nullptr);
            e.timeSec = m.timeSec();
            e.b = h.b;
            e.snrDb = h.snrDb; e.levelDb = m.tel.levelDb; e.cfoHz = (float)h.cfoHz;
            m.tel.log.insert(m.tel.log.begin(), e);
            if (m.tel.log.size() > kSarsatLogMax) m.tel.log.pop_back();
            m.tel.blocksOk++;
            m.tel.bursts++;
            m.tel.dataValid = true;
            m.tel.cfoHz = h.cfoHz;
            m.lastDecodeSec = m.timeSec();
            if (m.log) {
                char line[256];
                snprintf(line, sizeof line, "COSPAS-SARSAT second generation %s %s %s%s", h.b.hexId.c_str(), h.b.beaconType.c_str(), h.b.country.c_str(), h.b.hasPos ? " (position)" : "");
                m.log(line);
            }
            m.publish();
        }
    }
    if (m.timeSec() - m.lastPublish >= 0.25) { m.lastPublish = m.timeSec(); m.publish(); }
}

} // namespace dect2
