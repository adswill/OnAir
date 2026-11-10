// Satellite signals that have no public specification (or whose payload we do not decode): find them, measure them and say whose they are,
// without inventing a decoder. Three methods, chosen per system by the table in satdetect_systems:
//   Carrier: narrow lines followed through time, each matched against the catalogue's channels and, when elements are given, against satellite Doppler curves.
//   Channel: energy per channel of a known band plan: which channels are on, how wide, how often.
//   Frame:   a wideband signal's repetition: Starlink's 750 Hz frame gap (line of the power envelope), OneWeb's 1 ms repeating sequence (delayed correlation).
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/geo/passes.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SatDetectTelemetry;

namespace satmisc {

enum class SdMethod { Carrier, Channel, FrameGap, RepeatSeq };

struct SdChannelDef { double mhz; const char* name; };

struct SatSystem {
    int stdMode;                 // the engine standard
    const char* id;
    const char* name;
    double defMhz, minMhz, maxMhz;
    double sampleRate;
    SdMethod method;
    double binHz;                // resolution wanted
    double thresholdDb;
    double frameHz;              // FrameGap and RepeatSeq: the rate looked for
    double channelHz;            // Channel: the grid
    const SdChannelDef* channels;
    int numChannels;
    const char* note;            // what is known and from where, shown in the mode and the docs
    bool tleMatch;               // the carriers can be matched against satellites by Doppler
};

const std::vector<SatSystem>& satSystems();
const SatSystem* satSystem(int stdMode);
// Space station mode: the next band to look at (VHF 145.8125 covers 145.800 and 145.825 together, UHF 437.8 the repeater)
double issNextCentreMhz(double currentMhz);
inline bool isSatDetectStd(int stdMode) { return satSystem(stdMode) != nullptr; }

} // namespace satmisc

class SatDetectReceiver {
public:
    SatDetectReceiver();
    ~SatDetectReceiver();
    void setSystem(int stdMode);        // before configure()
    void configure(double inputRateHz);
    void setSignalOffset(double hz);    // not used: the whole capture is searched
    void setCenterHz(double hz);        // the radio's centre
    void setStartUtc(int64_t utcSec);   // tests: the UTC second of the first sample (default: now)
    bool ready() const;
    void reset();
    void clearResults();
    void setThresholdDb(double db);     // 0: the system's own
    void setSatellites(const std::vector<Tle>& tles, double latDeg, double lonDeg, double altKm);
    void feed(const cf32* x, size_t n);
    bool telemetry(SatDetectTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning satDetectTuning(int stdMode);
// The test signal of a system: a carrier with a satellite-like Doppler, bursts on channels, or the framed signals. modeOpt[0]: 0 signal, 1 noise only.
std::unique_ptr<ModeSynth> makeSatDetectSynth(int stdMode, const SynthConfig& cfg, double sampleRate);

} // namespace dect2
