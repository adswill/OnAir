// DVB-SH TDM (SH-B, the satellite waveform of ETSI EN 302 583 V1.2.1 clause 5.6) over the same channel coding as SH-A (dvbsh.h):
//  - the TDM SH frame (clause 5.5.2.4): a HEADER of 3 capacity units (the 288-bit SOF and the 5 760-bit signalling field: table 5.9 coded
//    with the 3GPP2 turbo code at rate 1/5 and the bit-wise interleaver H(w) = 73 w mod 5 760 of table 5.6), the DATA part (whole turbo
//    code words through the convolutional time interleaver) and the PADDING part (the PRBS of figure 5.13, restarted every SH frame);
//    the frame width in CU from table 5.12 (TDM and OFDM in the same bandwidth) or as signalled,
//  - QPSK, 8PSK (the DVB-S2 mappings of EN 302 307 clause 5.4) and 16APSK with gamma = 3, R1 = 1/sqrt(7), R2 = 3/sqrt(7) (figure 5.19),
//  - PL slots of 2 176 symbols: two pilot fields of 80 unmodulated symbols (1 + j) / sqrt(2), each before a sub-slot of 1 008 data
//    symbols (2, 3 or 4 CU per slot); the PL scrambling of clause 5.6.4.3 (the DVB-S2 Gold sequence, code number n, default 0),
//    restarted at every slot,
//  - the symbol rates of table 5.13 and root raised cosine shaping with roll-off 0.15, 0.25 or 0.35.
// The low latency extension (annex B) is not implemented: LL_par_present is read, LL bursts are not separated.
// Simulated only: the generator and the receiver below were checked against each other, never against a satellite.
#pragma once
#include "dvbsh.h"
#include <array>
#include <memory>
#include <string>

