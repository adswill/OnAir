// DAB / DAB+ test transmitter (ETSI EN 300 401 transmission mode I, ETSI TS 102 563 for DAB+): an endless, continuous signal that carries a small
// ensemble with real audio, so that the DAB receiver can be tried and tested without a radio. Nothing is transmitted; the samples only exist in
// memory or in a file.
//
// The chain: stereo tones or a melody -> AAC-LC (960 transform, 32 or 48 kHz, own encoder) -> DAB+ superframes (Fire code, access unit table, CRC-16 per
// access unit, 5 x RS(120,110) with the byte interleaving) or MPEG-1 Layer II (libavcodec's encoder) -> logical frames of 24 ms -> energy dispersal ->
// rate 1/4 convolutional code with the EEP puncturing profile -> time interleaving over 16 CIFs -> the CIF multiplex. The FIC carries the FIGs (0/0, 0/1,
// 0/2, 0/7, 0/10, 1/0, 1/1) in FIBs with CRC-16, energy dispersal and rate 1/3 coding. 76 OFDM symbols (null, phase reference, 3 FIC, 72 MSC) of
// 2048 points at 2.048 Msps with guard interval 504, DQPSK on 1536 carriers with the frequency interleaver, 96 ms per frame.
//
// The channel coding primitives in namespace dabgen are written from the standard and do not share code with the receiver (dab_fec.cpp), so a test can
// compare the two. The loop of the audio is 12 s; everything that repeats (the coded frames of every sub-channel) is precomputed.
#pragma once
#include "mode_synth.h"
#include "source.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// The entry point: the test signal at `sampleRate` (2 to 20 Msps), with cfg.snrDb (carrier to noise power in the 2.048 MHz sample band of the ensemble;
// the 1.536 MHz signal itself sees 1.25 dB better), cfg.cfoHz and cfg.sroPpm applied inside. nullptr for sample rates outside 2 to 21 Msps.
std::unique_ptr<ModeSynth> makeDabSynth(const SynthConfig& cfg, double sampleRate);

