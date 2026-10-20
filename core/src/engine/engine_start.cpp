// Engine::start(): opens the source, plans offset tuning, configures every receiver for the new sample rate and channel, and
// starts the worker threads. The long body is split into small steps called in the original order.
#include "dect2/engine/engine.h"
#include "dect2/automation/engine.h"
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

bool Engine::start(const DeviceInfo& dev, const TuneSettings& tune, const FileOptions& file) {
    stop();
    noTuneOffset_ = dev.kind == DeviceInfo::Audio;   // a sound card has no radio to tune off the DC spike: the signal is where the IF centre puts it (0 Hz)
    ring_.clear();
    ring_.resetDropped();
    radioMissing_ = 0; radioMissEvents_ = 0; cpuDropEvents_ = 0; ringDroppedSeen_ = 0;
    lastRadioMissNs_ = 0; lastCpuDropNs_ = 0; loadPct_ = 0; loadT_ = 0; deliveredPct_ = -1; rateT0_ = 0;
    holdDelivery(1000);   // a radio often stutters while its stream starts
    std::string err;
    if (int holder = 0; !claimDevice(dev, receiverId_, &holder)) { log("this radio is in use by " + deviceHolderText(holder)); return false; }
    if (dev.kind == DeviceInfo::File)
        src_ = makeFileSource(file.path, file.format, file.sampleRate, file.loop);
    else
        src_ = makeSource(dev);
    if (!src_) { log("no source"); releaseDevice(receiverId_); return false; }
    offHz_ = 0; offRate_ = 0; offsetDc_ = false;
    std::string offNote;
    if (autoOffset_ && dev.isRadio() && offsetAllowed() && mtvOn_.load() && multiChannelSupported(stdMode_.load()) && slotsInSpan(tune.sampleRate, tune.bandwidthMhz, 0) >= 2) {
        // a wide capture for multi-channel TV: the DC spike is inside the span whatever the offset (it would sit in a neighbouring channel), and
        // shifting 60 Msps back by the offset costs a double-precision mixer pass over every sample: the spike is removed instead
        offsetDc_ = true;
    } else if (autoOffset_ && dev.isRadio() && dev.kind != DeviceInfo::Hamlib && dev.board != "kiwisdr" && offsetAllowed()) {   // not a 12 kHz window
        // a HackRF has no listed maximum (20 Msps); the HackRF Pro tunes exactly only at up to 10 Msps and at 20, so it gets 20 above 10
        double maxRate = dev.maxRateHz > 0 ? dev.maxRateHz : dev.kind == DeviceInfo::HackRF ? 20e6 : 0;
        // a link slower than the converter (a PlutoSDR on its USB cable): no faster than it carries, or than the mode already runs above it
        if (linkCapHz(dev, tune) > 0) maxRate = std::max(tune.sampleRate, linkCapHz(dev, tune));
        OffsetPlan p = planOffset(tune.bandwidthMhz * 1e6, tune.sampleRate, maxRate, dev.minRateHz);
        if (p.ok && dev.kind == DeviceInfo::HackRF && p.rateHz > 10e6) p.rateHz = 20e6;
        // the only frequency test here: a radio cannot be tuned below 0 Hz, so a channel closer to 0 than the offset keeps the DC removal instead
        if (p.ok && tune.centerHz - p.offsetHz < 0) { p.ok = false; p.why = "the channel is too close to 0 Hz"; }
        if (p.ok) { offHz_ = p.offsetHz; offRate_ = p.rateHz; }
        else { offsetDc_ = true; offNote = "offset tuning not possible (" + p.why + "): removing the DC spike instead"; }
    }
    // the spectrum analyser on a HackRF: the radio's own sweep (DECT2_SPECAN_STEPPED=1 steps it like any other radio)
    specanSweep_ = stdMode_.load() == 44 && dev.kind == DeviceInfo::HackRF && !getenv("DECT2_SPECAN_STEPPED");
    if (!src_->start(hwTune(tune), ring_, err)) {
        log("source start failed: " + err + " (" + dev.name + ")");   // the radio the user selected, so a failure names it
        src_.reset();
        releaseDevice(receiverId_);
        return false;
    }
    if (offHz_ > 0 && src_->sampleRate() / 2 < offHz_ + tune.bandwidthMhz * 0.5e6) {
        // the radio did not take the rate the offset needs: back on the channel, with the DC removal
        offNote = "offset tuning not possible (the radio runs at " + std::to_string(src_->sampleRate() / 1e6).substr(0, 5) + " Msps): removing the DC spike instead";
        offHz_ = 0; offRate_ = 0; offsetDc_ = true;
        std::string e2;
        if (!src_->retune(hwTune(tune), e2)) log("retune failed: " + e2);
    }
    { std::lock_guard<std::mutex> lk(noteMu_); note_.clear(); retuneNote_.clear(); }
    setNote(offNote.empty() ? err : err.empty() ? offNote : err + "; " + offNote, false);
    srcRate_ = src_->sampleRate();
    rate_ = srcRate_;
    live_ = src_->realtimeHardware();
    if (tmSeconds_ > 0 && dev.kind != DeviceInfo::File) tm_.configure(tmSeconds_, srcRate_, tmDecim_, tmBudgetMB_);   // the time machine (time_machine.h)
    // FT8 / WSPR slots: a live radio follows the system clock; a recording or the test signal starts anywhere in a slot, so the
    // slots are found from the signals
    hfdig().ftx().setSlotSearch(!live_.load());
    offMix_.set(offHz_, srcRate_);
    decodeMix_.set(decodeShiftHz_.load(), srcRate_);
    offResample_ = false;
    if (offHz_ > 0) {
        // back to the mode's rate after the shift (if the radio runs faster than the mode asked for)
        if (tune.sampleRate > 0 && srcRate_ > tune.sampleRate * 1.001 && offRs_.configure(srcRate_, tune.sampleRate)) { offRs_.reset(); offResample_ = true; rate_ = tune.sampleRate; }
        char b[200];
        snprintf(b, sizeof b, "offset tuning: the radio sits %.3f MHz below the channel at %.2f Msps, its DC spike outside the channel; the receiver gets %.3f Msps",
                 offHz_.load() / 1e6, srcRate_ / 1e6, rate_.load() / 1e6);
        log(b);
    }
    {
        std::lock_guard<std::mutex> lk(tuneMu_);
        lastDev_ = dev; lastTune_ = tune; lastFile_ = file;
    }
    radioLost_ = false; lastSamples_ = std::chrono::steady_clock::now(); reconnectErr_.clear();
    automation::receiverState("started", "receiver started");
    logP1Count_ = 0; logGi_ = -2; logState_ = -1; logFrameSyms_ = 0;
    bwMhz_ = tune.bandwidthMhz; bwActive_ = bwMhz_;
    bwDetected_ = 0; bwReset_ = true; bwReq_ = false;
    configureTvReceivers(dev, tune);
    placeNarrowReceivers(dev, tune);
    configureDecoderReceivers(dev, tune);
    configureDataReceivers(dev, tune);
    checkRateAndResetState(dev, tune);
    announceAndRun(dev, tune);
    return true;
}

