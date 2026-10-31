// Automatic gain for every radio and every mode (docs/AUTOGAIN.md). One system with four parts, all plain logic with no clock of its own
// (the caller passes the time), so it is tested against a simulated radio (tests/source/autogain_sim.h):
//
//   1. radio models   RadioProfile: the radio's real gain chain (LNA / mixer / VGA / IF / amp / attenuator) with step tables, whether the driver
//                     takes a stage as a reduction (SDRplay IFGR / RFGR), rough noise figure and IIP3 per stage, notes for the user. planStages()
//                     spreads an overall gain over the stages for a policy: Sensitivity (front stages first: lowest noise figure), Linearity
//                     (back stages first: strong signals overload later), Balanced.
//   2. measurement    the ADC (peak, 99.9 % envelope, crest factor, clipped fraction, headroom), the noise floor and the channel's SNR from the
//                     spectrum, the active mode's own lock / SNR / MER / BER, and the overload signatures: an intermodulation product rises
//                     about 3 dB per dB of gain (a real signal 1 dB per dB), and a compressing front end shows less than 1 dB per dB of power.
//   3. mode targets   what each mode wants: broadcast TV judges MER with ADC headroom, narrowband voice the SNR in its channel (not the span),
//                     ADS-B the most sensitivity with overload protection, wide spans headroom.
//   4. control        AutoGainController: Auto (a slow continuous loop with dead band, hysteresis, fast attack / slow decay, a quality guard),
//                     Optimise once (searches gains and stage splits for the mode's best metric, settle times respected), Safe start (starts low
//                     and ramps up until the noise floor rises by X dB). History, a log of changes and plain-language advice for the UI.
//
// The amp and the antenna power (bias-tee) are the user's: the controller never switches them unless the user allowed the amp explicitly
// (AgSettings::ampAllowed, default off); it only suggests "switch the amp on".
#pragma once
#include "dect2/dsp/spectrum.h"
#include "dect2/source/pulse_gain.h"
#include "dect2/source/source.h"
#include "dect2/t2/t2rx.h"
#include <atomic>
#include <complex>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
class Engine;
}

namespace dect2::ag {

// ============================================================================================================================ 1. radio models
enum class StageKind { Lna, Mixer, Vga, If, Amp, Atten, Overall };
const char* stageKindName(StageKind k);

struct GainStage {
    std::string id;               // "lna", "vga", "overall", "rfgr", "amp" ...
    std::string label;
    StageKind kind = StageKind::Overall;
    std::vector<double> steps;    // the gains (dB) the stage can be at, ascending; a reduction stage is listed as gain (max reduction = 0 dB gain)
    bool reduction = false;       // the driver takes it as a gain reduction (SDRplay IFGR / RFGR): setting = steps.back() - gain
    bool settable = true;         // OnAir can set it (false: the driver handles it inside the overall gain; listed for the notes and the model)
    bool userOnly = false;        // amp: only the user switches it (the auto modes need ampAllowed)
    bool front = true;            // before the channel filter: sees every strong signal of the band (intermodulation)
    double nfDb = 3;              // the stage's noise figure at its full gain (typical)
    double nfLowGainDb = 0;       // extra noise figure at its lowest gain (an LNA turned down lets the next stages' noise in)
    double iip3Dbm = 0;           // input IP3 (typical; dBm at the antenna for the first stage)
    std::string settingKey;       // a stage reached through a radio setting (TuneSettings::radio[settingKey]); empty: a gain field of TuneSettings
    bool settingIsIndex = false;  // the setting holds the step index (SDRplay rfgr: state 0 = most gain), not dB
    std::string note;             // what this gain does, for the user
    std::vector<double> actualDb; // measured real gain of each step (same size as steps; empty: the nominal steps are right), from GainCalibration

