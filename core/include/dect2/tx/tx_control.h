// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Full control over transmitting (3.8, docs/TX-CONTROL-3.8.md): the per-radio TX calibration, the IQ correction of the output stage, the schedule,
// and the plan of what each radio driver is asked for (frequency, rate, filter, gain stages, amplifier) worked out from a TxTune without touching
// the radio. Every driver's transmit sink calls its plan function and writes the values with its own library, so the tests check the real mapping
// with a fake radio that records the calls (TxHwOps). Nothing here limits a frequency, a gain or a power: values are clamped only to the scale the
// radio itself reports, and a warning is a note, never a refusal.
#pragma once
#include "dect2/source/source.h"
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct TxRequest;

// ---- calibration (per radio, saved by the app under the radio's claim key) -------------------------------------------------------------------
struct TxPowerPoint {
    double gainDb = 0;   // the TX gain setting (DeviceInfo::txGainMinDb .. txGainMaxDb)
    bool amp = false;    // the radio's amplifier (HackRF +14 dB) was on
    double dbm = 0;      // the output power measured at that setting (a power meter, a calibrated receiver, ...)
};
struct TxCalibration {
    double ppm = 0;                    // the transmitter's clock error in parts per million (positive: it runs fast); the frequency asked of the radio is divided by 1 + ppm * 1e-6
    double offsetHz = 0;               // a fixed correction added to the frequency the radio is told (a radio that radiates a set amount off)
    double dcI = 0, dcQ = 0;           // DC added to I and Q (full scale 1.0) to cancel the carrier leak at the centre
    double iqGainDb = 0;               // I/Q amplitude balance: I is scaled by +g/2 dB and Q by -g/2 dB
    double iqPhaseDeg = 0;             // I/Q phase balance: Q gets sin(phase) of I mixed in
    std::vector<TxPowerPoint> power;   // measured output power at some gain settings (any number, any order)
    bool empty() const { return ppm == 0 && offsetHz == 0 && dcI == 0 && dcQ == 0 && iqGainDb == 0 && iqPhaseDeg == 0 && power.empty(); }
};
// "ppm=..;hz=..;dci=..;dcq=..;iqg=..;iqp=..;pw=gain:amp:dbm/gain:amp:dbm" and back (unknown keys are skipped, a bad number is 0)
std::string txCalToString(const TxCalibration& c);
TxCalibration txCalFromString(const std::string& s);
// The output power the table predicts at a gain (dBm): linear in dB between the two nearest measured points of the same amplifier setting, beyond
// them along the slope of the last two (1 dB per dB with one point). NaN when there is no point for that amplifier setting.
double txCalPowerDbm(const TxCalibration& c, double gainDb, bool amp);
// The gain the table says gives `dbm` (the inverse of txCalPowerDbm). NaN when there is no point. Not clamped: the caller shows the radio's range.
double txCalGainForDbm(const TxCalibration& c, double dbm, bool amp);
// The calibration into a request: tune.radio["ppm"] (unless the request already has one), tune.offsetHz += offsetHz, and the output stage's
// out.dcI / out.dcQ / out.iqGainDb / out.iqPhaseDeg (unless the request already sets them). An empty calibration changes nothing.
void txApplyCalibration(const TxCalibration& c, TxRequest& r);

// ---- IQ correction (the output stage, every radio, keys out.iqGainDb / out.iqPhaseDeg / out.dcI / out.dcQ) --------------------------------
// I' = a I, Q' = b (Q cos p + I sin p) + dc, with a = 10^(g/40), b = 10^(-g/40). All zero: the samples are not touched (bit-identical).
void txIqCorrect(cf32* x, size_t n, double gainDb, double phaseDeg, double dcI, double dcQ);

