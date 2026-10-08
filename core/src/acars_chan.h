// One ACARS channel: from the wide input down to audio, AM detection, MSK demodulation of the 2400 bit/s data, and the bit-level
// framing up to the check sequence. The first stage is a 4th-order CIC (integer arithmetic, so it cannot drift) down to about
// 100 kS/s, then a channel filter down to 25 kS/s, the envelope, and 12.5 kS/s audio for the demodulator.
// MSK demodulator and framing follow acarsdec (msk.c, acars.c: matched filter, 1800 Hz VCO, 3 pi/2 per bit, SYN SYN SOH search).
#pragma once
#include "dect2/ring.h"
#include <cstdint>
#include <vector>

namespace dect2 {

struct AcarsRawBlock {
    uint8_t txt[256];            // after SOH, up to and including the suffix, parity bits still on
    int len = 0;
    uint8_t crc[2] = {0, 0};
    float levelDb = -120;        // carrier level during the block, dB below full scale
    bool ok = false;             // the check sequence matched (after the repair, which is already applied to txt)
    int fixedBits = 0, parityErrors = 0;
};

class AcarsChannelRx {
public:
    // inRate: the input sample rate (at least 1 Msps). offsetHz: where the channel sits in the input, relative to 0 Hz.
    void configure(double inRate, double offsetHz);
    void reset();
    void setOffset(double offsetHz);           // move to another channel (a reset follows)
    void process(const cf32* x, size_t n);
    // blocks that ended since the last call
    void takeBlocks(std::vector<AcarsRawBlock>& out) { out.insert(out.end(), blocks_.begin(), blocks_.end()); blocks_.clear(); }
    uint64_t framesStarted() const { return frames_; }
    float carrierDb() const;                 // smoothed carrier level of the last samples, dB below full scale
    double audioRate() const { return audioRate_; }
    // for the tests: the demodulator's bits (polarity as the framer sees them) since the last call, when recording is on
    void recordBits(bool on) { rec_ = on; bits_.clear(); }
    const std::vector<uint8_t>& bits() const { return bits_; }

private:
    // The MSK demodulator can settle on a sampling instant that suits the data badly, and then loses a bit in a long block. Several
    // demodulators with different starting phases run side by side on the same audio; the one whose block passes the check wins.
    static constexpr int kInst = 3;
    struct Inst {
        double phi = 0, df = 0, clk = 0;
        unsigned s = 0;
        std::vector<cf32> inb; int idx = 0;
        enum St { WSYN, SYN2, SOH1, TXT, CRC1, CRC2, END } st = WSYN;
        uint8_t outbits = 0;
        int nbits = 1;
        AcarsRawBlock cur;
        int perr = 0;
        double lvlSum = 0; int lvlN = 0;
    };
    struct Pending { int64_t t = 0; AcarsRawBlock b; };
    void audioSample(float a);
    void demod(Inst& in, float a);
    void putBit(Inst& in, float v);
    void byteReady(Inst& in);
    void framerReset(Inst& in);
    void finish(Inst& in);

    // front end
    double inRate_ = 0, offsetHz_ = 0, audioRate_ = 12500;
    int d1_ = 20;
    cf32 ph_{1.f, 0.f}, step_{1.f, 0.f};
    int phCount_ = 0;
    uint64_t integ_[2][4] = {};              // I and Q integrators
    uint64_t comb_[2][4] = {};
    int cicCount_ = 0;
    float cicScale_ = 1.f;
    std::vector<float> h1_;                  // channel filter taps, decimate by 4
    std::vector<cf32> buf1_; int pos1_ = 0, cnt1_ = 0;
    float carrier_ = 0;                      // leaky mean of the envelope
    std::vector<float> h2_;                  // audio filter, decimate by 2
    std::vector<float> buf2_; int pos2_ = 0, cnt2_ = 0;
    double w0_ = 0;
    int flen_ = 11;
    std::vector<float> hm_;                  // matched filter, oversampled
    Inst inst_[kInst];
    int64_t audioCount_ = 0;
    std::vector<Pending> pending_;
    std::vector<AcarsRawBlock> blocks_;
    uint64_t frames_ = 0;
    bool rec_ = false;
    std::vector<uint8_t> bits_;
};

} // namespace dect2
