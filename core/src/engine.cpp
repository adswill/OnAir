#include "dect2/engine.h"
#include "dect2/dvbt.h"
#include "dect2/isdbt.h"
#include "dect2/modes.h"
#include "dect2/t2rx.h"
#include <chrono>
#include "dect2/platform.h"
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

// A radio that delivers floating-point samples (LimeSDR, SoapySDR, a cf32 recording) can hand over NaN or infinite values after a
// glitch or with mismatched firmware. They would poison every filter, FFT and gain loop after them, so they become silence here.
static void dropNonFinite(cf32* x, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!std::isfinite(x[i].real()) || !std::isfinite(x[i].imag())) x[i] = cf32(0.f, 0.f);
}

// The receiver that runs for an engine standard code (setStandard): 0 and 1 are DVB-T2, 2 DVB-T, ... so it is the code minus one
static int activeStdFor(int stdMode) { return stdMode >= 2 ? stdMode - 1 : 0; }

Engine::Engine() : t0_(std::chrono::steady_clock::now()) {}
Engine::~Engine() { stop(); }

void Engine::log(const std::string& line) {
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    char buf[32];
    snprintf(buf, sizeof buf, "%7.2fs ", t);
    std::lock_guard<std::mutex> lk(logMu_);
    log_.push_back(buf + line);
    if (log_.size() > 5000) log_.erase(log_.begin(), log_.begin() + 1000);
}

std::string Engine::loadProfile() const {
    char b[200];
    snprintf(b, sizeof b, "analysis thread: receiver %.2f s for %.2f s of signal processed (the spectrum thread used %.2f s)", tRx_, nSamp_ / std::max(1.0, rate_.load()), tSpec_);
    return b;
}

std::vector<std::string> Engine::logSnapshot(size_t& total) {
    std::lock_guard<std::mutex> lk(logMu_);
    total = log_.size();
    return log_;
}

void Engine::clearLog() {
    std::lock_guard<std::mutex> lk(logMu_);
    log_.clear();
}

bool Engine::start(const DeviceInfo& dev, const TuneSettings& tune, const FileOptions& file) {
    stop();
    ring_.clear();
    std::string err;
    if (dev.kind == DeviceInfo::File)
        src_ = makeFileSource(file.path, file.format, file.sampleRate, file.loop);
    else
        src_ = makeSource(dev);
    if (!src_) { log("no source"); return false; }
    if (!src_->start(tune, ring_, err)) {
        log("source start failed: " + err);
        src_.reset();
        return false;
    }
    if (!err.empty()) log("note: " + err);
    rate_ = src_->sampleRate();
    {
        std::lock_guard<std::mutex> lk(tuneMu_);
        lastDev_ = dev; lastTune_ = tune;
    }
    radioLost_ = false; lastSamples_ = std::chrono::steady_clock::now(); reconnectErr_.clear();
    logP1Count_ = 0; logGi_ = -2; logState_ = -1; logFrameSyms_ = 0;
    bwMhz_ = tune.bandwidthMhz; bwActive_ = bwMhz_;
    bwDetected_ = 0; bwReset_ = true; bwReq_ = false;
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
    rxI_.configure(rate_);
    rxI_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });
    rxFm_.configure(rate_);
    if (stdMode_.load() == 7 && !rxFm_.ready()) log("FM needs a sample rate of at least 500 ksps");
    rxDvbs_.configure(rate_);
    rxDvbs_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });   // runs on the receiver thread
    rxDvbs_.setLogCallback([this](const std::string& s) { log(s); });
    rxDtmb_.configure(rate_);
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
    if (const ModeTuning* mt = modeTuning(stdMode_.load()))
        if (rate_.load() < mt->minSampleRate - 1) {
            char m[200];
            snprintf(m, sizeof m, "sample rate %.2f Msps is too low for %s (at least %.2f Msps is needed) - the receiver cannot work with this radio", rate_.load() / 1e6, mt->name, mt->minSampleRate / 1e6);
            log(m);
        }
    logIMode_ = logIGi_ = logITmcc_ = -1; logISync_ = false;
    rxA3_.configure(rate_);
    rxA3_.setBlocking(!src_->realtimeHardware());
    rxA3_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });   // runs on the ATSC 3.0 worker thread
    atsc3Seq_ = 0; logA3State_ = -1; logA3Svc_ = false;
    { std::lock_guard<std::mutex> lk(atsc3Mu_); atsc3Tel_ = Atsc3Telemetry(); }
    dabSeq_ = 0; logDState_ = -1; logDEns_ = false;
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
        unpack_.setSink([this](const uint8_t* pkt) { demux_.feed(pkt); ttx_.feedTs(pkt); frameBuf_.insert(frameBuf_.end(), pkt, pkt + 188); });
    }
    rx_.setPlpCallback([this](const PlpResult& r) { if (plpDump_) plpDump_(r); onPlp(r); });
    {
        RxTelemetry t;
        if (!rx_.telemetry(t, 0)) {}
        if (t.seq && !t.rateOk) log("sample rate is not 1x or 2x the native rate for this bandwidth - only spectrum available");
    }
    if (stdMode_ == 5 && rate_.load() < 6.5e6) {
        char m[200];
        snprintf(m, sizeof m, "sample rate %.2f Msps is too low for an ATSC 3.0 channel (about 6.5 Msps or more is needed) - the receiver cannot lock with this radio", rate_.load() / 1e6);
        log(m);
    }
    if (stdMode_ == 6 && rate_.load() < 6.0e6) {
        char m[200];
        snprintf(m, sizeof m, "sample rate %.2f Msps is too low for an ISDB-T channel (about 6 Msps or more is needed) - the receiver cannot lock with this radio", rate_.load() / 1e6);
        log(m);
    }
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
    running_ = true;
    specStop_ = false;
    specQ_.clear();
    specTh_ = std::thread([this] { spectrumLoop(); });
    th_ = std::thread([this] { analysisLoop(); });
    return true;
}