    double minDb() const { return steps.empty() ? 0 : steps.front(); }
    double maxDb() const { return steps.empty() ? 0 : steps.back(); }
    double snap(double db) const;            // the closest step
    double snapDown(double db) const;        // the highest step <= db (the lowest when none)
    int indexOf(double db) const;            // the index of the closest step
};

struct RadioProfile {
    std::string id;               // "hackrf", "rtl-r820t", "rtl-blog-v4", "sdrplay", ...
    std::string name;
    int adcBits = 8;
    std::vector<GainStage> stages;   // in signal order: antenna first
    bool hasAmp = false, hasBias = false;
    bool driverSplits = false;    // one overall gain for OnAir; the driver spreads it over its stages itself
    bool remote = false;          // a radio over the network (rtl_tcp, SpyServer): each change takes a round trip, settle longer
    double settleSec = 0.25;      // how long a gain change takes to reach the samples (USB buffers, the tuner's own settling)
    double postIip3Dbm = 10;      // IIP3 of what follows the front stages (mixer, filter, ADC driver), at their output: front gain costs linearity
    std::string ampNote, notes;

    int stageIndex(const std::string& id) const;
    int settableCount(bool includeUserOnly = false) const;
    bool singleControl() const { return settableCount() <= 1; }
    double minTotal(bool ampOn) const;
    double maxTotal(bool ampOn) const;
    // The real gain change of going from one overall setting to another: the measured steps of this unit when it was calibrated (GainCalibration),
    // else the difference itself. Only a radio with one settable stage has measured steps.
    double realDelta(double fromTotal, double toTotal) const;
    bool calibrated() const;
};

// Every radio OnAir knows (listed in the advanced popup and docs/AUTOGAIN.md)
const std::vector<RadioProfile>& radioProfiles();
const RadioProfile* radioProfileById(const std::string& id);
// The profile of a listed radio by its board and name (an RTL-SDR Blog V4 by its name, an R828D tuner ...); an unknown radio gets a
// single overall gain over its reported range (DeviceInfo::gainMinDb .. gainMaxDb). The ranges of generic radios follow the driver's.
RadioProfile radioProfileFor(const DeviceInfo& d);
RadioProfile genericProfile(const std::string& name, double minDb, double maxDb, double stepDb);

enum class Policy { Sensitivity, Balanced, Linearity };
const char* policyName(Policy p);

struct StagePlan {
    std::vector<double> db;       // per stage of the profile (gain, dB; the amp 0 or its gain)
    double totalDb = 0;
    double nfDb = 0;              // cascaded noise figure (Friis) of the plan
    double iip3Dbm = 0;           // cascaded input IP3 of the front stages
    bool ampOn = false;
    bool operator==(const StagePlan& o) const { return db == o.db && ampOn == o.ampOn; }
};

// Spreads totalDb over the stages. mask: bit i set = stage i may move (the rest stay at fixed[i], or their lowest step when fixed is short).
// The amp is never switched here: it is on when ampOn (then its gain is part of totalDb). frontCapDb limits the summed gain of the front
// stages (a strong neighbour: the gain is carried after the channel filter instead).
StagePlan planStages(const RadioProfile& r, double totalDb, Policy pol, uint32_t mask, const std::vector<double>& fixed, bool ampOn,
                     double frontCapDb = 1e9);
void cascade(const RadioProfile& r, StagePlan& p);
// All overall gains the stages in mask reach (ascending, unique within 0.05 dB) with that policy
std::vector<double> totalSteps(const RadioProfile& r, Policy pol, uint32_t mask, bool ampOn);
// The settings value of a stage for a plan (dB as text, an index for settingIsIndex, a reduction for reduction stages)
std::string stageSettingValue(const GainStage& s, double gainDb);

// ============================================================================================================================ 2. measurement
struct BlockStats {
    double peak = 0;              // linear, full scale 1
    double p999 = 0;              // the 99.9 % point of |I| and |Q| (a robust peak: one spike does not count)
    double rmsDbfs = -120;
    double crestDb = 0;           // 20 log10(p999) - rms (complex rms: a complex tone reads 0 dB, Gaussian noise about 7.4 dB)
    double clipFraction = 0;      // samples at full scale
    double headroomDb = 120;      // full scale over the robust peak
    double holdHeadroomDb = 120;  // full scale over the 2 s peak-hold (GainMeter; 120: not measured, then headroomDb counts)
    size_t n = 0;
};
BlockStats measureBlock(const std::complex<float>* iq, size_t n, float clipLevel = 0.995f);
BlockStats fromSignalStats(const SignalStats& s);   // the engine's spectrum statistics (peak, rms, clipping, histogram)

struct FloorEstimate {
    bool valid = false;
    double floorDbfs = -200;      // the noise per bin: the median of the quietest half of the bins outside the channel
    double chanDbfs = -200;       // mean per bin inside the channel
    double snrDb = 0;             // the channel over the floor (the SNR the mode sees, not the span's)
    double adcMarginDb = 0;       // the floor over the ADC's own quantisation noise per bin
    double oobDbfs = -200;        // total power outside the channel (strong neighbours)
    double strongestDbfs = -200;  // the strongest bin outside the channel
    double spanDbfs = -200;       // total power of the span
};
FloorEstimate estimateFloor(const std::vector<float>& dbfs, double rateHz, double chanBwHz, double offsetHz, int adcBits);

struct ImdReport {
    bool valid = false;
    int spurs = 0;                // bins that rose much faster than the gain: intermodulation products
    int appeared = 0;             // bins that came out of the floor although the gain rose only a little
    double worstExcessDb = 0;     // the most a bin rose beyond the gain change
    double slopeDbPerDb = 1;      // wideband power change per dB of gain (1: linear; < 0.6: compression)
    bool compressed = false;
    bool overload = false;        // spurs or compression: the front end is overloaded at the higher gain
    std::vector<int> bins;
};
// Two spectra (same size, DC-centred) at gain g and g + dgDb (dg > 0)
ImdReport imdCompare(const std::vector<float>& lo, const std::vector<float>& hi, double dgDb);

struct Quality {
    bool locked = false;
    bool snrValid = false; double snrDb = 0;
    bool merValid = false; double merDb = 0;
    bool berValid = false; double ber = 0;     // pre-FEC bit error ratio
    double fecOk = -1;                         // fraction of FEC blocks decoded (-1 unknown)
    bool pulseValid = false;                   // pulsed message modes (ADS-B): the cumulative counters below are the receiver's
    uint64_t pulseMsgs = 0, pulseLoud = 0;     // good messages and the ones near full scale (see pulse_gain.h)
};
Quality qualityFrom(const RxTelemetry& rx);

// ============================================================================================================================ 3. mode targets
enum class Metric { Auto, Mer, Snr, Ber, Lock, ChannelSnr, Sensitivity, Headroom };
const char* metricName(Metric m);
enum class ModeKind { BroadcastTv, Ofdm, FmBroadcast, NarrowVoice, NarrowData, Pulsed, Gnss, WideSpan, Generic };
const char* modeKindName(ModeKind k);

struct ModeTarget {
    ModeKind kind = ModeKind::Generic;
    Metric metric = Metric::ChannelSnr;   // what Optimise once maximises (the mode's own number while it is locked; else the channel SNR)
    Policy policy = Policy::Balanced;
    double headroomDb = 10;      // the robust peak stays this far below full scale
    double windowDb = 3;         // dead band around it (no change inside)
    double clipLimit = 2e-4;     // clipped fraction that counts as overload
    double marginGoalDb = 8;     // the noise floor wants to be this far above the ADC's own noise
    double floorRiseDb = 6;      // Safe start stops when the floor has risen this much (the antenna's noise is then well above the ADC's)
    double lockSnrDb = 6;        // a decoder locked above this SNR freezes the loop (only overloads still act)
    double maxStepDb = 6;        // largest upward step of the loop
    // Auto v2 (the hill climb on the measured objective): headroom is a floor, not a set-point
    double minHeadroomDb = 4;    // the robust peak (the 2 s peak-hold for pulsed modes) stays at least this far under full scale
    double improveDb = 0.8;      // a step must buy at least this much of the objective (and 1.5 sigma of its noise) to be kept
    double comfortDb = 6;        // a decoder this far over its lock threshold lets the loop rest; under it the loop keeps optimising
    bool usePeakHold = false;    // judge the headroom from the peak-hold (pulses), not the 99.99 % point
    const char* why = "";
    const char* bandNote = "";   // what the band the radio is tuned to changes (set by modeTargetFor when freqHz is given), for the user
    const char* bandName = "";
};
// freqHz > 0 adds the band's bias: HF and the strong broadcast / mobile bands want headroom and a linear front end, quiet microwave bands
// want sensitivity (bandBiasNote in the target says so in one line)
ModeTarget modeTargetFor(int stdMode, double chanBwHz, double rateHz, double freqHz = 0);
// A short id of the band a frequency is in ("hf", "fm", "tv-uhf", "cell", "microwave" ...): gain memory and the app re-optimise when it changes
const char* bandIdFor(double freqHz);

// ============================================================================================================================ 4. control
enum class AgMode { Off, Auto, Once, Safe };
const char* agModeName(AgMode m);
enum class Phase { Idle, Settling, Tracking, Frozen, Sweeping, Refining, Ramping, Done, Failed };
const char* phaseName(Phase p);

struct AgSettings {
    AgMode mode = AgMode::Off;
    double headroomDb = -1;      // < 0: the mode's own target
    double maxGainDb = 1e9;      // the highest overall gain the auto modes may use
    double stepDb = 0;           // 0: automatic (the radio's steps, 3 dB for Safe start)
    double loopSpeed = 1;        // 0.25 (slow, calm) .. 4 (fast): scales the hold and settle times
    uint32_t stageMask = 0xffffffffu;   // which stages may be adjusted (bit = stage index of the profile)
    Metric metric = Metric::Auto;
    bool ampAllowed = false;     // the user's explicit permission to switch the amp (default never)
    double safeRiseDb = -1;      // < 0: the mode's floorRiseDb
    bool imdProbeAuto = false;   // Auto also checks for intermodulation now and then (a short 3 dB step up and back)
    double settleSec = 0.8;      // extra wait after each change before measuring (on top of the radio's own)
    double measureSec = 1.2;     // Optimise once / Safe: how long each step is measured
    bool quickStart = true;      // Auto begins (and re-begins after a retune / Re-optimise) with a short bracketing search of 4-6 gains
    double bracketSec = 0.15;    // how long each gain of that search is measured (after the radio's own settling)
    bool v2 = true;              // Auto v2: climb the measured objective (channel SNR / the decoder's MER) under constraints; false: the old headroom servo
    double dwellSec = 1.0;       // v2: how long each gain is measured before a decision (after the settling)
};

struct AgInput {
    double now = 0;
    BlockStats adc;
    const std::vector<float>* spectrum = nullptr;   // DC-centred dBFS bins over rateHz
    double rateHz = 0, chanBwHz = 0, offsetHz = 0;   // the channel the mode listens to
    Quality q;
};

struct AgCommand {
    StagePlan plan;              // apply this (plan.totalDb, plan.db per stage, plan.ampOn)
    std::string why;
    std::map<std::string, std::string> settings;   // radio settings to set with it (TuneSettings::radio[key]); empty for most
};

struct AgEvent { double t = 0; double fromDb = 0, toDb = 0; std::string why; };
struct AgPoint { double t = 0, gainDb = 0, headroomDb = 0, floorDbfs = -200, metric = 0; bool metricValid = false; };
enum class Advice { None, Overload, StrongNeighbour, Intermod, AtMax, EnableAmp, CheckAntenna, AtMin, Compressed };
struct AgAdvice { Advice kind = Advice::None; std::string text; };

struct AgMeters {
    bool valid = false;
    double headroomDb = 0, peakDbfs = -120, rmsDbfs = -120, crestDb = 0, clipFraction = 0;
    bool floorValid = false; double floorDbfs = -200, adcMarginDb = 0, chanSnrDb = 0, oobDbfs = -200;
    bool clipping = false, overload = false, imd = false, compressed = false;
    double holdHeadroomDb = 120, slopeDbPerDb = 1, objective = 0;
    bool objectiveValid = false;
    int overloads = 0;
};

// One step of Optimise once / Safe start, as measured
struct AgTrial {
    StagePlan plan;
    Policy pol = Policy::Balanced;
    int n = 0, nMetric = 0, nLocked = 0;
    double metricSum = 0, headroomMin = 120, clipMax = 0, floorSum = 0, snrSum = 0; int nFloor = 0, nSnr = 0;
    std::vector<float> spec;     // a mean spectrum (for the intermodulation comparison)
    bool imd = false, done = false;
    double metric() const { return nMetric ? metricSum / nMetric : -1e9; }
    double floor() const { return nFloor ? floorSum / nFloor : -200; }
    double chanSnr() const { return nSnr ? snrSum / nSnr : -100; }
};

class AutoGainController {
public:
    void configure(const RadioProfile& r, const ModeTarget& t, const AgSettings& s);
    void setSettings(const AgSettings& s);       // live changes from the advanced popup (keeps the state)
    const AgSettings& settings() const { return set_; }
    const RadioProfile& radio() const { return radio_; }
    const ModeTarget& target() const { return tgt_; }
    // Starts the chosen mode from the radio's present setting (current = its stages; null: planned from currentTotalDb)
    void start(double now, double currentTotalDb, bool ampOn, const StagePlan* current = nullptr);
    void stop();
    // Feed the newest measurement; true when `out` must be applied to the radio
    bool tick(const AgInput& in, AgCommand& out);