namespace dect2 {
namespace dvbsh {

enum TdmMod { kTdmQpsk = 0, kTdm8psk = 1, kTdm16apsk = 2 };
constexpr int kSlotSymbols = 2176, kPilotField = 80, kSubSlot = 1008, kSlotData = 2016;
constexpr int kHeaderCu = 3, kSofBits = 288, kSfBits = 5760;
inline int tdmBitsPerSymbol(int mod) { return mod + 2; }
inline int tdmCuPerSlot(int mod) { return mod + 2; }   // table 5.14
const char* tdmModName(int mod);
// position of each slot symbol: true for the pilot fields (symbols 0..79 and 1088..1167)
inline bool tdmIsPilot(int i) { return i < kPilotField || (i >= kPilotField + kSubSlot && i < 2 * kPilotField + kSubSlot); }

double tdmRollOff(int rollIdx);                         // 0 0.15, 1 0.25, 2 0.35
// Table 5.12 (TDM and OFDM in the same bandwidth): CU of a TDM SH frame. ofdmGuard 0..3 = 1/32 .. 1/4 (as dvbt::Guard), OFDM QPSK or 16-QAM.
int tdmFrameCu(int tdmMod, int ofdmGuard, int rollIdx, bool ofdm16qam);
// Table 5.13: the TDM symbol rate in symbols/s for the bandwidth (8, 7, 6, 5 or 1.7 MHz), the OFDM guard and the roll-off; 0 if unknown
double tdmSymbolRate(double bwMhz, int ofdmGuard, int rollIdx);
// Whole code words in the DATA part of a frame of frameCu CU (the rest after the header is padding)
inline int tdmCodewords(int frameCu, int pat) { const int cw = nbil(pat) / kCuBits; return cw > 0 && frameCu > kHeaderCu ? (frameCu - kHeaderCu) / cw : 0; }
inline int tdmDataCu(int frameCu, int pat) { return tdmCodewords(frameCu, pat) * (nbil(pat) / kCuBits); }

// ---- the signalling field (table 5.9)
struct TdmSignalling {
    int version = 0;
    int frameWidthCu = 0;       // Frame_Width_CUs (12 bits)
    int pat = 4;                // Punct_Pat_ID
    TiConfig ti;
    bool llPresent = false, llSatActive = false, nextConf = false;
    int llPat = 0, bursts = 0;
    std::array<uint16_t, 16> burst{};
    bool operator==(const TdmSignalling& o) const;
};
// the 1 146 payload bits (one per byte): fields, CRC-16 over the first 96 bits at 96, the LL part and its CRC at 384, zeros after
void packSignalling(const TdmSignalling& s, uint8_t* bits1146);
bool unpackSignalling(const uint8_t* bits1146, TdmSignalling& s);   // false when the first CRC fails or the RFU byte is not 0
// rate 1/5 turbo code (Punct_Pat_ID 0) and the bit-wise interleaver of table 5.6: 5 760 bits
void encodeSignalling(const TdmSignalling& s, std::vector<uint8_t>& out5760);
// from 5 760 LLRs (> 0 bit 0) in transmission order; iters: turbo iterations used
bool decodeSignalling(const float* llr5760, TdmSignalling& s, int* iters = nullptr);
const std::array<uint8_t, kSofBits>& sofBits();         // F3484536 ... 1EDDACCF9E4F, the first bit first

// ---- constellations (unit mean energy; index = label, the MSB the first bit)
const cf32* tdmConstellation(int mod);
// max-log LLRs (> 0 bit 0), sigma2 the noise variance per real dimension
void tdmDemap(const cf32* y, int n, int mod, float sigma2, float* llr);
// exp(j R(i) pi / 2) of the PL scrambling sequence for Gold code n, i < 2176 (restarted every slot)
const std::vector<cf32>& tdmScrambling(int goldN);

// ---- generator
struct TdmParams {
    int mod = kTdmQpsk;
    int pat = 4;                // Punct_Pat_ID (rate 1/3)
    TiConfig ti;
    double bwMhz = 5;
    int ofdmGuard = 3;          // the OFDM parameters the frame is aligned to (table 5.12 / 5.13): guard 1/4
    int rollIdx = 0;            // 0.15
    bool ofdm16qam = false;
    int frameCu = 0;            // 0: table 5.12
    int goldN = 0;
    int frameCuOrTable() const { return frameCu > 0 ? frameCu : tdmFrameCu(mod, ofdmGuard, rollIdx, ofdm16qam); }
    double symbolRate() const { return tdmSymbolRate(bwMhz, ofdmGuard, rollIdx); }
};

class TdmGenerator {
public:
    using PacketSource = std::function<void(uint8_t* pkt188)>;
    explicit TdmGenerator(const TdmParams& p, PacketSource src = nullptr);
    ~TdmGenerator();
    const TdmParams& params() const { return p_; }
    // one PL slot of 2 176 symbols (scrambled), unit mean power on the data symbols
    void nextSlot(std::vector<cf32>& out);
    long codewordsSent() const { return codewords_; }
    long framesStarted() const { return frames_; }
private:
    void buildFrame();
    TdmParams p_;
    PacketSource src_;
    TimeInterleaver<uint8_t> ti_;
    std::vector<uint8_t> frameBits_;
    size_t pos_ = 0;
    long codewords_ = 0, frames_ = 0;
};

// The TDM signal at a sample rate: RRC shaping (4 samples per symbol), resampling to sampleRate with the transmitter clock error, the
// carrier offset and white noise at snrDb Es/N0
struct TdmSignalConfig {
    TdmParams p;
    double sampleRate = 8e6;
    double snrDb = 200;
    double cfoHz = 0;
    double clockPpm = 0;        // the symbol clock runs fast by this much
    double level = 0.25;        // rms of the output
    unsigned seed = 1;
};
class TdmSignal {
public:
    explicit TdmSignal(const TdmSignalConfig& c, TdmGenerator::PacketSource src = nullptr);
    ~TdmSignal();
    void generate(cf32* out, size_t n);
    const TdmGenerator& generator() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// ---- receiver
struct TdmStatus {
    int level = 0;              // 0 searching, 1 PL slots found, 2 frames found (SOF), 3 signalling decoded, 4 transport stream flowing
    int mod = -1;
    double symbolRate = 0, cfoHz = 0, snrDb = 0, clockPpm = 0;
    TdmSignalling sf;
    long slots = 0, frames = 0, sfOk = 0, sfBad = 0;
    long codewordsOk = 0, codewordsBad = 0, packetsOk = 0, packetsBad = 0;
    double avgIterations = 0;
    int tiLmax = 0;
    size_t tiBytes = 0;
    bool tiTooLarge = false;
    int rateCandidate = -1;     // automatic symbol rate: the row of table 5.13 in use
};

class TdmReceiver {
public:
    TdmReceiver();
    ~TdmReceiver();
    // input rate and channel width; the symbol rate is searched over the 12 rows of table 5.13 for that width unless set
    void configure(double fs, double bwMhz);
    void setSymbolRate(double symPerSec);       // 0: automatic
    void setGoldCode(int n);                    // PL scrambling code number (default 0)
    void setMaxInterleaverBytes(size_t b);
    void setPacketCallback(std::function<void(const uint8_t* pkts, size_t count)> cb);
    void reset();
    void feed(const cf32* x, size_t n);
    TdmStatus status() const;
    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

} // namespace dvbsh
} // namespace dect2