void Engine::stop() {
    player_.select(-1);
    if (!running_ && !src_) return;
    stopReq_ = true;
    if (th_.joinable()) th_.join();
    {
        std::lock_guard<std::mutex> lk(specQMu_);
        specStop_ = true;
    }
    specQCv_.notify_all();
    if (specTh_.joinable()) specTh_.join();
    if (src_) {
        src_->stop(); src_.reset();
        char b[160];
        double dur = nSamp_ / std::max(1.0, rate_.load());
        snprintf(b, sizeof b, "source stopped (CPU load: spectrum %.0f%%, receiver %.0f%% of real time)", 100 * tSpec_ / std::max(1e-9, dur), 100 * tRx_ / std::max(1e-9, dur));
        log(b);
        log(t2rxProfile());
    }
    rxA3_.stop();
    running_ = false;
}

bool Engine::retune(const TuneSettings& tune) {
    if (!src_) return false;
    std::string err;
    if (!src_->retune(tune, err)) { log("retune failed: " + err); return false; }
    { std::lock_guard<std::mutex> lk(tuneMu_); lastTune_ = tune; }
    return true;
}

bool Engine::retuneReset(const TuneSettings& tune) {
    if (!src_) return false;
    std::string err;
    if (!src_->retune(tune, err)) { log("retune failed: " + err); return false; }
    { std::lock_guard<std::mutex> lk(tuneMu_); lastTune_ = tune; }
    resetReq_ = true;
    return true;
}

