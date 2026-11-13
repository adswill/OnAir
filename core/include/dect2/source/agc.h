// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The auto-gain system ("AGC"): keeps a radio's front end at the best operating point for the active mode. See docs/auto-gain.md.
//
// Three parts, all plain logic with no clock of their own (the caller passes the time) so they are tested against a simulated front end
// (tests/source/agc_fake.h):
//   GainModel   what the radio's gain stages really are: the HackRF's LNA / VGA / amp, the RTL-SDR's tuner gain table, a plain range, the
//               AirspyHF+'s 6 dB steps. It turns one overall gain into the stages (LNA before VGA, the amp only when the user allows it).
//   AgcProfile  what the mode wants: a wide OFDM channel wants ADC headroom, a narrow weak-signal mode wants the noise well above the ADC's
//               own with strong neighbours kept out, a pulsed mode is judged on its peaks, broadcast FM is strong.
//   SmartAgc    the control loop: fast attack on overload, slow release, hysteresis, rate limits, frozen while a decoder is locked on good
//               data, re-optimised on a retune. It measures the ADC (peak, clipping, rms), the noise floor against the ADC's quantisation noise
//               (the station-health model, core/src/sweep/station_health.cpp), the power outside the channel (blockers) and the DC spike.
#pragma once
#include "dect2/dsp/spectrum.h"
#include "dect2/source/gain.h"
#include "dect2/source/source.h"
#include <string>
#include <vector>

namespace dect2 {

// ------------------------------------------------------------------------------------------------------------------------------ gain stages
struct GainModel {
    enum Kind { HackRf, Table, Range } kind = Range;
    int minDb = 0, maxDb = 0;     // the radio's overall gain scale in whole dB (HackRF: without the amp)
    int stepDb = 1;               // Range: the smallest change
    std::vector<int> table;       // Table: the gains the radio can be at, ascending (RTL-SDR tuner)
    int adcBits = 8;
    // A switchable amplifier. HackRF: the +14 dB amp (TuneSettings::ampOn). AirspyHF+: the +6 dB preamp that the driver's "auto" setting
    // switches on in the top 6 dB of the slider (ampTopDb): the AGC keeps below that unless it may use the amp.
    bool ampSwitch = false;
    int ampDb = 0;
    bool ampIsTopOfScale = false; // true: the amp belongs to the top ampDb of the overall scale (AirspyHF+), false: it is a stage of its own (HackRF)
    std::string ampSettingKey;    // AirspyHF+: "preamp" (the radio setting the amp is behind)
    // AirspyHF+: the output level does not follow the gain (the firmware compensates the preamp and sets its own gain distribution). The level
    // cannot say "more gain"; the loop keeps the least attenuation the radio allows and backs off only on clipping (see setAdcLevelCompensated)
    bool levelCompensated = false;

