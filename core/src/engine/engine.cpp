// The Engine core: sample ingest (cleaning, offset tuning), stop, recording, applyReset, and the per-receiver feed,
// reset and auto-select switches. Other Engine members live in engine_*.cpp next to this file.
#include "dect2/engine/engine.h"
#include "dect2/tv/dvb_text.h"
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
#include "dect2/dsp/sample_convert.h"   // dropNonFinite
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

// "Remove DC spike" and "IQ correction" of the samples (iq_correct.h), with the part skipped that would harm the active mode (tests/engine/test_iq_modes.cpp):
// the DC removal where the wanted carrier can sit on the centre (FM, and the modes whose ModeTuning says carrierAtCentre), the IQ correction
// where the signal is not circular around the centre (ModeTuning::notCircular)
static void cleanSamples(IqCorrector& fix, OffsetMixer& mix, bool forceDc, cf32* x, size_t n, int activeStd) {
    const ModeTuning* mt = modeTuning(activeStd + 1);
    fix.process(x, n, activeStd == 6 || (mt && mt->carrierAtCentre), mt && mt->notCircular, forceDc);
    mix.mix(x, n);   // offset tuning: the channel back to the centre, after the IQ correction (the imbalance mirrors around the radio's centre)
}

// Offset tuning only where one channel sits on the centre: the TV standards, DAB, FM, DVB-S, DTMB, DMR, DRM, ADS-B, GNSS and radiosondes.
// Not analog TV (its own DC handling, carriers both sides), and not the modes that already tune off the channel or watch several channels.
bool Engine::offsetAllowed() const {
    const int m = stdMode_.load();
    if (m == 10 || m == 16 || m == 18 || m == 21) return false;   // analog TV, AIS, ACARS, Iridium
    if (const ModeTuning* mt = modeTuning(m)) if (mt->tuneOffsetHz != 0) return false;
    return m >= 0 && m <= 15;
}

void Engine::ingest(cf32* x, size_t n) {
    if (convSwap_.load(std::memory_order_relaxed)) swapIq(x, n);   // a converter that mirrors the spectrum: the signal reads the right way round from here on
    rec_.push(x, n);   // the raw samples, before anything touches them
    tm_.push(x, n);    // the time machine's rolling memory (a copy into a ring, nothing else)
    if (iqTapOn_.load(std::memory_order_relaxed)) { std::lock_guard<std::mutex> lk(iqTapMu_); if (iqTap_) iqTap_(x, n, srcRate_); }   // selfcal
    ShareServer::get().offer(x, n);   // "Share my radio": the same samples to the rtl_tcp clients (nothing while sharing is off)
    dropNonFinite(x, n);
    cleanSamples(iqFix_, offMix_, offsetDc_.load(), x, n, activeStd_.load());
    if (nbOn_.load(std::memory_order_relaxed)) {   // the noise blanker: clicks out before the spectrum and the decoders see them
        const double r = srcRate_ > 0 ? srcRate_ : rate_.load();   // the samples here are still at the radio's rate
        nbWas_ = true;
        if (nbDirty_.exchange(false) || r != nbRate_) { nb_.set(nbThr_.load(), nbWidthUs_.load() * 1e-6, r); nbRate_ = r; }
        nb_.process(x, n);
        if (nb_.seen() - nbSeen0_ >= (uint64_t)std::max(1.0, r)) {   // about once a second
            nbPct_ = (float)(100.0 * (double)(nb_.blanked() - nbBlank0_) / (double)(nb_.seen() - nbSeen0_));
            nbSeen0_ = nb_.seen(); nbBlank0_ = nb_.blanked();
        }
    } else if (nbWas_) { nbWas_ = false; nbPct_ = 0.f; nb_.reset(); }   // switched off: the next start learns the level again
    const auto a0 = std::chrono::steady_clock::now();
    lastSamples_ = a0;   // the radio is alive, whether or not the resampler gives a sample out of this block
    if (offResample_) { offBuf_.clear(); offRs_.process(x, n, offBuf_); x = offBuf_.data(); n = offBuf_.size(); }
    if (!n) return;
    if (sigid_.armed()) sigid_.push(x, n);
    if (autodet_.armed()) autodet_.push(x, n);
    feedSpectrum(x, n);
    if (const int s = secAct_.load(); s >= 0 && s != activeStd_.load()) pushSecondary(x, n);   // the second mode: a copy, before the first receiver
    if (monitor_.enabled()) {   // "Monitor audio": the tuned channel, a copy
        double dial;
        { std::lock_guard<std::mutex> lk(tuneMu_); dial = lastTune_.centerHz; }
        monitor_.push(x, n, rate_.load(), dial + monitorOffsetHz_.load() - captureCentreHz());
    }
    // sticky tuning: only the first receiver gets the clicked signal at DC. The spectrum, the signal identifier, the second mode and the
    // monitor see the capture as the radio delivers it (their frequencies are relative to captureCentreHz()), so the display stays put
    // and the tuned marker moves to the click, instead of the whole spectrum sliding by the shift (#262)
    {
        cpu::Scope cs(cpuDec_.load(std::memory_order_relaxed));   // the receiver's CPU (and its pool jobs) to the decoder account
        decodeMix_.mix(x, n);
        feedRx(x, n);
    }
    tRx_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - a0).count();
    nSamp_ += n;
}

