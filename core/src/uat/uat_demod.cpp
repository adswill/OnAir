// UAT demodulator: phase-difference slicing at 2 samples per bit (dump978's algorithm, GPL: Oliver Jowett), with since 3.9:
//   - the input resampled to exactly twice the bit rate (2.083333 Msps; ExactResampler, which also low-passes a wider capture). Up to 3.8 the
//     demodulator took 2 samples of the input as one bit whatever its rate, so a real 1.041667 Mbit/s signal at the default 2 Msps slipped 4 %
//     (a sample every 12 bits) and never synced; only the old test signal, made the same way, decoded.
//   - the phase in floating point (fastAtan2) instead of an 8-bit table of I and Q: a weak signal from a 12 or 16-bit radio no longer drowns
//     in the rounding to 256 levels
//   - soft decisions: every byte keeps the margin of its weakest bit; a frame whose Reed-Solomon check fails is decoded again with its least
//     reliable bytes as erasures (2 to 4 of a long downlink frame, 2 of a basic one, 4 to 10 per uplink block)
//   - a 3-tap binomial low-pass before the phase (about 1.5 dB: the noise outside the main lobe no longer reaches the slicer)
//   - the level of a frame from its own samples and the noise floor from the quietest blocks (the reported SNR was about 0 dB before)
#include "dect2/uat/uat_demod.h"
#include "dect2/dsp/fast_phase.h"
#include "dect2/uat/uat_frame.h"
extern "C" {
#include "fec.h"
}
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {

constexpr int kSyncBits = 36;
constexpr uint64_t kAdsbSync = 0xEACDDA4E2UL;
constexpr uint64_t kUplinkSync = 0x153225B1DUL;
constexpr uint64_t kSyncMask = (1ULL << kSyncBits) - 1;
constexpr int kNoiseBlock = 1024;

int16_t phiDiff(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(b - a); }

int syncFuzzy(uint64_t word, uint64_t expected) {
    uint64_t diff = word ^ expected;
    for (int i = 0; i < 5 && diff; i++) diff &= diff - 1;
    return diff == 0;   // at most 4 bits wrong
}

bool checkSync(const uint16_t* phi, uint64_t pattern, int16_t* center) {
    int32_t z0 = 0, z1 = 0, n0 = 0, n1 = 0;
    for (int i = 0; i < kSyncBits; i++) {
        const int16_t d = phiDiff(phi[i * 2], phi[i * 2 + 1]);
        if (pattern & (1ULL << (35 - i))) { z1 += d; n1++; }
        else { z0 += d; n0++; }
    }
    if (n0 < 1 || n1 < 1) return false;
    *center = (int16_t)((z0 / n0 + z1 / n1) / 2);
    int err = 0;
    for (int i = 0; i < kSyncBits; i++) {
        const int16_t d = phiDiff(phi[i * 2], phi[i * 2 + 1]);
        const bool one = pattern & (1ULL << (35 - i));
        if (one ? d < *center : d >= *center) err++;
    }
    return err <= 4;
}

// hard bytes and, per byte, the margin of its weakest bit (the soft information for the erasures)
void demodBytes(const uint16_t* phi, uint8_t* frame, float* rel, int bytes, int16_t center) {
    for (int i = 0; i < bytes; i++) {
        uint8_t b = 0;
        int r = 1 << 20;
        for (int k = 0; k < 8; k++) {
            const int d = (int)phiDiff(phi[2 * k], phi[2 * k + 1]) - center;
            if (d > 0) b |= (uint8_t)(0x80 >> k);
            r = std::min(r, d < 0 ? -d : d);
        }
        frame[i] = b;
        rel[i] = (float)r;
        phi += 16;
    }
}

// the indices of the f least reliable of the first n bytes, in rising order
int weakest(const float* rel, int n, int f, int* out) {
    int idx[UAT_UPLINK_BLOCK_BYTES];
    for (int i = 0; i < n; i++) idx[i] = i;
    f = std::min(f, n);
    std::partial_sort(idx, idx + f, idx + n, [rel](int a, int b) { return rel[a] < rel[b] || (rel[a] == rel[b] && a < b); });
    std::sort(idx, idx + f);
    std::copy(idx, idx + f, out);
    return f;
}

} // namespace