    bool active() const { return mode_ != AgMode::Off && phase_ != Phase::Idle && phase_ != Phase::Done && phase_ != Phase::Failed; }
    AgMode mode() const { return mode_; }
    Phase phase() const { return phase_; }
    double progress() const;                     // 0..1 for Once / Safe
    const StagePlan& current() const { return cur_; }
    const StagePlan& startPlan() const { return startPlan_; }
    double headroomTarget() const;
    const AgMeters& meters() const { return met_; }
    const std::deque<AgPoint>& history() const { return hist_; }
    const std::deque<AgEvent>& log() const { return log_; }
    std::vector<AgAdvice> advice() const;
    const std::vector<AgTrial>& trials() const { return trials_; }
    std::string resultText() const { return result_; }
    Metric usedMetric() const { return used_; }
    double bestMetric() const { return bestMetric_; }
    int changes() const { return changes_; }
    double ceilingDb(double now) const { return now < ceilUntil_ ? ceil_ : 1e9; }
    // Auto: run the short bracketing search again from the next tick ("Re-optimise")
    void reoptimise() { if (mode_ == AgMode::Auto && phase_ != Phase::Idle) wantBracket_ = true; }
    // Auto: a gain taken from the memory needs no search at start
    void skipQuickSearch() { wantBracket_ = false; }
    // v2: a remembered gain is a PRIOR: the quick search runs again, narrowed to about +/-6 dB around it (never a skip: another antenna, place, hour)
    void setPrior(const StagePlan& p) { prior_ = p; havePrior_ = p.totalDb > -1e8; wantBracket_ = mode_ == AgMode::Auto && set_.quickStart; }
    // v2: the tuned frequency / mode / rate changed a lot (retune > 5 % of the rate, another band, a new sample rate): search again from the present gain
    void notifyRetune(const char* what) { if (mode_ == AgMode::Auto && phase_ != Phase::Idle) { wantBracket_ = true; settled_ = false; clTrial_ = false; retuneWhy_ = what ? what : "retune"; } }
    // v2: the one line for the user: what the gain is, what limits it and what the loop is doing now
    const std::string& status() const { return status_; }
    bool probing() const { return clTrial_; }          // a neighbouring gain is being tried right now
    double unitScale() const { return unitScale_; }
    // the gain/objective trace: one CSV line per call of takeTrace (t, gain, objective, headroom, floor, clip, state), for --autogain-trace and bug reports
    static const char* traceHeader() { return "t,gain_db,objective,snr_db,headroom_db,floor_dbfs,clip,state,why"; }
    std::string traceLine(double now) const;
    bool bracketing() const { return br_ != 0; }
    // true once after each finished bracketing search (the app stores the result in the memory)
    bool takeBracketDone() { const bool b = bracketDone_; bracketDone_ = false; return b; }
    // One line for the user: "Gain set to 32 dB: strong FM stations nearby, kept headroom" and the reasons behind it
    const std::string& explanation() const { return expl_; }
    const std::vector<std::string>& reasons() const { return reasons_; }