Engine::Engine() : t0_(std::chrono::steady_clock::now()) { wide_.setTuning(WideTuning()); }   // the whole-span banks get the CPU limit

void Engine::cpuAccounts() {
    const std::string p = "rx" + std::to_string(receiverId_) + " ";
    cpuFront_ = cpu::account(p + "front");
    cpuSpec_ = cpu::account(p + "spectrum");
    cpuDec_ = cpu::account(p + "decoder");
    cpuSec_ = cpu::account(p + "second");
}

double Engine::cpuPercent() const {
    double s = 0;
    for (int id : {cpuFront_.load(), cpuSpec_.load(), cpuDec_.load(), cpuSec_.load()}) s += cpu::percent(id);
    return s;
}

std::vector<cpu::Usage> Engine::cpuUsage() const {
    std::vector<cpu::Usage> v;
    for (int id : {cpuFront_.load(), cpuSpec_.load(), cpuDec_.load(), cpuSec_.load()}) {
        if (id > 0) v.push_back({cpu::name(id), cpu::percent(id), cpu::seconds(id)});
    }
    return v;
}
Engine::~Engine() { stop(); flushResults(); }

void Engine::stop() {
    monitor_.stop();   // the audio goes quiet with the radio (the setting stays; the next start brings it back)
    stopSecondary();   // stays asked for: the next start() brings it back
    player_.select(-1);
    if (!running_ && !src_) return;
    stopReq_ = true;
    if (th_.joinable()) th_.join();
    mtvActive_ = false;
    { std::shared_ptr<MultiTv> m; { std::lock_guard<std::mutex> lk(mtvMu_); m.swap(mtv_); } }   // the channels' receivers stop with the radio
    mtvKeyRate_ = -1;
    rec_.stop();   // a recording ends with the radio (the analysis thread is gone, nothing pushes any more)
    tm_.off();     // the rolling memory too (the next start() sets it up again)
    {
        std::lock_guard<std::mutex> lk(specQMu_);
        specStop_ = true;
    }
    specQCv_.notify_all();
    if (specTh_.joinable()) specTh_.join();
    live_ = false;
    if (src_) {
        src_->stop(); src_.reset();
        char b[160];
        double dur = nSamp_ / std::max(1.0, rate_.load());
        snprintf(b, sizeof b, "source stopped (CPU load: spectrum %.0f%%, receiver %.0f%% of real time)", 100 * tSpec_ / std::max(1e-9, dur), 100 * tRx_ / std::max(1e-9, dur));
        log(b);
        log(t2rxProfile());
        automation::receiverState("stopped", "receiver stopped");
    }
    rxA3_.stop();
    running_ = false;
    nap_.reset();
    releaseDevice(receiverId_);
}

bool Engine::syntheticSource() const {
    std::lock_guard<std::mutex> lk(tuneMu_);
    return lastDev_.kind == DeviceInfo::Synthetic;
}

bool Engine::startRecording(const std::string& path, FileFormat fmt, std::string& err) {
    DeviceInfo::Kind kind;
    { std::lock_guard<std::mutex> lk(tuneMu_); kind = lastDev_.kind; }
    if (!running_ || !src_) { err = "start the radio first"; return false; }
    if (kind == DeviceInfo::File) { err = "recording a file playback is not needed: the file is already there"; return false; }
    TuneSettings t;
    { std::lock_guard<std::mutex> lk(tuneMu_); t = lastTune_; }
    rec_.setSigmf(recSigmf_, kind == DeviceInfo::Synthetic ? 0.0 : radioTune(t).centerHz, recordingModeId(stdMode_.load()));
    return rec_.start(path, fmt, srcRate_, err);
}

std::string Engine::recordingName(FileFormat fmt) const {
    TuneSettings t;
    { std::lock_guard<std::mutex> lk(tuneMu_); t = lastTune_; }
    return recordingFileName(recordingModeId(stdMode_.load()), radioTune(t).centerHz, srcRate_, fmt);
}

