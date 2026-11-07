// 802.15.4 sub-GHz BPSK demodulator (see zigbee_bpsk.h).
#include "zigbee_bpsk.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr int kSeq0[15] = {1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0};   // chips of a 0 (a 1 sends the inverse)
constexpr int kNeedBits = 64 + 8 + 8 + 8 * 127;                              // up to 64 more preamble bits, SFD, PHR, longest PSDU
constexpr int kThreshRuns = 12;                                              // preamble symbols in a row at one phase
constexpr float kRho = 0.55f;
struct Pn {
    float pn[15];     // +-1
    float pp[15];     // pn[k] pn[k-1]
    Pn() {
        for (int k = 0; k < 15; k++) pn[k] = kSeq0[k] ? 1.f : -1.f;
        pp[0] = 0;
        for (int k = 1; k < 15; k++) pp[k] = pn[k] * pn[k - 1];
    }
};
const Pn& pnTable() { static const Pn p; return p; }
}

void Bpsk154Demod::init(double chipRateHz) { chipRate_ = chipRateHz; reset(); }

void Bpsk154Demod::reset() {
    buf_.clear(); base_ = 0; g_ = -1;
    for (int& r : run_) r = 0;
    skip_ = 0; cands_.clear(); decodedUntil_ = 0; needAt_ = 0;
}

std::complex<float> Bpsk154Demod::at(int64_t i) const {
    const int64_t k = i - base_;
    return k >= 0 && k < (int64_t)buf_.size() ? buf_[(size_t)k] : std::complex<float>(0, 0);
}

// the chip-product correlation of the symbol window starting at sample s (chips at s + 4k)
bool Bpsk154Demod::qAt(int64_t s, std::complex<float>& q, float& e) const {
    if (s < base_ || s + kSps * (kChips - 1) >= base_ + (int64_t)buf_.size()) return false;
    const Pn& P = pnTable();
    std::complex<float> prev = at(s), acc(0, 0);
    float en = std::norm(prev);
    for (int k = 1; k < kChips; k++) {
        const std::complex<float> x = at(s + kSps * k);
        acc += P.pp[k] * (x * std::conj(prev));
        en += std::norm(x);
        prev = x;
    }
    q = acc; e = en;
    return true;
}

void Bpsk154Demod::push(std::complex<float> y) {
    buf_.push_back(y);
    g_++;
    const int64_t s = g_ - kSps * (kChips - 1);
    if (s >= base_) {
        std::complex<float> q; float e;
        if (qAt(s, q, e)) {
            const int ph = (int)(((s % kBit) + kBit) % kBit);
            const bool hit = e > 1e-12f && std::abs(q) > kRho * e * (14.f / 15.f);
            if (hit) {
                if (++run_[ph] >= kThreshRuns && s >= skip_) {
                    cands_.push_back({s});
                    skip_ = s + 16 * kBit;
                    preambles++;
                    for (int& r : run_) r = 0;
                }
            } else run_[ph] = 0;
        }
    }
    // decode candidates once the longest frame has been received
    // (tried again every few bit times while the frame is still arriving: its length is only known after the PHR)
    while (!cands_.empty() && cands_.front().at + (int64_t)100 * kBit < g_ && g_ >= needAt_) {
        const Cand c = cands_.front();
        if (c.at < decodedUntil_) { cands_.erase(cands_.begin()); continue; }
        needAt_ = 0;
        if (decode(c.at) == 1 && c.at + (int64_t)kNeedBits * kBit >= g_) break;   // wait for more (needAt_ says until when)
        cands_.erase(cands_.begin());
    }
    const int64_t keep = (cands_.empty() ? g_ : cands_.front().at) - 4 * kBit;
    if (keep - base_ > 65536) {
        buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)(keep - base_));
        base_ = keep;
    }
}

