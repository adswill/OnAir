// Ham science modes, signal processing layer (no engine here): the weak-signal and slow digital modes FSK441, ISCAT, PI4 and Throb, the slow-CW
// grabbers QRSS / DFCW, and the three science receivers SuperSID (VLF station level and solar flare detector), riometer (cosmic noise absorption)
// and pulsar folding. Real audio (the upper sideband audio of a receiver, dial at 0 Hz) in for the digital modes, power or complex samples in for the others.
//
// Third-party facts: the FSK441 / ISCAT / PI4 parameters and tables were read from the WSJT family as documented in MSHV (GPL-3.0, LZ2HV) and WSJT
// (K1JT, GPL-3.0); the Throb tone pair tables and timing from fldigi (W1HKJ, GPL-3.0, after gmfsk by OH2BNS, ThrobX by KD8ATU). All receivers and
// generators here are new code. Nothing in this layer has been checked against an on-air signal.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace hamsci {

constexpr double kAudioRate = 12000.0;   // the rate the receivers hand to the decoders (any other rate works too)

// ------------------------------------------------------------------------------------------------------------------ FSK441 (meteor scatter)
// 4 tones 882 / 1323 / 1764 / 2205 Hz, 441 baud, three tones per character (base 4, 6 bits, the first digit is never 3), the message repeats.
struct Fsk441Burst {
    double startSec = 0;       // signal time of the start of the block, seconds
    double durSec = 0;
    double freqOffsetHz = 0;   // tuning error found (tones sit at 882 ... 2205 Hz plus this)
    double snrDb = 0;          // tone contrast, dB (not a calibrated SNR)
    std::string text;
};
std::vector<float> fsk441Generate(const std::string& msg, double fs, int repeats, double offsetHz = 0, double amp = 0.5);
// Decodes one block of audio of a burst (or a continuous signal) of at most a few seconds. Returns false when it does not read as FSK441.
bool fsk441DecodeBlock(const float* x, size_t n, double fs, Fsk441Burst& out, double tolHz = 300);
// Streaming: finds the bursts above the noise and decodes them.
class Fsk441Decoder {
public:
    explicit Fsk441Decoder(double fs = kAudioRate);
    void reset();
    void feed(const float* x, size_t n);
    std::vector<Fsk441Burst> take();
    double signalSec() const { return (double)total_ / fs_; }
    double levelDb() const { return level_; }
    int bursts() const { return nBursts_; }
    void setThresholdDb(double db) { thr_ = db; }
private:
    void process(bool flushAll);
    double fs_;
    std::vector<float> buf_;            // audio not yet analysed
    uint64_t base_ = 0, total_ = 0;     // sample index of buf_[0]; samples seen
    std::vector<float> noise_;          // recent frame band powers
    struct Run { bool on = false; size_t start = 0, scan = 0; int below = 0; } run_;
    double thr_ = 6.0, level_ = -120;
    int nBursts_ = 0;
    std::vector<Fsk441Burst> out_;
};

// ------------------------------------------------------------------------------------------------------------------ ISCAT-A / ISCAT-B
// 42 tones (A: 21.53 Hz spacing, 46.4 ms symbols, from 1012 Hz; B: 43.07 Hz, 23.2 ms, from 560 Hz), a 24-symbol frame (4 Costas tones, 2 length tones,
// 18 data tones) that cycles through "@" + the message.
struct IscatResult {
    std::string text;
    int variant = 0;           // 0 ISCAT-A, 1 ISCAT-B
    double freqOffsetHz = 0;
    double snrDb = 0;          // energy at the expected tones against the rest, dB (not calibrated)
    double sync = 0;           // sync score (1 = noise)
    int frames = 0;
    double startSec = 0;
    int weakChars = 0;         // characters shown as '?' because the tone was not clear
};
std::vector<float> iscatGenerate(const std::string& msg, int variant, double fs, double seconds, double offsetHz = 0, double amp = 0.5, double startSec = 0);
bool iscatDecode(const float* x, size_t n, double fs, int variant, IscatResult& out, double tolHz = 100);   // variant 0 A, 1 B
class IscatDecoder {
public:
    IscatDecoder(double fs = kAudioRate, int variant = 2);   // 2 = try both
    void reset();
    void feed(const float* x, size_t n);
    std::vector<IscatResult> take();
    void setVariant(int v) { variant_ = v; }
    double levelDb() const { return level_; }
private:
    double fs_; int variant_;
    std::vector<float> buf_;
    uint64_t total_ = 0, nextTry_ = 0;
    std::string last_;
    double level_ = -120;
    std::vector<IscatResult> out_;
};

