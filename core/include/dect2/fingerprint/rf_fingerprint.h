// RF fingerprint of an individual NFM / WFM / AM transmitter (Discord request "FM TX Fingerprint"). Pure DSP, no ImGui, no engine.
//
// What is measured from the channel of one carrier (the app mixes it to 0 Hz and decimates it with Channel):
//  - carrier frequency offset (lag-1 phase estimator, mHz when the carrier is unmodulated), its warm-up drift,
//  - keying transient: rise time, overshoot, start-up frequency chirp and settling time (needs the carrier to turn on inside the capture),
//  - FM: audio deviation profile, pre-emphasis tilt, mains hum; WFM: 19 kHz pilot level, exact pilot frequency (the exciter's own clock) and the
//    phase jitter of the pilot; AM: modulation depth,
//  - spurs and the I/Q image from a wideband power spectrum (WidePsd).
// Library keeps named fingerprints and match() gives a 0..100 similarity. Watcher logs every transmission (also carrier-only ones, no audio).
// The receiver's own oscillator error adds to every offset (docs/RF-FINGERPRINT.md says how to cope); tested on synthetic IQ.
#pragma once
#include <complex>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "dect2/dsp/fftutil.h"

namespace dect2::fp {

using cf32 = std::complex<float>;
enum class Kind { NFM = 0, WFM = 1, AM = 2 };
const char* kindName(Kind k);

// Mixes a carrier at carrierOffsetHz (from the capture centre) down to 0 Hz and decimates to a few times the channel width. Streaming.
class Channel {
public:
    Channel(double inRateHz, double carrierOffsetHz, Kind kind, double chanBwHz = 0);   // chanBwHz 0 = the kind's usual width
    double outRate() const { return outRate_; }
    int decimation() const { return dec_; }
    double noiseLag1() const { return noiseLag1_; }   // lag-1 correlation of white noise after the channel filter (corrects the offset estimate)
    void push(const cf32* x, size_t n, std::vector<cf32>& out);   // appends the new channel samples to out
private:
    double outRate_, theta_, noiseLag1_ = 0;
    int dec_;
    std::vector<cf32> taps_, hist_;      // modulated low-pass taps; the last taps-1 input samples
    double ph_ = 0;                      // mixer phase at the next output sample
    size_t skip_ = 0;                    // input samples until the next output
};

struct Spur { double offHz = 0, dbc = -99; };

struct Features {
    bool valid = false;
    Kind kind = Kind::NFM;
    double tunedHz = 0;                  // the frequency the channel was tuned to (RF)
    double rate = 0;                     // channel sample rate
    double durationS = 0;                // steady-state part analysed
    // carrier
    double cfoHz = 0, cfoStdHz = 0;      // offset of the true carrier from tunedHz, and its uncertainty
    double driftHzPerS = 0;              // slow frequency drift while the carrier is up (oscillator warm-up)
    double snrDb = 0;
    double rxPpm = 0;                    // the receiver's known oscillator error when captured (0 = unknown), removed again when comparing
    bool carrierOnly = false;            // no modulation (the key-up of a repeater or a plain carrier)
    // modulation
    double devRmsHz = 0, devP95Hz = 0;   // FM: audio-band deviation. AM: depth in percent (rms, p95)
    double tiltDbOct = 0; bool tiltValid = false;   // pre-emphasis curve
    double hum[4] = {0, 0, 0, 0};        // 50, 60, 100, 120 Hz amplitude (Hz of deviation, AM: percent depth), 0 = not significant
    // WFM
    bool pilot = false; double pilotDevHz = 0, pilotErrHz = 0;   // 19 kHz pilot level and its frequency error from 19 kHz
    double pilotJitterDeg = 0;           // rms phase jitter of the pilot (the exciter's PLL), degrees
    // keying transient (needs the turn-on inside the capture)
    bool transient = false;
    double riseUs = 0, overshootPct = 0, chirpHz = 0, settleMs = 0;
    // wideband
    bool spurValid = false; double rxOffsetHz = 0;   // carrier offset from the capture centre
    std::vector<Spur> spurs;             // strongest first
    double imageDbc = 99;                // the mirror image of the carrier about the capture centre (receiver and transmitter I/Q imbalance)
};

struct Options {
    Kind kind = Kind::NFM;
    double tunedHz = 0;
    double carrierOnlyDevHz = 0;         // 0 = default of the kind: NFM 300 Hz, WFM 500 Hz, AM 2 percent
    double noiseLag1 = 0;                // Channel::noiseLag1()
    double noiseFloorPow = 0;            // channel noise power (mean |z|^2) when the caller tracked it; 0 = estimated from the capture
};

// z: channel samples (carrier near 0 Hz). Pass the whole transmission including some noise before the carrier to get the transient.
Features analyse(const cf32* z, size_t n, double rate, const Options& o);

// Welch power spectrum of the wideband capture, decayed average, for spurs and the I/Q image
class WidePsd {
public:
    explicit WidePsd(double rate, double memorySec = 0.5);
    void add(const cf32* x, size_t n);   // any block size
    bool ready() const { return segs_ > 0; }
    // fills spurValid, spurs, imageDbc, rxOffsetHz of f. occBwHz: width around the carrier that belongs to the signal itself
    void spurs(double carrierOffsetHz, double occBwHz, Features& f) const;
    double binHz() const { return rate_ / kN; }
    static constexpr int kN = 8192;
private:
    double rate_, alpha_;
    Fft fft_{kN};
    std::vector<float> win_;
    std::vector<double> p_;
    size_t discard_ = 0, skipSeg_ = 1;
    double lastPow_ = 0, norm_ = 0;
    std::vector<cf32> acc_;
    size_t segs_ = 0;
};

struct Part { std::string name; double d = 0, w = 0; };   // one compared feature: normalised distance and weight
struct Match { double score = 0; double weight = 0; std::vector<Part> parts; };
struct MatchOptions { double cfoTolHz = 3.0; };   // receiver drift allowance for the carrier offset
Match compare(const Features& a, const Features& b, const MatchOptions& mo = {});

struct Entry { std::string name, note; int64_t created = 0; Features f; };
class Library {
public:
    std::vector<Entry> items;
    void add(const std::string& name, const Features& f, const std::string& note = "");
    bool remove(size_t i);
    // best matches first; only entries of the same kind
    std::vector<std::pair<size_t, Match>> rank(const Features& f, const MatchOptions& mo = {}) const;
    std::string toText() const;
    bool fromText(const std::string& s);
    bool save(const std::string& path) const;
    bool load(const std::string& path);
};
std::string featuresToText(const Features& f);
bool featuresFromText(const std::string& s, Features& f);

// Logs transmissions on one channel from the wideband stream. Feed it from the engine's IQ tap (cheap); call poll() from the UI thread (does the analysis).
struct Event {
    double tStart = 0, durationS = 0;    // seconds since the watcher started
    int64_t wallStart = 0;               // unix seconds
    Features f;
    bool partial = false;                // too short for the fine measurements
};
struct Live { bool carrier = false; double levelDb = -120, snrDb = 0, cfoHz = 0, devHz = 0; };
class Watcher {
public:
    Watcher(double inRateHz, double carrierOffsetHz, Kind kind, double tunedHz, double chanBwHz = 0);
    void feed(const cf32* x, size_t n, int64_t wallNow = 0);
    std::vector<Event> poll();           // finished transmissions since the last poll
    Live live() const { return live_; }
    // the last `sec` seconds of channel samples (for "capture now" on a signal that is simply there)
    std::vector<cf32> recent(double sec) const;
    Features captureNow(double sec = 1.0) const;   // analyse the last seconds + spurs from the wide spectrum
    double rate() const { return ch_.outRate(); }
    double elapsed() const { return t_; }
    void setThresholdDb(double on) { onDb_ = on; }
    double carrierOffsetHz() const { return off_; }
private:
    Channel ch_; WidePsd psd_; Kind kind_; double tuned_, off_, bw_;
    std::vector<cf32> tmp_, ring_, cur_;
    size_t ringPos_ = 0, ringFill_ = 0, blockFill_ = 0;
    double blockPow_ = 0, t_ = 0, noise_ = 0, noiseMin_ = 1e30, noiseWinT_ = 0;
    std::vector<double> noiseHist_;
    bool on_ = false; int onRun_ = 0, offRun_ = 0; double tOn_ = 0; int64_t wallOn_ = 0;
    size_t blockN_;
    double onDb_ = 8;
    cf32 lagAcc_{0, 0}, prev_{0, 0}; bool havePrev_ = false;
    struct Pending { Event ev; std::vector<cf32> z; WidePsd psd; double noise = 0; };
    std::vector<Pending> pending_;
    Live live_;
    void finish(double tEnd);
};

} // namespace dect2::fp
