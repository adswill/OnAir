// DTMB transmitter model (symbol rate): transport stream packets -> scrambler -> BCH -> LDPC -> mapping (and interleaver) -> frame body
// (system information, carrier interleaver, 3780 point inverse FFT) -> PN header. Used by the test signal and the tests; nothing is transmitted.
#pragma once
#include "dtmb_defs.h"
#include "dtmb_fft.h"
#include "dtmb_ldpc.h"
#include "ring.h"
#include <functional>
#include <memory>

namespace dect2::dtmb {

struct TxConfig {
    Header header = Header::Pn945;
    int carriers = 3780;           // 3780: multi-carrier (OFDM body); 1: single carrier (the body is time domain: 36 system information symbols, then the data)
    Profile profile;               // 64QAM, rate 0.6, interleaver mode 1
    bool phaseRotate = true;       // PN phase rotation inside the super-frame (PN420, PN945)
    int firstFrame = 0;            // frame number inside the super-frame at which the stream starts
    bool warmUp = true;            // run the transmitter until the interleaver is full before the first frame that is returned (else the
                                   // first 84 or 253 ms carry mostly empty symbols)
};

class FrameTx {
public:
    using TsSource = std::function<void(uint8_t*)>;   // fills one 188 byte packet
    FrameTx(const TxConfig& cfg, TsSource ts);
    const TxConfig& config() const { return cfg_; }
    int frameLength() const { return dtmb::frameLength(cfg_.header); }
    // Writes the symbols of the next signal frame: the header, then the 3780 body symbols (time domain)
    void nextFrame(cf32* out);
    // Same, and the 3744 data symbols before the interleaver are not exposed: tests that want the frequency domain use bodyCarriers().
    long frameNumber() const { return frame_; }       // frames produced so far
    // The 3780 carriers (physical order, before the inverse FFT) of the frame produced last; for C=1 the 3780 body symbols
    const std::vector<cf32>& lastCarriers() const { return carriers_; }
    // Frame number inside the super-frame of the next frame, and its PN phase
    int superIndex() const;

private:
    void encodeCodeword();
    TxConfig cfg_;
    TsSource ts_;
    const LdpcCode& ldpc_;
    MixedFft fft_;
    Scrambler scr_;
    long pkCount_ = 0, frame_ = 0;
    std::vector<uint8_t> coded_;      // transmitted LDPC bits not yet used
    size_t coded0_ = 0;
    ConvInterleaver<cf32> symIl_;
    ConvInterleaver<uint8_t> bitIl_;
    std::vector<cf32> carriers_;
    std::vector<uint8_t> siChips_;
};

} // namespace dect2::dtmb
