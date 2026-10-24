// The engine's two worker threads: the spectrum thread (feedSpectrum, spectrumLoop) and the analysis thread (analysisLoop) that
// moves samples from the ring into the active receiver.
#include "dect2/engine/engine.h"
#include "engine_internal.h"
#include "dect2/source/converter.h"
#include "dect2/net/share.h"
#include "dect2/power/power.h"
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
#include "dect2/common/threadpool.h"
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

void Engine::feedSpectrum(const cf32* x, size_t n) {
    {
        std::lock_guard<std::mutex> lk(specQMu_);
        if (specQ_.size() >= 6) return;   // the display is statistical: when the spectrum thread is behind, skip a piece
        if (specFree_.empty()) specQ_.emplace_back(x, x + n);
        else { specQ_.push_back(std::move(specFree_.back())); specFree_.pop_back(); specQ_.back().assign(x, x + n); }
    }
    specQCv_.notify_one();
}

void Engine::spectrumLoop() {
    setThreadName("onair-spectrum");   // a copy of each block, dropped when behind: never slows the decoder
    setThreadPriority(ThreadPriority::High);   // the display stays smooth (macOS: a performance core)
    cpu::bindThread(cpuSpec_.load());
    std::vector<cf32> chunk;
    for (;;) {
        cpu::tick();
        {
            std::unique_lock<std::mutex> lk(specQMu_);
            specQCv_.wait(lk, [this] { return specStop_ || !specQ_.empty(); });
            if (specQ_.empty()) return;   // stopping
            chunk = std::move(specQ_.front());
            specQ_.pop_front();
        }
        const auto a0 = std::chrono::steady_clock::now();
        if (const double sh = specShiftHz_.load(); sh != 0 && rate_.load() > 0) {   // the display range moved (a sound card's audio range centred): the decoders never see this
            const double w = 2 * 3.14159265358979323846 * sh / rate_.load();
            for (cf32& v : chunk) {
                v *= cf32((float)std::cos(specPhase_), (float)std::sin(specPhase_));
                specPhase_ += w;
                if (specPhase_ > 3.14159265358979) specPhase_ -= 2 * 3.14159265358979323846;
                else if (specPhase_ < -3.14159265358979) specPhase_ += 2 * 3.14159265358979323846;
            }
        }
        analyzer_.feed(chunk.data(), chunk.size());
        tSpec_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - a0).count();
        {   // the block's storage goes back for the next one
            std::lock_guard<std::mutex> lk(specQMu_);
            if (specFree_.size() < 8) specFree_.push_back(std::move(chunk));
        }
        chunk = std::vector<cf32>();
    }
}