// ------------------------------------------------------------------------------------------------------------------ PI4
// 4-FSK, 146 symbols of 185.76 ms (2048 samples at 11025 Hz), tone spacing 5.383 Hz, 8 characters of a 38-character set in 42 bits, rate 1/2 K=32
// convolutional code, bit-reversal interleaver, a fixed sync vector in the low bit of the tone number.
struct Pi4Result {
    std::string text;
    double freqHz = 0;         // frequency of the lowest tone (tone number 0)
    double startSec = 0;
    double snrDb = 0;
    double sync = 0;
};
std::vector<float> pi4Generate(const std::string& msg8, double fs, double f0Hz, double amp = 0.5, double startSec = 0, double totalSec = 30);
bool pi4Pack(const std::string& msg, uint8_t bits42[6]);          // upper-cases, pads to 8 with spaces; false when a character is not in the set
std::string pi4Unpack(const uint8_t bits42[6]);
bool pi4Decode(const float* x, size_t n, double fs, Pi4Result& out, double fLoHz = 400, double fHiHz = 2600);
constexpr int kPi4Symbols = 146;
constexpr double kPi4SymbolSec = 2048.0 / 11025.0;
class Pi4Decoder {
public:
    explicit Pi4Decoder(double fs = kAudioRate);
    void reset();
    void feed(const float* x, size_t n);      // decodes whenever 32 s are there (the transmission is 27.1 s long, slots 30 s)
    std::vector<Pi4Result> take();
    double levelDb() const { return level_; }
private:
    double fs_;
    std::vector<float> buf_;
    uint64_t total_ = 0;
    double level_ = -120;
    std::string last_;
    std::vector<Pi4Result> out_;
};

// ------------------------------------------------------------------------------------------------------------------ Throb
// Throb1 / 2 / 4 (9 tones, one or two tones per symbol, 45 characters with a shift symbol) and ThrobX1 / 2 / 4 (11 tones, 55 symbols, idle and space alternate).
enum ThrobVariant { Throb1 = 0, Throb2, Throb4, ThrobX1, ThrobX2, ThrobX4, kThrobVariants };
const char* throbName(int v);
double throbSymbolSec(int v);          // 1.024, 0.512, 0.256 s
double throbToneSpacing(int v);        // 8, 8, 16, 7.8125, 7.8125, 15.625 Hz
std::vector<float> throbGenerate(const std::string& text, int variant, double fs, double centreHz, double amp = 0.5, int preambleSymbols = 4);
struct ThrobResult {
    std::string text;
    int variant = -1;
    double centreHz = 0;
    double snrDb = 0;
    double score = 0;          // 0 .. 1, how much of the energy sits in the expected two tones per symbol
    int symbols = 0;
};
// Looks for the tone comb in 300 .. 3500 Hz, tries every variant and timing, returns the best reading. variant: -1 automatic, or a ThrobVariant.
bool throbDecode(const float* x, size_t n, double fs, ThrobResult& out, int variant = -1);
class ThrobDecoder {
public:
    explicit ThrobDecoder(double fs = kAudioRate);
    void reset();
    void feed(const float* x, size_t n);      // analyses the last 25 s every 5 s
    std::vector<ThrobResult> take();
    void setVariant(int v) { variant_ = v; }
    double levelDb() const { return level_; }
private:
    double fs_;
    int variant_ = -1;
    std::vector<float> buf_;
    uint64_t total_ = 0, nextTry_ = 0;
    std::string lastText_;
    double level_ = -120;
    std::vector<ThrobResult> out_;
};

