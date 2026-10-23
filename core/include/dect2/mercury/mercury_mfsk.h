// Mercury's MFSK floor (the bottom rung of its ARQ ladder, and the floor of a BW500 session), receive side and the test signal.
// Written for OnAir from Mercury's modem/modem_mfsk.c and modem/mfsk/*.c (commit c72997be9d3c; the waveform comes from Mercury v1 by
// Fadi Jerji): 32-FSK over OFDM subcarriers. A symbol is one of 32 tones, Gray coded, hopped by 13 tones per symbol, an FFT of 256
// (MFSK500: 512) samples behind a cyclic prefix of a quarter of it, tones 31.25 Hz (15.625 Hz) apart on 1015.6 .. 1984.4 Hz
// (1257.8 .. 1742.2 Hz) of the audio. A burst: 4 preamble symbols (tones 4, 20, 12, 28), 320 data symbols (the 1600 bits of the rate
// 8/16 LDPC codeword of a 100-byte frame, 98 bytes and the CRC-16, spread by a fixed pseudo-random interleaver), 4 postamble symbols
// (the preamble's tones + 8). 13.1 s (26.2 s) on the air. The receiver is OnAir's own: a spectrogram at half-bin resolution, the
// preamble and postamble found by the share of energy in their tones over 13 (25) frequency offsets up to +-94 Hz, the sample clock
// from the two anchors, the soft bits as Mercury's demodulator computes them.
#pragma once
#include "dect2/mercury/mercury_phy.h"
#include <memory>
#include <vector>

namespace dect2 {
namespace mercury {

struct MfskGeometry {
    int mode;                   // kMfsk or kMfsk500
    int nfft, ngi, nofdm;       // FFT size, cyclic prefix, samples per symbol (8 kHz)
    double fcHz;                // Mercury's carrier (the tones sit symmetrically about 1500 Hz)
    double spacingHz;           // 8000 / nfft
    int preSyms, paySyms;       // 4, 320
    int burstSamples;
};
const MfskGeometry& mfskGeometry(int mode);
// the audio frequency of tone a (0 .. 31) of a symbol
double mfskToneHz(int mode, int a);
// the transmitted-slot -> codeword-bit permutation (mfsk_interleave_init)
const std::vector<int>& mfskInterleaver();

// One burst as the analytic signal of the 8 kHz audio (as modulateBurst); frame: 100 bytes (makeMfskFrame)
std::vector<cf> modulateMfskBurst(int mode, const std::vector<uint8_t>& frame);
std::vector<uint8_t> makeMfskFrame(const uint8_t* payload, size_t n, uint16_t seed = 0);

// The MFSK part of the receiver: the same 8 kHz input as OfdmReceiver (1500 Hz at 0 Hz); OfdmReceiver runs one for each MFSK mode
class MfskReceiver {
public:
    explicit MfskReceiver(int mode);
    ~MfskReceiver();
    void reset();
    void push(const cf* x, size_t n);    // with the absolute index of x[0] = the samples pushed before
    void flush();
    std::vector<OfdmFrame> take();
    void addDecodedSpan(int64_t start, int64_t end);   // a burst another receiver decoded: nothing inside it is tried
    uint64_t detections() const;
    uint64_t failures() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace mercury
} // namespace dect2