void Engine::analysisLoop() {
    setThreadName("onair-analysis");
    setThreadPriority(ThreadPriority::Realtime); // the sample path must never wait for decoders or the UI
    cpu::bindThread(cpuFront_.load());           // ingest() charges the receiver's part to the decoder account
    std::vector<cf32> buf(1 << 16);
    auto next = std::chrono::steady_clock::now();
    SpectrumFrame f;
    uint64_t rxSeq = 0;
    while (!stopReq_) {
        if (resetReq_.exchange(false)) { applyReset(); rxSeq = 0; }
        size_t n;
        catchUp();
        while ((n = ring_.read(buf.data(), buf.size())) > 0) { ingest(buf.data(), n); if (std::chrono::steady_clock::now() > next + std::chrono::milliseconds(250)) break; } // keep publishing spectrum/telemetry even when the receiver is behind
        // 30 spectrum / telemetry updates a second; half of that in Low power mode, a few a second while the window is hidden and the user chose to save power
        next += std::chrono::milliseconds(power::pauseWhenHidden() ? 200 : power::lowPower() ? 66 : 33);
        watchRadio();
        checkDelivery();
        syncMultiTv();
        if (const uint64_t d = ring_.dropped(); d != ringDroppedSeen_.load()) { ringDroppedSeen_ = d; cpuDropEvents_++; lastCpuDropNs_ = steadyNs(); }
        {   // the receiver's load: its time on this thread against the signal time it handled, over about two seconds
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (loadT_ == 0 || nSamp_ < loadSamp_) { loadT_ = now; loadRx_ = tRx_; loadSamp_ = nSamp_; }
            else if (now - loadT_ >= 2.0) {
                const double sig = (nSamp_ - loadSamp_) / std::max(1.0, rate_.load());
                if (sig > 0.05) loadPct_ = (float)(100.0 * (tRx_ - loadRx_) / sig);
                loadT_ = now; loadRx_ = tRx_; loadSamp_ = nSamp_;
            }
        }
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
                    publishRx(std::move(t));
                }
            } else if (activeStd_.load() == 5) {
                if (rxI_.telemetry(t, rxSeq)) {
                    rxSeq = t.seq;
                    logIsdbtEvents(t);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    publishRx(std::move(t));
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
                    publishRx(std::move(t));
                }
            } else if (activeStd_.load() == 3) {
                DabTelemetry dt;
                if (rxD_.telemetry(dt, dabSeq_)) {
                    dabSeq_ = dt.seq;
                    // a DMB video service plays like a TV service: the player follows the selected DMB sub-channel (a new one starts afresh)
                    if (dt.dmb.sub != dmbSub_) {
                        if (dmbSub_ >= 0) player_.select(-1);
                        dmbSub_ = dt.dmb.sub;
                        if (dmbSub_ >= 0) {
                            { std::lock_guard<std::mutex> lk(tsMu_); demux_.reset(); tsSnap_ = TsSnapshot(); }
                            player_.select(DmbRemux::kProgram);
                        }
                    }
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
                    publishRx(std::move(t));
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
                    publishRx(std::move(t));
                }
            } else if (mtvActive_.load() && activeStd_.load() <= 1) {
                if (auto m = mtvGet()) {
                    RxTelemetry mt;
                    if (m->poll(mt)) {
                        mt.tvChannels = m->channels();
                        std::lock_guard<std::mutex> lk(rxMu_);
                        publishRx(std::move(mt));
                    }
                }
            } else if (const int pm = pollMode(activeStd_.load(), t, modeSeq_); pm >= 0) {
                if (pm == 1) { std::lock_guard<std::mutex> lk(rxMu_); publishRx(std::move(t)); }
                if (stdReq_.load()) {   // another receiver asked for while running, without a restart (the P25 follower hopping to a Phase 2 channel)
                    const int was = activeStd_.load();
                    autoSelect(rxTel_, false);
                    if (activeStd_.load() != was) placeReceiverAsPrimary(activeStd_.load());
                }
            } else if (dvbt ? rxT_.telemetry(t, rxSeq) : rx_.telemetry(t, rxSeq)) {
                rxSeq = t.seq;
                if (dvbt) logDvbtEvents(t); else logRxEvents(t);
                autoSelect(t, dvbt && (t.dvbt.tpsOk || t.dvbt.fecSync));
                std::lock_guard<std::mutex> lk(rxMu_);
                publishRx(std::move(t));
            } else autoSelect(rxTel_, dvbt && rxTel_.dvbt.tpsOk);
        }
        if (analyzer_.takeFrame(f)) {
            {
                std::lock_guard<std::mutex> lk(specMu_);
                spec_ = f;
            }
            bandwidthStep(f, rxTel_, activeStd_.load() == 1 && rxTel_.dvbt.tpsOk);
        }
        // keep reading while we wait so the ring never fills. Between pieces the thread sleeps until about 5 ms of signal is there (the
        // radio's write wakes it) or the next telemetry tick: an idle radio costs no wake-ups, a slow one one per USB transfer.
        const size_t minN = (size_t)std::clamp(srcRate_ * 0.005, 1.0, (double)buf.size());
        while (!stopReq_) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= next) break;
            catchUp();
            size_t m = ring_.read(buf.data(), buf.size());
            if (m) ingest(buf.data(), m);
            else ring_.waitData(minN, std::chrono::duration_cast<std::chrono::microseconds>(next - now));
        }
    }
}

}   // namespace dect2