int Bpsk154Demod::decode(int64_t sEnd) {
    const Pn& P = pnTable();
    // the symbol windows seen: one every kBit samples back from sEnd. Alignment: the offset (-2 .. +2) with the strongest chip-product sum
    int64_t best = sEnd; float bestSum = -1;
    for (int d = -2; d <= 2; d++) {
        float sum = 0;
        for (int j = 0; j < 8; j++) {
            std::complex<float> q; float e;
            if (qAt(sEnd + d - (int64_t)j * kBit, q, e)) sum += std::abs(q);
        }
        if (sum > bestSum) { bestSum = sum; best = sEnd + d; }
    }
    // the chip-to-chip rotation (carrier offset) from the preamble windows
    std::complex<float> acc(0, 0);
    for (int j = 0; j < 8; j++) { std::complex<float> q; float e; if (qAt(best - (int64_t)j * kBit, q, e)) acc += q; }
    if (std::abs(acc) < 1e-12f) return 0;
    const double phi = std::atan2(acc.imag(), acc.real());      // rad per chip
    const double w = phi / kSps;                                // rad per sample
    const double cfo = phi * chipRate_ / (2 * kPi);
    // despread one bit window at start s with the offset removed
    auto despread = [&](int64_t s) {
        std::complex<float> c(0, 0);
        for (int k = 0; k < kChips; k++) {
            const double a = -w * (double)(s - best + kSps * k);   // referred to the one sample `best` for every window, so the windows stay in step
            c += P.pn[k] * at(s + kSps * k) * std::complex<float>((float)std::cos(a), (float)std::sin(a));
        }
        return c;
    };
    // walk the bits from the first window seen with the preamble, a window (and its predecessor) at a time
    int64_t s = best - (int64_t)7 * kBit;     // the first of the 8 windows used above
    std::complex<float> prev = despread(s);
    s += kBit;
    int state = 0, zeros = 8, sfd = 0, len = 0, bitIx = 0;
    std::vector<uint8_t> bytes;
    const int sfdBits[8] = {1, 1, 1, 0, 0, 1, 0, 1};
    double resid = 0; int nres = 0;
    for (int n = 0; n < kNeedBits + 24; n++) {
        if (s + kBit + 2 + kSps * kChips > base_ + (int64_t)buf_.size()) {   // the frame is still arriving
            needAt_ = state >= 3 ? s + (int64_t)(len * 8 - bitIx + 3) * kBit + 4 * kBit : g_ + 20 * kBit;
            return 1;
        }
        // timing: the window a sample early or late may be stronger when the senders clock is off
        std::complex<float> c = despread(s), cm = despread(s - 1), cp = despread(s + 1);
        int adj = 0;
        if (std::abs(cm) > std::abs(c) * 1.03f && std::abs(cm) >= std::abs(cp)) { c = cm; adj = -1; }
        else if (std::abs(cp) > std::abs(c) * 1.03f) { c = cp; adj = 1; }
        s += kBit + adj;
        const std::complex<float> z = c * std::conj(prev);
        prev = c;
        const int bit = z.real() < 0 ? 1 : 0;
        const float mag = std::abs(z);
        if (mag > 1e-12f) { const double cosv = std::fabs(z.real()) / mag; resid += 1 - cosv * cosv; nres++; }
        if (state == 0) {
            if (bit == 0) { if (++zeros > 40) return 0; continue; }
            if (zeros < 8) return 0;
            sfd = 0; state = 1;
            if (bit != sfdBits[0]) return 0;
            sfd = 1;
            continue;
        }
        if (state == 1) {
            if (bit != sfdBits[sfd]) return 0;
            if (++sfd == 8) { state = 2; bitIx = 0; len = 0; }
            continue;
        }
        if (state == 2) {
            len |= bit << bitIx;
            if (++bitIx == 8) {
                len &= 0x7F;
                if (len < 3) return 0;
                bytes.assign((size_t)len, 0);
                state = 3; bitIx = 0;
            }
            continue;
        }
        bytes[(size_t)(bitIx >> 3)] |= (uint8_t)(bit << (bitIx & 7));
        if (++bitIx == len * 8) {
            Result r;
            r.psdu = std::move(bytes);
            float pw = 0; int np = 0;
            for (int64_t i = best - 7 * (int64_t)kBit; i <= best; i += kSps) { pw += std::norm(at(i)); np++; }
            r.rssiDb = (float)(10 * std::log10(pw / std::max(1, np) + 1e-20));
            r.cfoHz = cfo;
            const double rr = nres ? resid / nres : 1;
            r.snrDb = rr > 1e-6 ? std::min(40.0, 10 * std::log10(1.0 / rr)) : 40;
            r.endSample = (double)s;
            decodedUntil_ = s;
            if (onFrame) onFrame(r);
            return 0;
        }
    }
    return 0;
}

} // namespace dect2