int UatDemod::demodDownlink(const uint16_t* phi, uint8_t* to, int* rs) {
    int16_t center = 0;
    if (!checkSync(phi, kAdsbSync, &center)) { *rs = 9999; return 0; }
    float rel[UAT_LONG_FRAME_BYTES];
    demodBytes(phi + kSyncBits * 2, to, rel, UAT_LONG_FRAME_BYTES, center);
    uint8_t keep[UAT_LONG_FRAME_BYTES];
    std::memcpy(keep, to, sizeof keep);
    int ft = correct_adsb_frame(to, rs);
    if (ft < 1 && soft_) {
        int eL[8], eS[8];
        for (int f = 2; f <= 4 && ft < 1; f += 2) {   // at most 4 of 14 (long) and 2 of 12 (short) roots spent: noise then passes about once in 1e6 syncs
            std::memcpy(to, keep, sizeof keep);
            const int nl = weakest(rel, UAT_LONG_FRAME_BYTES, f, eL), ns = weakest(rel, UAT_SHORT_FRAME_BYTES, 2, eS);
            ft = correct_adsb_frame_eras(to, eL, nl, eS, ns, rs);
            if (ft >= 1) erasureFixes_++;
        }
    }
    if (ft == 1) return kSyncBits + UAT_SHORT_FRAME_BITS;
    if (ft == 2) return kSyncBits + UAT_LONG_FRAME_BITS;
    return 0;
}

int UatDemod::demodUplink(const uint16_t* phi, uint8_t* to, int* rs) {
    int16_t center = 0;
    if (!checkSync(phi, kUplinkSync, &center)) { *rs = 9999; return 0; }
    uint8_t il[UAT_UPLINK_FRAME_BYTES];
    float rel[UAT_UPLINK_FRAME_BYTES];
    demodBytes(phi + kSyncBits * 2, il, rel, UAT_UPLINK_FRAME_BYTES, center);
    int total = 0;
    bool usedEras = false;
    for (int b = 0; b < UAT_UPLINK_FRAME_BLOCKS; b++) {   // deinterleave block b (byte i of the block is byte i x 6 + b of the frame) and correct it
        uint8_t blk[UAT_UPLINK_BLOCK_BYTES], keep[UAT_UPLINK_BLOCK_BYTES];
        float r[UAT_UPLINK_BLOCK_BYTES];
        for (int i = 0; i < UAT_UPLINK_BLOCK_BYTES; i++) { blk[i] = il[i * UAT_UPLINK_FRAME_BLOCKS + b]; r[i] = rel[i * UAT_UPLINK_FRAME_BLOCKS + b]; }
        std::memcpy(keep, blk, sizeof blk);
        int n = correct_uplink_block_eras(blk, nullptr, 0);
        if (n > 10) n = -1;                                   // dump978's limit without erasures
        if (n < 0 && soft_) {
            int e[10];
            for (int f = 4; f <= 10 && n < 0; f += 3) {
                std::memcpy(blk, keep, sizeof blk);
                n = correct_uplink_block_eras(blk, e, weakest(r, UAT_UPLINK_BLOCK_BYTES, f, e));
                usedEras |= n >= 0;
            }
        }
        if (n < 0) { *rs = 9999; return 0; }
        total += n;
        std::memcpy(to + b * UAT_UPLINK_BLOCK_DATA_BYTES, blk, UAT_UPLINK_BLOCK_DATA_BYTES);
    }
    if (usedEras) erasureFixes_++;
    *rs = total;
    return kSyncBits + UAT_UPLINK_FRAME_BITS;
}

void UatDemod::configure(double inputRateHz) {
    rate_ = inputRateHz;
    pass_ = std::fabs(rate_ / kWorkRate - 1.0) < 1e-5;
    if (!pass_ && rate_ > 0) pass_ = !rs_.configure(rate_, kWorkRate, 8);   // a ratio the resampler cannot do: as it comes (does not happen for >= 1.9 Msps)
    reset();
    static bool fecOnce = false;
    if (!fecOnce) { init_fec(); fecOnce = true; }
}

void UatDemod::reset() {
    phi_.clear();
    pw_.clear();
    rs_.reset();
    samples_ = 0;
    workBase_ = 0;
    blkSum_ = 0; blkN_ = 0;
    nf_ = 0;
    noiseDbfs_ = -120;
}

void UatDemod::feed(const cf32* x, size_t n) {
    if (!ready()) return;
    if (pass_) pushPhi(x, n);
    else { tmp_.clear(); rs_.process(x, n, tmp_); pushPhi(tmp_.data(), tmp_.size()); }
    samples_ += n;
    processPhi();
}

