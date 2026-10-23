// Retuning a running engine: the radio and hardware tune settings (radioTune/hwTune), retune, retune with receiver reset, and the
// restart when the sample rate has to change.
#include "dect2/engine/engine.h"
#include "engine_internal.h"
#include "dect2/source/converter.h"
#include "dect2/net/share.h"
#include "dect2/engine/device_claims.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/isdbt/isdbt.h"
#include "dect2/engine/modes.h"
#include "dect2/t2/t2rx.h"
#include <chrono>
#include "dect2/common/platform.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dect2 {

// The radio of a narrow-channel mode is tuned tuneOffsetHz above the user's frequency, so the channel stays off the DC spike.
TuneSettings Engine::radioTune(const TuneSettings& t) const {
    TuneSettings r = t;
    if (const ModeTuning* mt = noTuneOffset_.load() ? nullptr : modeTuning(stdMode_.load())) {
        r.centerHz += mt->tuneOffsetHz;
        if (mt->tuneOffsetHz != 0) r.radio["tune_offset_hz"] = std::to_string(mt->tuneOffsetHz);   // a source with only a few kHz of spectrum (KiwiSDR) tunes to the dial and shifts the samples by it
    }
    if (noTuneOffset_.load()) if (const ModeTuning* mt = modeTuning(stdMode_.load())) if (mt->minSampleRate > 0) r.radio["audio_min_rate"] = std::to_string(mt->minSampleRate);   // a sound card: interpolated up to what the mode's receiver needs
    if (offHz_ > 0) { r.centerHz -= offHz_; r.sampleRate = offRate_; r.basebandFilterHz = 0; }   // offset tuning: the radio below the channel
    else if (const int d = radioDecimation(t); d > 1 && t.sampleRate > 0 && !specanSweep_ && src_ && src_->realtimeHardware())
        r.sampleRate = t.sampleRate * d;   // #233: the radio runs faster, start() resamples back down to the mode's rate (offRs_)
    if (specanSweep_) {   // HackrfSource sweeps this range itself (MHz)
        double lo = 0, hi = 0;
        rxSpecan_.range(lo, hi);
        r.radio["specan_sweep"] = std::to_string(lo / 1e6) + ":" + std::to_string(hi / 1e6);
        r.sampleRate = 20e6; r.basebandFilterHz = 15e6;
    }
    return r;
}

// The converter (converter.h): the user's frequency is the one at the antenna, the radio goes to what comes out of the converter.
// Not for a file or the synthetic signal (they have no radio), nor the HackRF sweep (its range is the radio's own).
TuneSettings Engine::hwTune(const TuneSettings& t) const {
    TuneSettings r = radioTune(t);
    const Converter c = converterFromRadio(t.radio);
    const bool live = c.active && !specanSweep_ && src_ && src_->realtimeHardware();
    convSwap_.store(live && c.inverts());
    if (live && c.reachable(r.centerHz)) r.centerHz = c.rfToIf(r.centerHz);
    return r;
}

