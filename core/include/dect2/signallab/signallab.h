// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal Lab (Tools > Signal Lab): blind analysis of an UNKNOWN digital signal, from samples to a frame description.
// Stages, each a pure function with its own unit test (tests/signallab):
//   1 channel  : isolate the signal, measure its bandwidth, carrier offset, bursts and duty cycle
//   2 classify : modulation family (OOK/ASK, FSK 2/4/8, MSK/GMSK, PSK 2/4/8, QAM, OFDM, spread-spectrum hint) with a confidence
//   3 rate     : symbol rate by four methods (cyclostationary, spectral line, zero crossings, autocorrelation), then symbol recovery
//                with timing and carrier loops set from the estimate
//   4 linecode : NRZ, NRZI, Manchester, differential Manchester, Miller, differential, LFSR descramblers; polarity and bit order
//   5 frame    : preamble, sync word, frame length, CRC / checksum search over candidate fields
//   6 fields   : per-column statistics of the aligned frames, suggested field boundaries, interpretations, diffs, exports
// Clean-room, standard DSP only; no dependencies. Nothing here decrypts anything: encrypted data shows up as high-entropy bits.
#pragma once
#include <complex>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::signallab {

using cf = std::complex<float>;
using Bits = std::vector<uint8_t>;   // one bit (0/1) per element

// ================================================================ small DSP helpers (dsp.cpp)
void fft(std::vector<std::complex<double>>& a, bool inverse = false);   // radix 2, size must be a power of two
size_t nextPow2(size_t n);
// Welch power spectrum, nfft bins, DC in the middle (bin nfft/2), linear power
std::vector<double> psd(const cf* x, size_t n, size_t nfft);
// Mix offHz to 0, low-pass to bwHz (windowed sinc) and decimate by an integer so the new rate stays >= 4 x bwHz. fsOut receives the new rate.
std::vector<cf> channelise(const std::vector<cf>& x, double fs, double offHz, double bwHz, double& fsOut);
std::vector<cf> mix(const std::vector<cf>& x, double fs, double hz);

// ================================================================ stage 1: channel and bursts (channel.cpp)
struct Burst { size_t start = 0, end = 0; };
struct ChannelInfo {
    double bw3dBHz = 0;        // width at -3 dB from the (smoothed) peak
    double bw99Hz = 0;         // occupied bandwidth holding 99 % of the power
    double offsetHz = 0;       // centre of the 99 % band from the samples' centre (coarse carrier offset)
    double snrDb = 0;          // in-band power over the noise floor
    double noiseFloor = 0;     // per-bin linear noise power
    std::vector<Burst> bursts; // sample ranges with signal
    double dutyCycle = 0;      // fraction of time on
    double burstPeriodSec = 0; // median start-to-start spacing when bursts repeat, else 0
    double meanBurstSec = 0;
    bool continuous = false;   // one burst covering (nearly) everything
};
struct ChannelOptions {
    double minGapSec = 0.004;  // a pause shorter than this does not split a burst
    double minBurstSec = 0.0004;
    size_t nfft = 4096;
};
std::vector<Burst> findBursts(const std::vector<cf>& x, double fs, double minGapSec, double minBurstSec, double* noiseOut = nullptr);
ChannelInfo measureChannel(const std::vector<cf>& x, double fs, const ChannelOptions& o = {});

// ================================================================ stage 2: modulation family (classify.cpp)
enum class Mod { Unknown, OOK, ASK, FSK, MSK, GMSK, PSK, QAM, OFDM };
const char* modName(Mod m);
struct ModFeatures {
    double envCv = 0;          // std / mean of |x| over the active samples
    double envKurt = 0;        // E|x|^4 / (E|x|^2)^2  (1 constant envelope, 2 complex Gaussian)
    double lowFrac = 0;        // fraction of active samples below 30 % of the strong level (OOK has many)
    double ask2Ratio = 0;      // low level / high level of a two-level amplitude fit
    double line[9] = {};       // power-law line strength for x^n (n = 1, 2, 4, 8): peak / mean of |FFT(x^n)|^2
    double lineHz[9] = {};     // where the x^n line sits (n x carrier offset)
    int freqClusters = 0;      // levels in the instantaneous-frequency histogram (2/4/8), 0 none
    double freqLevels[8] = {}; // their centres, Hz
    double freqSep = 0;        // ratio of between- to within-cluster spread of the instantaneous frequency
    double freqSepK[9] = {};   // the same for each level count k = 2, 4, 8 tried
    double cpCorr = 0;         // best normalised autocorrelation at an OFDM cyclic-prefix lag
    size_t cpLag = 0;
    double flatness = 0;       // spectral flatness inside the 99 % band (1 = flat top)
    double bwHz = 0;
};
struct ModGuess { Mod mod = Mod::Unknown; int order = 0; double confidence = 0; std::string why; };
struct ModResult {
    ModFeatures f;
    std::vector<ModGuess> ranked;   // best first, confidences sum to <= 1
    double deviationHz = 0;         // FSK: peak deviation (half the outer tone spacing)
    double toneSpacingHz = 0;       // FSK: spacing of adjacent tones
    double modIndex = 0;            // FSK/MSK: tone spacing / symbol rate (set once the rate is known)
    bool spreadHint = false;        // wide flat spectrum with no envelope structure: maybe DSSS
    ModGuess best() const { return ranked.empty() ? ModGuess{} : ranked[0]; }
};
// segs: the bursts to look at (nullptr: all of x)
ModFeatures modFeatures(const std::vector<cf>& x, double fs, double bwHz, const std::vector<Burst>* segs = nullptr);
ModResult classify(const std::vector<cf>& x, double fs, double bwHz, const std::vector<Burst>* segs = nullptr);
// instantaneous frequency (Hz) of each sample, x[n] * conj(x[n-1])
std::vector<float> instFreq(const std::vector<cf>& x, double fs);

