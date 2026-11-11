// UAT demodulator: phase-difference slicing at 2 samples/symbol (dump978 algorithm).
#include "dect2/uat/uat_demod.h"
#include "dect2/uat/uat_frame.h"
#include "dect2/dsp/fast_phase.h"
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

static int16_t phiDiff(uint16_t a, uint16_t b) {
    const int32_t d = (int32_t)b - (int32_t)a;
    if (d >= 32768) return (int16_t)(d - 65536);
    if (d < -32768) return (int16_t)(d + 65536);
    return (int16_t)d;
}

static int syncFuzzy(uint64_t word, uint64_t expected) {
    if (word == expected) return 1;
    uint64_t diff = word ^ expected;
    for (int i = 0; i < 4; i++) {
        diff &= diff - 1;
        if (!diff) return 1;
    }
    return 0;
}

static bool checkSync(const uint16_t* phi, uint64_t pattern, int16_t* center) {
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

static void demodBytes(const uint16_t* phi, uint8_t* frame, int bytes, int16_t center) {
    while (--bytes >= 0) {
        uint8_t b = 0;
        if (phiDiff(phi[0], phi[1]) > center) b |= 0x80;
        if (phiDiff(phi[2], phi[3]) > center) b |= 0x40;
        if (phiDiff(phi[4], phi[5]) > center) b |= 0x20;
        if (phiDiff(phi[6], phi[7]) > center) b |= 0x10;
        if (phiDiff(phi[8], phi[9]) > center) b |= 0x08;
        if (phiDiff(phi[10], phi[11]) > center) b |= 0x04;
        if (phiDiff(phi[12], phi[13]) > center) b |= 0x02;
        if (phiDiff(phi[14], phi[15]) > center) b |= 0x01;
        *frame++ = b;
        phi += 16;
    }
}

static int demodDownlink(const uint16_t* phi, uint8_t* to, int* rs) {
    int16_t center = 0;
    if (!checkSync(phi, kAdsbSync, &center)) { *rs = 9999; return 0; }
    demodBytes(phi + kSyncBits * 2, to, UAT_LONG_FRAME_BYTES, center);
    const int ft = correct_adsb_frame(to, rs);
    if (ft == 1) return kSyncBits + UAT_SHORT_FRAME_BITS;
    if (ft == 2) return kSyncBits + UAT_LONG_FRAME_BITS;
    return 0;
}

static int demodUplink(const uint16_t* phi, uint8_t* to, int* rs) {
    int16_t center = 0;
    if (!checkSync(phi, kUplinkSync, &center)) { *rs = 9999; return 0; }
    uint8_t interleaved[UAT_UPLINK_FRAME_BYTES];
    demodBytes(phi + kSyncBits * 2, interleaved, UAT_UPLINK_FRAME_BYTES, center);
    if (correct_uplink_frame(interleaved, to, rs) == 1) return kSyncBits + UAT_UPLINK_FRAME_BITS;
    return 0;
}

} // namespace

// the phase as 0 .. 65535 for 0 .. 2 pi (dump978's scale), from the float sample: dump978's 8-bit table quantised a weak signal's phase so
// coarsely (a recording at -20 dBFS has about 10 levels a rail) that the frequency steps of the slicer disappeared
uint16_t UatDemod::iqPhase(const cf32& v) {
    const float a = fastAtan2(v.imag(), v.real()) + 3.14159265359f;
    return (uint16_t)std::clamp((int)std::lround(a * (32768.0f / 3.14159265359f)), 0, 65535);
}

void UatDemod::configure(double inputRateHz) {
    rate_ = inputRateHz;
    // two samples a bit (DO-282: 1.041667 Mbit/s): dump978's 2.083333 Msps. The slicer reads every second phase step, so any other rate is
    // resampled to it (at 2 Msps the bits used to be taken as 1 Mbit/s: 4 % slow, and a frame slipped 17 bits)
    workRate_ = 2 * kUatBitRate;
    reset();
    static bool fecOnce = false;
    if (!fecOnce) { init_fec(); fecOnce = true; }
}

void UatDemod::reset() {
    phi_.clear();
    rsPos_ = 0; rsHave_ = false; rsLast_ = cf32(0, 0);
    samples_ = 0;
    noiseDbfs_ = -120;
}

void UatDemod::feed(const cf32* x, size_t n) {
    if (!ready() || n == 0) return;
    const double ratio = rate_ / workRate_;
    if (std::fabs(ratio - 1.0) < 1e-4) {
        pushPhi(x, n);
        samples_ += n;
    } else {   // linear, carried over the blocks: the next output sits at input position rsPos_ (-1 .. 0: between the previous block's last sample and x[0])
        std::vector<cf32> y;
        y.reserve((size_t)((double)n / ratio) + 4);
        while (rsPos_ < (double)n - 1) {
            const double p = rsPos_;
            const long i = (long)std::floor(p);
            const float f = (float)(p - (double)i);
            const cf32 a = i < 0 ? (rsHave_ ? rsLast_ : x[0]) : x[i], b = x[i + 1];
            y.push_back(a * (1.f - f) + b * f);
            rsPos_ += ratio;
        }
        rsPos_ -= (double)n;
        rsLast_ = x[n - 1]; rsHave_ = true;
        pushPhi(y.data(), y.size());
        samples_ += n;
    }
    processPhi();
}

