// Iridium radio layer: the constants of the burst format, the DQPSK mapping, the burst detector over the whole capture and the
// per-burst demodulator. Facts from gr-iridium (github.com/muccc/gr-iridium: include/iridium/iridium.h, lib/burst_downmix_impl.cc,
// lib/iridium_qpsk_demod_impl.cc, lib/fft_burst_tagger_impl.cc) and iridium-toolkit (bitsparser.py, util.py).
#pragma once
#include "ring.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {
namespace iridium {

constexpr double kSymbolRate = 25000;                 // iridium.h SYMBOLS_PER_SECOND
constexpr double kChannelHz = 1e7 / 240;              // util.py: 30 sub-bands of 8 frequency accesses in 10 MHz (41.667 kHz)
constexpr double kBaseHz = 1616e6;                    // util.py base_freq
constexpr double kSimplexMinHz = 1626e6;              // iridium.h SIMPLEX_FREQUENCY_MIN
constexpr int kDuplexChannels = 240, kSimplexChannels = 12;
constexpr int kUwLen = 12;                            // iridium.h UW_LENGTH
constexpr int kPreambleShort = 16, kPreambleLong = 64;   // iridium.h PREAMBLE_LENGTH_SHORT / _LONG
constexpr int kMaxSymbolsNormal = 191 - kUwLen;       // iridium.h MAX_FRAME_LENGTH_NORMAL counts the unique word
constexpr int kMaxSymbolsSimplex = 444 - kUwLen;      // MAX_FRAME_LENGTH_SIMPLEX
constexpr double kRrcAlpha = 0.4;                     // burst_downmix_impl.cc: root raised cosine, roll-off 0.4

// Unique words as QPSK symbol indices (index m = phase 45 + 90 m degrees, the preamble is index 0): iridium.h UW_DL / UW_UL.
extern const uint8_t kUwDl[kUwLen];
extern const uint8_t kUwUl[kUwLen];
// The same unique words after differential decoding, as bits (bitsparser.py iridium_access / uplink_access)
extern const char* const kUwDlBits;   // "001100000011000011110011"
extern const char* const kUwUlBits;   // "110011000011110011111100"

// Centre frequency of channel ch: 0..239 duplex (1616.0208 .. 1625.9792 MHz), 240..251 simplex (frequency access 1..12).
inline double channelHz(int ch) { return kBaseHz + kChannelHz * (ch + 0.5); }
int nearestChannel(double hz);

// DQPSK as iridium_qpsk_demod_impl.cc decode_deqpsk: a phase step of k quarter turns (counter-clockwise) carries the bit pair
// 0 -> 00, 1 -> 10, 2 -> 11, 3 -> 01 (first bit = high bit of the pair).
inline int dqpskStep(int b0, int b1) { static const int s[4] = {0, 3, 1, 2}; return s[(b0 << 1) | b1]; }
inline int dqpskPair(int step) { static const int p[4] = {0, 2, 3, 1}; return p[step & 3]; }

// QPSK symbol indices of a whole burst: preamble (DL: index 0 repeated; UL: 2, 0 alternating), unique word, then the data bits
// (pairs, differentially encoded from the last unique word symbol). An odd bit count is padded with a 0.
std::vector<uint8_t> burstSymbols(const std::vector<uint8_t>& bits, bool downlink, int preambleSymbols);
// The reverse of the data part: absolute symbol indices -> bits, starting from reference symbol ref.
void symbolsToBits(const uint8_t* sym, size_t n, int ref, std::vector<uint8_t>& bits);
// gr-iridium check_sync_word: 90 degree errors count 1, 180 degree errors 2; accepted when the sum is at most 2.
int uwDistance(const uint8_t* sym, bool downlink);

// Root raised cosine taps (unit energy), span in symbols on each side.
std::vector<float> rrcTaps(double sps, double alpha, int halfSpanSymbols);

// One burst found by the detector, in input samples (absolute sample numbers since configure/reset).
struct DetectedBurst {
    uint64_t start = 0, end = 0;     // first and one past the last sample that may hold the burst (margins included)
    double freqHz = 0;               // centre, relative to 0 Hz of the input
    float peakDb = 0;                // strongest bin over the noise floor
};

// FFT burst detector over the whole band, as gr-iridium fft_burst_tagger: power per bin over a per-bin noise floor, a burst starts
// where a bin rises above the threshold and ends when its channel falls back to the noise for a few FFT frames.
class BurstDetector {
public:
    void configure(double rate, double thresholdDb = 13.0);
    void setThreshold(double thresholdDb);   // keeps the sample count, the noise floor and the bursts in progress
    void reset();
    // Appends the bursts that ended within these samples.
    void feed(const cf32* x, size_t n, std::vector<DetectedBurst>& out);
    double binHz() const { return rate_ / fft_; }
    double noisePerHz() const;               // mean noise floor (power per Hz of the input, for SNR figures)
    uint64_t samples() const { return pos_; }
    int fftSize() const { return fft_; }
private:
    struct Active { double freqHz; int bin; uint64_t startFrame, lastFrame; float peakDb; int quiet; };
    void frame();
    void finish(const Active& a, std::vector<DetectedBurst>& out);
    double rate_ = 0;
    int fft_ = 0, log2n_ = 0, wBins_ = 0, cBins_ = 0;
    float thr_ = 20, thr2_ = 24, thrCont_ = 2;
    std::vector<float> win_, re_, im_, pow_, prev_, sum_, noise_;
    std::vector<cf32> buf_;
    size_t fill_ = 0;
    uint64_t pos_ = 0, frames_ = 0;
    std::vector<Active> act_;
    std::vector<DetectedBurst>* out_ = nullptr;
    int maxFrames_ = 0;
};

// Result of demodulating one burst.
struct DemodResult {
    bool found = false;              // a unique word candidate was found
    bool uwOk = false;               // its hard decisions match the unique word (gr-iridium rule)
    bool downlink = true;
    std::vector<uint8_t> bits;       // bits after the unique word, one per byte
    int nSymbols = 0;                // symbols after the unique word
    double freqHz = 0;               // refined centre, relative to 0 Hz of the input
    double uwTime = 0;               // time of the first unique word symbol, seconds after the first input sample of the segment
    float confidence = 0;            // percent of symbols within 22 degrees of a constellation point (gr-iridium)
    float snrDb = 0;                 // Es/N0 from the error vector of the symbols
    float levelDb = 0;               // mean symbol power, dB full scale
};

// Per-burst demodulator: mix to 0 Hz, decimate (CIC + half-band) to 10..20 samples a symbol, matched filter, unique word search by
// differential correlation (insensitive to the carrier error), carrier and phase from the known preamble and unique word, a decision
// directed PLL through the data, DQPSK to bits.
class BurstDemod {
public:
    void configure(double inputRate);
    // x: input samples of one detected burst (margins included), freqHz: its centre relative to 0 Hz of the input,
    // maxSymbols: data symbols after the unique word at most.
    void demod(const cf32* x, size_t n, double freqHz, int maxSymbols, DemodResult& out);
    double innerRate() const { return fs2_; }
private:
    void prepare(const cf32* x, size_t n, double freqHz);
    void demodAt(const cf32* x, size_t n, double freqHz, int maxSymbols, DemodResult& out);
    double rate_ = 0, fs1_ = 0, fs2_ = 0, sps_ = 0;
    int m1_ = 1;
    std::vector<float> hb_, rrc_, cic_, cic2_, hb2_, rrc2_;
    std::vector<cf32> s1_, s2_, mf_, d_, mix_, s1Base_, tab_;
    double freqBase_ = 0, tabW_ = 1e30;
};

} // namespace iridium
} // namespace dect2