namespace dabgen {

constexpr int kTu = 2048, kTg = 504, kTs = 2552, kTnull = 2656, kSymbols = 76, kCarriers = 1536, kFrame = 196608;
constexpr int kCifBits = 55296, kCifCu = 864, kLoopSeconds = 12;
constexpr double kRate = 2048000.0;

// ---- channel coding primitives, from the standard
void prbs(uint8_t* bits, int n);                      // the first n bits of the energy dispersal sequence (x^9 + x^5 + 1, register all ones)
void scramble(uint8_t* bits, int n);                  // modulo 2 addition of that sequence, one bit per byte, in place
uint16_t crc16(const uint8_t* d, int n);              // CRC-CCITT x^16+x^12+x^5+1, register all ones, result inverted (FIB, access units)
uint16_t fireCode(const uint8_t* d, int n);           // x^16+x^14+x^13+x^12+x^11+x^5+x^3+x^2+x+1 (DAB+ superframe header)
void rsEncode(const uint8_t* data110, uint8_t* cw120);          // RS(120,110) shortened from RS(255,245), field polynomial 0x11D, roots alpha^0..alpha^9
void convEncode(const uint8_t* bits, int n, uint8_t* mother);   // rate 1/4, K=7, generators 133 171 145 133 (octal): 4 * (n + 6) bits with the tail
const char* puncturingVector(int pi);                 // PI_1 .. PI_24 as 32 characters of 0 and 1 (EN 300 401 table 13)
void punctureFic(const uint8_t* mother, uint8_t* out);                                    // 3096 bits -> 2304
// Capacity units of an EEP sub-channel: bitrate kbit/s (a multiple of 8 for option 0 / 32 for option 1), option 0 = A profiles, 1 = B, level 0..3 = 1..4.
// 0 for a combination that is not allowed.
int eepSize(int bitrateKbps, int option, int level);
bool punctureEep(const uint8_t* mother, int bitrateKbps, int option, int level, uint8_t* out);   // 4*(24*bitrate/8*... + 6) bits -> eepSize * 64

// ---- AAC-LC with the 960 transform (DAB+ uses frameLengthFlag = 1), long blocks only, one scale factor per channel
class AacLcEncoder {
public:
    AacLcEncoder(int rateHz, int channels);            // 32000 or 48000 Hz, 1 or 2 channels
    ~AacLcEncoder();
    void reset();                                      // the next block overlaps with silence
    // pcm: the next 960 samples per channel, interleaved, +-1.0 is full scale. Produces the raw_data_block of the block before this one overlapped with
    // this one, padded with zero bytes to exactly budgetBytes. false when nothing fits (a silent block is produced instead).
    bool encode(const float* pcm, int budgetBytes, std::vector<uint8_t>& block);
    int rate() const;
    int channels() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// DAB+ superframe of `aus` (every access unit with its CRC-16 already appended): returns the 120 * N bytes, N = bitrate / 8, in the order they are
// transmitted. The access units must tile 110 * N - firstStart bytes exactly (see superframeLayout). Header byte: dac (48 kHz) and stereo flags.
void superframeLayout(int bitrateKbps, int nAu, int* firstStart, std::vector<int>* auLen);
std::vector<uint8_t> buildSuperframe(const std::vector<std::vector<uint8_t>>& aus, int bitrateKbps, bool dacRate48, bool stereo);
inline int accessUnitsPerSuperframe(int rateHz) { return rateHz == 48000 ? 6 : 4; }

// ---- the ensemble
struct TxService {
    uint16_t sid = 0xCE01;
    std::string label;                  // up to 16 characters
    int subId = 1;
    int bitrate = 48;                   // kbit/s of the sub-channel (a multiple of 8)
    bool dabPlus = true;                // DAB+ (AAC-LC) or DAB (MPEG-1 Layer II)
    int sampleRate = 48000;             // DAB+: 48000 or 32000; DAB: 48000
    double leftHz = 1000, rightHz = 3000;   // the tones (periods that divide the frame length)
    bool melody = false;                // a tune instead of the tones (12 s, left: the tune, right: the same an octave higher)
    double amplitude = 0.5;             // peak of every channel, 1.0 = full scale
    int option = 0, level = 2;          // EEP-3A
};
struct TxConfig {
    std::string ensembleLabel = "OnAir DAB";
    uint16_t eid = 0xCE15;
    std::vector<TxService> services;    // empty: defaultServices()
    double snrDb = 200, cfoHz = 0, sroPpm = 0;
    unsigned seed = 1;
    int64_t utcSeconds = -1;            // time of frame 0 for FIG 0/10; negative: the clock
    // TII (EN 300 401 clause 14.8): transmitters whose identification is added to the null symbol of the even frames (CIF count 0 - 3 modulo
    // 8). Several stand for the transmitters of a single-frequency network as one receiver hears them; levelDb is each one's carrier level
    // against the main signal (0 = as strong as a data carrier). Empty: no TII.
    struct Tii { int mainId = 0, subId = 0; double levelDb = 0; };
    std::vector<Tii> tii;
};
// 1 "OnAir Tones 1" DAB+ 48 kHz 1 kHz / 3 kHz, 48 kbit/s; 2 "OnAir Tones 2" DAB+ 32 kHz 2 kHz / 500 Hz, 32 kbit/s; 3 "OnAir Melody" DAB+ 48 kHz, 64 kbit/s;
// 4 "OnAir MP2" DAB 48 kHz 1.5 kHz / 750 Hz, 128 kbit/s (left out when libavcodec has no MP2 encoder)
std::vector<TxService> defaultServices();

struct SubLayout { int subId, start, size, bitrate, option, level; };

class Transmitter {
public:
    explicit Transmitter(const TxConfig& cfg);
    ~Transmitter();
    // The next transmission frame: 196608 samples at 2.048 Msps (null symbol first) with an rms of 0.2 over the active symbols, no noise, no clipping.
    void nextFrame(std::vector<cf32>& out);
    uint64_t frameIndex() const;                        // frames made so far
    const TxConfig& config() const;                     // with the defaults filled in
    const std::vector<SubLayout>& layout() const;       // where every service sits in the CIF
    // The 12 FIBs (32 bytes each, CRC included) of frame m: a pure function of m
    void fibs(uint64_t m, uint8_t out[12][32]) const;
    // The coded content (test hooks). Logical frame f of service i, 24 ms, bitrate * 3 bytes (the input of the energy dispersal)
    void logicalFrame(int service, int64_t f, std::vector<uint8_t>& out) const;
    // the access units of a DAB+ service's loop (raw AAC data without the CRC); empty for a DAB service
    const std::vector<std::vector<uint8_t>>& accessUnits(int service) const;
    // The MP2 frames of a DAB service's loop
    const std::vector<std::vector<uint8_t>>& mp2Frames(int service) const;
    // The punctured, not yet interleaved bits of logical frame f (one bit per byte)
    const uint8_t* codedBits(int service, int64_t f) const;
    // The frequency domain symbols of frame m (kSymbols x 2048 bins, bin = k mod 2048, symbol 0 = phase reference)
    void symbols(uint64_t m, std::vector<cf32>& z) const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// The frequency domain phase reference symbol (EN 300 401 clause 14.3.2), 2048 bins (bin = k mod 2048)
const std::vector<cf32>& phaseReferenceSymbol();
// carrier number k (-768..768) of the n-th QPSK symbol of an OFDM symbol (clause 14.6.1)
const std::vector<int>& frequencyInterleaver();
// The test sounds: sample `i` of channel `ch` of a service at its sample rate (a pure function of i, periodic with the loop)
float testSound(const TxService& s, int ch, int64_t i);

} // namespace dabgen

// The same with a given ensemble (and, for tests, a fixed time of day); cfg.snrDb, cfoHz and sroPpm replace those of `tx`.
std::unique_ptr<ModeSynth> makeDabSynth(const dabgen::TxConfig& tx, const SynthConfig& cfg, double sampleRate);

} // namespace dect2