// ------------------------------------------------------------------------------------------------------------------ QRSS / DFCW grabber
// Slow CW: dots of 1 .. 120 s. The grabber makes a spectrogram of a narrow window and reads the keyed tone. DFCW: all elements have the same length, dots on
// one frequency and dashes on a second one a few tenths of a Hz to some Hz higher (assumed: dot lower, dash upper; the other order can be chosen), no gap inside
// a letter, one element of silence between letters, three between words. These conventions are the usual amateur ones, not a formal specification.
struct QrssFrame { double timeSec = 0; double peakHz = 0; float peakDb = 0; };
struct QrssTelemetryCore {
    double centreHz = 0;       // audio frequency of the window centre
    double dotSec = 0;         // element length found (0 = not yet)
    double snrDb = 0;
    bool locked = false;
    std::string text;          // decoded so far
    std::vector<std::vector<float>> rows;    // newest last; each row: power in dB over the noise floor, spanBins values
    double rowSec = 0, binHz = 0, spanHz = 0;
};
class QrssGrabber {
public:
    QrssGrabber(double fs = kAudioRate, bool dfcw = false);
    void reset();
    void setDfcw(bool on) { dfcw_ = on; reset(); }
    void setCentreHz(double hz) { centre_ = hz; auto_ = false; reset(); }   // manual: the window centre in audio Hz
    void setAutoCentre(bool on) { auto_ = on; }
    void setDotSec(double s) { dotFixed_ = s; }                          // 0 = automatic
    void setSpanHz(double hz) { spanHz_ = hz; reset(); }               // width of the window (default 12 Hz)
    void setDfcwReverse(bool on) { dfcwRev_ = on; }
    void feed(const float* x, size_t n);
    const QrssTelemetryCore& state() const { return st_; }
    std::string takeText();
    size_t maxRows = 400;
private:
    void analyse();
    double fs_; bool dfcw_;
    double centre_ = 1000, spanHz_ = 12, dotFixed_ = 0;
    bool auto_ = true, dfcwRev_ = false;
    std::vector<float> in_;                 // audio kept for the centre search (last 40 s)
    std::vector<std::complex<float>> dec_;  // decimated stream, 100 Hz
    std::complex<double> ph_{1, 0}, osc_{1, 0}, step_{1, 0};
    double phase_ = 0; int decCnt_ = 0; std::complex<double> acc_{0, 0};
    uint64_t sampleN_ = 0;
    uint64_t decN_ = 0;
    uint64_t rowsDone_ = 0;
    double lastSearchSec_ = -1e9, dotKnown_ = 0;
    std::vector<QrssFrame> frames_;
    std::string emitted_;
    size_t textTaken_ = 0;
    QrssTelemetryCore st_;
};
// Offline helpers used by the tests and the generator
std::vector<float> qrssGenerate(const std::string& text, double dotSec, double fs, double toneHz, bool dfcw, double shiftHz = 2.0, double amp = 0.2, double leadSec = 0);
// Reads the keyed tone from a decimated power track: element length (seconds) and text. Used by the grabber.
bool qrssReadTrack(const std::vector<float>& level, const std::vector<float>& freq, double frameSec, bool dfcw, bool dfcwRev, double dotFixed, double& dotSec, std::string& text);