void Engine::applyReset() {
    ring_.clear();
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
    autoMark_ = nSamp_ / std::max(1.0, rate_.load()); lastLockSec_ = autoMark_;
    analyzer_.reset();
    {
        std::lock_guard<std::mutex> lk(tsMu_);
        demux_.reset();
        unpack_ = BbUnpacker();
        unpack_.setSink([this](const uint8_t* pkt) { demux_.feed(pkt); ttx_.feedTs(pkt); frameBuf_.insert(frameBuf_.end(), pkt, pkt + 188); });
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
    for (size_t i = 0; i < n; i++) {
        const uint8_t* q = pk + i * 188;
        demux_.feed(q);
        ttx_.feedTs(q);
    }
    demux_.advance(secs);
    tsSnap_ = demux_.snapshot();
    outputs_.packets(pk, n, &tsSnap_);
    if (tap_) tap_(pk, n, &tsSnap_);
    if (player_.selected() >= 0) player_.push(pk, n, tsSnap_);
    outputs_.burstDone(secs);
}

// Auto mode alternates between the two receivers until one locks, then stays with it. Only the active receiver sees samples,
// so idle searching costs one receiver, not two.
void Engine::feedRx(const cf32* x, size_t n) {
    const int a = activeStd_.load();
    if (a == 5) rxI_.feed(x, n); else if (a == 4) rxA3_.feed(x, n); else if (a == 3) rxD_.feed(x, n); else if (a == 2) rxA_.feed(x, n); else if (a == 1) rxT_.feed(x, n); else if (a == 6) rxFm_.feed(x, n); else if (a == 7) rxDvbs_.feed(x, n); else if (a == 8) rxDtmb_.feed(x, n); else if (a == 9) rxAtv_.feed(x, n); else if (a == 10) rxDmr_.feed(x, n); else if (a == 11) rxDrm_.feed(x, n); else if (a == 12) rxAdsb_.feed(x, n); else rx_.feed(x, n);
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
    if (!bwAuto_ || activeStd_ >= 2) return;   // an ATSC channel is always 6 MHz, a DAB ensemble 1.536 MHz
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

void Engine::autoSelect(const RxTelemetry& t, bool tLocked) {
    const double now = nSamp_ / std::max(1.0, rate_.load());
    if (stdReq_.exchange(false)) {
        const int m = stdMode_.load();
        if (m >= 1 && activeStd_ != activeStdFor(m)) {
            activeStd_ = activeStdFor(m);
            switch (activeStd_.load()) {
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
            default: break;
            }
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

void Engine::logDabEvents(const RxTelemetry& t) {
    const DabTelemetry& d = t.dab;
    if (d.state != logDState_) {
        if (d.state == 2) { char b[120]; snprintf(b, sizeof b, "DAB: frame sync, carrier offset %+.0f Hz, SNR %.1f dB", d.cfoHz, d.snrDb); log(b); }
        else if (logDState_ == 2) log("DAB: sync lost");
        logDState_ = d.state;
    }
    if (d.ensemble && !logDEns_) {
        char b[160];
        snprintf(b, sizeof b, "DAB: ensemble \"%s\", %d services", d.ensembleLabel.c_str(), d.services);
        log(b);
        logDEns_ = true;
    } else if (!d.ensemble) logDEns_ = false;
}

void Engine::logAtsc3Events(const Atsc3Telemetry& a) {
    const int st = a.locked ? 2 : a.bootstraps > 0 ? 1 : 0;
    if (st != logA3State_) {
        char b[200];
        if (st == 2) snprintf(b, sizeof b, "ATSC 3.0: frames decoding, carrier offset %.0f Hz", a.cfoHz);
        else if (st == 1) snprintf(b, sizeof b, "ATSC 3.0: bootstrap found, waiting for a frame that decodes");
        else snprintf(b, sizeof b, "ATSC 3.0: searching for a bootstrap");
        log(b);
        logA3State_ = st;
    }
    const bool svc = a.serviceReady;
    if (svc != logA3Svc_) {
        logA3Svc_ = svc;
        if (svc) { char b[160]; snprintf(b, sizeof b, "ATSC 3.0: signaling of service %d received", a.selected); log(b); }
    }
}

void Engine::logAtscEvents(const RxTelemetry& t) {
    const AtscTelemetry& a = t.atsc;
    const int level = a.tsOk ? 4 : a.fieldSync ? 3 : a.segSync ? 2 : a.pilot ? 1 : 0;
    if (level != logAState_) {
        static const char* nm[] = {"ATSC: searching for the pilot and segment sync", "ATSC: pilot locked", "ATSC: segment sync, symbol clock locked", "ATSC: field sync found", "ATSC: transport stream flowing"};
        log(nm[level]);
        if (level >= 3) { char b[120]; snprintf(b, sizeof b, "ATSC: carrier offset %+.0f Hz, symbol clock %+.1f ppm, SNR %.1f dB", a.cfoHz, a.sroPpm, a.snrDb); log(b); }
        logAState_ = level;
    }
}

void Engine::logIsdbtEvents(const RxTelemetry& t) {
    char b[240];
    if (t.fftN && (t.fftN != logIMode_ || t.giIdx != logIGi_)) {
        snprintf(b, sizeof b, "ISDB-T found: mode %d (%s carrier spacing), guard interval %s, CFO %+.1f Hz", t.isdbt.mode ? t.isdbt.mode : (t.fftN == 2048 ? 1 : t.fftN == 4096 ? 2 : 3),
                 t.fftN == 2048 ? "4 kHz" : t.fftN == 4096 ? "2 kHz" : "1 kHz", isdbt::guardName(t.giIdx), t.cfoHz);
        log(b);
        logIMode_ = t.fftN; logIGi_ = t.giIdx;
    }
    const int tm = t.isdbt.tmccOk ? 1 : 0;
    if (tm != logITmcc_) {
        if (tm) {
            std::string s;
            for (int i = 0; i < 3; i++) {
                const auto& L = t.isdbt.layer[i];
                if (!L.segments) continue;
                snprintf(b, sizeof b, "%s%c: %d segment%s %s %s", s.empty() ? "" : ", ", 'A' + i, L.segments, L.segments == 1 ? "" : "s", isdbt::modName(L.mod), isdbt::rateName(L.rate));
                s += b;
            }
            snprintf(b, sizeof b, "TMCC decoded: %s%s", s.c_str(), t.isdbt.partial ? " (with a one-segment layer)" : "");
            log(b);
        } else if (logITmcc_ == 1) log("TMCC lost");
        logITmcc_ = tm;
    }
    bool sync = false;
    for (int i = 0; i < 3; i++) sync |= t.isdbt.layer[i].synced;
    if (sync != logISync_) {
        log(sync ? "transport stream locked (Viterbi + Reed-Solomon)" : "transport stream sync lost");
        logISync_ = sync;
    }
}

void Engine::logDvbtEvents(const RxTelemetry& t) {
    char b[200];
    if (t.fftN && (t.fftN != logTMode_ || t.giIdx != logTGi_)) {
        snprintf(b, sizeof b, "DVB-T found: %s, guard interval %s, CFO %+.1f Hz", t.fftN == 8192 ? "8K" : "2K", dvbt::guardName(t.giIdx), t.cfoHz);
        log(b);
        logTMode_ = t.fftN; logTGi_ = t.giIdx;
    }
    const int tps = t.dvbt.tpsOk ? 1 : 0;
    if (tps != logTTps_) {
        if (tps) {
            snprintf(b, sizeof b, "TPS decoded: %s, code rate %s, cell id %d", dvbt::modName(t.dvbt.mod), dvbt::rateName(t.dvbt.crHp), t.dvbt.cellId);
            log(b);
        } else if (logTTps_ == 1) log("TPS lost");
        logTTps_ = tps;
    }
    if (t.dvbt.fecSync != logTFec_) {
        log(t.dvbt.fecSync ? "transport stream locked (Viterbi + Reed-Solomon)" : "transport stream sync lost");
        logTFec_ = t.dvbt.fecSync;
    }
}

namespace {
// How long the analysis thread waits for, and holds, the transport-stream lock while it hands a decoded frame on (see engineWaitProfile()).
std::atomic<uint64_t> gPlpFrames{0}, gPlpWaitUs{0}, gPlpHoldUs{0}, gPlpPushUs{0};
inline uint64_t usSince(std::chrono::steady_clock::time_point t) { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count(); }
}

std::string engineWaitProfile() {
    char b[200];
    snprintf(b, sizeof b, "analysis thread, per decoded frame: lock wait %.2f ms, lock held %.2f ms (player push %.2f ms); %llu frames", 1e-3 * gPlpWaitUs / std::max<uint64_t>(1, gPlpFrames), 1e-3 * gPlpHoldUs / std::max<uint64_t>(1, gPlpFrames), 1e-3 * gPlpPushUs / std::max<uint64_t>(1, gPlpFrames), (unsigned long long)gPlpFrames.load());
    return b;
}

void Engine::onPlp(const PlpResult& r) {
    if (getenv("DECT2_FRAMESTATS")) fprintf(stderr, "[frame] t2 %d: blocks %d ok %d bad %d MER %.1f preBER %.3f iters %.1f\n", r.t2Frame, r.blocks, r.blocksOk, r.bchFailed, r.merDb, r.preBer, r.avgLdpcIters);
    const auto tWait0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(tsMu_);
    gPlpWaitUs += usSince(tWait0);
    struct HoldTimer { std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now(); ~HoldTimer() { gPlpHoldUs += usSince(t0); gPlpFrames++; } } holdTimer;
    frameBuf_.clear();
    if (lastT2Frame_ >= 0 && ((r.t2Frame - lastT2Frame_) & 0xFF) != 1) { unpack_.lost(); demux_.markLoss(); }
    lastT2Frame_ = r.t2Frame;
    for (const auto& f : r.frames) {
        if (f.bits.empty()) { unpack_.lost(); demux_.markLoss(); continue; }
        unpack_.push(f.bits, f.header);
    }
    demux_.advance(r.frameSec);
    tsSnap_ = demux_.snapshot();
    outputs_.packets(frameBuf_.data(), frameBuf_.size() / 188, &tsSnap_);
    if (tap_) tap_(frameBuf_.data(), frameBuf_.size() / 188, &tsSnap_);
    if (player_.selected() >= 0) { const auto tp = std::chrono::steady_clock::now(); player_.push(frameBuf_.data(), frameBuf_.size() / 188, tsSnap_); gPlpPushUs += usSince(tp); }
    frameBuf_.clear();
    outputs_.burstDone(r.frameSec);
}

void Engine::setPacketTap(std::function<void(const uint8_t*, size_t, const TsSnapshot*)> f) { std::lock_guard<std::mutex> lk(tsMu_); tap_ = std::move(f); }
TsSnapshot Engine::tsSnapshot() { std::lock_guard<std::mutex> lk(tsMu_); return tsSnap_; }
std::map<int, std::vector<EpgEvent>> Engine::epg() {
    std::lock_guard<std::mutex> lk(tsMu_);
    return demux_.epg();
}

BbStats Engine::bbStats() { std::lock_guard<std::mutex> lk(tsMu_); return unpack_.stats(); }

bool Engine::latestRx(RxTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(rxMu_);
    if (rxTel_.seq <= lastSeq) return false;
    out = rxTel_;
    return true;
}

void Engine::logRxEvents(const RxTelemetry& t) {
    char b[200];
    if (t.p1Count != logP1Count_ && t.p1Count > 0) {
        bool first = logP1Count_ == 0 || t.p1.s2field1 != logS2_ || t.p1.s1 != logS1_;
        if (first) {
            snprintf(b, sizeof b, "P1 found: %s, S2 FFT %s%s, CFO %+.1f Hz, conf %.2f", s1Name(t.p1.s1),
                     fftModeFromS2(t.p1.s2field1) ? fftModeFromS2(t.p1.s2field1)->name : "?", t.p1.mixed ? " (mixed)" : "",
                     t.p1.cfoHz, t.p1.conf);
            log(b);
        }
        logP1Count_ = t.p1Count;
        logS2_ = t.p1.s2field1;
        logS1_ = t.p1.s1;
    }
    if (t.giIdx != logGi_) {
        if (t.giIdx >= 0) {
            snprintf(b, sizeof b, "guard interval %s detected (margin %.2fx), %d symbols locked", guardName(t.giIdx), t.giMargin, (int)t.symbols);
            log(b);
        }
        logGi_ = t.giIdx;
    }
    if (t.state != logState_) {
        static const char* nm[] = {"searching for P1", "waiting for guard interval", "locked"};
        log(std::string("sync: ") + nm[t.state]);
        logState_ = t.state;
    }
    if (t.symbolsPerFrame != logFrameSyms_ && t.symbolsPerFrame > 0) {
        snprintf(b, sizeof b, "frame length %.2f ms = %d symbols after P1, SRO %+.1f ppm", t.frameMs, t.symbolsPerFrame, t.sroPpm);
        log(b);
        logFrameSyms_ = t.symbolsPerFrame;
    }
}

bool Engine::latestSpectrum(SpectrumFrame& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(specMu_);
    if (spec_.seq <= lastSeq || spec_.dbfs.empty()) return false;
    out = spec_;
    return true;
}

// A radio that was delivering samples and stops (the cable was pulled) is reported, and a HackRF is opened again with the settings in use
// as soon as it is back, so that the receiver carries on by itself.
void Engine::watchRadio() {
    if (!src_ || !src_->realtimeHardware()) return;
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (!radioLost_) {
        if (now - lastSamples_ > seconds(2)) {
            radioLost_ = true;
            nextReconnect_ = now + seconds(1);
            log(lastDev_.kind == DeviceInfo::HackRF ? "radio stopped sending samples (unplugged?) - waiting for it to come back" : "radio stopped sending samples - press Stop and Start to open it again");
        }
        return;
    }
    if (lastDev_.kind != DeviceInfo::HackRF || now < nextReconnect_) return;
    nextReconnect_ = now + seconds(2);
    TuneSettings tune;
    { std::lock_guard<std::mutex> lk(tuneMu_); tune = lastTune_; }
    src_->stop();
    std::string err;
    if (src_->start(tune, ring_, err)) {
        radioLost_ = false;
        lastSamples_ = steady_clock::now();
        reconnectErr_.clear();
        resetReq_ = true;   // the receiver starts again from the new samples
        log("radio reconnected");
    } else if (err != reconnectErr_) {
        reconnectErr_ = err;
        log("radio not available yet: " + err);
    }
}

// The receiver cannot keep up (the ring is more than half backlog): jump to the newest samples in one go. One clean gap that the receiver
// resynchronises after is far better than the radio thread dropping a few samples out of every chunk, which ruins every frame.
// Only for live radios: a recording or the synthetic signal waits for a slow receiver instead (see the paced source), and must never lose samples.
void Engine::catchUp() {
    if (!src_ || !src_->realtimeHardware()) return;
    const size_t skipped = ring_.dropBacklog();
    if (!skipped) return;
    if (activeStd_.load() == 0) rx_.markGap(skipped);
    skippedSamples_ += skipped;
    skipEvents_++;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastSkipLog_ > std::chrono::seconds(10)) {
        lastSkipLog_ = now;
        char b[200];
        snprintf(b, sizeof b, "the receiver cannot keep up with the sample rate: skipped %.2f s of signal (%llu skips so far). Close other programs, use a narrower channel or a lower sample rate", skipped / std::max(1.0, rate_.load()), (unsigned long long)skipEvents_);
        log(b);
    }
}

void Engine::feedSpectrum(const cf32* x, size_t n) {
    {
        std::lock_guard<std::mutex> lk(specQMu_);
        if (specQ_.size() >= 6) return;   // the display is statistical: when the spectrum thread is behind, skip a piece
        specQ_.emplace_back(x, x + n);
    }
    specQCv_.notify_one();
}

void Engine::spectrumLoop() {
    std::vector<cf32> chunk;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(specQMu_);
            specQCv_.wait(lk, [this] { return specStop_ || !specQ_.empty(); });
            if (specQ_.empty()) return;   // stopping
            chunk = std::move(specQ_.front());
            specQ_.pop_front();
        }
        const auto a0 = std::chrono::steady_clock::now();
        analyzer_.feed(chunk.data(), chunk.size());
        tSpec_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - a0).count();
    }
}

void Engine::analysisLoop() {
    setThreadPriority(ThreadPriority::Realtime); // the sample path must never wait for decoders or the UI
    std::vector<cf32> buf(1 << 16);
    auto next = std::chrono::steady_clock::now();
    SpectrumFrame f;
    uint64_t rxSeq = 0;
    while (!stopReq_) {
        if (resetReq_.exchange(false)) { applyReset(); rxSeq = 0; }
        size_t n;
        catchUp();
        while ((n = ring_.read(buf.data(), buf.size())) > 0) { dropNonFinite(buf.data(), n); auto a0 = std::chrono::steady_clock::now(); lastSamples_ = a0; feedSpectrum(buf.data(), n); feedRx(buf.data(), n); auto a2 = std::chrono::steady_clock::now(); tRx_ += std::chrono::duration<double>(a2 - a0).count(); nSamp_ += n; if (std::chrono::steady_clock::now() > next + std::chrono::milliseconds(250)) break; } // keep publishing spectrum/telemetry even when the receiver is behind
        next += std::chrono::milliseconds(33);
        watchRadio();
        {
            RxTelemetry t;
            const bool dvbt = activeStd_.load() == 1;
            if (activeStd_.load() == 2) {
                AtscTelemetry at;
                if (rxA_.telemetry(at, atscSeq_)) {
                    atscSeq_ = at.seq;
                    t.standard = 2;
                    t.atsc = at;
                    t.seq = at.seq;
                    t.state = at.fieldSync ? 2 : at.segSync ? 1 : 0;
                    t.cfoHz = at.cfoHz; t.sroPpm = at.sroPpm;
                    t.dataValid = at.fieldSync && at.eqTrained;
                    t.dataSnrDb = (float)at.snrDb;
                    t.blocksOk = at.rsClean + at.rsCorrected;
                    t.blocksBad = at.rsFailed;
                    t.rateOk = rxA_.rateOk();
                    logAtscEvents(t);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (activeStd_.load() == 5) {
                if (rxI_.telemetry(t, rxSeq)) {
                    rxSeq = t.seq;
                    logIsdbtEvents(t);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (activeStd_.load() == 4) {
                Atsc3Telemetry at;
                if (rxA3_.telemetry(at, atsc3Seq_)) {
                    atsc3Seq_ = at.seq;
                    t.standard = 4;
                    t.seq = at.seq;
                    t.state = at.locked ? 2 : at.bootstraps > 0 ? 1 : 0;
                    t.cfoHz = at.cfoHz;
                    t.dataValid = at.locked && at.serviceReady;
                    t.blocksOk = at.bbPackets - at.bbBad; t.blocksBad = at.bbBad;
                    t.rateOk = true;
                    logAtsc3Events(at);
                    {
                        std::lock_guard<std::mutex> lk(atsc3Mu_);
                        atsc3Tel_ = at;
                    }
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (activeStd_.load() == 3) {
                DabTelemetry dt;
                if (rxD_.telemetry(dt, dabSeq_)) {
                    dabSeq_ = dt.seq;
                    t.standard = 3;
                    t.seq = dt.seq;
                    t.state = dt.state;
                    t.cfoHz = dt.cfoHz;
                    t.dataValid = dt.ensemble && dt.ficRecentOk > 0;
                    t.dataSnrDb = (float)dt.snrDb;
                    t.blocksOk = dt.fibOk; t.blocksBad = dt.fibBad;
                    t.rateOk = true;
                    t.dab = std::move(dt);
                    logDabEvents(t);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (activeStd_.load() == 6) {
                FmTelemetry ft;
                if (rxFm_.telemetry(ft, fmSeq_)) {
                    fmSeq_ = ft.seq;
                    t.standard = 6;
                    t.seq = ft.seq;
                    t.state = ft.state;
                    t.cfoHz = ft.cfoHz;
                    t.dataValid = ft.state >= 1;
                    t.dataSnrDb = ft.snrDb;
                    t.rateOk = true;
                    t.fm = std::move(ft);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (activeStd_.load() == 7) {
                DvbsTelemetry mt;
                if (rxDvbs_.telemetry(mt, modeSeq_[0])) {
                    modeSeq_[0] = mt.seq;
                    t.standard = 7;
                    t.seq = mt.seq;
                    t.state = mt.state;
                    t.cfoHz = mt.cfoHz;
                    t.dataValid = mt.dataValid;
                    t.dataSnrDb = mt.snrDb;
                    t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad;
                    t.rateOk = rxDvbs_.ready();
                    t.dvbs = std::move(mt);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (activeStd_.load() == 8) {
                DtmbTelemetry mt;
                if (rxDtmb_.telemetry(mt, modeSeq_[1])) {
                    modeSeq_[1] = mt.seq;
                    t.standard = 8;
                    t.seq = mt.seq;
                    t.state = mt.state;
                    t.cfoHz = mt.cfoHz;
                    t.dataValid = mt.dataValid;
                    t.dataSnrDb = mt.snrDb;
                    t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad;
                    t.rateOk = rxDtmb_.ready();
                    t.dtmb = std::move(mt);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (activeStd_.load() == 9) {
                AtvTelemetry mt;
                if (rxAtv_.telemetry(mt, modeSeq_[2])) {
                    modeSeq_[2] = mt.seq;
                    t.standard = 9;
                    t.seq = mt.seq;
                    t.state = mt.state;
                    t.cfoHz = mt.cfoHz;
                    t.dataValid = mt.dataValid;
                    t.dataSnrDb = mt.snrDb;
                    t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad;
                    t.rateOk = rxAtv_.ready();
                    t.atv = std::move(mt);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (activeStd_.load() == 10) {
                DmrTelemetry mt;
                if (rxDmr_.telemetry(mt, modeSeq_[3])) {
                    modeSeq_[3] = mt.seq;
                    t.standard = 10;
                    t.seq = mt.seq;
                    t.state = mt.state;
                    t.cfoHz = mt.cfoHz;
                    t.dataValid = mt.dataValid;
                    t.dataSnrDb = mt.snrDb;
                    t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad;
                    t.rateOk = rxDmr_.ready();
                    t.dmr = std::move(mt);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (activeStd_.load() == 11) {
                DrmTelemetry mt;
                if (rxDrm_.telemetry(mt, modeSeq_[4])) {
                    modeSeq_[4] = mt.seq;
                    t.standard = 11;
                    t.seq = mt.seq;
                    t.state = mt.state;
                    t.cfoHz = mt.cfoHz;
                    t.dataValid = mt.dataValid;
                    t.dataSnrDb = mt.snrDb;
                    t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad;
                    t.rateOk = rxDrm_.ready();
                    t.drm = std::move(mt);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (activeStd_.load() == 12) {
                AdsbTelemetry mt;
                if (rxAdsb_.telemetry(mt, modeSeq_[5])) {
                    modeSeq_[5] = mt.seq;
                    t.standard = 12;
                    t.seq = mt.seq;
                    t.state = mt.state;
                    t.cfoHz = mt.cfoHz;
                    t.dataValid = mt.dataValid;
                    t.dataSnrDb = mt.snrDb;
                    t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad;
                    t.rateOk = rxAdsb_.ready();
                    t.adsb = std::move(mt);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    rxTel_ = std::move(t);
                }
            } else if (dvbt ? rxT_.telemetry(t, rxSeq) : rx_.telemetry(t, rxSeq)) {
                rxSeq = t.seq;
                if (dvbt) logDvbtEvents(t); else logRxEvents(t);
                autoSelect(t, dvbt && (t.dvbt.tpsOk || t.dvbt.fecSync));
                std::lock_guard<std::mutex> lk(rxMu_);
                rxTel_ = std::move(t);
            } else autoSelect(rxTel_, dvbt && rxTel_.dvbt.tpsOk);
        }
        if (analyzer_.takeFrame(f)) {
            {
                std::lock_guard<std::mutex> lk(specMu_);
                spec_ = f;
            }
            bandwidthStep(f, rxTel_, activeStd_.load() == 1 && rxTel_.dvbt.tpsOk);
        }
        // keep reading while we wait so the ring never fills
        while (std::chrono::steady_clock::now() < next && !stopReq_) {
            catchUp();
            size_t m = ring_.read(buf.data(), buf.size());
            if (m) { dropNonFinite(buf.data(), m); auto a0 = std::chrono::steady_clock::now(); lastSamples_ = a0; feedSpectrum(buf.data(), m); feedRx(buf.data(), m); auto a2 = std::chrono::steady_clock::now(); tRx_ += std::chrono::duration<double>(a2 - a0).count(); nSamp_ += m; }
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

} // namespace dect2