// Configures the TV, DAB, FM, DVB-S, DTMB, analog TV and first group of receivers for the current sample rate
void Engine::configureTvReceivers(const DeviceInfo& dev, const TuneSettings& tune) {
    rx_.configure(rate_, tune.bandwidthMhz);
    {   // the resampler on its own thread: live radios on Windows by default (not yet tried elsewhere); DECT2_PIPELINE=0/1 forces it off/on
#ifdef _WIN32
        bool pipe = src_->realtimeHardware();
#else
        bool pipe = false;
#endif
        if (const char* e = getenv("DECT2_PIPELINE")) pipe = atoi(e) != 0;
        rx_.setPipelined(pipe);
    }
    rxT_.configure(rate_, tune.bandwidthMhz);
    rxT_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });
    rxA_.configure(rate_);
    rxA_.setBlocking(!src_->realtimeHardware());
    rxA_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });   // runs on the ATSC worker thread
    rxD_.configure(rate_);
    rxD_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });   // DMB video, on the receiver thread
    rxI_.configure(rate_, tune.isdbtBwMhz);
    rxI_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });
    rxFm_.configure(rate_);
    rxFm_.setStationFinder(lastDev_.kind == DeviceInfo::File || lastDev_.kind == DeviceInfo::Synthetic);   // a live radio stays on the frequency tuned: a weak station is not traded for a strong neighbour
    if (stdMode_.load() == 7 && !rxFm_.ready()) log("FM needs a sample rate of at least 500 ksps");
    rxDvbs_.configure(rate_);
    rxDvbs_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { rxDvbs_.feedTs(pk, n); onTsPackets(pk, n, secs); });   // runs on the receiver thread
    rxDvbs_.setLogCallback([this](const std::string& s) { log(s); });
    rxDtmb_.configure(rate_, tune.bandwidthMhz);
    rxDtmb_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });   // runs on the receiver thread
    rxDtmb_.setLogCallback([this](const std::string& s) { log(s); });
    rxAtv_.configure(rate_);
    rxAtv_.setLogCallback([this](const std::string& s) { log(s); });
    rxDmr_.configure(rate_);
    rxDmr_.setLogCallback([this](const std::string& s) { log(s); });
    rxDrm_.configure(rate_);
    rxDrm_.setLogCallback([this](const std::string& s) { log(s); });
    rxAdsb_.configure(rate_);
    rxAdsb_.setLogCallback([this](const std::string& s) { log(s); });
    rxGnss_.configure(rate_);
    rxGnss_.setLogCallback([this](const std::string& s) { log(s); });
    rxGnss_.setClockCompare(dev.isRadio());   // the satellites' UTC against the computer's clock: a radio only (a file or the simulator keeps its own date)
    wide_.setLogCallback([this](const std::string& s) { log(s); });
}

