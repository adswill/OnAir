// DAB TII: the patterns of table 26, the carriers of clause 14.8.1 (transmission mode I) and the detector. See dab_tii.h.
#include "dect2/dab_tii.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace dect2::dabtii {

namespace {
constexpr int kTu = 2048;
// the four quarters of the band: -768 <= k < -384, -384 <= k < 0, 0 < k <= 384, 384 < k <= 768 (clause 14.8.1)
constexpr int kQuarter[4] = {-768, -384, 1, 385};

const std::array<uint8_t, kMainIds>& patterns() {
    static const std::array<uint8_t, kMainIds> t = [] {
        std::array<uint8_t, kMainIds> a{};
        int n = 0;
        for (int v = 0; v < 256 && n < kMainIds; v++) if (__builtin_popcount((unsigned)v) == 4) a[(size_t)n++] = (uint8_t)v;
        return a;
    }();
    return t;
}
} // namespace

uint8_t pattern(int mainId) { return mainId >= 0 && mainId < kMainIds ? patterns()[(size_t)mainId] : 0; }

int mainIdOfPattern(uint8_t bits) {
    const auto& t = patterns();
    for (int p = 0; p < kMainIds; p++) if (t[(size_t)p] == bits) return p;
    return -1;
}

void pairCarriers(int mainId, int subId, std::vector<int>& k) {
    k.clear();
    if (mainId < 0 || mainId >= kMainIds || subId < 0 || subId >= kSubIds) return;
    const uint8_t a = pattern(mainId);
    for (int q = 0; q < 4; q++)
        for (int b = 0; b < kPairs; b++)
            if ((a >> (7 - b)) & 1) k.push_back(kQuarter[q] + 2 * subId + 48 * b);
}

void Detector::reset() {
    for (auto& par : p_) for (auto& c : par) for (double& v : c) v = 0;
    n_[0] = n_[1] = 0;
}

void Detector::addNull(const std::vector<cf32>& X, uint64_t frame) {
    if ((int)X.size() != kTu) return;
    const int j = (int)(frame & 1);
    // a running mean over the first 32 null symbols of each kind, then an average that forgets with a time constant of 32 of them (3 s at
    // one every 192 ms): a transmitter that fades or a retune show within seconds
    const double w = 1.0 / std::min(n_[j] + 1, 32);
    for (int c = 0; c < kSubIds; c++)
        for (int b = 0; b < kPairs; b++) {
            double s = 0;
            for (int q = 0; q < 4; q++) {
                const int k = kQuarter[q] + 2 * c + 48 * b;
                s += std::norm(X[(size_t)((k + kTu) % kTu)]) + std::norm(X[(size_t)((k + 1 + kTu) % kTu)]);
            }
            p_[j][c][b] += w * (s - p_[j][c][b]);
        }
    n_[j]++;
}

std::vector<Found> Detector::found() const {
    std::vector<Found> out;
    if (n_[0] < 3 || n_[1] < 3) return out;
    // the frames with TII carry more energy in their null symbol than the ones without
    double tot[2] = {0, 0};
    for (int j = 0; j < 2; j++) for (auto& c : p_[j]) for (double v : c) tot[j] += v;
    const int a = tot[1] > tot[0] ? 1 : 0, d = 1 - a;
    // the noise of one pair position (8 carriers) in the frames without TII: the median over all 192 positions
    std::vector<double> all;
    for (auto& c : p_[d]) for (double v : c) all.push_back(v);
    std::nth_element(all.begin(), all.begin() + (long)all.size() / 2, all.end());
    const double nf = all[all.size() / 2];
    // the spread of the difference of two averages of a sum of 8 exponential powers over n null symbols
    const int n = std::min(std::min(n_[0], n_[1]), 32);
    const double sigma = nf * std::sqrt(2.0 / (8.0 * n));
    double eMax = 0;
    for (int c = 0; c < kSubIds; c++) for (int b = 0; b < kPairs; b++) eMax = std::max(eMax, p_[a][c][b] - p_[d][c][b]);
    if (eMax <= 0) return out;
    for (int c = 0; c < kSubIds; c++) {
        std::array<std::pair<double, int>, kPairs> e;
        for (int b = 0; b < kPairs; b++) e[(size_t)b] = {p_[a][c][b] - p_[d][c][b], b};
        std::sort(e.begin(), e.end(), [](auto& x, auto& y) { return x.first > y.first; });
        const double e4 = e[3].first, e5 = std::max(0.0, e[4].first);
        // four pairs well above the noise, and clearly apart from the other four (one transmitter on this comb, not two overlapping)
        if (e4 < 6.0 * sigma || e4 < 3.0 * e5 || e4 < 1e-6 * eMax) continue;
        uint8_t bits = 0;
        double sum = 0;
        for (int i = 0; i < 4; i++) { bits |= (uint8_t)(0x80 >> e[(size_t)i].second); sum += e[(size_t)i].first; }
        const int p = mainIdOfPattern(bits);
        if (p < 0) continue;
        Found f;
        f.mainId = p; f.subId = c;
        f.levelDb = (float)std::min(60.0, 10.0 * std::log10(sum / 4.0 / std::max(nf, 1e-30)));
        f.marginDb = (float)std::min(60.0, 10.0 * std::log10(e4 / std::max(std::max(e5, sigma), 1e-30)));
        out.push_back(f);
    }
    std::sort(out.begin(), out.end(), [](const Found& x, const Found& y) { return x.levelDb > y.levelDb; });
    return out;
}

} // namespace dect2::dabtii