double Engine::setTimeMachine(double seconds, int decimate, int memBudgetMB) {
    tmSeconds_ = seconds; tmDecim_ = decimate; tmBudgetMB_ = memBudgetMB;
    if (!running_ || !src_ || srcRate_ <= 0) return 0;
    DeviceInfo::Kind kind;
    { std::lock_guard<std::mutex> lk(tuneMu_); kind = lastDev_.kind; }
    if (kind == DeviceInfo::File) { tm_.off(); return 0; }   // a file is already there
    return tm_.configure(seconds, srcRate_, decimate, memBudgetMB);
}

std::string Engine::timeMachineName() const {
    TuneSettings t;
    { std::lock_guard<std::mutex> lk(tuneMu_); t = lastTune_; }
    return recordingFileName("rewind-" + recordingModeId(stdMode_.load()), radioTune(t).centerHz, tm_.storedRate(), FileFormat::CS8);
}

bool Engine::timeMachineSaveTo(const std::string& path, double preSeconds, double postSeconds, std::string& err) {
    TuneSettings t;
    DeviceInfo::Kind kind;
    { std::lock_guard<std::mutex> lk(tuneMu_); t = lastTune_; kind = lastDev_.kind; }
    return tm_.trigger(path, preSeconds, postSeconds, kind == DeviceInfo::Synthetic ? 0.0 : radioTune(t).centerHz, recSigmf_, err);
}

void Engine::applyReset() {
    tm_.discard();   // the history is at the old centre frequency
    std::lock_guard<std::mutex> secLk(secFeedMu_);   // the second mode's receiver is reset with the rest, never while it takes a block
    {
        std::lock_guard<std::mutex> lk(secQMu_);
        secQ_.clear(); secQN_ = 0;
    }
    secQCv_.notify_all();
    secFlush_ = true;
    ring_.clear();
    holdDelivery(300);
    if (auto m = mtvGet()) m->reset();   // multi-channel TV: the channels are found again
    rx_.reset();
    rxT_.reset();
    rxA_.reset();
    rxD_.reset();
    rxA3_.reset();
    rxI_.reset();
    rxFm_.reset();
    rxDvbs_.reset();
    rxDtmb_.reset();
    rxAtv_.reset();
    rxDmr_.reset();
    rxDrm_.reset();
    rxAdsb_.reset();
    rxGnss_.reset();
    rxSonde_.reset();
    rxAis_.reset();
    rxMarine_.reset();
    rxAcars_.reset();
    rxInmc_.reset();
    rxAero_.reset();
    rxIridium_.reset();
    rxMesh_.reset();
    rxHdr_.reset();
    rxCdr_.reset();
    rxPager_.reset();
    rxPacket_.reset();
    rxHfdig_.reset();
    rxAirband_.reset();
    rxVdl2_.reset();
    rxIsm_.reset();
    rxVor_.reset();
    rxNdb_.reset();
    rxDme_.reset();
    rxM17_.reset();
    rxHfdl_.reset();
    rxCell_.reset();
    rxCw_.reset();
    rxDstar_.reset();
    rxNxdn_.reset();
    rxDpmr_.reset();
    rxMac_.reset();
    rxP25_.reset();
    rxGsmr_.reset();
    rxTetra_.reset();
    rxPsk_.reset();
    rxJs8_.reset();
    rxSpecx_.reset();
    rxSpecan_.reset();
    rxAstro_.reset();
    rxDvbc_.reset();
    rxOrb_.reset();
    rxUplink_.reset();
    rxAle_.reset();
    rxBle_.reset();
    rxIono_.reset();
    rxQo100_.reset();
    rxWifi_.reset();
    rxGeneral_.reset();
    for (auto& g : extraGeneral_) g.reset();
    rxTime_.reset();
    rxCube_.reset();
    rxGs_.reset();
    rxVdes_.reset();
    rxZb_.reset();
    rxFlarm_.reset();
    rxHfb_.reset();
    rxDji_.reset();
    rxNb_.reset();
    rxHl_.reset();
    rxCd_.reset();
    rxWb_.reset();
    rxDw_.reset();
    rxSame_.reset();
    rxAnt_.reset();
    rxDect_.reset();
    rxTetrapol_.reset();
    rxLbj_.reset();
    rxKlw_.reset();
    rxTdcs_.reset();
    rxHorus_.reset();
    rxDgps_.reset();
    rxEloran_.reset();
    rxNfc_.reset();
    rxMt_.reset();
    rxLr_.reset();
    rxLw_.reset();
    rxLs_.reset();
    rxPac_.reset();
    rxPskd_.reset();
    rxWs_.reset();
    rxZw_.reset();
    rxEcz_.reset();
    rxFpv_.reset();
    rxWmb_.reset();
    rxTones_.reset();
    rxSar_.reset();
    rxMt63_.reset();
    rxFsq_.reset();
    rxIfkp_.reset();
    rxKaal_.reset();
    rxKait_.reset();
    rxMarineVhf_.reset();
    rxMilhf_.reset();
    rxSbas_.reset();
    rxArgos_.reset();
    rxSfx_.reset();
    rxSb_.reset();
    rxIsdbs_.reset();
    rxKaws_.reset();
    rxKapp_.reset();
    rxKavb_.reset();
    rxKaft_.reset();
    rxCq_.reset();
    rxMs_.reset();
    rxGd_.reset();
    rxRid_.reset();
    rxMeteor_.reset();
    rxNr_.reset();
    rxGoes_.reset();
    rxEl_.reset();
    rxUat_.reset();
    rxVdl4_.reset();
    rxHrpt_.reset();
    autoMark_ = nSamp_ / std::max(1.0, rate_.load()); lastLockSec_ = autoMark_;
    analyzer_.resize(specFft_.load() > 0 ? (size_t)specFft_.load() : SpectrumAnalyzer::autoSize(rate_.load()));   // finer bins for a wide capture
    analyzer_.reset();
    {
        std::lock_guard<std::mutex> lk(tsMu_);
        demux_.reset();
        unpack_ = BbUnpacker();
        unpack_.setSink([this](const uint8_t* pkt) { demux_.feed(pkt); if (!playerExternal_) { ttx_.feedTs(pkt); dvbsub_.feedTs(pkt); } frameBuf_.insert(frameBuf_.end(), pkt, pkt + 188); });
        tsSnap_ = TsSnapshot();
        lastT2Frame_ = -1;
        frameBuf_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(specMu_);
        spec_ = SpectrumFrame();
    }
    {
        std::lock_guard<std::mutex> lk(rxMu_);
        rxTel_ = RxTelemetry();
    }
    logP1Count_ = 0; logGi_ = -2; logState_ = -1; logFrameSyms_ = 0;
    resets_++;
}