    // the gains reachable without the amp / the highest total with the amp fixed as `ampOn` and the LNA limited to lnaCap
    int maxTotal(bool ampOn, bool ampAllowed, int lnaCap) const;
    int minTotal(bool ampOn) const;
    // the achievable gain closest to `db` (clamped to the range)
    int snap(int db, int lo, int hi) const;
    // one achievable gain from `cur` in the direction of deltaDb (at least one step when |deltaDb| >= half a step), within [lo, hi]
    int nextToward(int cur, double deltaDb, int lo, int hi) const;
    // HackRF: the stages for an overall gain. total includes the amp's dB when ampOn. LNA first (best noise figure), the VGA takes the rest,
    // the LNA limited to lnaCap (a strong neighbour is handled by the VGA after the filter instead).
    GainSetting split(int totalDb, bool ampOn, int lnaCap) const;
};
// The gain model of a listed radio (by its board; unknown radios get a plain range from DeviceInfo::gainMinDb .. gainMaxDb)
GainModel gainModelFor(const DeviceInfo& d, int adcBits = 8);
// The R820T tuner's gain table (whole dB, ascending); the E4000 and others differ and are read from the driver in the radio, this is the default
const std::vector<int>& rtlTunerGainTable();

// ------------------------------------------------------------------------------------------------------------------------------ mode profile
enum class AgcClass { Wideband, Narrow, Pulsed, Strong };
struct AgcProfile {
    AgcClass cls = AgcClass::Wideband;
    const char* name = "wideband";
    double targetRms = -16, window = 4;  // the rms level (dBFS) aimed for, and the dead band around it (no change inside)
    double maxPeak = 0.8;                // linear: the peaks stay below this
    double marginGoal = 10;              // the noise floor wants to be this far above the ADC's quantisation noise (dB)
    double oobCeil = -8;                 // the power outside the channel stays below this (dBFS over the whole rate): strong neighbours
    int lnaCap = 40;                     // HackRF LNA limit when nothing disturbs
    double lnaOutCeil = -35;             // what the out-of-band power reaching the end of the LNA may be (dBFS-equivalent: ADC level less the VGA gain)
    double lockSnrDb = 6;                // a decoder locked with at least this SNR (or with no SNR figure) freezes the gain
};
// The profile for an engine standard (ModeTuning::stdMode; 0..7 are the TV / radio families), the channel width (Hz) and the radio's rate (Hz)
AgcProfile agcProfileFor(int stdMode, double chanBwHz, double rateHz, int adcBits);

// ------------------------------------------------------------------------------------------------------------------------------ measurement
struct AgcMeasure {
    double rmsDbfs = -120;
    float peak = 0, clipFraction = 0;
    bool haveSpec = false;        // the fields below come from a spectrum
    bool haveFloor = false;       // floorMarginDb is valid (the capture is wider than the channel)
    double floorMarginDb = 0;     // noise floor over the ADC's quantisation noise (adcMarginDb)
    double inbandDbfs = -200;     // power inside the channel (over the whole rate)
    double oobDbfs = -200;        // power outside the channel and its skirts: strong neighbours (blockers)
    double blockerBinDb = -200;   // the strongest bin outside it
    double dcSpurDb = 0;          // the DC bin over its surroundings
};
// dbfs: the spectrum frame (DC-centred, one value per FFT bin over rateHz), the channel chanBwHz wide centreOffsetHz off the middle
AgcMeasure measureFront(const SignalStats& st, const std::vector<float>& dbfs, double rateHz, double chanBwHz, double centreOffsetHz, int adcBits);

struct AgcQuality {
    bool locked = false;          // a decoder is locked on a frame / burst / carrier
    bool snrValid = false;
    double snrDb = 0;             // its own SNR / MER / Es/N0
};

// ------------------------------------------------------------------------------------------------------------------------------ control loop
enum class AgcReason { None, Settling, Hold, Overload, TooHot, TooLow, Margin, Wasted, Blocker, Locked, Reverted, Recovering, LimitHit,
                       TopGain };   // TopGain: a level-compensated radio (AirspyHF+) kept at the least attenuation allowed
const char* agcReasonName(AgcReason r);

struct AgcCommand {
    int totalDb = 0;              // the overall gain to set (HackRF: LNA + VGA + amp's dB)
    bool ampOn = false;           // the amp state to set (the user's own one unless AGC may switch it)
    int lnaCap = 40;
    GainSetting stages;           // HackRF: the stages for totalDb / ampOn / lnaCap
    AgcReason reason = AgcReason::None;
};

class SmartAgc {
public:
    SmartAgc() = default;
    void configure(const GainModel& m, const AgcProfile& p);
    // The AGC may switch the amp by itself only when this is true (default false; the user's own switch, saved per radio).
    void setAmpAllowed(bool allowed) { ampAllowed_ = allowed; }
    bool ampAllowed() const { return ampAllowed_; }
    // Starts (or restarts: AGC switched on, a retune, another mode) from the radio's present gain: a quick acquisition follows.
    void begin(double now, int currentTotalDb, bool ampOn);
    bool begun() const { return begun_; }
    // Feed the newest measurement; returns true when `out` must be applied to the radio.
    bool update(double now, const AgcMeasure& m, const AgcQuality& q, AgcCommand& out);

    int totalDb() const { return cur_; }
    bool ampOn() const { return amp_; }
    int lnaCap() const { return lnaCap_; }
    AgcReason reason() const { return reason_; }
    const AgcProfile& profile() const { return prof_; }
    const GainModel& model() const { return model_; }
    double rmsDbfs() const { return rms_; }
    int overloads() const { return overloads_; }
    double ceilingDb(double now) const { return now < ceilUntil_ ? ceil_ : -1; }

private:
    void apply(double now, int total, AgcReason why, AgcCommand& out);
    int effMax() const;
    int effMin() const;
    GainModel model_;
    AgcProfile prof_;
    bool begun_ = false, ampAllowed_ = false, amp_ = false;
    int cur_ = 0, lnaCap_ = 40, steadyDb_ = -1;
    bool snapPending_ = false;   // begin() snapped the radio's gain into the allowed range; not yet sent (level-compensated radios)
    double last_ = 0, changedAt_ = -1e9, moveAt_ = -1e9, acquireUntil_ = 0;
    double rms_ = -120, peak_ = 0, clip_ = 0, oob_ = -200, margin_ = 0, snr_ = 0;
    bool haveFloor_ = false, snrValid_ = false, init_ = false;
    int dir_ = 0;
    // recovery from a strong burst
    double lastOverload_ = -1e9, ceilUntil_ = 0, ceil_ = 0, ampBanUntil_ = 0, capChangedAt_ = -1e9;
    int preAttack_ = -1, overloads_ = 0;
    std::vector<double> ovTimes_;
    // quality check of an upward step
    bool pending_ = false;
    int upFrom_ = 0;
    double snrBefore_ = 0, pendingAt_ = 0;
    AgcReason reason_ = AgcReason::None;
};

} // namespace dect2