    // The score a measurement gets for the metric (higher is better); false when it has none
    static bool score(Metric m, const Quality& q, const FloorEstimate& fl, const BlockStats& adc, const ModeTarget& t, double headroomTarget, double& out);

private:
    double maxGain() const;
    double bracketHead() const;
    double minGain() const;
    double settleTime() const;
    void move(double now, double totalDb, Policy pol, const std::string& why, AgCommand& out);
    void logEvent(double now, double from, double to, const std::string& why);
    void measure(const AgInput& in);
    bool tickAuto(const AgInput& in, AgCommand& out);
    bool tickBracket(const AgInput& in, AgCommand& out);
    void beginBracket(const AgInput& in);
    void explain(const std::string& lead);
    bool tickOnce(const AgInput& in, AgCommand& out);
    bool tickSafe(const AgInput& in, AgCommand& out);
    bool beginTrial(double now, double totalDb, Policy pol, AgCommand& out);
    void accumulate(AgTrial& t, const AgInput& in);
    int chooseTrial(Metric& used, double minHead = 0.5, double maxMargin = 1e9) const;
    void finish(double now, int idx, Metric used, AgCommand& out, bool& changed);
    Metric resolvedMetric(const Quality& q) const;
    std::vector<double> stepsFor(Policy pol) const;
    double nextStep(double from, double deltaDb, Policy pol) const;