// ---- timeout and schedule ------------------------------------------------------------------------------------------------------------------
// The timeout timer (key tx.totSec of TxRequest::opt, 0 = off): the session ends by itself, with the key-down fade, after that many seconds,
// whatever "Repeat until stopped" says. It is the operator's own limit; OnAir sets none.
double txTimeoutSec(const TxRequest& r);
// A transmission started later and / or again: first at startUtc (0 = when Start is pressed), then every everySec seconds (0 = once), count times
// in all (0 = until the schedule is cancelled). Times are seconds since 1970 (UTC).
struct TxSchedule {
    double startUtc = 0;
    double everySec = 0;
    int count = 0;
    bool active() const { return startUtc > 0 || everySec > 0; }
};
// The next start: firstly startUtc (or now when it is 0 or already past), then lastStartUtc + everySec. -1 when the schedule is complete
// (done == count, or a one-off schedule that has run).
double txScheduleNext(const TxSchedule& s, double nowUtc, double lastStartUtc, int done);
// "HH:MM" or "HH:MM:SS" (UTC) to the next such time at or after nowUtc; -1 when the text is not a time
double txParseUtcTime(const std::string& hhmmss, double nowUtc);

// ---- what each radio's transmit path is asked for -------------------------------------------------------------------------------------------
struct TxHwRange {
    double lo = 0, hi = 0;
    bool known() const { return hi > lo; }
    double clamp(double v) const { return known() ? (v < lo ? lo : v > hi ? hi : v) : v; }
};
struct TxHwPlan {
    double rateHz = 0;                                   // complex samples per second asked for
    uint32_t rateNum = 0, rateDiv = 1;                   // HackRF: the rate as rateNum / rateDiv (whole numbers)
    double filterHz = 0;                                 // the analogue filter
    double loHz = 0;                                     // the frequency written to the radio: dial + offset, ppm and the radio's own crystal correction applied
    std::vector<std::pair<std::string, double>> gains;   // the gain stages in the order written ("" = the driver's overall gain)
    int ampOn = -1;                                      // -1: the radio has no amplifier switch
    int channel = 0;
    std::string antenna;                                 // "" = the driver's default
    std::string err;                                     // the plan cannot be sent (the radio's own format cannot hold the value): the driver's plain-words text
    std::string note;                                    // a warning for the log (never a refusal)
};
// The calls a radio's transmit path takes. The real drivers implement them with their library; the tests with a fake that records them.
class TxHwOps {
public:
    virtual ~TxHwOps() = default;
    virtual bool setRate(const TxHwPlan& p, std::string& err) = 0;
    virtual bool setFilter(double hz, std::string& err) = 0;
    virtual bool setFrequency(double hz, std::string& err) = 0;
    virtual bool setGain(const std::string& stage, double db, std::string& err) = 0;
    virtual bool setAmp(bool on, std::string& err) { (void)on; (void)err; return true; }
};
// Writes a plan in the drivers' order: rate and filter (unless withRate is false: a live retune), frequency, the gain stages, the amplifier.
bool txHwApply(const TxHwPlan& p, TxHwOps& ops, bool withRate, std::string& err);

// The per-stage gains the operator typed for a SoapySDR radio (tune.radio keys "txgain.<stage>", e.g. txgain.PAD = 40); written after the overall gain.
std::vector<std::pair<std::string, double>> txStageGains(const std::map<std::string, std::string>& radio);
// "PAD=40, IAMP=-3" into txgain.* keys of a radio map (the old txgain.* keys are removed); false when an item is not name=number
bool txParseStageGains(const std::string& text, std::map<std::string, std::string>& radio);
std::string txStageGainsText(const std::map<std::string, std::string>& radio);

// HackRF (OnAir's USB driver): rate as a fraction, MAX2837 filter (auto from the rate), TX VGA 0..47 dB, the +14 dB amplifier
TxHwPlan hackrfTxPlan(const TxTune& t);
// PlutoSDR / AD936x (libiio): runRateHz is what the radio runs at (in full duplex the receiver's rate); gain is the hardwaregain (attenuation as a
// negative gain); xoPpm the crystal correction the radio itself has
TxHwPlan plutoTxPlan(const TxTune& t, double runRateHz, TxHwRange gain, double xoPpm);
// USRP (UHD): the overall gain, the TX/RX connector
TxHwPlan usrpTxPlan(const TxTune& t, double runRateHz, TxHwRange gain);
// SoapySDR (LimeSDR, bladeRF, USRP, XTRX, ... through SoapySDR): the overall gain clamped to its range, then the typed stages clamped to theirs
TxHwPlan soapyTxPlan(const TxTune& t, double runRateHz, TxHwRange gain, const std::map<std::string, TxHwRange>& stages);

} // namespace dect2
