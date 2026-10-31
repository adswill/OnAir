// LoRa physical layer (explicit header): the coding chain (whitening, Hamming, diagonal interleaving, Gray mapping, header, CRC),
// a modulator for the test signal and a demodulator for one channel and spreading factor.
// Facts from tapparelj/gr-lora_sdr (EPFL, GPL-3.0): lib/tables.h (whitening), hamming_enc_impl.cc, interleaver_impl.cc, gray_demap_impl.cc,
// header_impl.cc (header checksum), add_crc_impl.cc (CRC), modulate_impl.cc (preamble, sync word, 2.25 down-chirps), fft_demod_impl.cc
// (reduced rate: bins divided by 4), and the paper "An open-source LoRa physical layer prototype on GNU Radio" (Tapparel et al., 2020);
// the air time formula and the 16 ms low data rate rule from the Semtech SX1261/2 datasheet (6.1.4) and RadioLib.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {
namespace lora {

struct Params {
    int sf = 11;                 // 5..12 (SF5/6 as on the SX126x: two more symbols after the SFD, a full-rate header block)
    double bwHz = 250000;        // 62.5, 125, 250, 500 kHz (any value works)
    int cr = 5;                  // 5..8 for 4/5..4/8
    int preamble = 16;           // up-chirps before the sync word
    uint8_t syncWord = 0x2B;
    std::vector<uint8_t> moreSync;   // further sync words tried by the same demodulator (one preamble search serves all of them)
    bool ldro = false;           // low data rate optimisation
    bool crc = true;
    bool implicitHdr = false;    // no header: the length (implicitLen), coding rate (cr) and CRC (crc) are agreed beforehand (plain LoRa links)
    int implicitLen = 0;
    double carrierHz = 0;        // cfoTrack > 0: the carrier frequency when known (a Doppler shift stretches the symbol clock by the same fraction)
    double cfoTrack = 0;         // 0: the carrier offset is measured once per frame (every plain LoRa link); > 0: followed through the frame at this loop gain (satellites: Doppler rate)
    // A CRC-bad frame is searched for sample jumps (recoverJumps); 0 turns the search off. First jumps of multiples of this many chips
    // (an RTL-SDR at 2.048 Msps drops ~96 samples at a time, 11.7 chips at 250 kHz, #386), then, when that finds nothing, any shift
    // (one or two jumps), because a link drops whatever count of samples it drops.
    double jumpChips = 11.7;
};

// RadioLib turns the low data rate optimisation on when a symbol lasts 16 ms or more (SX126x::setSpreadingFactor, autoLDRO)
bool autoLdro(int sf, double bwHz);
double symbolSeconds(int sf, double bwHz);
// SF5 and SF6 (SX1261/2 datasheet 6.1.4: Npreamble + 6.25 + 8 + ...): two more symbols between the SFD and the data, and the header
// block carries sf codewords at the full rate instead of sf - 2 at the reduced rate. Not checked against a real radio.
inline int sfdExtraSymbols(int sf) { return sf < 7 ? 2 : 0; }
inline int headerSfApp(int sf) { return sf < 7 ? sf : sf - 2; }
// FFT bin (0 .. 2^sf - 1) of sync word symbol 0 (high nibble) or 1 (low nibble). The radios send nibble << 3 as a signed 7-bit value:
// Meshtastic's 0x2B is 16 and -40 (2^sf - 40), which equals 88 only at SF7 (measured on an SX1262 at SF11, 2026-10-09).
int syncSymbol(uint8_t syncWord, int index, int sf);

// coding pieces (exposed for the known-answer tests)
uint8_t whitening(size_t i);                                   // byte i of the whitening sequence (0xFF, 0xFE, 0xFC, ...)
uint8_t hammingEncode(uint8_t nibble, int cr);                 // cr 5..8: a codeword of cr bits, first bit = bit 0 of the nibble
uint8_t hammingDecode(uint8_t cw, int cr, int* errors);        // errors: 0, 1 (corrected), 2 (detected, not corrected; 4/8 only)
uint8_t headerChecksum(uint8_t n0, uint8_t n1, uint8_t n2);    // 5 bits over the three header nibbles
uint16_t payloadCrc(const uint8_t* p, size_t n);               // the 16 bits sent after the payload (sent low byte first)
// Samples lost in the receive chain shift every later symbol of a frame by the same number of bins. For an explicit-header frame whose
// CRC failed: finds up to maxJumps such points, block by block with the Hamming parity, trying shifts of m x quantum +- 2 bins
// (m = +-1..+-15). The payload is taken only when every block's parity is clean, the CRC is right and exactly one solution exists.
// Returns the number of jumps undone (0: not recovered, payload untouched). A jump inside the header is not handled.
int recoverJumps(int sf, bool ldro, const std::vector<uint16_t>& bins, double quantum, std::vector<uint8_t>& payload, int maxJumps = 3);
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

// Soft information of one demodulated symbol: its strongest bins with their log-likelihoods (ln I0 of the noncoherent envelope),
// and the level a bin outside the list is assumed to have. Used by FrameDecoder::pushSoft for soft Hamming decoding.
struct SoftSym {
    static constexpr int M = 4;
    uint16_t bin[M] = {};
    float m[M] = {};
    float floorM = 0;
    int n = 0;
};

// Turns data symbol bins back into bytes: push() each bin in order; after 8 the header is known, after needed() the payload.
class FrameDecoder {
public:
    void start(int sf, bool ldro);
    void startImplicit(int sf, bool ldro, int len, int cr, bool crc);   // implicit header: no header nibbles, the settings are known
    void push(uint16_t bin);
    void pushSoft(const SoftSym& s);                           // the bin is s.bin[0]; the rest feeds the soft decoding of every codeword
    int pushed() const { return n_; }
    bool headerDone() const { return n_ >= 8; }
    const Header& header() const { return hdr_; }
    int needed() const { return need_; }                       // total data symbols of the frame (8 before the header is known)
    bool done() const { return hdr_.ok && n_ >= need_; }
    // after done(): the payload, whether its CRC was right, and how many codewords needed a correction
    bool finish(std::vector<uint8_t>& payload, bool& crcOk, int& corrected);
    bool softFixed() const { return fixed_; }                  // the CRC was only right after the soft list search (a few uncertain symbols replaced)
    const std::vector<uint16_t>& bins() const { return bins_; }   // the hard bins pushed so far
private:
    void block(size_t at, int cwLen, int sfApp, bool reduced);
    bool assemble(const std::vector<uint8_t>& nib, std::vector<uint8_t>& payload, bool& crcOk) const;
    int sf_ = 11, n_ = 0, need_ = 8, corrected_ = 0, hn_ = 5;   // hn_: header nibbles in front of the payload (5, or 0 implicit)
    bool ldro_ = false, implicit_ = false, fixed_ = false;
    Header hdr_;
    std::vector<uint16_t> bins_;
    std::vector<SoftSym> soft_;
    std::vector<uint8_t> nib_;
    void blockInto(const std::vector<SoftSym>* sft, size_t at, int cwLen, int sfApp, bool reduced, std::vector<uint8_t>& nib, int& corr) const;
};

// Frame samples for the test signal: adds the frame's waveform (amplitude amp) to out[k] for sample times t0 + k / rate.
// The frame starts at startSec; the transmitter's clock runs (1 + sroPpm 1e-6) slow, its carrier sits at freqHz.
struct TxFrame {
    Params p;
    std::vector<uint16_t> data;  // encode() output
    double startSec = 0, freqHz = 0, sroPpm = 0;
    float amp = 0.3f;
    double phase0 = 0;           // carrier phase at the start, cycles
    double freqRateHzS = 0;      // the carrier's drift in Hz per second (a satellite's Doppler rate); 0 for every ground link
    double endSec() const;
    void render(cf32* out, size_t n, double t0, double rate) const;
};

// A demodulated frame
struct RxFrame {
    std::vector<uint8_t> payload;
    Header hdr;
    bool crcOk = false;
    int corrected = 0;           // codewords with a corrected bit
    double cfoHz = 0;            // carrier offset from the channel centre (at the end of the frame)
    double cfoStartHz = 0;       // the same at the first data symbol, and the drift in Hz per second (both only with Params::cfoTrack > 0)
    double cfoRateHzS = 0;
    double sfoPpm = 0;           // clock offset measured over the frame
    double snrDb = 0;            // in the LoRa bandwidth
    int softFixed = 0;           // 1: the frame's CRC only came out right after the soft-decision list search
    int jumpsFixed = 0;          // sample jumps undone to get the CRC right (recoverJumps)
    double levelDb = 0;          // signal power, dB relative to full scale at the channel output
    double startSample = 0;      // start of the frame (its preamble of the configured length), channel samples since the start
    double endSample = 0;
    uint8_t syncWord = 0;
};

// Events of the demodulator for the receiver's statistics
struct DemodStats {
    uint64_t preambles = 0, syncBad = 0, headerBad = 0, frames = 0, crcBad = 0, jumpFixed = 0;
};

// The channel resampled once at the chip rate (chip m sits at channel sample m x rho; cubic interpolation): every spreading factor of a
// channel searches its preambles on this one stream instead of interpolating its own windows.
class ChanBuf;
class ChipStream {
public:
    explicit ChipStream(double rho) : rho_(rho) {}
    double rho() const { return rho_; }
    void extend(const ChanBuf& b);                             // appends the chips the buffer holds enough samples for
    int64_t begin() const { return base_; }
    int64_t end() const { return base_ + (int64_t)re_.size(); }
    const float* re(int64_t m) const { return re_.data() + (m - base_); }
    const float* im(int64_t m) const { return im_.data() + (m - base_); }
    void trimToSample(int64_t keepSample);                     // forgets the chips before this channel sample
private:
    double rho_;
    int64_t base_ = 0;
    std::vector<float> re_, im_;
};

// Demodulator of one spreading factor on one channel. The channel samples arrive at fsChan (>= 2 x bandwidth) with the channel at 0 Hz.
// process() consumes what it can of the samples the channel buffer holds and returns finished frames.
class ChanBuf;
class Demod {
public:
    // shared: a stream made by the owner of the channel and extended/trimmed by it (several spreading factors on one channel); otherwise
    // the demodulator keeps its own
    Demod(const Params& p, double fsChan, int searchFftPerSymbol = 1, std::shared_ptr<ChipStream> shared = nullptr);
    ~Demod();
    static inline bool softOn = true;           // soft-decision decoding of the Hamming codewords (switch for comparisons)
    static inline int searchStride = 1;          // 2: only every second window searches for a preamble (half the search cost; costs reliability: off)
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
    static inline double kMinRatio = 2.0;   // output rate >= kMinRatio x bandwidth
    static int decimFor(double inRate, double bwHz);
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
    std::vector<float> tr_, ti_, sr_, si_;   // mixer phasor tables
    int64_t nIn_ = 0;            // input samples mixed so far
};


// Several channels of one wide capture through ONE shared forward FFT (overlap-save, frequency-domain decimation by a power of two per
// channel, a smooth band-pass on the chosen bins): the cost grows with the channels only by a small inverse FFT each. The channel
// samples are numbered from the first input sample (no group delay); the output rate of channel i is outRate(i) = inRate / decim(i).
class FdChannelizer {
public:
    struct Spec { double offsetHz, bwHz; };
    static inline bool kEnabled = true;           // the banks use it from 3 channels on (switch for comparisons)
    FdChannelizer(double inRate, const std::vector<Spec>& chans);
    ~FdChannelizer();
    size_t channels() const;
    double outRate(size_t i) const;
    int decim(size_t i) const;
    void reset();
    // appends the output of channel i to *out[i] (out has one entry per channel)
    void process(const cf32* x, size_t n, const std::vector<ChanBuf*>& out);
private:
    struct Chan;
    double inRate_;
    int M_ = 0, O_ = 0, H_ = 0, lm_ = 0;
    int64_t inBase_ = 0, block_ = 0;
    std::vector<Chan> ch_;
    std::vector<cf32> in_, tmp_;
    std::vector<float> xr_, xi_;
};

} // namespace lora
} // namespace dect2