// ================================================================ stage 3: symbol rate and recovery (rate.cpp, recover.cpp)
enum class RateMethod { Cyclostationary, SpectralLine, ZeroCrossing, Autocorrelation };
const char* rateMethodName(RateMethod m);
struct RateEstimate { RateMethod method; double baud = 0; double strength = 0; };
struct RateResult { double baud = 0; double confidence = 0; std::vector<RateEstimate> methods; };
// Each method alone (real-valued "symbol signal" for the last three: FM discriminator for FSK, envelope for OOK, I for BPSK)
// Each takes the bursts as separate segments; their spectra / statistics add up without the joins counting.
RateEstimate rateCyclo(const std::vector<std::vector<cf>>& segs, double fs);
RateEstimate rateSpectralLine(const std::vector<std::vector<float>>& segs, double fs);
RateEstimate rateZeroCrossing(const std::vector<std::vector<float>>& segs, double fs);
RateEstimate rateAutocorr(const std::vector<std::vector<float>>& segs, double fs);
// Combine: rates that agree within 2 % vote; harmonics (2x, 3x, 1/2) of the strongest are folded in
RateResult combineRates(const std::vector<RateEstimate>& m);
// the real-valued signal the rate estimators and the real slicer work on, for this modulation
std::vector<float> symbolSignal(const std::vector<cf>& x, double fs, Mod m);

struct Symbols {
    std::vector<float> soft;             // real slicer: the sampled values
    std::vector<cf> points;              // complex slicer: the constellation after carrier and timing recovery
    std::vector<int> sym;                // decided symbol index 0..order-1
    std::vector<float> levels;           // real slicer: level centres, ascending
    int order = 2;
    double evm = 0;                      // rms error vector / rms level
    double residualHz = 0;               // carrier loop: frequency it settled on (complex) / level offset (real)
    double timingJitter = 0;             // rms of the timing error, in symbols
};
// Gardner timing loop with cubic interpolation, at sps = fs / baud; loop bandwidth bn (per symbol)
std::vector<float> timingRecoverReal(const std::vector<float>& s, double sps, double bn = 0.01, double* jitter = nullptr);
std::vector<cf> timingRecoverComplex(const std::vector<cf>& x, double sps, double bn = 0.01, double* jitter = nullptr);
// carrier: coarse from the x^M line, then a decision-directed second-order PLL for M-PSK (M = 2/4/8) or 16-QAM
std::vector<cf> carrierRecover(const std::vector<cf>& sym, int m, bool qam, double* freqPerSym = nullptr);
double powerLawOffset(const std::vector<cf>& x, double fs, int m, const std::vector<Burst>* segs = nullptr);   // carrier offset from the x^m line, Hz
// k-means on 1-D values: order levels, ascending
std::vector<float> kmeans1d(const std::vector<float>& v, int k, int iters = 25);
Symbols recoverSymbols(const std::vector<cf>& x, double fs, double baud, Mod m, int order);
// symbols to bits: PSK/QAM Gray, FSK/ASK natural or Gray by level index, MSB of each symbol first
Bits symbolsToBits(const Symbols& s, Mod m, bool gray = true);