// ------------------------------------------------------------------------------------------------------------------ SuperSID (VLF stations)
struct SidStation { const char* call; double kHz; const char* where; double lat, lon; };
const SidStation* sidStations(int& count);
struct SidStationState {
    std::string call;
    double kHz = 0;
    bool inSpan = false;
    float levelDb = -200;      // dB over the local noise, last 5 s
    float absDb = -200;        // dBFS in the carrier bins
    float baselineDb = -200;   // median of the last 60 min
    float riseDb = 0;          // level now (2 min mean) minus baseline
    bool present = false;      // the carrier stands 6 dB over the noise
    bool event = false;        // a sudden rise in progress (SID)
    double eventPeakDb = 0;
    std::vector<float> history;   // dB over the noise, one value per minute, newest last, 24 h at most
};
struct SidState {
    double timeSec = 0;
    std::vector<SidStationState> stations;
    int eventsTotal = 0;
    double lastEventAgoSec = -1;
    double lastEventRiseDb = 0;
    std::string lastEventCall;
};
class SidMonitor {
public:
    SidMonitor();
    void configure(double rate, double dialHz);      // complex input at `rate` (a VLF receiver, usually 24 .. 48 kHz) around the dial
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    void setRiseThresholdDb(double db) { riseDb_ = db; }     // default 1.5 dB
    void setSampleSec(double s) { avgSec_ = s; }              // power average, default 5 s (SuperSID standard)
    const SidState& state() const { return st_; }
    // test hook: feeds one level per station directly (dB over the noise), dt seconds long
    void pushLevels(const std::vector<float>& levelsDb, double dtSec);
    int nStations() const { return (int)st_.stations.size(); }
private:
    void finishAverage();
    void evaluate(size_t i);
    double rate_ = 0, dial_ = 0, riseDb_ = 1.5, avgSec_ = 5;
    int nfft_ = 0;
    std::vector<std::complex<float>> win_;
    std::vector<float> w_;
    std::vector<double> acc_;
    int accN_ = 0;
    double nextAvg_ = 0, sinceMin_ = 0;
    std::vector<std::vector<float>> recent_;     // per station: levels of the last 60 min, one per average
    std::vector<std::vector<float>> minuteAcc_;
    SidState st_;
    std::vector<int> binLo_, binHi_;
    std::vector<int> nzLo_, nzHi_;
};
std::vector<std::complex<float>> sidRender(double rate, double dialHz, double seconds, const std::vector<std::pair<double, double>>& carriersHzDb,
                                           double noiseRms, const std::vector<std::vector<float>>& gainDbPerSecond, uint32_t seed);

// ------------------------------------------------------------------------------------------------------------------ Riometer
struct RioState {
    double timeSec = 0;
    double lstHours = 0;            // local sidereal time, hours
    float powerDb = -200;           // quiet-sky power estimate (interference rejected), dB re full scale
    float qdcDb = -200;             // quiet day curve at this sidereal time, dB (-200 none yet)
    float absorptionDb = 0;         // qdc - power (>= 0 means absorption); 0 when no curve yet
    bool haveQdc = false;
    int qdcFill = 0;                // sidereal bins with a value, of 288
    double interferedFrac = 0;      // share of the last minutes that were rejected as interference
    bool event = false;             // absorption over the threshold now
    std::vector<float> absHistory;  // one value per minute, newest last (24 h)
    std::vector<float> powerHistory;
};
class Riometer {
public:
    Riometer();
    void configure(double rate);
    void reset();                    // keeps the curve
    void feed(const std::complex<float>* x, size_t n);
    void setPosition(double lonDeg) { lon_ = lonDeg; }      // east positive; sets the sidereal time (default 20 E)
    void setStartUnix(double t) { startUnix_ = t; }
    void setThresholdDb(double d) { thr_ = d; }               // default 0.3 dB
    const RioState& state() const { return st_; }
    // a minute of power directly (tests): linear power of the quiet-sky estimate
    void pushMinute(double powerLin, double unixTime, bool interfered = false);
    std::string exportQdc() const;    // text, for the settings
    bool importQdc(const std::string& s);
    static double siderealHours(double unixTime, double lonDeg);
private:
    double rate_ = 0, lon_ = 20, startUnix_ = 0, thr_ = 0.3;
    double sampleN_ = 0;
    std::vector<float> blk_;         // 1 s means of power
    double blkAcc_ = 0; size_t blkN_ = 0;
    std::vector<double> secs_;       // the seconds of the current minute
    double minuteEnd_ = 60;
    std::vector<float> qdc_;         // 288 bins (5 min of sidereal time), linear power, upper envelope
    std::vector<int> qdcN_;
    int rejects_ = 0, minutes_ = 0;
    std::vector<float> floorSecs_;
    RioState st_;
};
std::vector<std::complex<float>> rioRender(double rate, double seconds, double powerLin, double unixStart, double lonDeg, double sidereal24hModDepth,
                                           const std::vector<std::pair<double, double>>& absorbSecDb, double interferenceFrac, uint32_t seed);