void Engine::onTsPackets(const uint8_t* pk, size_t n, double secs) {
    std::lock_guard<std::mutex> lk(tsMu_);
    demux_.setGbDefault(stdMode_.load() == 9);   // DTMB
    dvbsi::setGbDefaultGlobal(stdMode_.load() == 9);
    for (size_t i = 0; i < n; i++) {
        const uint8_t* q = pk + i * 188;
        demux_.feed(q);
        if (!playerExternal_) ttx_.feedTs(q);
        if (!playerExternal_) dvbsub_.feedTs(q);
    }
    demux_.advance(secs);
    tsSnap_ = demux_.snapshot();
    outputs_.packets(pk, n, &tsSnap_);
    if (tap_) tap_(pk, n, &tsSnap_, secs);
    if (rtap_) rtap_(pk, n, &tsSnap_, secs);
    if (player_.selected() >= 0 && !playerExternal_) player_.push(pk, n, tsSnap_);
    outputs_.burstDone(secs);
}

// Auto mode alternates between the two receivers until one locks, then stays with it. Only the active receiver sees samples,
// so idle searching costs one receiver, not two.
void Engine::feedRx(const cf32* x, size_t n) { feedReceiver(activeStd_.load(), x, n); }

void Engine::setPluginSink(PluginIqSink f) {
    std::atomic_store(&pluginSink_, f ? std::make_shared<const PluginIqSink>(std::move(f)) : std::shared_ptr<const PluginIqSink>());
}

