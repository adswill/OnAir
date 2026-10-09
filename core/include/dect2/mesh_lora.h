// LoRa physical layer (explicit header): the coding chain (whitening, Hamming, diagonal interleaving, Gray mapping, header, CRC),
// a modulator for the test signal and a demodulator for one channel and spreading factor.
// Facts from tapparelj/gr-lora_sdr (EPFL, GPL-3.0): lib/tables.h (whitening), hamming_enc_impl.cc, interleaver_impl.cc, gray_demap_impl.cc,
// header_impl.cc (header checksum), add_crc_impl.cc (CRC), modulate_impl.cc (preamble, sync word, 2.25 down-chirps), fft_demod_impl.cc
// (reduced rate: bins divided by 4), and the paper "An open-source LoRa physical layer prototype on GNU Radio" (Tapparel et al., 2020);
// the air time formula and the 16 ms low data rate rule from the Semtech SX1261/2 datasheet (6.1.4) and RadioLib.
#pragma once
#include "ring.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {
namespace lora {

struct Params {
    int sf = 11;                 // 7..12
    double bwHz = 250000;        // 62.5, 125, 250, 500 kHz (any value works)
    int cr = 5;                  // 5..8 for 4/5..4/8
    int preamble = 16;           // up-chirps before the sync word
    uint8_t syncWord = 0x2B;
    bool ldro = false;           // low data rate optimisation
    bool crc = true;
};

// RadioLib turns the low data rate optimisation on when a symbol lasts 16 ms or more (SX126x::setSpreadingFactor, autoLDRO)
bool autoLdro(int sf, double bwHz);
double symbolSeconds(int sf, double bwHz);
// FFT bin (0 .. 2^sf - 1) of sync word symbol 0 (high nibble) or 1 (low nibble). The radios send nibble << 3 as a signed 7-bit value:
// Meshtastic's 0x2B is 16 and -40 (2^sf - 40), which equals 88 only at SF7 (measured on an SX1262 at SF11, 2026-10-09).
int syncSymbol(uint8_t syncWord, int index, int sf);

// coding pieces (exposed for the known-answer tests)
uint8_t whitening(size_t i);                                   // byte i of the whitening sequence (0xFF, 0xFE, 0xFC, ...)
uint8_t hammingEncode(uint8_t nibble, int cr);                 // cr 5..8: a codeword of cr bits, first bit = bit 0 of the nibble
uint8_t hammingDecode(uint8_t cw, int cr, int* errors);        // errors: 0, 1 (corrected), 2 (detected, not corrected; 4/8 only)
uint8_t headerChecksum(uint8_t n0, uint8_t n1, uint8_t n2);    // 5 bits over the three header nibbles
uint16_t payloadCrc(const uint8_t* p, size_t n);               // the 16 bits sent after the payload (sent low byte first)
inline uint32_t gray(uint32_t x) { return x ^ (x >> 1); }
uint32_t grayDecode(uint32_t g);
int dataSymbols(const Params& p, size_t payloadLen);           // symbols after the SFD: 8 + ceil(...) (CR + 4)
double airSeconds(const Params& p, size_t payloadLen);         // the whole frame: preamble + 4.25 + data symbols

// the data symbols (FFT bins 0..2^sf-1 after the SFD) of an explicit-header frame
std::vector<uint16_t> encode(const Params& p, const uint8_t* payload, size_t n);

struct Header {
    int len = 0, cr = 0;         // cr 5..8
    bool crc = false, ok = false;
};

// Turns data symbol bins back into bytes: push() each bin in order; after 8 the header is known, after needed() the payload.
class FrameDecoder {
public:
    void start(int sf, bool ldro);
    void push(uint16_t bin);
    int pushed() const { return n_; }
    bool headerDone() const { return n_ >= 8; }
    const Header& header() const { return hdr_; }
    int needed() const { return need_; }                       // total data symbols of the frame (8 before the header is known)
    bool done() const { return hdr_.ok && n_ >= need_; }
    // after done(): the payload, whether its CRC was right, and how many codewords needed a correction
    bool finish(std::vector<uint8_t>& payload, bool& crcOk, int& corrected);
private:
    void block(const uint16_t* bins, int cwLen, int sfApp, bool reduced);
    int sf_ = 11, n_ = 0, need_ = 8, corrected_ = 0;
    bool ldro_ = false;
    Header hdr_;
    std::vector<uint16_t> bins_;
    std::vector<uint8_t> nib_;
};

// Frame samples for the test signal: adds the frame's waveform (amplitude amp) to out[k] for sample times t0 + k / rate.
// The frame starts at startSec; the transmitter's clock runs (1 + sroPpm 1e-6) slow, its carrier sits at freqHz.
struct TxFrame {
    Params p;
    std::vector<uint16_t> data;  // encode() output
    double startSec = 0, freqHz = 0, sroPpm = 0;
    float amp = 0.3f;
    double phase0 = 0;           // carrier phase at the start, cycles
    double endSec() const;
    void render(cf32* out, size_t n, double t0, double rate) const;
};

// A demodulated frame
struct RxFrame {
    std::vector<uint8_t> payload;
    Header hdr;
    bool crcOk = false;
    int corrected = 0;           // codewords with a corrected bit
    double cfoHz = 0;            // carrier offset from the channel centre
    double sfoPpm = 0;           // clock offset measured over the frame
    double snrDb = 0;            // in the LoRa bandwidth
    double levelDb = 0;          // signal power, dB relative to full scale at the channel output
    double startSample = 0;      // start of the frame (its preamble of the configured length), channel samples since the start
    double endSample = 0;
    uint8_t syncWord = 0;
};

// Events of the demodulator for the receiver's statistics
struct DemodStats {
    uint64_t preambles = 0, syncBad = 0, headerBad = 0, frames = 0, crcBad = 0;
};

// Demodulator of one spreading factor on one channel. The channel samples arrive at fsChan (>= 2 x bandwidth) with the channel at 0 Hz.
// process() consumes what it can of the samples the channel buffer holds and returns finished frames.
class ChanBuf;
class Demod {
public:
    Demod(const Params& p, double fsChan, int searchFftPerSymbol = 1);
    ~Demod();
    const Params& params() const;
    void reset(int64_t fromSample);
    void process(const ChanBuf& buf, std::vector<RxFrame>& out);
    int64_t oldestNeeded() const;                              // the buffer must keep samples from here on
    bool busy() const;                                         // in a frame (preamble found)
    const DemodStats& stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// History of a channel's samples with absolute indices
class ChanBuf {
public:
    void clear(int64_t base = 0) { x_.clear(); base_ = base; }
    void append(const cf32* x, size_t n) { x_.insert(x_.end(), x, x + n); }
    void trim(int64_t keepFrom);                               // forget samples before keepFrom
    int64_t begin() const { return base_; }
    int64_t end() const { return base_ + (int64_t)x_.size(); }
    const cf32* at(int64_t i) const { return x_.data() + (i - base_); }
private:
    std::vector<cf32> x_;
    int64_t base_ = 0;
};

// Mixes a channel to 0 Hz and decimates it (FIR) by an integer factor. Processes the input in fixed blocks so the output does not
// depend on how the input was cut into chunks.
class Channelizer {
public:
    // offsetHz: where the channel sits in the input; bwHz: LoRa bandwidth; output rate = inRate / decim with decim = floor(inRate / (3 bw))
    Channelizer(double inRate, double offsetHz, double bwHz);
    double outRate() const { return outRate_; }
    int decim() const { return decim_; }
    double offsetHz() const { return offsetHz_; }
    double bwHz() const { return bwHz_; }
    double delayIn() const { return 0.5 * (double)(taps_.size() - 1) + 0.5 * (double)(taps2_.size() - 1) * decim_; }   // filter delay, input samples
    void reset();
    // input samples numbered from 0 at reset(); appends the output to buf
    void process(const cf32* x, size_t n, ChanBuf& buf);
private:
    double inRate_, offsetHz_, bwHz_, outRate_;
    int decim_ = 1;
    std::vector<float> taps_, taps2_;
    std::vector<float> re_, im_, ore_, oim_, re2_, im2_;
    std::vector<cf32> tmp_;
    int64_t nIn_ = 0;            // input samples mixed so far
};

} // namespace lora
} // namespace dect2