// Gives every narrow-channel receiver the channel offset and centre/dial frequency it needs
void Engine::placeNarrowReceivers(const DeviceInfo& dev, const TuneSettings& tune) {
    {   // a narrow channel sits tuneOffsetHz away from the radio's centre (a file holds the channel as recorded: no offset)
        const ModeTuning* mto = modeTuning(stdMode_.load());
        const double o = mto && dev.kind != DeviceInfo::File && dev.kind != DeviceInfo::Audio ? -mto->tuneOffsetHz : 0.0;
        for (int wm : {10, 32, 34, 39, 46, 51, 24, 25}) wide_.configure(wm, rate_, o);
        wide_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : tune.centerHz);   // the logs of a whole-span decoder keep their rows' frequencies over a retune
        rxSonde_.setSignalOffset(o);
        rxAis_.setSignalOffset(o);
        rxMarine_.setSignalOffset(o);
        rxAcars_.setSignalOffset(o);
        if (dev.kind != DeviceInfo::File) rxAcars_.setCenterHz(radioTune(tune).centerHz);   // channel frequencies and the 25 kHz grid come from it
        rxInmc_.setSignalOffset(o);
        rxAero_.setSignalOffset(o);
        rxIridium_.setSignalOffset(o);
        rxMesh_.setSignalOffset(o);
        rxPager_.setSignalOffset(o);
        rxPacket_.setSignalOffset(o);
        rxHfdig_.setSignalOffset(o);
        rxAirband_.setSignalOffset(o);
        rxVdl2_.setSignalOffset(o);
        rxVdl2_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxAirband_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the channels are placed from it
        rxIsm_.setSignalOffset(o);
        rxIsm_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the devices' frequencies
        rxVor_.setSignalOffset(o);
        rxVor_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // VOR, localizer, glide slope or marker by the frequency
        rxNdb_.setSignalOffset(o);
        rxM17_.setSignalOffset(o);
        rxHfdl_.setSignalOffset(o);
        rxHfdl_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : (tune.centerHz >= 2e6 && tune.centerHz <= 30e6 ? tune.centerHz : 0.0));   // the channels are placed from it; a recording: the frequency entered for it (HFDL is HF), else 8.9 MHz
        rxCell_.setSignalOffset(o);
        rxCell_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the cells' EARFCNs
        rxCw_.setSignalOffset(o);
        rxDstar_.setSignalOffset(o);
        rxNxdn_.setSignalOffset(o);
        rxDpmr_.setSignalOffset(o);
        rxMac_.setSignalOffset(o);
        rxP25_.setSignalOffset(o);
        rxGsmr_.setSignalOffset(o);
        rxGsmr_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the carriers' ARFCNs
        rxTetra_.setSignalOffset(o);
        rxTetra_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the carriers' frequencies
        rxPsk_.setSignalOffset(o);
        rxJs8_.setSignalOffset(o);
        rxAle_.setSignalOffset(o);
        rxAle_.setDialHz(dev.kind != DeviceInfo::File ? tune.centerHz : 0.0);   // the dial, logged with each call
        rxOrb_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // absolute frequencies and Doppler
        rxBle_.setSignalOffset(o);
        rxBle_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the advertising channel tried first
        rxQo100_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the IF: where the beacons and transponders lie
        rxTime_.setSignalOffset(o);
        rxCube_.setSignalOffset(o);
        rxGs_.setSignalOffset(o);
        rxGs_.setDialHz(dev.kind != DeviceInfo::File ? tune.centerHz : 0.0);   // places the 13 channels
        rxVdes_.setSignalOffset(o);
        rxVdes_.setDialHz(dev.kind != DeviceInfo::File ? tune.centerHz : 0.0);   // places the ASM channels and the VDE-TER bands
        rxFlarm_.setSignalOffset(o);
        rxDji_.setSignalOffset(o);
        rxDji_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the bands and frequencies of the signals
        rxCube_.setDialHz(dev.kind != DeviceInfo::File ? tune.centerHz : 0.0);   // names frames after the satellite table
        rxLr_.setSignalOffset(o);
        rxWs_.setSignalOffset(o);
        rxEcz_.setSignalOffset(o);
        rxFpv_.setSignalOffset(o);
        rxFpv_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // which channels of the plans lie in the capture
        rxWmb_.setSignalOffset(o);
        rxWmb_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // which channels lie in the capture
        rxTones_.setSignalOffset(o);
        rxSar_.setSignalOffset(o);
        rxMt63_.setSignalOffset(o);
        rxKaws_.setSignalOffset(o);
        rxFsq_.setSignalOffset(o);
        rxIfkp_.setSignalOffset(o);
        rxCq_.setSignalOffset(o);
        rxArd_.setSignalOffset(o);
        rxZw_.setDialHz(tune.centerHz);
        rxZw_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxSb_.setSignalOffset(o);
        rxSb_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // for the CSV and the screen
        rxMs_.setSignalOffset(o);
        rxGd_.setSignalOffset(o);
        rxGd_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // for the channel numbers and the CSV
        rxModeAc_.setSignalOffset(o);
        rxBt_.setSignalOffset(o);
        rxMs_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // for the CSV and the screen
        rxBt_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the advertising channel tried first
        rxWs_.setDialHz(tune.centerHz);   // a file: where its channels are when it does not say
        rxWs_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // where every Wi-SUN channel lies in the band taken in
        rxZb_.setSignalOffset(o);
        rxZb_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // which 802.15.4 channel the radio is on
        rxNb_.setSignalOffset(o);
        rxNb_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // which raster points the radio takes in
        rxHl_.setSignalOffset(o);
        rxHl_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // which HaLow channel the radio is on
        rxCd_.setSignalOffset(o);
        rxDw_.setSignalOffset(o);
        rxDw_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // which buoy channel the radio is on
        rxSame_.setSignalOffset(o);
        rxAnt_.setSignalOffset(o);
        rxAnt_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxDect_.setSignalOffset(o);
        rxDect_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxTetrapol_.setSignalOffset(o);
        rxTetrapol_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxKlw_.setSignalOffset(o);
        rxTdcs_.setSignalOffset(o);
        rxKlw_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxTdcs_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxLbj_.setSignalOffset(o);
        rxLbj_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxEloran_.setSignalOffset(o);
        rxEloran_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxDgps_.setSignalOffset(o);
        rxFusion_.setSignalOffset(o);
        rxFusion_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxMilhf_.setSignalOffset(o);
        rxMilhf_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxDgps_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxMpt_.setSignalOffset(o);
        rxMpt_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxLtr_.setSignalOffset(o);
        rxLtr_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxEdacs_.setSignalOffset(o);
        rxEdacs_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
        rxMt_.setSignalOffset(o);
        rxCd_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // which part of the HF radar bands the radio is on
        rxTime_.setDialHz(dev.kind != DeviceInfo::File ? tune.centerHz : 0.0);   // the station when none is chosen
        rxWifi_.setSignalOffset(o);
        rxGeneral_.setSignalOffset(o);
        for (auto& g : extraGeneral_) g.setSignalOffset(o);
        rxRid_.setSignalOffset(o);
        rxRid_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // which BLE and Wi-Fi channels lie in the band
        rxMeteor_.setSignalOffset(o);
        rxNr_.setSignalOffset(o);
        rxNr_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the sync raster points searched
        rxGoes_.setSignalOffset(o);
        rxEl_.setSignalOffset(o);
        rxVdl4_.setSignalOffset(o);
        rxHrpt_.setSignalOffset(o);
        rxWifi_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);   // the channel noted with each network
    }
}