void Engine::feedReceiver(int a, const cf32* x, size_t n) {
    if (a >= 1001) {   // a plugin mode (mode_tuning.h kPluginStdBase): the plugin host takes the samples
        if (auto sink = std::atomic_load(&pluginSink_)) (*sink)(reinterpret_cast<const float*>(x), n, rate_.load());
        return;
    }
    if (a <= 1 && mtvActive_.load(std::memory_order_relaxed)) { if (auto m = mtvGet()) { m->feed(x, n); return; } }   // multi-channel TV: one receiver per channel of the span
    if (wideOn_.load(std::memory_order_relaxed) && WideModes::supports(a)) {
        if (a == 24) wide_.setPagerOptions(rxPager_.protocols(), rxPager_.pocsagBauds());   // the screens' POCSAG / FLEX / baud choice reaches every carrier
        if (a == 24 || a == 25) wide_.setNarrowChannels(rxPager_.narrowChannel(), rxPacket_.narrowChannel());   // the 12.5 / 25 kHz choice too
        if (a == 25) wide_.setPacketOptions(rxPacket_.fixBits(), rxPacket_.emphasis());                         // and fix bits / emphasis
        wide_.feed(a, x, n);
        return;
    }   // whole-span decoding: one decoder per carrier
    if (a == 5) rxI_.feed(x, n); else if (a == 4) rxA3_.feed(x, n); else if (a == 3) rxD_.feed(x, n); else if (a == 2) rxA_.feed(x, n); else if (a == 1) rxT_.feed(x, n); else if (a == 6) rxFm_.feed(x, n); else if (a == 7) rxDvbs_.feed(x, n); else if (a == 8) rxDtmb_.feed(x, n); else if (a == 9) rxAtv_.feed(x, n); else if (a == 10) rxDmr_.feed(x, n); else if (a == 11) rxDrm_.feed(x, n); else if (a == 12) rxAdsb_.feed(x, n); else if (a == 13) rxGnss_.feed(x, n); else if (a == 14) rxSonde_.feed(x, n); else if (a == 15) rxAis_.feed(x, n); else if (a == 16) rxMarine_.feed(x, n); else if (a == 17) rxAcars_.feed(x, n); else if (a == 18) rxInmc_.feed(x, n); else if (a == 19) rxAero_.feed(x, n); else if (a == 20) rxIridium_.feed(x, n); else if (a == 21) rxMesh_.feed(x, n); else if (a == 22) rxHdr_.feed(x, n); else if (a == 23) rxCdr_.feed(x, n); else if (a == 24) rxPager_.feed(x, n); else if (a == 25) rxPacket_.feed(x, n); else if (a == 26) rxHfdig_.feed(x, n); else if (a == 27) rxAirband_.feed(x, n); else if (a == 28) rxVdl2_.feed(x, n); else if (a == 29) rxIsm_.feed(x, n); else if (a == 30) rxNdb_.feed(x, n); else if (a == 31) rxVor_.feed(x, n); else if (a == 32) rxM17_.feed(x, n); else if (a == 33) rxCell_.feed(x, n); else if (a == 35) rxCw_.feed(x, n); else if (a == 34) rxDstar_.feed(x, n); else if (a == 36) rxHfdl_.feed(x, n); else if (a == 39) rxNxdn_.feed(x, n); else if (a == 46) rxDpmr_.feed(x, n); else if (a == 40) rxGsmr_.feed(x, n); else if (a == 37) rxPsk_.feed(x, n); else if (a == 38) rxJs8_.feed(x, n); else if (a == 42) rxSpecx_.feed(x, n); else if (a == 43) rxSpecan_.feed(x, n); else if (a == 41) rxDvbc_.feed(x, n); else if (a == 45) rxOrb_.feed(x, n); else if (a == 44) rxUplink_.feed(x, n); else if (a == 47) rxTetra_.feed(x, n); else if (a == 50) rxAle_.feed(x, n); else if (a == 48) rxMac_.feed(x, n); else if (a == 49) rxAstro_.feed(x, n); else if (a == 51) rxP25_.feed(x, n); else if (a == 52) rxBle_.feed(x, n); else if (a == 53) rxIono_.feed(x, n); else if (a == 54) rxQo100_.feed(x, n); else if (a == 55) rxWifi_.feed(x, n); else if (a == 56) { rxGeneral_.feed(x, n); for (int i = 0, k = extraGeneralN_.load(std::memory_order_relaxed); i < k; i++) extraGeneral_[(size_t)i].feed(x, n); } else if (a == 59) rxTime_.feed(x, n); else if (a == 57) rxRid_.feed(x, n); else if (a == 60) rxMeteor_.feed(x, n); else if (a == 58) rxNr_.feed(x, n); else if (a == 61) rxGoes_.feed(x, n); else if (a == 62) rxCube_.feed(x, n); else if (a == 63) rxGs_.feed(x, n); else if (a == 64) rxVdes_.feed(x, n); else if (a == 65) rxHrpt_.feed(x, n); else if (a == 66) rxZb_.feed(x, n); else if (a == 67) rxFlarm_.feed(x, n); else if (a == 68) rxHfb_.feed(x, n); else if (a == 69) rxDji_.feed(x, n); else if (a == 70) rxNb_.feed(x, n); else if (a == 71) rxHl_.feed(x, n); else if (a == 73) rxCd_.feed(x, n); else if (a == 74) rxWb_.feed(x, n); else if (a == 75) rxDw_.feed(x, n); else if (a == 77) rxSame_.feed(x, n); else if (a == 78) rxAnt_.feed(x, n); else if (a == 79) rxDect_.feed(x, n); else if (a == 80) rxTetrapol_.feed(x, n); else if (a == 81) rxEl_.feed(x, n); else if (a == 82) rxLbj_.feed(x, n); else if (a == railway::kKlwActiveStandard) rxKlw_.feed(x, n); else if (a == railway::kTdcsActiveStandard) rxTdcs_.feed(x, n); else if (a == horus::kActiveStandard) rxHorus_.feed(x, n); else if (a == 83) rxVdl4_.feed(x, n); else if (a == 84) rxUat_.feed(x, n); else if (a == 85) rxWs_.feed(x, n); else if (a == 86) rxEcz_.feed(x, n); else if (a == 87) rxWmb_.feed(x, n); else if (a == 102) rxFpv_.feed(x, n); else if (a == 90) rxTones_.feed(x, n); else if (a == 93) rxSar_.feed(x, n); else if (a == 96) rxMt63_.feed(x, n); else if (a == 114) rxFsq_.feed(x, n); else if (a == 117) rxIfkp_.feed(x, n); else if (a == 121) rxDgps_.feed(x, n); else if (a == 99) rxZw_.feed(x, n); else if (a == 105) rxMs_.feed(x, n); else if (a == 76) rxMt_.feed(x, n); else if (a == 72) rxLr_.feed(x, n); else if (a == 108) rxCq_.feed(x, n); else if (a == 122) rxDme_.feed(x, n); else if (a == 123) rxKaal_.feed(x, n); else if (a == 127) rxEloran_.feed(x, n); else if (a == 149) rxNfc_.feed(x, n); else if (a == 128) rxMpt_.feed(x, n); else if (a == 129) rxFusion_.feed(x, n); else if (a == 141) rxArd_.feed(x, n); else if (a == 142) rxGd_.feed(x, n); else if (a == 143) rxModeAc_.feed(x, n); else if (a == 144) rxBt_.feed(x, n); else if (a == 145) rxSb_.feed(x, n); else if (a == 124) rxKavb_.feed(x, n); else if (a == 134) rxKaws_.feed(x, n); else if (a == 146) rxMilhf_.feed(x, n); else if (a == 153) rxArgos_.feed(x, n); else if (a == 154) rxSfx_.feed(x, n); else if (a == 151) rxSbas_.feed(x, n); else if (a == 152) rxAsterix_.feed(x, n); else if (a == 130) rxLtr_.feed(x, n); else if (a == 131) rxEdacs_.feed(x, n); else if (a == 181) rxP25p2_.feed(x, n); else if (a == 156) rxMarineVhf_.feed(x, n); else if (a == 132) rxKait_.feed(x, n); else if (a == 125) rxKapp_.feed(x, n); else if (a == 126) rxKaft_.feed(x, n); else if (a == 201) rxLw_.feed(x, n); else if (a == 202) rxLs_.feed(x, n); else if (a == 155) rxIsdbs_.feed(x, n); else if (a == 204) rxPac_.feed(x, n); else if (a == 203) rxPskd_.feed(x, n); else rx_.feed(x, n);
}