// ================================================================ stage 4: line codes, scramblers, polarity, bit order (linecode.cpp)
enum class LineCode { NRZ, NRZI, Manchester, ManchesterInv, DiffManchester, Miller, Differential };
const char* lineCodeName(LineCode c);
Bits decodeLine(const Bits& in, LineCode c, int phase = 0, double* violationRate = nullptr);
Bits encodeLine(const Bits& in, LineCode c);   // for tests and the generator
struct Scrambler {
    const char* name;
    uint32_t taps;       // polynomial without the x^0 term, bit k = x^(k+1)  (e.g. x^17 + x^12 + 1 -> bits 16 and 11)
    int degree;
    bool additive;       // additive (synchronous) PN, else multiplicative (self-synchronising)
};
const std::vector<Scrambler>& scramblers();
Bits descramble(const Bits& in, const Scrambler& s, uint32_t seed = 0xFFFFFFFFu);
Bits scramble(const Bits& in, const Scrambler& s, uint32_t seed = 0xFFFFFFFFu);
// how structured a stream looks: 1 - (entropy of 8-bit windows) / 8, plus repeated 24-bit pattern weight
double structureScore(const Bits& b);
struct LineCodeGuess { LineCode code = LineCode::NRZ; int phase = 0; int scrambler = -1; bool inverted = false; double score = 0; double violations = 0; };
std::vector<LineCodeGuess> rankLineCodes(const std::vector<Bits>& streams);
Bits invert(const Bits& b);
Bits applyLineGuess(const Bits& raw, const LineCodeGuess& g);

// ================================================================ stage 5: frame structure and checksums (frame.cpp, checksum.cpp)
struct SyncCandidate { Bits pattern; double score = 0; int hits = 0; bool inverted = false; };
struct FrameInfo {
    size_t preambleBits = 0;        // alternating 1010... before the sync word (typical, bursty input)
    SyncCandidate sync;             // best sync word
    std::vector<SyncCandidate> syncCandidates;
    size_t frameBits = 0;           // payload bits after the sync word (fixed length), or the shortest when lengths vary
    bool variableLength = false;
    std::vector<size_t> autocorrPeaks;   // continuous streams: lags of the bit-stream autocorrelation peaks
    std::vector<Bits> frames;       // payloads after the sync word, aligned
    std::vector<size_t> frameStart; // where each one started (bit index in its stream)
    std::vector<int> frameStream;   // which input stream
};
size_t preambleLength(const Bits& b, size_t from = 0);   // alternating run length starting at from
std::vector<SyncCandidate> findSyncCandidates(const std::vector<Bits>& streams, int minLen = 8, int maxLen = 32);
std::vector<double> bitAutocorr(const Bits& b, size_t maxLag);
FrameInfo findFrames(const std::vector<Bits>& streams);
// the frames that follow each occurrence of the sync word (or its inverse); frameBits 0: up to the next one / the stream's end
FrameInfo framesFor(const std::vector<Bits>& streams, const SyncCandidate& sync, size_t frameBits = 0);
// find a pattern allowing maxErr bit errors; returns the positions just after it
std::vector<size_t> findPattern(const Bits& b, const Bits& pat, int maxErr = 0, size_t from = 0);

enum class CheckKind { CRC, Fletcher16, Sum8, Xor8, Sum16 };
struct CheckAlgo {
    const char* name; CheckKind kind; int width;
    uint32_t poly, init; bool refin, refout; uint32_t xorout;
    uint32_t check;      // value over "123456789" (catalogue self-test)
};
const std::vector<CheckAlgo>& checkAlgos();
uint32_t checksum(const CheckAlgo& a, const uint8_t* d, size_t n);
std::vector<uint8_t> packBytes(const Bits& b, size_t from, size_t nBits, bool lsbFirst);
struct ChecksumHit {
    int algo = -1;          // index into checkAlgos()
    size_t coverStart = 0;  // bits from the frame start (the sync word's end), or from the sync word's start when coversSync
    bool coversSync = false;
    size_t coverEnd = 0;    // the checksum starts here, bits from the frame start (fixed-length frames)
    bool lsbFirst = false;  // bit order inside bytes
    bool inverted = false;  // frames were inverted
    bool littleEndian = false;    // the stored value's low byte comes first
    bool lengthFromEnd = false;   // variable-length frames: the checksum is at the end of each frame, tailBits before it
    size_t tailBits = 0;
    int matched = 0, tested = 0;
    double rate() const { return tested ? (double)matched / tested : 0; }
};
// search the catalogue over coverage starts, checksum positions, bit orders and polarity
std::vector<ChecksumHit> findChecksums(const std::vector<Bits>& frames, const Bits& sync, int maxHits = 5);

