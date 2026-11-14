// PCSK225 demod + frame assembly, ported from main.c of sp5wwp/e-Czas (https://github.com/sp5wwp/e-Czas, GPL-3.0; OnAir is GPL-3.0 too, THIRD_PARTY.md).
// Input is the int16 symbol stream at 500 Hz: the phase change over each 20 ms bit (the first version took it from an FM discriminator).
#include "dect2/eczas/eczas_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dect2::eczas {

namespace {

constexpr int kPacketBits = 96;
constexpr int kSamplesPerBit = 10;
constexpr int kSyncSymbols = 16;
constexpr int kLargeBufLen = kPacketBits * kSamplesPerBit + 5;
constexpr int kSkipConfirmed = 1500 - 50;
constexpr int kSkipUnconfirmed = 20;
constexpr int32_t kThreshNominal = 8192; // sp5wwp: half of nominal +/-16384 symbol deviation after AGC

const int8_t kSyncPattern[kSyncSymbols] = {-1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1};

class Pcsk225Demod {
public:
    void reset() {
        sIdx_ = 0;
        skipSamples_ = false;
        skipCnt_ = 0;
        skipLen_ = 0;
        peakAbs_ = kThreshNominal * 2;
        std::memset(s_, 0, sizeof(s_));
        for (int i = 0; i < kSyncSymbols; i++) symbols_[i] = &s_[i * kSamplesPerBit];
    }

    // 4.1: the symbol is the keying state itself (+-16384 about the midpoint of the two phases, from a coherent reference), not the phase change
    // over a bit: the bit is the sign, and the sync needs only the first symbol on the right side
    void setCoherent(bool on) { coherent_ = on; }

    void setPeakHint(int32_t peakAbs) { peakAbs_ = std::max(peakAbs_, std::max(peakAbs, 512)); }

    void push(int16_t sample, uint64_t idx, std::vector<EczasRxPacket>& out, EczasRxStats& st) {
        peakAbs_ = std::max(peakAbs_ - (peakAbs_ >> 14), (int32_t)std::abs(sample));   // forgets a noise spike in about a minute
        // sp5wwp uses a fixed 8192 slice threshold after AGC; cap so int16 near full scale still demods.
        const int32_t thresh = std::min(kThreshNominal, std::max(512, peakAbs_ / 2));

        s_[sIdx_] = sample;
        sIdx_++;
        if (sIdx_ >= kLargeBufLen) sIdx_ = 0;

        for (int i = 0; i < kSyncSymbols; i++) {
            if (symbols_[i] < &s_[kLargeBufLen - 1])
                symbols_[i]++;
            else
                symbols_[i] = s_;
        }

        if (skipSamples_) {
            skipCnt_++;
            if (skipCnt_ >= skipLen_) {
                skipCnt_ = 0;
                skipSamples_ = false;
            }
            return;
        }

        int32_t corr = 0;
        for (int i = 0; i < kSyncSymbols; i++) corr += *symbols_[i] * kSyncPattern[i];

        if (corr > kSyncSymbols * thresh && *symbols_[0] < (coherent_ ? 0 : -thresh)) {
            int32_t corrMax = corr;
            uint8_t shiftMax = 0;
            for (uint8_t shift = 1; shift <= 5; shift++) {
                int32_t corrS = 0;
                for (int i = 0; i < kSyncSymbols; i++) {
                    ptrdiff_t off = symbols_[i] - s_ + shift;
                    off %= kLargeBufLen;
                    if (off < 0) off += kLargeBufLen;
                    corrS += s_[(size_t)off] * kSyncPattern[i];
                }
                if (corrS > corrMax) {
                    corrMax = corrS;
                    shiftMax = shift;
                } else
                    break;
            }

            uint8_t raw[kFrameBytes]{};
            uint8_t b = 1;
            for (int i = 0; i < kPacketBits; i++) {
                const uint16_t idx = (uint16_t)((sIdx_ + shiftMax + i * kSamplesPerBit) % kLargeBufLen);
                const int16_t symb = s_[idx];
                if (coherent_) b = symb > 0 ? 1 : 0;
                else if (std::abs(symb) > thresh) b = uint8_t(!b);
                raw[i / 8] |= (uint8_t)(b << (7 - (i % 8)));
            }

            EczasRxPacket pkt{};
            std::memcpy(pkt.frame, raw, kFrameBytes);
            pkt.endIdx = idx + shiftMax;
            pkt.syncQuality = (float)corrMax / (float)(kSyncSymbols * thresh);
            const uint8_t syncOk = (raw[0] == 0x55 && raw[1] == 0x55);
            st.syncs++;
            if (!syncOk) st.falseSync++;
            else {
                st.frames++;
                if (decodeTimeFrame(raw, pkt.time)) st.crcOk++;
                else if (pkt.time.status == DecodeStatus::BadCrc) st.crcBad++;
                else st.otherType++;
                out.push_back(pkt);   // every frame behind a sync word; pkt.time.status says what it is
            }

            skipLen_ = syncOk ? kSkipConfirmed : kSkipUnconfirmed;
            skipSamples_ = true;
            skipCnt_ = 0;
        }
    }

private:
    int16_t s_[kLargeBufLen]{};
    uint16_t sIdx_ = 0;
    int16_t* symbols_[kSyncSymbols]{};
    bool skipSamples_ = false;
    uint16_t skipCnt_ = 0;
    uint16_t skipLen_ = 0;
    int32_t peakAbs_ = kThreshNominal * 2;
    bool coherent_ = false;
};

static int decimationFactor(double pcmRateHz) {
    const int d = (int)std::lround(pcmRateHz / kDiscRateHz);
    return d >= 1 ? d : 1;
}

} // namespace