void Engine::changeBandwidth(double mhz) {
    if (mhz == bwMhz_) return;
    char b[160];
    snprintf(b, sizeof b, "bandwidth: %.4g MHz -> %.4g MHz", bwMhz_, mhz);
    log(b);
    bwMhz_ = mhz; bwActive_ = mhz;
    rx_.configure(rate_, mhz);
    rxT_.configure(rate_, mhz);
    applyReset();
}

// While nothing is locked: average the spectrum, measure the width of the flat top and switch to that channel bandwidth
void Engine::bandwidthStep(const SpectrumFrame& f, const RxTelemetry& t, bool tLocked) {
    const double now = nSamp_ / std::max(1.0, rate_.load());
    if (bwReset_.exchange(false)) { bwDet_.reset(); bwMark_ = now; bwVote_ = 0; }
    if (bwReq_.exchange(false)) { changeBandwidth(bwReqVal_); bwDet_.reset(); bwMark_ = now; }
    if (!bwAuto_ || activeStd_ >= 2 || mtvActive_.load() || decodeShiftHz_.load() != 0) return;   // a shifted decode: the centre of the spectrum is not the channel   // an ATSC channel is always 6 MHz, a DAB ensemble 1.536 MHz
    const bool locked = activeStd_ == 0 ? t.state == 2 : tLocked;
    if (locked) { bwDet_.reset(); bwMark_ = now; bwVote_ = 0; return; }
    bwDet_.add(f.dbfs, rate_ / 1e6);
    if (bwDet_.frames() < 12) return;
    const BandwidthEstimate e = bwDet_.estimate();
    if (e.valid) {
        bwVote_ = e.bwMhz == bwVote_ ? bwVote_ : e.bwMhz;
        if (e.bwMhz == bwVote_ && bwDet_.frames() >= 20) {
            bwDetected_ = e.bwMhz;
            if (e.bwMhz != bwMhz_) { char b[160]; snprintf(b, sizeof b, "signal is %.2f MHz wide: %.4g MHz channel", e.occupiedMhz, e.bwMhz); log(b); changeBandwidth(e.bwMhz); }
            bwDet_.reset(); bwMark_ = now;
        }
    }
    if (now - bwMark_ > 6.0) { bwDet_.reset(); bwMark_ = now; }   // keep the average fresh
}