// ================================================================ stage 6: fields (fields.cpp)
struct ColumnStat { double p1 = 0; double entropy = 0; double change = 0; int n = 0; };
std::vector<ColumnStat> columnStats(const std::vector<Bits>& frames);
enum class FieldType { Constant, Counter, Checksum, Ascii, Enum, Value, Bytes };
const char* fieldTypeName(FieldType t);
struct Field { std::string name; size_t start = 0, length = 0; FieldType type = FieldType::Value; std::string value; };
std::vector<Field> suggestFields(const std::vector<Bits>& frames, const std::vector<ChecksumHit>& checks);
uint64_t bitsValue(const Bits& b, size_t start, size_t len, bool lsbFirst = false);
std::string interpretHex(const Bits& b, size_t start, size_t len, bool lsbFirst = false);
std::string interpretAscii(const Bits& b, size_t start, size_t len, bool lsbFirst = false);
std::string interpretDecimal(const Bits& b, size_t start, size_t len, bool lsbFirst = false, bool isSigned = false);
std::string interpretBits(const Bits& b, size_t start, size_t len, int group = 4);
Bits diffFrames(const Bits& a, const Bits& b);   // XOR, the length of the shorter

// ================================================================ the whole pipeline (pipeline.cpp)
struct Options {
    double minGapSec = 0.004;
    Mod forceMod = Mod::Unknown; int forceOrder = 0;   // 0 / Unknown: automatic
    double forceBaud = 0;
    bool gray = true;
};
struct Analysis {
    double fs = 0;                      // rate of the analysed (channelised) samples
    ChannelInfo channel;
    ModResult mod;
    RateResult rate;
    Mod usedMod = Mod::Unknown; int usedOrder = 0;
    std::vector<Symbols> symbols;       // per burst
    std::vector<Bits> rawBits;          // per burst, straight from the slicer
    std::vector<LineCodeGuess> lineCodes;
    LineCodeGuess line;
    std::vector<Bits> bits;             // per burst, after the chosen line code / descrambler / polarity
    FrameInfo frame;
    std::vector<ChecksumHit> checks;
    std::vector<Field> fields;
    std::vector<ColumnStat> columns;
    std::string name = "unknown signal";
    std::string notes;
    std::vector<std::string> log;       // what each stage decided, in words
};
Analysis analyse(const std::vector<cf>& x, double fs, const Options& o = {});
// from bits already known (stages 4-6 only): one stream per line
Analysis analyseBits(const std::vector<Bits>& streams, double baud = 0);
// recompute stages 5-6 after the user changed the sync word / frame length
void reframe(Analysis& a, const Bits& sync, size_t frameBits);
// recompute stages 4-6 with another line code / descrambler / polarity (from a.rawBits)
void applyLineCode(Analysis& a, const LineCodeGuess& g);

// ================================================================ exports (export.cpp)
// The Mode Builder description (docs/SIGNAL-LAB.md): symbol rate, modulation, sync word, frame length, field list, checksum.
std::string toModeJson(const Analysis& a);
std::string toCsv(const Analysis& a);
std::string toHexDump(const Analysis& a);
std::string saveAnalysisJson(const Analysis& a);          // the description plus frames and notes
bool loadAnalysisJson(const std::string& text, Analysis& a, std::string& err);
std::string bitsToString(const Bits& b);
Bits stringToBits(const std::string& s);                 // 0/1 characters, others ignored
std::string bitsToHex(const Bits& b);

// a tiny JSON reader for the exchange format (objects, arrays, strings, numbers, true/false/null)
struct Json {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false; double n = 0; std::string s;
    std::vector<Json> a; std::map<std::string, Json> o;
    const Json& operator[](const std::string& k) const;
    const Json& operator[](size_t i) const;
    double num(double d = 0) const { return t == Num ? n : d; }
    std::string str(const std::string& d = "") const { return t == Str ? s : d; }
};
bool parseJson(const std::string& text, Json& out, std::string& err);

// ================================================================ synthetic signals and the evaluation harness (gen.cpp)
struct GenSpec {
    Mod mod = Mod::FSK; int order = 2;
    double fs = 250000, baud = 9600, offsetHz = 0, snrDb = 25;
    double h = 1.0;                 // FSK modulation index (tone spacing / baud); MSK/GMSK 0.5
    double clockPpm = 0;            // transmitter symbol clock error
    LineCode line = LineCode::NRZ;
    int scrambler = -1;             // index into scramblers(), -1 none
    int preambleBits = 32;
    Bits sync;                      // sync word
    int payloadBytes = 12;
    int crcAlgo = -1;               // index into checkAlgos(), -1 none
    int frames = 12;
    double gapSec = 0.02;           // silence between frames (0: continuous stream with frames back to back)
    double rolloff = 0.35;          // PSK/QAM root-raised-cosine roll-off
    uint32_t seed = 1;
};
struct GenTruth { std::vector<Bits> payloads; Bits sync; size_t frameBits = 0; };
std::vector<cf> generate(const GenSpec& g, GenTruth* truth = nullptr);
GenSpec randomSpec(uint32_t seed);
struct EvalStage { const char* name; int tested = 0, ok = 0; };
struct EvalReport { std::vector<EvalStage> stages; int cases = 0; std::string text; };
EvalReport evaluate(int cases, uint32_t seed, bool verbose = false);

} // namespace dect2::signallab