// Configures the narrow-channel decoders (sondes, ais, acars... tetra, psk, spectrum tools, astro, ...) with their log callbacks
void Engine::configureDecoderReceivers(const DeviceInfo& dev, const TuneSettings& tune) {
    rxSonde_.configure(rate_);
    rxSonde_.setLogCallback([this](const std::string& s) { log(s); });
    rxAis_.configure(rate_);
    rxAis_.setLogCallback([this](const std::string& s) { log(s); });
    rxMarine_.configure(rate_);
    rxMarine_.setLogCallback([this](const std::string& s) { log(s); });
    rxAcars_.configure(rate_);
    rxAcars_.setLogCallback([this](const std::string& s) { log(s); });
    rxInmc_.configure(rate_);
    rxInmc_.setLogCallback([this](const std::string& s) { log(s); });
    rxAero_.configure(rate_);
    rxAero_.setLogCallback([this](const std::string& s) { log(s); });
    rxIridium_.configure(rate_);
    rxIridium_.setLogCallback([this](const std::string& s) { log(s); });
    rxMesh_.configure(rate_);
    rxMesh_.setLogCallback([this](const std::string& s) { log(s); });
    rxHdr_.configure(rate_);
    rxHdr_.setLogCallback([this](const std::string& s) { log(s); });
    rxCdr_.configure(rate_);
    rxCdr_.setLogCallback([this](const std::string& s) { log(s); });
    rxPager_.configure(rate_);
    rxPager_.setLogCallback([this](const std::string& s) { log(s); });
    rxPacket_.configure(rate_);
    rxPacket_.setLogCallback([this](const std::string& s) { log(s); });
    rxHfdig_.configure(rate_);
    rxHfdig_.setLogCallback([this](const std::string& s) { log(s); });
    rxAirband_.configure(rate_);
    rxVdl2_.configure(rate_);
    rxVdl2_.setLogCallback([this](const std::string& s) { log(s); });
    rxAirband_.setLogCallback([this](const std::string& s) { log(s); });
    rxIsm_.configure(rate_);
    rxIsm_.setLogCallback([this](const std::string& s) { log(s); });
    rxVor_.configure(rate_);
    rxVor_.setLogCallback([this](const std::string& s) { log(s); });
    rxNdb_.configure(rate_);
    rxNdb_.setLogCallback([this](const std::string& s) { log(s); });
    rxDme_.configure(rate_);
    rxDme_.setLogCallback([this](const std::string& s) { log(s); });
    rxM17_.configure(rate_);
    rxHfdl_.configure(rate_);
    rxHfdl_.setLogCallback([this](const std::string& s) { log(s); });
    rxM17_.setLogCallback([this](const std::string& s) { log(s); });
    rxCell_.configure(rate_);
    rxCell_.setLogCallback([this](const std::string& s) { log(s); });
    rxCw_.configure(rate_);
    rxCw_.setLogCallback([this](const std::string& s) { log(s); });
    rxDstar_.configure(rate_);
    rxDstar_.setLogCallback([this](const std::string& s) { log(s); });
    rxNxdn_.configure(rate_);
    rxDpmr_.configure(rate_);
    rxDpmr_.setChannel(tune.centerHz / 1e6);   // the PMR446 channel it sits on (the scan steps from there)
    rxDpmr_.setLogCallback([this](const std::string& s) { log(s); });
    rxMac_.configure(rate_);
    rxMac_.setLogCallback([this](const std::string& s) { log(s); });
    rxNxdn_.setLogCallback([this](const std::string& s) { log(s); });
    rxP25_.configure(rate_);
    rxP25_.setLogCallback([this](const std::string& s) { log(s); });
    rxGsmr_.configure(rate_);
    rxGsmr_.setLogCallback([this](const std::string& s) { log(s); });
    rxTetra_.configure(rate_);
    rxTetra_.setLogCallback([this](const std::string& s) { log(s); });
    // the other receivers say what they listen for when configured, and the last of these stays on the bottom line: name the active one last
    if (stdMode_.load() == 34) log("Cell towers: searching for LTE cells");
    if (stdMode_.load() == 41) log("GSM-R: searching for base stations (BCCH carriers)");
    if (stdMode_.load() == 48) log("TETRA: searching for base stations (receive only, nothing is decrypted)");
    rxPsk_.configure(rate_);
    rxPsk_.setLogCallback([this](const std::string& s) { log(s); });
    rxJs8_.configure(rate_);
    rxSpecx_.configure(rate_);
    rxSpecx_.setLogCallback([this](const std::string& s) { log(s); });
    rxSpecan_.configure(rate_);
    rxSpecan_.setLogCallback([this](const std::string& s) { log(s); });
    rxSpecan_.setCenterHz(dev.kind != DeviceInfo::File ? radioTune(tune).centerHz : 0.0);
    // stepped sweeps retune the radio straight from the receiver (no hold, no reset: the receiver skips the settling samples itself);
    // a file, the test signal and the HackRF's own sweep do not step
    if (stdMode_.load() == 44 && dev.isRadio() && !specanSweep_)
        rxSpecan_.setRetuneCallback([this](double hz) {
            if (!src_) return false;
            TuneSettings t; { std::lock_guard<std::mutex> lk(tuneMu_); t = lastTune_; }
            t.centerHz = hz;
            std::string err;
            return src_->retune(hwTune(t), err);
        });
    else rxSpecan_.setRetuneCallback(nullptr);
    rxAstro_.configure(rate_);
    rxAstro_.setLogCallback([this](const std::string& s) { log(s); });
    // frequency switching retunes the radio straight from the receiver (it skips the settling samples itself); a file or the test signal cannot
    if (stdMode_.load() == 50 && dev.isRadio())
        rxAstro_.setRetuneCallback([this](double offsetHz) {
            if (!src_) return false;
            TuneSettings t; { std::lock_guard<std::mutex> lk(tuneMu_); t = lastTune_; }
            t.centerHz += offsetHz;
            std::string err;
            return src_->retune(hwTune(t), err);
        });
    else rxAstro_.setRetuneCallback(nullptr);
}

