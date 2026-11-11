// GSM 06.10 full-rate speech codec (RPE-LTP, 13 kbit/s), the codec EchoLink carries in its RTP packets (payload type 3).
// Written from the algorithm description of ETSI EN 300 961 (GSM 06.10): the 16-bit fixed-point arithmetic of its section 5 (add, mult_r,
// norm, div ...), preprocessing, LPC analysis (autocorrelation, Schur, log-area ratios), short-term analysis / synthesis filters, long-term
// predictor, RPE grid selection and APCM quantisation. The codec is royalty-free (no patents are enforced on GSM full rate).
// Frame: 160 samples of 16-bit linear PCM at 8 kHz (20 ms) <-> 33 bytes: a 4-bit 0xD signature and the 260 coded bits, most significant
// bit first, in the order LARc[0..7] (6 6 5 5 4 4 3 3 bits), then for each of the 4 subframes Nc (7) bc (2) Mc (2) xmaxc (6) xMc[0..12] (3 each).
// That is the byte layout of the widely used libgsm "gsm_frame" (and of RTP payload type 3, RFC 3551 section 4.5.8).
#pragma once
#include <cstdint>

namespace dect2 {
namespace echolink {

constexpr int kGsmFrameSamples = 160;
constexpr int kGsmFrameBytes = 33;

// The 76 coded parameters of one frame
struct GsmParams {
    int16_t LARc[8];
    int16_t Nc[4], bc[4], Mc[4], xmaxc[4];
    int16_t xMc[4][13];
};

void gsmPack(const GsmParams& p, uint8_t out[kGsmFrameBytes]);
// False when the 0xD signature is missing (the parameters are still unpacked)
bool gsmUnpack(const uint8_t in[kGsmFrameBytes], GsmParams& p);

class GsmEncoder {
public:
    void reset();
    void encode(const int16_t pcm[kGsmFrameSamples], GsmParams& p);
    void encode(const int16_t pcm[kGsmFrameSamples], uint8_t out[kGsmFrameBytes]);
private:
    int16_t dp0_[280] = {};
    int16_t z1_ = 0;
    int32_t Lz2_ = 0;
    int16_t mp_ = 0;
    int16_t u_[8] = {};
    int16_t LARpp_[2][8] = {};
    int j_ = 0;
};

class GsmDecoder {
public:
    void reset();
    void decode(const GsmParams& p, int16_t pcm[kGsmFrameSamples]);
    // False (and 160 samples of silence) when the frame has no 0xD signature
    bool decode(const uint8_t in[kGsmFrameBytes], int16_t pcm[kGsmFrameSamples]);
private:
    int16_t dp0_[280] = {};
    int16_t nrp_ = 40;
    int16_t v_[9] = {};
    int16_t LARpp_[2][8] = {};
    int j_ = 0;
    int16_t msr_ = 0;
};

} // namespace echolink
} // namespace dect2