    RadioProfile radio_;
    ModeTarget tgt_;
    AgSettings set_;
    AgMode mode_ = AgMode::Off;
    Phase phase_ = Phase::Idle;
    StagePlan cur_, startPlan_;
    Policy pol_ = Policy::Balanced;
    double changedAt_ = -1e9, measuredFrom_ = -1e9, lastUp_ = -1e9, lastDown_ = -1e9, startedAt_ = 0;
    int dir_ = 0; double dirAt_ = -1e9;
    // smoothed measurement since the last settle
    double rmsAvg_ = -120, headAvg_ = 120, crestAvg_ = 8; int nAvg_ = 0;
    FloorEstimate fl_;
    Quality q_;
    AgMeters met_;
    // overload memory
    double ceil_ = 1e9, ceilUntil_ = 0, linearUntil_ = 0, preOvDb_ = 0;
    std::vector<double> ovTimes_;
    // quality guard of an upward step
    bool guard_ = false; double guardFrom_ = 0, guardMetric_ = 0; Metric guardM_ = Metric::Auto;
    // IMD probe (Auto)
    int probe_ = 0; double probeAt_ = -1e9, probeFrom_ = 0; std::vector<float> probeLo_;
    // the short bracketing search of Auto: br_ 1 = running; wantBracket_ = start it at the next settled tick
    int br_ = 0, brExtra_ = 0; bool wantBracket_ = false, bracketDone_ = false; double brFrom_ = 0;
    std::string expl_;
    std::vector<std::string> reasons_;
    // Auto v2
    struct Run { double n = 0, mean = 0, m2 = 0; void add(double x) { n += 1; const double d = x - mean; mean += d / n; m2 += d * (x - mean); } double var() const { return n > 1 ? m2 / (n - 1) : 1.0; } };
    bool tickClimb(const AgInput& in, AgCommand& out);
    bool objective(double& j) const;
    bool comfortable() const;
    double effHeadroom(const AgInput& in) const;
    void climbSample(const AgInput& in, double j);
    void climbReset(double now);
    bool startTrial(const AgInput& in, AgCommand& out);
    bool evaluateTrial(const AgInput& in, AgCommand& out);
    void setStatus(const std::string& lead);
    StagePlan prior_; bool havePrior_ = false;
    bool clTrial_ = false, settled_ = false;
    int cand_ = 0, candKind_ = 0;            // next candidate in the cycle; the running one (1 up, 2 down, 3 split)
    double trialAt_ = 0, settledAt_ = -1e9, settledJ_ = 0, clStep_ = 2, lockLostAt_ = -1;
    bool wasLocked_ = false;
    Run base_, tri_, mBase_, mTri_;   // the objective and the noise floor over the ADC's own noise, at the baseline and at the trial
    StagePlan basePlan_; Policy basePol_ = Policy::Balanced;
    std::vector<double> specPow_; int specCnt_ = 0;     // mean power spectrum of this dwell (linear)
    std::vector<float> specBase_;                        // ... of the baseline, dB
    double clMark_ = -1, dwellFrom_ = 0, statusAt_ = -1e9;
    int clDir_ = 1;
    BlockStats adcLast_;
    double pjMark_ = -1;
    PulseJudge pj_;   // pulsed modes: messages per second and loud share at the present gain
    double floorBase_ = -200;
    std::string status_, limit_, lastDecision_, retuneWhy_;
    double slope_ = 1, unitScale_ = 1;     // unitScale_: the real dB per nominal dB of this unit, learned from the strongest line's rise on each step
    int rejectRun_ = 0, rejUp_ = 0, idleCycles_ = 0;
    double goodGain_ = -1e9;     // the gain the loop last settled at without clipping (a burst is recovered to this, not to the gain that clipped)
    double stepReal(double from, double to) const;
    double jRecent_ = 0; int jRecentN_ = 0;
    // Optimise once / Safe
    std::vector<AgTrial> trials_;
    std::vector<std::pair<double, Policy>> queue_;
    int ti_ = -1;
    double safeBase_ = -200;
    bool refined_ = false;
    std::string result_;
    Metric used_ = Metric::Auto;
    double bestMetric_ = 0;
    int changes_ = 0;
    bool imdSeen_ = false, compSeen_ = false;
    std::deque<AgPoint> hist_;
    std::deque<AgEvent> log_;
    double histAt_ = -1e9;
};

// ============================================================================================================================ memory
// The best gain per radio, per mode and per band, kept by the app in its settings (serialise / parse)
struct GainMemoryEntry { StagePlan plan; double metric = 0; std::string metricName; int64_t when = 0; };
class GainMemory {
public:
    static std::string key(const std::string& radioKey, int stdMode, double freqMhz);   // "radio|mode|band"
    void put(const std::string& key, const GainMemoryEntry& e);
    // exact key, else the same radio and mode on any band (the closest band); null when none
    const GainMemoryEntry* find(const std::string& key) const;
    void forget(const std::string& key);
    void clear() { m_.clear(); }
    const std::map<std::string, GainMemoryEntry>& entries() const { return m_; }
    std::string serialise() const;
    bool parse(const std::string& s);
private:
    std::map<std::string, GainMemoryEntry> m_;
};

// ============================================================================================================================ 5. integration
// What a caller drives the controller with when it owns the radio (the scanner, the first-run wizard's worker, the command line)
struct AgRadioIo {
    virtual ~AgRadioIo() = default;
    virtual double now() = 0;
    virtual void sleep(double sec) = 0;
    virtual bool apply(const AgCommand& c) = 0;
    virtual bool measure(AgInput& in) = 0;      // false: no new measurement yet
    virtual bool cancelled() { return false; }
};
struct AgRunResult {
    bool ok = false;
    StagePlan plan;
    Metric metric = Metric::Auto;
    double metricValue = 0;
    std::string text;
    std::vector<AgEvent> log;
};
// Runs Optimise once or Safe start to the end (or timeoutSec) and leaves the radio at the result
AgRunResult autogainRun(AgRadioIo& io, const RadioProfile& r, const ModeTarget& t, AgSettings s, double startDb, bool ampOn, double timeoutSec = 90);

// The engine adapter: a running Engine and the TuneSettings it runs with (the HackRF's stages, a generic radio's overall gain, settings-key stages)
class EngineAgIo : public AgRadioIo {
public:
    EngineAgIo(Engine& e, TuneSettings& tune, const RadioProfile& r, double chanBwHz, double offsetHz = 0);
    double now() override;
    void sleep(double sec) override;
    bool apply(const AgCommand& c) override;
    bool measure(AgInput& in) override;
    bool cancelled() override { return cancel_ && cancel_->load(); }
    void setCancelFlag(const std::atomic<bool>* c) { cancel_ = c; }
private:
    Engine& e_;
    TuneSettings& tune_;
    RadioProfile r_;
    double bw_, off_;
    uint64_t specSeq_ = 0, rxSeq_ = 0;
    std::vector<float> spec_;
    RxTelemetry rx_;
    bool haveRx_ = false;
    const std::atomic<bool>* cancel_ = nullptr;
};
// Writes a plan into TuneSettings (lnaDb / vgaDb / ampOn for the HackRF, gainDb for one overall gain, radio[settingKey] for setting stages)
void applyPlan(const RadioProfile& r, const StagePlan& p, TuneSettings& tune);
// The plan a TuneSettings is at now
StagePlan planFromTune(const RadioProfile& r, const TuneSettings& tune);
// The engine API: optimise the running engine's gain for this radio and mode (blocking; the scanner, the wizard's worker thread)
AgRunResult autogainRun(Engine& e, TuneSettings& tune, const DeviceInfo& radio, int stdMode, AgMode how = AgMode::Once, AgSettings s = AgSettings(),
                        double timeoutSec = 60, const std::atomic<bool>* cancel = nullptr);
// Read-only status for the Web server / remote API
std::string autogainJson(const AutoGainController& c, double now);

} // namespace dect2::ag