// ------------------------------------------------------------------------------------------------------------------ Pulsar folding
struct PulsarTarget { const char* name; double periodS; double dm; double fluxJy; double freqMHz; const char* note; };
const PulsarTarget* pulsarTargets(int& count);
struct PulsarResult {
    bool ok = false;
    double snr = 0;
    double periodS = 0;            // period of the best fold
    double dm = 0;                 // dispersion measure of the best trial
    double phasePeak = 0;          // 0 .. 1
    double dutyCycle = 0;
    std::vector<float> profile;    // folded, mean-subtracted, scaled to noise rms units
    double observedSec = 0;
    std::vector<float> dmCurve;    // S/N against trial DM
    std::vector<float> periodCurve;
};
struct PulsarConfig {
    double centreHz = 408e6;       // only used for the dispersion delay
    double rateHz = 2.4e6;
    int channels = 64;             // power of two
    double sampleSec = 0.5e-3;     // power sampling time after integration (>= 100 us)
    double periodS = 0.7145;       // nominal period (the user's ephemeris)
    double dm = 26.76;
    int bins = 64;
    double dmSpan = 4.0;           // trial DMs: dm +- span (pc cm^-3)
    int dmSteps = 9;
    double periodRelSpan = 2e-4;   // trial periods: period * (1 +- span)
    int periodSteps = 21;
};
class PulsarFolder {
public:
    explicit PulsarFolder(const PulsarConfig& c = PulsarConfig());
    void configure(const PulsarConfig& c);
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    // Run the search on what has been collected (can be called every few seconds; cost grows with the record)
    PulsarResult search();
    double observedSec() const { return (double)samplesOut_ * cfg_.sampleSec; }
    size_t blocksStored() const { return series_.size() / (size_t)cfg_.channels; }
    const PulsarConfig& config() const { return cfg_; }
    // test hook: one spectrum row of channel powers per call
    void pushRow(const float* chPower);
private:
    PulsarConfig cfg_;
    int fftN_ = 64;
    std::vector<std::complex<float>> fft_;
    int fftFill_ = 0;
    std::vector<double> rowAcc_;
    int rowCnt_ = 0, rowTarget_ = 1;
    std::vector<float> series_;          // channels x time, time-major (row = channels floats)
    uint64_t samplesOut_ = 0;
    std::vector<float> win_;
};
// Radiometer equation: effective area (m^2) needed for a given S/N
double pulsarRequiredAreaM2(double fluxJy, double bandwidthHz, double tsysK, double seconds, double dutyCycle, double snr, int npol = 2);
// Dispersion delay between two frequencies (seconds), dm in pc cm^-3, frequencies in MHz
double pulsarDelaySec(double dm, double loMHz, double hiMHz);
// Filter bank rows (channel powers, one row of `channels` floats per sample time, noise mean 1) with dispersed pulses: the same thing the folder gets after its
// FFT front end, without synthesising the radio samples (the radio data rate makes minutes of IQ unusable in a test)
std::vector<float> pulsarRenderRows(const PulsarConfig& c, double seconds, double snrPerChannelPerRow, uint32_t seed, double dutyCycle = 0.03,
                                    double trueDm = -1, double truePeriod = -1, double noiseRel = -1);
// Test signal: pulses with dispersion in noise (IQ; short records only)
std::vector<std::complex<float>> pulsarRender(const PulsarConfig& c, double seconds, double snrPerPulseInBand, uint32_t seed, double dutyCycle = 0.03, double trueDm = -1, double truePeriod = -1);

}  // namespace hamsci
}  // namespace dect2