// A fresh start of the receiver of active standard a (the first mode switching to it, the second mode starting)
void Engine::resetReceiver(int a) {
    if (WideModes::supports(a)) wide_.reset();
    switch (a) {
    case 0: rx_.reset(); break;
    case 1: rxT_.reset(); break;
    case 2: rxA_.reset(); break;
    case 3: rxD_.reset(); break;
    case 4: rxA3_.reset(); break;
    case 5: rxI_.reset(); break;
    case 6: rxFm_.reset(); break;
    case 7: rxDvbs_.reset(); break;
    case 8: rxDtmb_.reset(); break;
    case 9: rxAtv_.reset(); break;
    case 10: rxDmr_.reset(); break;
    case 11: rxDrm_.reset(); break;
    case 12: rxAdsb_.reset(); break;
    case 13: rxGnss_.reset(); break;
    case 14: rxSonde_.reset(); break;
    case 15: rxAis_.reset(); break;
    case 16: rxMarine_.reset(); break;
    case 17: rxAcars_.reset(); break;
    case 18: rxInmc_.reset(); break;
    case 19: rxAero_.reset(); break;
    case 20: rxIridium_.reset(); break;
    case 21: rxMesh_.reset(); break;
    case 22: rxHdr_.reset(); break;
    case 23: rxCdr_.reset(); break;
    case 24: rxPager_.reset(); break;
    case 25: rxPacket_.reset(); break;
    case 26: rxHfdig_.reset(); break;
    case 27: rxAirband_.reset(); break;
    case 28: rxVdl2_.reset(); break;
    case 29: rxIsm_.reset(); break;
    case 30: rxNdb_.reset(); break;
    case 122: rxDme_.reset(); break;
    case 31: rxVor_.reset(); break;
    case 32: rxM17_.reset(); break;
    case 36: rxHfdl_.reset(); break;
    case 33: rxCell_.reset(); break;
    case 35: rxCw_.reset(); break;
    case 34: rxDstar_.reset(); break;
    case 39: rxNxdn_.reset(); break;
    case 46: rxDpmr_.reset(); break;
    case 48: rxMac_.reset(); break;
    case 51: rxP25_.reset(); break;
    case 40: rxGsmr_.reset(); break;
    case 47: rxTetra_.reset(); break;
    case 37: rxPsk_.reset(); break;
    case 38: rxJs8_.reset(); break;
    case 42: rxSpecx_.reset(); break;
    case 43: rxSpecan_.reset(); break;
    case 49: rxAstro_.reset(); break;
    case 41: rxDvbc_.reset(); break;
    case 45: rxOrb_.reset(); break;
    case 44: rxUplink_.reset(); break;
    case 50: rxAle_.reset(); break;
    case 52: rxBle_.reset(); break;
    case 53: rxIono_.reset(); break;
    case 54: rxQo100_.reset(); break;
    case 55: rxWifi_.reset(); break;
    case 56: rxGeneral_.reset(); for (auto& g : extraGeneral_) g.reset(); break;
    case 59: rxTime_.reset(); break;
    case 62: rxCube_.reset(); break;
    case 63: rxGs_.reset(); break;
    case 64: rxVdes_.reset(); break;
    case 67: rxFlarm_.reset(); break;
    case 68: rxHfb_.reset(); break;
    case 69: rxDji_.reset(); break;
    case 72: rxLr_.reset(); break;
    case 201: rxLw_.reset(); break;
    case 202: rxLs_.reset(); break;
    case 204: rxPac_.reset(); break;
    case 203: rxPskd_.reset(); break;
    case 85: rxWs_.reset(); break;
    case 99: rxZw_.reset(); break;
    case 86: rxEcz_.reset(); break;
    case 102: rxFpv_.reset(); break;
    case 87: rxWmb_.reset(); break;
    case 90: rxTones_.reset(); break;
    case 93: rxSar_.reset(); break;
    case 96: rxMt63_.reset(); break;
    case 114: rxFsq_.reset(); break;
    case 117: rxIfkp_.reset(); break;
    case 123: rxKaal_.reset(); break;
    case 132: rxKait_.reset(); break;
    case 134: rxKaws_.reset(); break;
    case 125: rxKapp_.reset(); break;
    case 124: rxKavb_.reset(); break;
    case 126: rxKaft_.reset(); break;
    case 108: rxCq_.reset(); break;
    case 105: rxMs_.reset(); break;
    case 142: rxGd_.reset(); break;
    case 143: rxModeAc_.reset(); break;
    case 144: rxBt_.reset(); break;
    case 146: rxMilhf_.reset(); break;
    case 151: rxSbas_.reset(); break;
    case 152: break;      // ASTERIX keeps its tracks over a retune
    case 153: rxArgos_.reset(); break;
    case 154: rxSfx_.reset(); break;
    case 145: rxSb_.reset(); break;
    case 155: rxIsdbs_.reset(); break;
    case 130: rxLtr_.reset(); break;
    case 131: rxEdacs_.reset(); break;
    case 156: rxMarineVhf_.reset(); break;
    case 181: rxP25p2_.reset(); break;
    case 61: rxGoes_.reset(); break;
    case 65: rxHrpt_.reset(); break;
    case 66: rxZb_.reset(); break;
    case 70: rxNb_.reset(); break;
    case 71: rxHl_.reset(); break;
    case 73: rxCd_.reset(); break;
    case 74: rxWb_.reset(); break;
    case 75: rxDw_.reset(); break;
    case 77: rxSame_.reset(); break;
    case 78: rxAnt_.reset(); break;
    case 79: rxDect_.reset(); break;
    case 80: rxTetrapol_.reset(); break;
    case 81: rxEl_.reset(); break;
    case 82: rxLbj_.reset(); break;
    case railway::kKlwActiveStandard: rxKlw_.reset(); break;
    case railway::kTdcsActiveStandard: rxTdcs_.reset(); break;
    case horus::kActiveStandard: rxHorus_.reset(); break;
    case 121: rxDgps_.reset(); break;
    case 127: rxEloran_.reset(); break;
    case 149: rxNfc_.reset(); break;
    case 128: rxMpt_.reset(); break;
    case 129: rxFusion_.reset(); break;
    case 141: rxArd_.reset(); break;
    case 84: rxUat_.reset(); break;
    case 83: rxVdl4_.reset(); break;
    case 76: rxMt_.reset(); break;
    case 57: rxRid_.reset(); break;
    case 60: rxMeteor_.reset(); break;
    case 58: rxNr_.reset(); break;
    default: break;
    }
}