void UatDemod::pushPhi(const cf32* x, size_t n) {
    const size_t base = phi_.size();
    phi_.resize(base + n);
    pw_.resize(base + n);
    for (size_t i = 0; i < n; i++) {
        // low-pass before the phase (binomial taps at the work rate: the noise beyond the signal's main lobe is left out)
        for (int k = 4; k > 0; k--) pf_[k] = pf_[k - 1];
        pf_[0] = x[i];
        const cf32 y = pre_ == 0 ? pf_[0] : pre_ == 1 ? 0.25f * pf_[0] + 0.5f * pf_[1] + 0.25f * pf_[2]
                                                     : 0.0625f * (pf_[0] + pf_[4]) + 0.25f * (pf_[1] + pf_[3]) + 0.375f * pf_[2];
        const float a = fastAtan2(y.imag(), y.real());
        phi_[base + i] = (uint16_t)(int32_t)std::lrintf(a * 10430.378f);   // 32768 / pi: a full turn is 65536
        const float p = std::norm(x[i]);
        pw_[base + i] = p;
        blkSum_ += p;
        if (++blkN_ == kNoiseBlock) {   // noise floor: falls at once to a quieter block, rises slowly (bursts are short)
            const double m = blkSum_ / kNoiseBlock;
            nf_ = nf_ <= 0 ? m : (m < nf_ ? 0.5 * nf_ + 0.5 * m : std::min(m, nf_ * 1.02));
            blkSum_ = 0; blkN_ = 0;
            noiseDbfs_ = (float)(10.0 * std::log10(nf_ + 1e-20));
        }
    }
}

float UatDemod::levelOf(int index, int bits) const {
    const int end = std::min((int)pw_.size(), index + 2 * bits);
    double s = 0;
    for (int i = index; i < end; i++) s += pw_[(size_t)i];
    return end > index ? (float)(10.0 * std::log10(s / (end - index) + 1e-20)) : -120.0f;
}

void UatDemod::processPhi() {
    if (!sink_) return;
    const int len = (int)phi_.size();
    const int need = (kSyncBits + UAT_UPLINK_FRAME_BITS) * 2 + 8;
    if (len < need) return;

    uint64_t sync0 = 0, sync1 = 0;
    const int lenbits = len / 2 - (kSyncBits + UAT_UPLINK_FRAME_BITS);
    int bit = 0;
    uint8_t bufA[UAT_UPLINK_FRAME_BYTES], bufB[UAT_UPLINK_FRAME_BYTES];

    auto emit = [&](const uint8_t* buf, bool uplink, int rs, int index, int bits) {
        UatRawFrame f;
        f.uplink = uplink;
        f.rsErrors = rs;
        f.dataBytes = uplink ? UAT_UPLINK_FRAME_DATA_BYTES : ((buf[0] >> 3) == 0 ? UAT_SHORT_FRAME_DATA_BYTES : UAT_LONG_FRAME_DATA_BYTES);
        std::memcpy(f.data, buf, (size_t)f.dataBytes);
        f.timeSec = (double)(workBase_ + (uint64_t)index) / kWorkRate;
        f.levelDbfs = levelOf(index, bits);
        sink_(f);
    };

    for (; bit < lenbits; bit++) {
        const int16_t d0 = phiDiff(phi_[(size_t)bit * 2], phi_[(size_t)bit * 2 + 1]);
        const int16_t d1 = phiDiff(phi_[(size_t)bit * 2 + 1], phi_[(size_t)bit * 2 + 2]);
        sync0 = ((sync0 << 1) | (d0 > 0 ? 1 : 0)) & kSyncMask;
        sync1 = ((sync1 << 1) | (d1 > 0 ? 1 : 0)) & kSyncMask;
        if (bit < kSyncBits) continue;
        candidates_++;
        const bool down = syncFuzzy(sync0, kAdsbSync) || syncFuzzy(sync1, kAdsbSync);
        const bool up = !down && (syncFuzzy(sync0, kUplinkSync) || syncFuzzy(sync1, kUplinkSync));
        if (!down && !up) continue;
        const uint64_t pat = down ? kAdsbSync : kUplinkSync;
        const int start = bit - kSyncBits + 1;
        const int index = start * 2 + (syncFuzzy(sync0, pat) ? 0 : 1);
        int rs0 = -1, rs1 = -1;
        const int sk0 = down ? demodDownlink(phi_.data() + index, bufA, &rs0) : demodUplink(phi_.data() + index, bufA, &rs0);
        const int sk1 = down ? demodDownlink(phi_.data() + index + 1, bufB, &rs1) : demodUplink(phi_.data() + index + 1, bufB, &rs1);
        if (sk0 && rs0 <= rs1) { emit(bufA, up, rs0, index, sk0); bit = start + sk0; continue; }
        if (sk1 && rs1 <= rs0) { emit(bufB, up, rs1, index + 1, sk1); bit = start + sk1; continue; }
    }

    const int consumed = std::min(len, std::max(0, (bit - kSyncBits) * 2));
    if (consumed > 0) {
        phi_.erase(phi_.begin(), phi_.begin() + consumed);
        pw_.erase(pw_.begin(), pw_.begin() + consumed);
        workBase_ += (uint64_t)consumed;
    }
}

} // namespace dect2