// Configures the remaining decoders (cube, flarm, DJI, wifi, general, goes...), their log callbacks and clocks
void Engine::configureDataReceivers(const DeviceInfo& dev, const TuneSettings& tune) {
    rxDvbc_.configure(rate_);
    rxDvbc_.setBlocking(dev.kind == DeviceInfo::File);
    rxDvbc_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });   // runs on the receiver thread
    rxDvbc_.setLogCallback([this](const std::string& s) { log(s); });
    rxOrb_.configure(rate_);
    rxOrb_.setLogCallback([this](const std::string& s) { log(s); });
    rxUplink_.configure(rate_);
    rxUplink_.setLogCallback([this](const std::string& s) { log(s); });
    rxAle_.configure(rate_);
    rxAle_.setLogCallback([this](const std::string& s) { log(s); });
    rxBle_.configure(rate_);
    rxBle_.setLogCallback([this](const std::string& s) { log(s); });
    rxIono_.configure(rate_);
    rxIono_.setLogCallback([this](const std::string& s) { log(s); });
    rxIono_.setCenterHz(radioTune(tune).centerHz);   // 0 .. 30 MHz is placed from it (a file: the frequency it was set to)
    // sweep starts are whole seconds of UTC with a radio (the computer clock at the first sample); a file or the test signal counts from 0
    rxIono_.setTimeOrigin(dev.isRadio() ? std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() : 0.0);
    rxQo100_.configure(rate_);
    rxTime_.configure(rate_);
    rxCube_.configure(rate_);
    rxGs_.configure(rate_);
    rxGs_.setWallClock(dev.kind != DeviceInfo::File);
    rxGs_.setLogCallback([this](const std::string& s) { log(s); });
    rxVdes_.configure(rate_);
    rxVdes_.setWallClock(dev.kind != DeviceInfo::File);
    rxVdes_.setLogCallback([this](const std::string& s) { log(s); });
    rxFlarm_.configure(rate_);
    rxFlarm_.setWallClock(true);   // the packets are keyed to the time of day; a file is decoded as if it were live
    rxFlarm_.setLogCallback([this](const std::string& s) { log(s); });
    rxDji_.configure(rate_);
    rxDji_.setWallClock(dev.kind != DeviceInfo::File);
    rxDji_.setLogCallback([this](const std::string& s) { log(s); });
    rxCube_.setWallClock(dev.kind != DeviceInfo::File);
    rxCube_.setLogCallback([this](const std::string& s) { log(s); });
    rxZb_.configure(rate_);
    rxNb_.configure(rate_);
    rxNb_.setLogCallback([this](const std::string& s) { log(s); });
    rxHl_.configure(rate_);
    rxHl_.setLogCallback([this](const std::string& s) { log(s); });
    rxCd_.configure(rate_);
    rxWb_.configure(rate_);
    rxCd_.setLogCallback([this](const std::string& s) { log(s); });
    rxDw_.configure(rate_);
    rxDw_.setLogCallback([this](const std::string& s) { log(s); });
    rxSame_.configure(rate_);
    rxSame_.setLogCallback([this](const std::string& s) { log(s); });
    rxAnt_.configure(rate_);
    rxAnt_.setLogCallback([this](const std::string& s) { log(s); });
    rxDect_.configure(rate_);
    rxDect_.setLogCallback([this](const std::string& s) { log(s); });
    rxTetrapol_.configure(rate_);
    rxTetrapol_.setLogCallback([this](const std::string& s) { log(s); });
    rxKlw_.configure(rate_);
    rxTdcs_.configure(rate_);
    rxKlw_.setLogCallback([this](const std::string& s) { log(s); });
    rxTdcs_.setLogCallback([this](const std::string& s) { log(s); });
    rxLbj_.configure(rate_);
    rxLbj_.setLogCallback([this](const std::string& s) { log(s); });
    rxEloran_.configure(rate_);
    rxEloran_.setLogCallback([this](const std::string& s) { log(s); });
    rxMilhf_.configure(rate_);
    rxMilhf_.setLogCallback([this](const std::string& s) { log(s); });
    rxDgps_.configure(rate_);
    rxFusion_.configure(rate_);
    rxFusion_.setLogCallback([this](const std::string& s) { log(s); });
    rxDgps_.setLogCallback([this](const std::string& s) { log(s); });
    rxMpt_.configure(rate_);
    rxMpt_.setLogCallback([this](const std::string& s) { log(s); });
    rxLtr_.configure(rate_);
    rxLtr_.setLogCallback([this](const std::string& s) { log(s); });
    rxEdacs_.configure(rate_);
    rxEdacs_.setLogCallback([this](const std::string& s) { log(s); });
    rxMt_.configure(rate_);
    rxMt_.setLogCallback([this](const std::string& s) { log(s); });
    rxZb_.setLogCallback([this](const std::string& s) { log(s); });
    rxLr_.configure(rate_);
    rxLr_.setLogCallback([this](const std::string& s) { log(s); });
    rxWs_.configure(rate_);
    rxWs_.setLogCallback([this](const std::string& s) { log(s); });
    rxFpv_.configure(rate_);
    rxFpv_.setLogCallback([this](const std::string& s) { log(s); });
    rxTones_.setAudioInput(dev.kind == DeviceInfo::Audio);   // a sound card gives the audio itself, a radio gives an FM channel
    rxTones_.configure(rate_);
    rxTones_.setLogCallback([this](const std::string& s) { log(s); });
    rxSar_.configure(rate_);
    rxSar_.setLogCallback([this](const std::string& s) { log(s); });
    rxMt63_.configure(rate_);
    rxMt63_.setLogCallback([this](const std::string& s) { log(s); });
    rxFsq_.configure(rate_);
    rxFsq_.setLogCallback([this](const std::string& s) { log(s); });
    rxIfkp_.configure(rate_);
    rxIfkp_.setLogCallback([this](const std::string& s) { log(s); });
    rxKaal_.configure(rate_);
    rxKaal_.setLogCallback([this](const std::string& s) { log(s); });
    rxKait_.configure(rate_);
    rxKait_.setLogCallback([this](const std::string& s) { log(s); });
    rxKavb_.configure(rate_);
    rxKavb_.setLogCallback([this](const std::string& s) { log(s); });
    rxKaws_.configure(rate_);
    rxKaws_.setLogCallback([this](const std::string& s) { log(s); });
    rxKapp_.configure(rate_);
    rxKapp_.setLogCallback([this](const std::string& s) { log(s); });
    rxKaft_.configure(rate_);
    rxKaft_.setLogCallback([this](const std::string& s) { log(s); });
    rxCq_.configure(rate_);
    rxCq_.setLogCallback([this](const std::string& s) { log(s); });
    rxArd_.configure(rate_);
    rxArd_.setLogCallback([this](const std::string& s) { log(s); });
    rxZw_.configure(rate_);
    rxZw_.setLogCallback([this](const std::string& m) { log(m); });
    rxEcz_.configure(rate_);
    rxWmb_.configure(rate_);
    rxWmb_.setLogCallback([this](const std::string& s) { log(s); });
    rxModeAc_.configure(rate_);
    rxModeAc_.setLogCallback([this](const std::string& s) { log(s); });
    rxSb_.configure(rate_);
    rxSb_.setLogCallback([this](const std::string& s) { log(s); });
    rxMs_.configure(rate_);
    rxBt_.configure(rate_);
    rxMs_.setLogCallback([this](const std::string& s) { log(s); });
    rxGd_.configure(rate_);
    rxGd_.setLogCallback([this](const std::string& s) { log(s); });
    rxBt_.setLogCallback([this](const std::string& s) { log(s); });
    rxEcz_.setLogCallback([this](const std::string& s) { log(s); });
    rxEcz_.setClockMode(dev.kind == DeviceInfo::File ? 0 : 1);   // a file has no clock to compare; a radio and the test signal run on the computer's
    rxHfb_.configure(rate_);
    rxHfb_.setLogCallback([this](const std::string& s) { log(s); });
    rxHfb_.setCenterHz(radioTune(tune).centerHz);
    // NCDXF slots are told from the computer clock at the first sample with a radio; a file or the test signal counts from 0 (no slots)
    rxHfb_.setTimeOrigin(dev.isRadio() ? std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() : 0.0, dev.isRadio());
    rxTime_.setLogCallback([this](const std::string& s) { log(s); });
    rxTime_.setClockMode(dev.kind == DeviceInfo::File ? 0 : 1);   // a file has no clock to compare; a radio and the test signal run on the computer's
    rxWifi_.configure(rate_);
    rxGeneral_.configure(rate_);
    rxGeneral_.setLogCallback([this](const std::string& s) { log(s); });
    for (auto& g : extraGeneral_) { g.configure(rate_); g.setSilent(true); g.setLogCallback([this](const std::string& s) { log(s); }); }
    rxRid_.configure(rate_);
    rxRid_.setLogCallback([this](const std::string& s) { log(s); });
    rxNr_.configure(rate_);
    rxNr_.setLogCallback([this](const std::string& s) { log(s); });
    rxWifi_.setLogCallback([this](const std::string& s) { log(s); });
    rxMeteor_.configure(rate_);
    rxMeteor_.setLogCallback([this](const std::string& s) { log(s); });
    rxGoes_.configure(rate_);
    rxEl_.configure(rate_);
    rxHrpt_.configure(rate_);
    rxHrpt_.setLogCallback([this](const std::string& s) { log(s); });
    rxGoes_.setLogCallback([this](const std::string& s) { log(s); });
    rxEl_.setLogCallback([this](const std::string& s) { log(s); });
    rxUat_.configure(rate_);
    rxUat_.setLogCallback([this](const std::string& s) { log(s); });
    rxVdl4_.configure(rate_);
    rxVdl4_.setLogCallback([this](const std::string& s) { log(s); });
    rxQo100_.setLogCallback([this](const std::string& s) { log(s); });
    rxJs8_.setLogCallback([this](const std::string& s) { log(s); });
}

