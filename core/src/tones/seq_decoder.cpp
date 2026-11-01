// Five-tone sequential decoder: see seq_decoder.h. The detection is the one of the Tones mode (Goertzel filters over 16 ms windows every 8 ms,
// two-hop confirmation of a change).
#include "dect2/tones/seq_decoder.h"
#include "dect2/tones/tones_proto.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
constexpr double kRate = 8000.0;
constexpr int kN = 128, kHop = 64, kRingN = 256;
double goertzel(const float* x, int n, double f) {
    const double coeff = 2 * std::cos(2 * dect2::math::kPi * f / kRate);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { const double s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}
} // namespace

SeqToneDecoder::SeqToneDecoder() : ring_(kRingN, 0.f) {}

void SeqToneDecoder::reset() {
    std::fill(ring_.begin(), ring_.end(), 0.f);
    n_ = 0; segs_.clear(); cur_ = -1; run_ = 0; pend_ = -2; pendRun_ = 0; silent_ = 0;
}

void SeqToneDecoder::feed(const float* x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        ring_[(size_t)n_ & (kRingN - 1)] = x[i];
        n_++;
        if (n_ >= kN && n_ % kHop == 0) hop();
    }
}

void SeqToneDecoder::hop() {
    const std::vector<double>& pool = tones::seqPool();
    float win[kN];
    for (int i = 0; i < kN; i++) win[i] = ring_[(size_t)(n_ - kN + i) & (kRingN - 1)];
    // the mean goes first: a carrier offset or a discriminator offset is a DC term that would load every filter
    double mean = 0;
    for (float v : win) mean += v;
    mean /= kN;
    double e = 0;
    for (float& v : win) { v = (float)(v - mean); e += (double)v * v; }
    int cand = -1;
    if (e / kN >= 30.0 * 30.0) {
        const double nf = 2.0 / kN;
        double bp = 0;
        for (size_t i = 0; i < pool.size(); i++) {
            const double f = nf * goertzel(win, kN, pool[i]) / e;
            if (f > bp) { bp = f; cand = (int)i; }
        }
        if (bp < 0.55) cand = -1;
    }
    if (cand == cur_) { run_++; pend_ = -2; pendRun_ = 0; }
    else if (cand == pend_) {
        if (++pendRun_ >= 2) {
            if (cur_ >= 0 && run_ >= 2 && run_ <= 40) segs_.push_back({cur_, run_ * 1000 * kHop / (int)kRate + 8});
            cur_ = cand; run_ = pendRun_; pend_ = -2; pendRun_ = 0;
            silent_ = 0;
        }
    } else { pend_ = cand; pendRun_ = 1; }
    if (run_ > 40) segs_.clear();          // a tone of 0.3 s and more is no sequential tone
    if (cur_ < 0) silent_++; else silent_ = 0;
    if (!segs_.empty() && cur_ < 0 && silent_ * kHop * 1000 / (int)kRate > 160) close();
    else if (segs_.size() > 12) segs_.clear();
}

void SeqToneDecoder::close() {
    std::vector<Seg> s;
    s.swap(segs_);
    if (s.size() < 3) return;
    const std::vector<double>& pool = tones::seqPool();
    std::vector<int> dw;
    for (const Seg& g : s) dw.push_back(g.dwellMs);
    std::sort(dw.begin(), dw.end());
    const int med = dw[dw.size() / 2];
    struct Cand { int set; std::string digits, raw; };
    std::vector<Cand> c;
    for (int si = 0; si < tones::kSeqSetN; si++) {
        const tones::SeqSet& st = tones::kSeqSets[si];
        std::string d, raw; char prev = '?';
        bool all = true;
        for (const Seg& g : s) {
            const double f = pool[(size_t)g.pool];
            int sym = -1;
            for (int k = 0; k < 10; k++) if (std::fabs(st.digit[k] - f) < 0.5) sym = k;
            if (std::fabs(st.rep - f) < 0.5) sym = 10;
            if (sym < 0) { all = false; break; }
            if (sym == 10) { d += prev; raw += 'E'; } else { prev = (char)('0' + sym); d += prev; raw += prev; }
        }
        if (all) c.push_back({si, d, raw});
    }
    if (c.empty()) { rejected_++; return; }
    // CCIR (100 ms) and EEA (40 ms) share their tones: the tone length decides; the others by closeness to their usual length
    auto score = [&](const Cand& k) { return std::fabs((double)med - tones::kSeqSets[k.set].toneMs) + (k.set == 0 ? 0 : 0.01 * k.set); };
    std::sort(c.begin(), c.end(), [&](const Cand& a, const Cand& b) { return score(a) < score(b); });
    SeqCall out;
    for (size_t i = 1; i < c.size(); i++) out.alsoFits += (out.alsoFits.empty() ? "" : ", ") + std::string(tones::kSeqSets[c[i].set].name);
    out.digits = c[0].digits;
    out.raw = c[0].raw;
    out.standard = tones::kSeqSets[c[0].set].name;
    out.tones = (int)s.size();
    out.toneMs = med;
    out.endSec = (double)n_ / kRate;
    if (cb_) cb_(out);
}

} // namespace dect2
