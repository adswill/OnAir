// Generic PSK demodulator and analyser (BPSK / QPSK / OQPSK / 8PSK): the pure DSP of the "PSK demodulator" mode (family 240).
// Everything here works on blocks or streams of complex samples at any rate; no engine, no threads, no I/O except the bit sinks in pskdsp_out.cpp.
//   channel  : find the strongest narrow signal in a span, its carrier and bandwidth (zooming in for narrow signals in wide captures)
//   analyse  : blind symbol rate (|x|^2 envelope line, x^2 / x^4 side lines, refined by the timing loop), modulation order (power-law lines,
//              then a trial demodulation per order scored by constellation fit and point use), roll-off, carrier and Doppler, MER / Es/N0 / Eb/N0
//   demod    : streaming channeliser (mixer, CIC, polyphase interpolator, FIR, any rate in, about 4-8 samples per symbol out), matched filter
//              (RRC or rectangular), Gardner timing loop with cubic interpolation, decision-directed Costas loop with an FLL assist
//   bits     : Gray demapping, differential decoding (symbol DPSK, NRZ-M, NRZ-S / NRZI), descramblers (G3RUH, CCSDS), sync-word hunt that resolves
//              the carrier phase ambiguity (rotation and spectral inversion), soft bits
//   gen      : test signals with a known payload (rates, offsets, Doppler ramps, noise) for tests/pskdsp
// Signal Lab (dect2/signallab) supplies the FFT, the Welch spectrum, the G3RUH descrambler and the blind frame analysis this mode hands bits to.
#pragma once
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::pskd {

using cf = std::complex<float>;
using Bits = std::vector<uint8_t>;   // one bit (0/1) per element

enum class Order { Auto = 0, Bpsk = 1, Qpsk = 2, Oqpsk = 3, Psk8 = 4 };
const char* orderName(Order o);   // "BPSK", ...
int orderPoints(Order o);         // 2, 4, 4, 8 (Auto: 0)
int orderBits(Order o);           // 1, 2, 2, 3

// ================================================================ channel (pskdsp_channel.cpp)
struct Carrier {
    bool found = false;
    double offsetHz = 0;     // centre of the signal from the samples' centre
    double bw3Hz = 0;        // width at -3 dB from the signal's (smoothed) top, noise subtracted
    double bw10Hz = 0;       // width at -10 dB
    double occHz = 0;        // 99 % occupied bandwidth
    double snrDb = 0;        // in-band power over the noise in the same band
    double peakDb = 0;       // top of the smoothed spectrum over the noise floor
};
// The strongest narrow signal: the region of the spectrum above the noise with the most power, ignoring a DC spike (a line one or two bins
// wide at 0 Hz). nearHz / nearOnly: only the region that contains nearHz (or the nearest one within maxDistHz). Zooms in (mixes and
// decimates) until the signal is at least ~40 bins wide, so a 25 Bd signal in an 8 Msps capture is measured as well as a wide one.
Carrier findCarrier(const cf* x, size_t n, double fs, bool nearOnly = false, double nearHz = 0, double maxDistHz = 0);

// ================================================================ building blocks (pskdsp_dsp.cpp)
std::vector<float> rrcTaps(double sps, double alpha, int spanSym);   // unit energy; alpha 0..1
std::vector<float> lowpassTaps(double cutoff, int taps);              // cutoff in cycles per sample (0..0.5), Blackman window, unit DC gain
// cubic (Catmull-Rom style Lagrange) interpolation of v at fractional index t; needs 1 sample before and 2 after floor(t)
cf interpCubic(const cf* v, double frac);

// Streaming channeliser: mixes offsetHz to 0, low-passes to passHz (two-sided width) and changes the rate from fsIn to about outRate()
// (an integer decimation, or an integer interpolation, chosen so the output holds the signal with 3-6x oversampling of passHz).
class Channeliser {
public:
    void configure(double fsIn, double offsetHz, double passHz, double minOutRate = 0);
    void setOffset(double hz);            // retune without losing the filter state
    double offset() const { return off_; }
    double outRate() const { return fsOut_; }
    double inRate() const { return fsIn_; }
    void reset();
    void process(const cf* x, size_t n, std::vector<cf>& out);   // appends
    static double planRate(double fsIn, double passHz, double minOutRate, int* dec, int* up);
private:
    double fsIn_ = 0, fsOut_ = 0, off_ = 0, pass_ = 0;
    int cicD_ = 1, up_ = 1, firD_ = 1;
    double phase_ = 0, dphi_ = 0;
    // CIC order 3
    std::complex<double> i1_{}, i2_{}, i3_{}, c1_{}, c2_{}, c3_{};
    int cicN_ = 0;
    std::vector<float> fir_;              // the low-pass (after the interpolator: its taps include the image rejection)
    std::vector<cf> hist_;                // FIR history, doubled ring
    size_t hpos_ = 0;
    int firN_ = 0;
};

struct DemodConfig {
    Order order = Order::Bpsk;
    double sps = 4;              // samples per symbol at the input of the demodulator
    double alpha = 0.35;         // RRC roll-off; <= 0: rectangular pulses (an integrate-and-dump matched filter)
    double timingBn = 0.004;     // loop noise bandwidth per symbol
    double carrierBn = 0.01;
    double fllGain = 0.02;       // FLL assist (decision-free, M-th power), per symbol; 0 off
    double initFreq = 0;         // carrier frequency at the start, radians per sample
};

struct DemodStats {
    double freq = 0;             // NCO frequency, radians per sample
    double spsNow = 0;           // the timing loop's samples per symbol (the measured symbol rate is fs / spsNow)
    double evm = 0;              // rms error vector over rms level, recent symbols
    double lockMetric = 0;       // 1 - normalised phase-error variance (1 = clean lock)
    uint64_t symbols = 0;
};

// Matched filter, Gardner timing loop (cubic interpolation; OQPSK: the Q rail is read half a symbol later), Costas loop (decision directed,
// M = 2 / 4 / 8) with an FLL assist from the M-th power of the symbols. Emits one normalised symbol per symbol period.
class Demod {
public:
    void configure(const DemodConfig& c);
    void reset();
    // appends symbols; phErr / tErr (optional) receive the phase error (radians) and the timing error (symbols) of each
    void process(const cf* x, size_t n, std::vector<cf>& sym, std::vector<float>* phErr = nullptr, std::vector<float>* tErr = nullptr,
                 std::vector<float>* eye = nullptr);   // eye: the I rail, 16 points per symbol (resampled), for the eye diagram
    DemodStats stats() const;
    const DemodConfig& config() const { return c_; }
private:
    DemodConfig c_;
    std::vector<float> mf_;
    std::vector<cf> mfHist_; size_t mfPos_ = 0;
    std::vector<cf> buf_;        // matched-filter output since bufBase_
    double bufBase_ = 0;         // absolute index of buf_[0]
    double nextT_ = 0;           // absolute fractional index of the next symbol strobe
    double spsAdj_ = 0;          // timing loop integrator
    double tKp_ = 0, tKi_ = 0, cKp_ = 0, cKi_ = 0;
    double phase_ = 0, freq_ = 0;
    double agc_ = 1;
    cf prevSym_{0, 0}, prevM_{0, 0};
    bool havePrev_ = false;
    double evmAcc_ = 0, phVar_ = 0;
    uint64_t nIn_ = 0, nSym_ = 0;
    double eyeT_ = 0;
};

// decide: nearest M-PSK point (BPSK 0 / 180 degrees, QPSK and OQPSK +-45 / +-135, 8PSK k x 45); idx 0..M-1 counts counter-clockwise
cf decide(cf z, Order o, int* idx = nullptr);

// ================================================================ blind analysis (pskdsp_analyse.cpp)
struct RateCandidate { double baud = 0; double conf = 0; std::string how; bool snapped = false; };
struct AnalyseOptions {
    Order forceOrder = Order::Auto;
    double forceBaud = 0;        // 0: blind
    double forceAlpha = -2;      // -2 automatic, -1 rectangular, 0..1 RRC roll-off
    bool snapStandard = true;    // snap to 75 / 150 / ... / 9600 x 2^k, 1k2 x n and the other common rates when within 0.6 %
    bool haveCarrier = false;    // carrierHz / bwHz given (a click in the spectrum); else the strongest signal is found
    double carrierHz = 0, bwHz = 0;
    double maxSeconds = 30;      // analyse at most this much of the input (the latest part)
};
struct Analysis {
    bool signal = false, rateFound = false, locked = false;
    std::string status;          // plain words: "searching for a signal", "locked at 1200 baud BPSK, Eb/N0 9 dB", ...
    std::vector<std::string> log;
    Carrier carrier;             // where the signal is in the input
    double fsChan = 0;           // rate the analysis ran at (after the channeliser)
    std::vector<RateCandidate> rates;   // best first
    double baud = 0, baudConf = 0; bool baudSnapped = false; double baudRaw = 0;   // baudRaw: before snapping, after the loop refinement
    Order order = Order::Auto; double orderConf = 0;
    double line2 = 0, line4 = 0, line8 = 0, lineEnv = 0;   // power-law / envelope line strengths, dB over the spectrum's median
    std::vector<std::pair<Order, double>> orderEvm;          // trial demodulation: EVM per order tried
    double alpha = 0.35; bool rect = false; std::string shape;   // pulse shape estimate
    double carrierHz = 0;        // final carrier from the samples' centre (channel offset + loop frequency)
    double dopplerHzPerSec = 0;  // slope of the loop frequency over the block
    double evm = 0, merDb = 0, esn0Db = 0, ebn0Db = 0;
    double bitRate = 0;
    // displays
    std::vector<float> envSpecDb; double envHzPerBin = 0; double envLineHz = 0;   // spectrum of |x|^2, 0 .. fsChan/2
    std::vector<float> powSpecDb; int powM = 2; double powHzPerBin = 0; double powLineHz = 0;   // spectrum of x^M, centred (bin size/2 = 0 Hz)
    std::vector<cf> symbols;     // the trial demodulation's symbols with the chosen order (constellation)
};
Analysis analyse(const cf* x, size_t n, double fs, const AnalyseOptions& o = {});
// the standard rate nearest b when within tol (fraction), else 0
double snapRate(double b, double tol = 0.006);
// roll-off from the power spectrum of the channel and the symbol rate: alpha, and rect = true for rectangular (sinc^2) pulses
double estimateRolloff(const cf* y, size_t n, double fs, double baud, bool* rect);

// ================================================================ bits (pskdsp_bits.cpp)
enum class Diff { None = 0, Dpsk = 1, NrzM = 2, NrzS = 3 };   // NRZI is NRZ-S here (a 0 is a change, as AX.25 / HDLC)
enum class Scrambler { None = 0, G3ruh = 1, Ccsds = 2 };
const char* diffName(Diff d);
const char* scramblerName(Scrambler s);
// symbol index (0..M-1, counter-clockwise from decide()) to bits, MSB first. BPSK: 0 -> 0. QPSK / OQPSK: bit 0 from I (I < 0 -> 1), bit 1
// from Q. 8PSK: Gray code along the circle.
void symbolBits(int idx, Order o, uint8_t* out);
int bitsSymbol(const uint8_t* b, Order o);
// soft bits (positive: 0 more likely), one float per bit
void symbolSoft(cf z, Order o, float* out);

struct BitOptions {
    Diff diff = Diff::None;
    Scrambler scr = Scrambler::None;
    Bits sync;                   // known sync word (resolves the phase ambiguity and aligns the CCSDS randomiser); empty: none
    int syncMaxErr = -1;         // -1: length / 12
    bool autoAmbiguity = true;   // try every rotation and both spectral senses against the sync word
    int rotation = 0; bool conj = false;   // used when autoAmbiguity is off or there is no sync word
    bool invert = false;         // invert the output bits
    size_t ccsdsFrameBits = 0;   // CCSDS randomiser: bits after each sync word it covers (0: up to the next sync word)
};
struct BitResult {
    Bits bits;                   // after differential decoding, descrambling, inversion
    std::vector<float> soft;     // soft bits (before differential decoding: only when diff == None)
    int rotation = 0; bool conj = false; bool ambiguityResolved = false;
    std::vector<size_t> syncAt;  // bit index just after each sync word found
    int syncHits = 0;
};
// one block: rotate / conjugate the symbols, demap, decode
BitResult symbolsToBits(const std::vector<cf>& sym, Order o, const BitOptions& b);
Bits ccsdsSequence(size_t n);            // the CCSDS pseudo-randomiser x^8+x^7+x^5+x^3+1 from all ones: FF 48 0E C0 9A ...
Bits g3ruhDescramble(const Bits& in);    // 1 + x^12 + x^17 self-synchronising (Signal Lab's)
Bits g3ruhScramble(const Bits& in);
Bits diffDecode(const Bits& in, Diff d); // NRZ-M / NRZ-S on bits
Bits diffEncode(const Bits& in, Diff d);
std::vector<size_t> findSync(const Bits& b, const Bits& sync, int maxErr);   // positions just after each match (non-overlapping)
std::vector<uint8_t> packBits(const Bits& b, bool lsbFirst = false);
Bits parseBitsOrHex(const std::string& s);   // "0101..." or hex "1ACFFC1D" / "0x1ACFFC1D"
std::string bitsHex(const Bits& b, size_t from, size_t n);
std::string bitsAscii(const Bits& b, size_t from, size_t n);   // printable bytes, others as '.'

// Streaming bit decoder for the receiver: the same as symbolsToBits but symbol by symbol, with a sync hunt over a window to fix the ambiguity.
class BitDecoder {
public:
    void configure(Order o, const BitOptions& b);
    void reset();
    void push(const cf* sym, size_t n, Bits& out, std::vector<float>* soft = nullptr);   // appends
    void drain(Bits& out, std::vector<float>* soft = nullptr);   // end of a block: decide on the symbols still held by the hunt, emit them
    void flush(Bits& out);                                        // and the bits the CCSDS de-randomiser holds back
    int rotation() const { return rot_; }
    bool conj() const { return conj_; }
    bool resolved() const { return resolved_; }
    uint64_t syncCount() const { return syncs_; }
private:
    void emit(cf z, Bits& out, std::vector<float>* soft);
    int huntBest(int& r, bool& c) const;   // sync hits of the best rotation / sense on hunt_
    Bits pend_;
    Order o_ = Order::Bpsk; BitOptions b_;
    int rot_ = 0; bool conj_ = false; bool resolved_ = false;
    std::vector<cf> hunt_;       // symbols held until the sync hunt decides
    int prevIdx_ = 0; uint8_t prevBit_ = 0; bool havePrev_ = false;
    uint32_t g3_ = 0;            // G3RUH descrambler register (past input bits)
    Bits tail_;                  // last bits for the sync search
    size_t sinceSync_ = 0, ccsdsPos_ = 0; bool ccsdsOn_ = false;
    uint64_t syncs_ = 0, sinceHit_ = 0;
};

// ================================================================ test signals (pskdsp_gen.cpp)
struct GenSpec {
    Order order = Order::Bpsk;
    double fs = 48000, baud = 1200;
    double offsetHz = 0;         // carrier from the centre at the start
    double dopplerHzPerSec = 0;  // carrier ramp
    double clockPpm = 0;         // symbol clock error
    double esn0Db = 30;          // Es/N0 in the symbol rate's bandwidth (noise is added over the whole capture)
    double alpha = 0.35;         // <= 0: rectangular pulses
    Diff diff = Diff::None;
    Scrambler scr = Scrambler::None;
    Bits sync;                   // sent before every payload (not scrambled; the CCSDS randomiser restarts after it)
    int payloadBits = 256;
    int frames = 20;
    double phase0 = 0.7;         // initial carrier phase, radians
    uint32_t seed = 1;
};
struct GenTruth { std::vector<Bits> payloads; Bits stream; };   // stream: everything sent, before differential encoding
std::vector<cf> generate(const GenSpec& g, GenTruth* truth = nullptr);

} // namespace dect2::pskd