void UatDemod::pushPhi(const cf32* x, size_t n) {
    const size_t base = phi_.size();
    phi_.resize(base + n);
    double pwr = 0;
    for (size_t i = 0; i < n; i++) {
        phi_[base + i] = iqPhase(x[i]);
        pwr += (double)std::norm(x[i]);
    }
    if (n) {
        const float db = (float)(10.0 * std::log10(pwr / (double)n + 1e-20));
        noiseDbfs_ = noiseDbfs_ < -100 ? db : 0.95f * noiseDbfs_ + 0.05f * db;
    }
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

    for (; bit < lenbits; bit++) {
        const int16_t d0 = phiDiff(phi_[(size_t)bit * 2], phi_[(size_t)bit * 2 + 1]);
        const int16_t d1 = phiDiff(phi_[(size_t)bit * 2 + 1], phi_[(size_t)bit * 2 + 2]);
        sync0 = ((sync0 << 1) | (d0 > 0 ? 1 : 0)) & kSyncMask;
        sync1 = ((sync1 << 1) | (d1 > 0 ? 1 : 0)) & kSyncMask;
        if (bit < kSyncBits) continue;
        candidates_++;

        if (syncFuzzy(sync0, kAdsbSync) || syncFuzzy(sync1, kAdsbSync)) {
            const int start = bit - kSyncBits + 1;
            const int shift = syncFuzzy(sync0, kAdsbSync) ? 0 : 1;
            const int index = start * 2 + shift;
            int rs0 = -1, rs1 = -1;
            const int sk0 = demodDownlink(phi_.data() + index, bufA, &rs0);
            const int sk1 = demodDownlink(phi_.data() + index + 1, bufB, &rs1);
            if (sk0 && rs0 <= rs1) {
                UatRawFrame f;
                f.uplink = false;
                f.rsErrors = rs0;
                f.dataBytes = (bufA[0] >> 3) == 0 ? UAT_SHORT_FRAME_DATA_BYTES : UAT_LONG_FRAME_DATA_BYTES;
                std::memcpy(f.data, bufA, (size_t)f.dataBytes);
                f.timeSec = (double)samples_ / rate_ - (double)(len - index) / workRate_;
                f.levelDbfs = noiseDbfs_;
                sink_(f);
                bit = start + sk0;
                continue;
            }
            if (sk1 && rs1 <= rs0) {
                UatRawFrame f;
                f.uplink = false;
                f.rsErrors = rs1;
                f.dataBytes = (bufB[0] >> 3) == 0 ? UAT_SHORT_FRAME_DATA_BYTES : UAT_LONG_FRAME_DATA_BYTES;
                std::memcpy(f.data, bufB, (size_t)f.dataBytes);
                f.timeSec = (double)samples_ / rate_ - (double)(len - index - 1) / workRate_;
                f.levelDbfs = noiseDbfs_;
                sink_(f);
                bit = start + sk1;
                continue;
            }
        } else if (syncFuzzy(sync0, kUplinkSync) || syncFuzzy(sync1, kUplinkSync)) {
            const int start = bit - kSyncBits + 1;
            const int shift = syncFuzzy(sync0, kUplinkSync) ? 0 : 1;
            const int index = start * 2 + shift;
            int rs0 = -1, rs1 = -1;
            const int sk0 = demodUplink(phi_.data() + index, bufA, &rs0);
            const int sk1 = demodUplink(phi_.data() + index + 1, bufB, &rs1);
            if (sk0 && rs0 <= rs1) {
                UatRawFrame f;
                f.uplink = true;
                f.rsErrors = rs0;
                f.dataBytes = UAT_UPLINK_FRAME_DATA_BYTES;
                std::memcpy(f.data, bufA, (size_t)f.dataBytes);
                f.timeSec = (double)samples_ / rate_ - (double)(len - index) / workRate_;
                f.levelDbfs = noiseDbfs_;
                sink_(f);
                bit = start + sk0;
                continue;
            }
            if (sk1 && rs1 <= rs0) {
                UatRawFrame f;
                f.uplink = true;
                f.rsErrors = rs1;
                f.dataBytes = UAT_UPLINK_FRAME_DATA_BYTES;
                std::memcpy(f.data, bufB, (size_t)f.dataBytes);
                f.timeSec = (double)samples_ / rate_ - (double)(len - index - 1) / workRate_;
                f.levelDbfs = noiseDbfs_;
                sink_(f);
                bit = start + sk1;
                continue;
            }
        }
    }

    const int consumed = std::max(0, (bit - kSyncBits) * 2);
    if (consumed > 0 && consumed < len) phi_.erase(phi_.begin(), phi_.begin() + consumed);
    else if (consumed >= len) phi_.clear();
}

} // namespace dect2