bool Engine::retune(const TuneSettings& tune) {
    if (!src_) return false;
    std::string err;
    holdDelivery(300);
    if (!src_->retune(hwTune(tune), err)) { log("retune failed: " + err); return false; }
    setNote(err, true);
    if (restartIfRateChanged(tune)) return running_;
    double oldCentre = 0;
    { std::lock_guard<std::mutex> lk(tuneMu_); oldCentre = lastTune_.centerHz; lastTune_ = tune; }
    // a retune that does not move (the gain, the AGC) leaves a second mode's receiver alone: placing it again would restart it
    const bool moved = std::fabs(oldCentre - tune.centerHz) > 0.5;
    wide_.setCenterHz(lastDev_.kind != DeviceInfo::File ? radioTune(tune).centerHz : tune.centerHz);
    if (lastDev_.isRadio()) rxAis_.setCenterHz(radioTune(tune).centerHz);   // AIS: the four channels are placed by their own frequencies
    if (moved || secAct_.load() < 0) {
        if (lastDev_.kind != DeviceInfo::File) { rxIono_.setCenterHz(radioTune(tune).centerHz); rxAcars_.setCenterHz(radioTune(tune).centerHz); rxAirband_.setCenterHz(radioTune(tune).centerHz); rxVdl2_.setCenterHz(radioTune(tune).centerHz); rxIsm_.setCenterHz(radioTune(tune).centerHz); rxVor_.setCenterHz(radioTune(tune).centerHz); rxCell_.setCenterHz(radioTune(tune).centerHz); rxHfdl_.setCenterHz(radioTune(tune).centerHz); rxGsmr_.setCenterHz(radioTune(tune).centerHz); rxTetra_.setCenterHz(radioTune(tune).centerHz); rxOrb_.setCenterHz(radioTune(tune).centerHz); rxAle_.setDialHz(tune.centerHz); rxBle_.setCenterHz(radioTune(tune).centerHz); }
        if (lastDev_.kind != DeviceInfo::File) { rxCube_.setDialHz(tune.centerHz); rxGs_.setDialHz(tune.centerHz); rxVdes_.setDialHz(tune.centerHz); rxTime_.setDialHz(tune.centerHz); rxWifi_.setCenterHz(radioTune(tune).centerHz); rxAcars_.setCenterHz(radioTune(tune).centerHz); rxAirband_.setCenterHz(radioTune(tune).centerHz); rxVdl2_.setCenterHz(radioTune(tune).centerHz); rxIsm_.setCenterHz(radioTune(tune).centerHz); rxVor_.setCenterHz(radioTune(tune).centerHz); rxCell_.setCenterHz(radioTune(tune).centerHz); rxHfdl_.setCenterHz(radioTune(tune).centerHz); rxGsmr_.setCenterHz(radioTune(tune).centerHz); rxTetra_.setCenterHz(radioTune(tune).centerHz); rxOrb_.setCenterHz(radioTune(tune).centerHz); rxAle_.setDialHz(tune.centerHz); rxBle_.setCenterHz(radioTune(tune).centerHz); rxQo100_.setCenterHz(radioTune(tune).centerHz); }
        if (lastDev_.kind != DeviceInfo::File) { rxRid_.setCenterHz(radioTune(tune).centerHz); rxWifi_.setCenterHz(radioTune(tune).centerHz); rxAcars_.setCenterHz(radioTune(tune).centerHz); rxAirband_.setCenterHz(radioTune(tune).centerHz); rxVdl2_.setCenterHz(radioTune(tune).centerHz); rxIsm_.setCenterHz(radioTune(tune).centerHz); rxVor_.setCenterHz(radioTune(tune).centerHz); rxCell_.setCenterHz(radioTune(tune).centerHz); rxHfdl_.setCenterHz(radioTune(tune).centerHz); rxGsmr_.setCenterHz(radioTune(tune).centerHz); rxTetra_.setCenterHz(radioTune(tune).centerHz); rxOrb_.setCenterHz(radioTune(tune).centerHz); rxAle_.setDialHz(tune.centerHz); rxBle_.setCenterHz(radioTune(tune).centerHz); rxQo100_.setCenterHz(radioTune(tune).centerHz); }
        if (lastDev_.kind != DeviceInfo::File) { rxWifi_.setCenterHz(radioTune(tune).centerHz); rxNr_.setCenterHz(radioTune(tune).centerHz); rxAcars_.setCenterHz(radioTune(tune).centerHz); rxAirband_.setCenterHz(radioTune(tune).centerHz); rxVdl2_.setCenterHz(radioTune(tune).centerHz); rxIsm_.setCenterHz(radioTune(tune).centerHz); rxVor_.setCenterHz(radioTune(tune).centerHz); rxCell_.setCenterHz(radioTune(tune).centerHz); rxHfdl_.setCenterHz(radioTune(tune).centerHz); rxGsmr_.setCenterHz(radioTune(tune).centerHz); rxTetra_.setCenterHz(radioTune(tune).centerHz); rxOrb_.setCenterHz(radioTune(tune).centerHz); rxAle_.setDialHz(tune.centerHz); rxBle_.setCenterHz(radioTune(tune).centerHz); rxQo100_.setCenterHz(radioTune(tune).centerHz); }
        if (lastDev_.kind != DeviceInfo::File) rxZb_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxNb_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxHl_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxWs_.setCenterHz(radioTune(tune).centerHz);
        rxWs_.setDialHz(tune.centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxFpv_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxZw_.setCenterHz(radioTune(tune).centerHz);
        rxZw_.setDialHz(tune.centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxCd_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxWmb_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxDw_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxMs_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxGd_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxBt_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxSb_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxIsdbs_.setCenterHz(radioTune(tune).centerHz);
    }
    refreshSecondary(moved);
    return true;
}

// Every receiver was set up for the rate the radio had at start(); a retune that changes it (another rate asked for, or the radio
// picking a different one) would leave them decoding at the wrong rate, so the engine starts again with the new settings
// A driver note is logged when it is new (a retune that brings the same one again, as the AGC does, does not fill the log)
void Engine::setNote(const std::string& n, bool retune) {
    {
        std::lock_guard<std::mutex> lk(noteMu_);
        std::string& slot = retune ? retuneNote_ : note_;
        if (n == slot) return;
        slot = n;
    }
    if (!n.empty()) log("note: " + n);
}

bool Engine::restartIfRateChanged(const TuneSettings& tune) {
    if (!src_ || std::fabs(src_->sampleRate() - srcRate_) < 0.5) return false;
    char b[160];
    snprintf(b, sizeof b, "the radio now runs at %.3f Msps (was %.3f): restarting the receivers", src_->sampleRate() / 1e6, srcRate_ / 1e6);
    log(b);
    const DeviceInfo dev = lastDev_;
    const FileOptions file = lastFile_;
    stop();
    start(dev, tune, file);
    return true;
}

bool Engine::retuneReset(const TuneSettings& tune) {
    if (!src_) return false;
    std::string err;
    holdDelivery(300);
    if (!src_->retune(hwTune(tune), err)) { log("retune failed: " + err); return false; }
    setNote(err, true);
    if (restartIfRateChanged(tune)) return running_;
    double oldCentre = 0;
    { std::lock_guard<std::mutex> lk(tuneMu_); oldCentre = lastTune_.centerHz; lastTune_ = tune; }
    // a retune that does not move (the gain, the AGC) leaves a second mode's receiver alone: placing it again would restart it
    const bool moved = std::fabs(oldCentre - tune.centerHz) > 0.5;
    wide_.setCenterHz(lastDev_.kind != DeviceInfo::File ? radioTune(tune).centerHz : tune.centerHz);
    if (lastDev_.isRadio()) rxAis_.setCenterHz(radioTune(tune).centerHz);   // AIS: the four channels are placed by their own frequencies
    if (moved || secAct_.load() < 0) {
        if (lastDev_.kind != DeviceInfo::File) { rxIono_.setCenterHz(radioTune(tune).centerHz); rxAcars_.setCenterHz(radioTune(tune).centerHz); rxAirband_.setCenterHz(radioTune(tune).centerHz); rxVdl2_.setCenterHz(radioTune(tune).centerHz); rxIsm_.setCenterHz(radioTune(tune).centerHz); rxVor_.setCenterHz(radioTune(tune).centerHz); rxCell_.setCenterHz(radioTune(tune).centerHz); rxHfdl_.setCenterHz(radioTune(tune).centerHz); rxGsmr_.setCenterHz(radioTune(tune).centerHz); rxTetra_.setCenterHz(radioTune(tune).centerHz); rxOrb_.setCenterHz(radioTune(tune).centerHz); rxAle_.setDialHz(tune.centerHz); rxBle_.setCenterHz(radioTune(tune).centerHz); }
        if (lastDev_.kind != DeviceInfo::File) { rxCube_.setDialHz(tune.centerHz); rxGs_.setDialHz(tune.centerHz); rxVdes_.setDialHz(tune.centerHz); rxTime_.setDialHz(tune.centerHz); rxWifi_.setCenterHz(radioTune(tune).centerHz); rxAcars_.setCenterHz(radioTune(tune).centerHz); rxAirband_.setCenterHz(radioTune(tune).centerHz); rxVdl2_.setCenterHz(radioTune(tune).centerHz); rxIsm_.setCenterHz(radioTune(tune).centerHz); rxVor_.setCenterHz(radioTune(tune).centerHz); rxCell_.setCenterHz(radioTune(tune).centerHz); rxHfdl_.setCenterHz(radioTune(tune).centerHz); rxGsmr_.setCenterHz(radioTune(tune).centerHz); rxTetra_.setCenterHz(radioTune(tune).centerHz); rxOrb_.setCenterHz(radioTune(tune).centerHz); rxAle_.setDialHz(tune.centerHz); rxBle_.setCenterHz(radioTune(tune).centerHz); rxQo100_.setCenterHz(radioTune(tune).centerHz); }
        if (lastDev_.kind != DeviceInfo::File) { rxRid_.setCenterHz(radioTune(tune).centerHz); rxWifi_.setCenterHz(radioTune(tune).centerHz); rxAcars_.setCenterHz(radioTune(tune).centerHz); rxAirband_.setCenterHz(radioTune(tune).centerHz); rxVdl2_.setCenterHz(radioTune(tune).centerHz); rxIsm_.setCenterHz(radioTune(tune).centerHz); rxVor_.setCenterHz(radioTune(tune).centerHz); rxCell_.setCenterHz(radioTune(tune).centerHz); rxHfdl_.setCenterHz(radioTune(tune).centerHz); rxGsmr_.setCenterHz(radioTune(tune).centerHz); rxTetra_.setCenterHz(radioTune(tune).centerHz); rxOrb_.setCenterHz(radioTune(tune).centerHz); rxAle_.setDialHz(tune.centerHz); rxBle_.setCenterHz(radioTune(tune).centerHz); rxQo100_.setCenterHz(radioTune(tune).centerHz); }
        if (lastDev_.kind != DeviceInfo::File) { rxWifi_.setCenterHz(radioTune(tune).centerHz); rxNr_.setCenterHz(radioTune(tune).centerHz); rxAcars_.setCenterHz(radioTune(tune).centerHz); rxAirband_.setCenterHz(radioTune(tune).centerHz); rxVdl2_.setCenterHz(radioTune(tune).centerHz); rxIsm_.setCenterHz(radioTune(tune).centerHz); rxVor_.setCenterHz(radioTune(tune).centerHz); rxCell_.setCenterHz(radioTune(tune).centerHz); rxHfdl_.setCenterHz(radioTune(tune).centerHz); rxGsmr_.setCenterHz(radioTune(tune).centerHz); rxTetra_.setCenterHz(radioTune(tune).centerHz); rxOrb_.setCenterHz(radioTune(tune).centerHz); rxAle_.setDialHz(tune.centerHz); rxBle_.setCenterHz(radioTune(tune).centerHz); rxQo100_.setCenterHz(radioTune(tune).centerHz); }
        if (lastDev_.kind != DeviceInfo::File) rxZb_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxNb_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxHl_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxWs_.setCenterHz(radioTune(tune).centerHz);
        rxWs_.setDialHz(tune.centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxFpv_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxZw_.setCenterHz(radioTune(tune).centerHz);
        rxZw_.setDialHz(tune.centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxCd_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxWmb_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxDw_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxMs_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxGd_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxBt_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxSb_.setCenterHz(radioTune(tune).centerHz);
        if (lastDev_.kind != DeviceInfo::File) rxIsdbs_.setCenterHz(radioTune(tune).centerHz);
    }
    refreshSecondary(moved);
    resetReq_ = true;
    return true;
}

}   // namespace dect2