void Engine::autoSelect(const RxTelemetry& t, bool tLocked) {
    const double now = nSamp_ / std::max(1.0, rate_.load());
    if (stdReq_.exchange(false)) {
        const int m = stdMode_.load();
        if (m >= 1 && activeStd_ != activeStdFor(m)) {
            if (dmbSub_ >= 0) { player_.select(-1); dmbSub_ = -1; }   // leaving DAB: the DMB picture stops with it
            activeStd_ = activeStdFor(m);
            // the second mode on this receiver stops first (secondaryLoop() checks secAct_ under secFeedMu_)
            if (secAct_.load() == activeStd_.load()) {
                std::lock_guard<std::mutex> fl(secFeedMu_);
                secAct_ = -1;
                setSecNote("suspended: the first mode is now the same receiver");
                configureReceiver(activeStd_.load(), rate_.load());
                placeReceiverAsPrimary(activeStd_.load());
            }
            resetReceiver(activeStd_.load());
        }
        autoMark_ = lastLockSec_ = now;
        return;
    }
    if (stdMode_.load() != 0) return;
    const bool locked = activeStd_ == 0 ? t.state == 2 : tLocked;
    if (locked) { lastLockSec_ = now; autoMark_ = now; return; }
    const double slice = activeStd_ == 0 ? 1.6 : 1.3;
    if (now - autoMark_ < slice) return;
    if (activeStd_ == 0) { activeStd_ = 1; rxT_.reset(); log("auto: trying DVB-T"); }
    else { activeStd_ = 0; rx_.reset(); log("auto: trying DVB-T2"); }
    autoMark_ = now;
}


} // namespace dect2