// Warns when the sample rate is too low for the mode, then resets the per-mode counters and the transport-stream state
void Engine::checkRateAndResetState(const DeviceInfo& dev, const TuneSettings& tune) {
    if (const ModeTuning* mt = modeTuning(stdMode_.load()))
        if (rate_.load() < mt->minSampleRate - 1) {
            char m[200];
            snprintf(m, sizeof m, "sample rate %.2f Msps is too low for %s (at least %.2f Msps is needed) - the receiver cannot work with this radio", rate_.load() / 1e6, mt->name, mt->minSampleRate / 1e6);
            log(m);
        }
    logIMode_ = logIGi_ = logITmcc_ = -1; logISync_ = false;
    rxA3_.setPaced(true);
    rxA3_.configure(rate_);
    rxA3_.setBlocking(!src_->realtimeHardware());
    rxA3_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });   // runs on the ATSC 3.0 worker thread
    atsc3Seq_ = 0; logA3State_ = -1; logA3Svc_ = false;
    { std::lock_guard<std::mutex> lk(atsc3Mu_); atsc3Tel_ = Atsc3Telemetry(); }
    dabSeq_ = 0; logDState_ = -1; logDEns_ = false; dmbSub_ = -1;
    for (auto& s : modeSeq_) s = 0;
    atscSeq_ = 0; logAState_ = -1;
    activeStd_ = activeStdFor(stdMode_.load());
    autoMark_ = 0; lastLockSec_ = 0; logTMode_ = logTGi_ = logTTps_ = -1; logTFec_ = false;
    {
        std::lock_guard<std::mutex> lk(tsMu_);
        demux_.reset();
        unpack_ = BbUnpacker();
        tsSnap_ = TsSnapshot();
        lastT2Frame_ = -1;
        unpack_.setSink([this](const uint8_t* pkt) { demux_.feed(pkt); if (!playerExternal_) { ttx_.feedTs(pkt); dvbsub_.feedTs(pkt); } frameBuf_.insert(frameBuf_.end(), pkt, pkt + 188); });
    }
    rx_.setPlpCallback([this](const PlpResult& r) { if (plpDump_) plpDump_(r); onPlp(r); });
    {   // DVB-T2 / DVB-T, ATSC and DAB at a rate their receiver cannot use (the telemetry test that stood here never fired: the receiver
        // publishes nothing before its first samples, so the user got no word at all, e.g. an RTL-SDR at 2.56 Msps on an 8 MHz channel)
        const int sm = stdMode_.load();
        const double need = sm <= 4 ? minSampleRateFor(sm, tune.bandwidthMhz) : 0;
        if (need > 0 && rate_.load() < need - 1) {
            char what[64], m[240];
            if (sm == 3) snprintf(what, sizeof what, "an ATSC channel");
            else if (sm == 4) snprintf(what, sizeof what, "a DAB ensemble");
            else snprintf(what, sizeof what, "the %g MHz %s channel", tune.bandwidthMhz, sm == 1 ? "DVB-T2" : sm == 2 ? "DVB-T" : "DVB-T2 / DVB-T");
            snprintf(m, sizeof m, "sample rate %.2f Msps is too low for %s (at least %.2f Msps is needed) - %s", rate_.load() / 1e6, what, need / 1e6,
                     sm <= 2 ? "only the spectrum is shown" : "the receiver cannot lock with this radio");
            log(m);
        }
    }
    if (stdMode_ == 5 && rate_.load() < 6.5e6) {
        char m[200];
        snprintf(m, sizeof m, "sample rate %.2f Msps is too low for an ATSC 3.0 channel (about 6.5 Msps or more is needed) - the receiver cannot lock with this radio", rate_.load() / 1e6);
        log(m);
    }
    if (stdMode_ == 6 && rate_.load() < minSampleRateFor(6, tune.isdbtBwMhz ? tune.isdbtBwMhz : 6)) {
        char m[200];
        snprintf(m, sizeof m, "sample rate %.2f Msps is too low for a %d MHz ISDB-T channel (about %.0f Msps or more is needed) - the receiver cannot lock with this radio", rate_.load() / 1e6,
                 tune.isdbtBwMhz ? tune.isdbtBwMhz : 6, minSampleRateFor(6, tune.isdbtBwMhz ? tune.isdbtBwMhz : 6) / 1e6);
        log(m);
    }
}

// Logs the started source, then starts the spectrum and analysis threads and the secondary receiver
void Engine::announceAndRun(const DeviceInfo& dev, const TuneSettings& tune) {
    char b[160];
    snprintf(b, sizeof b, "source started: %s  fs=%.4f Msps", dev.name.c_str(), rate_.load() / 1e6);
    log(b);
    if (dev.isGeneric()) {
        snprintf(b, sizeof b, "tuned %.3f MHz  gain %.0f dB", tune.centerHz / 1e6, tune.gainDb);
        log(b);
    }
    if (dev.kind == DeviceInfo::HackRF) {
        snprintf(b, sizeof b, "tuned %.3f MHz  LNA %d dB  VGA %d dB  amp %s", tune.centerHz / 1e6, tune.lnaDb, tune.vgaDb, tune.ampOn ? "on" : "off");
        log(b);
    }
    tSpec_ = tRx_ = 0; nSamp_ = 0;
    stopReq_ = false;
    startEpoch_++;
    running_ = true;
    specStop_ = false;
    specQ_.clear();
    specTh_ = std::thread([this] { spectrumLoop(); });
    th_ = std::thread([this] { analysisLoop(); });
    startSecondary();   // the second mode asked for before (or kept from the last start)
}

}   // namespace dect2