struct EczasReceiver::Impl {
    Pcsk225Demod demod;
};

EczasReceiver::EczasReceiver() = default;
EczasReceiver::~EczasReceiver() = default;

void EczasReceiver::reset() {
    inCount_ = 0;
    outCount_ = 0;
    queue_.clear();
    stats_ = {};
    if (demod_) demod_->demod.reset();
}

void EczasReceiver::setPeakHint(int32_t peakAbsSample) {
    if (!demod_) demod_ = std::make_unique<Impl>();
    demod_->demod.setPeakHint(peakAbsSample);
}

void EczasReceiver::setCoherent(bool on) {
    if (!demod_) demod_ = std::make_unique<Impl>();
    demod_->demod.setCoherent(on);
}

void EczasReceiver::configure(double pcmRateHz, int decimPhase) {
    pcmRate_ = pcmRateHz > 0 ? pcmRateHz : kDiscRateHz;
    decim_ = decimationFactor(pcmRate_);
    decimPhase_ = decimPhase % std::max(decim_, 1);
    if (decimPhase_ < 0) decimPhase_ += decim_;
    if (!demod_) demod_ = std::make_unique<Impl>();
    demod_->demod.reset();
    queue_.clear();
    stats_ = {};
    inCount_ = 0;
    outCount_ = 0;
}

void EczasReceiver::feed(const int16_t* x, size_t n) {
    if (!demod_) configure(pcmRate_);
    for (size_t i = 0; i < n; i++, inCount_++) {
        if (decim_ == 1 || (int)(inCount_ % (uint64_t)decim_) == decimPhase_) demod_->demod.push(x[i], outCount_++, queue_, stats_);
    }
}

bool EczasReceiver::pop(EczasRxPacket& out) {
    if (queue_.empty()) return false;
    out = queue_.front();
    queue_.erase(queue_.begin());
    return true;
}

std::vector<EczasRxPacket> decodeDiscPcm(const int16_t* x, size_t n, double pcmRateHz, int decimPhase) {
    std::vector<EczasRxPacket> out;
    int32_t peak = 1;
    for (size_t i = 0; i < n; i++) peak = std::max(peak, std::abs(x[i]));
    EczasReceiver rx;
    rx.configure(pcmRateHz, decimPhase);
    rx.setPeakHint(peak);
    rx.feed(x, n);
    EczasRxPacket pkt;
    while (rx.pop(pkt)) if (pkt.time.status == DecodeStatus::CrcOk) out.push_back(pkt);
    return out;
}

} // namespace dect2::eczas
