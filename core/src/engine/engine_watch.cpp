// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Watching the radio and the sample path: unplugged-radio detection and reconnect, catching up after a stall, and the
// delivery/sample-loss accounting that feeds the status bar.
#include "dect2/engine/engine.h"
#include "dect2/common/diag.h"
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

// A radio that was delivering samples and stops (the cable was pulled) is reported, and a HackRF is opened again with the settings in use
// as soon as it is back, so that the receiver carries on by itself. The other radios (native_common.h: RTL-SDR, Airspy, rtl_tcp and the
// other network radios) reopen themselves: when their samples come back, the loss is cleared here.
void Engine::watchRadio() {
    if (!src_ || !src_->realtimeHardware()) return;
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (!radioLost_) {
        if (now - lastSamples_ > seconds(2)) {
            radioLost_ = true;
            automation::receiverState("dropped", "the radio stopped sending samples");
            nextReconnect_ = now + seconds(1);
            lostSince_ = lastSamples_;
            DIAG(805884, "The radio stopped sending samples for more than 2 seconds, so OnAir waits for it to come back (or reopens it).");
            // a HackRF is reopened below and the native radios reopen themselves; a SoapySDR (SoapyRemote too), Hamlib or audio radio does not
            const bool reopens = lastDev_.kind == DeviceInfo::HackRF || lastDev_.kind == DeviceInfo::Native;
            log(!reopens ? "radio stopped sending samples - press Stop and Start to open it again"
                : lastNetwork_.load() ? "network radio stopped sending samples - waiting for its server to come back"
                : "radio stopped sending samples (unplugged?) - waiting for it to come back");
        }
        return;
    }
    // 3.5 (Android): a USB radio that has been silent for 8 s although its own driver should have reopened it (a phone whose USB stream died and
    // did not come back, a frozen spectrum) is stopped and started again from here, like a HackRF
    bool restartHere = lastDev_.kind == DeviceInfo::HackRF;
#ifdef __ANDROID__
    if (lastDev_.kind == DeviceInfo::Native && !lastNetwork_.load() && now - lostSince_ > seconds(8)) {
        if (!restartHere && now >= nextReconnect_) log("the radio has been silent for 8 s: restarting its stream");
        restartHere = true;
    }
#endif
    if (!restartHere || (lastDev_.kind != DeviceInfo::HackRF && now - lastSamples_ < milliseconds(500))) {
        // the source reopened the radio or its link by itself, or a stall shorter than the source's own timeout ended (rtl_tcp: 3 s)
        if (now - lastSamples_ < milliseconds(500)) {
            radioLost_ = false;
            holdDelivery(1000);   // the reconnect gap is not a loss of the link
            // no receiver reset: the decoders resynchronise on the new samples by themselves (as after a catch-up jump), and what they
            // already know (Meshtastic nodes, ADS-B aircraft, the DAB ensemble...) stays, instead of being found and announced again
            char b[96];
            snprintf(b, sizeof b, "radio is sending samples again, after %.1f s", duration<double>(lastSamples_ - lostSince_).count());
            log(b);
            automation::receiverState("reconnected", "the radio is back");
        }
        return;
    }
    if (now < nextReconnect_) return;
    nextReconnect_ = now + seconds(2);
    TuneSettings tune;
    { std::lock_guard<std::mutex> lk(tuneMu_); tune = lastTune_; }
    src_->stop();
    std::string err;
    if (src_->start(hwTune(tune), ring_, err)) {
        radioLost_ = false;
        lastSamples_ = steady_clock::now();
        reconnectErr_.clear();
        resetReq_ = true;   // the receiver starts again from the new samples
        log("radio reconnected");
        automation::receiverState("reconnected", "the radio is back");
    } else if (err != reconnectErr_) {
        reconnectErr_ = err;
        log("radio not available yet: " + err);
        DIAG(230972, "Reopening the radio after it stopped sending samples failed (%s), so OnAir keeps trying every two seconds.", err.c_str());
    }
}

// The receiver cannot keep up (the ring is more than half backlog): jump to the newest samples in one go. One clean gap that the receiver
// resynchronises after is far better than the radio thread dropping a few samples out of every chunk, which ruins every frame.
// Only for live radios: a recording or the synthetic signal waits for a slow receiver instead (see the paced source), and must never lose samples.
void Engine::catchUp() {
    if (!src_ || !src_->realtimeHardware()) return;
    const size_t skipped = ring_.dropBacklog();
    if (!skipped) return;
    rxPipeFlush();   // the gap mark comes after the blocks before it (the receiver on its own thread, 4.0)
    if (activeStd_.load() == 0) rx_.markGap(skipped);
    skippedSamples_ += skipped;
    skipEvents_++;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastSkipLog_ > std::chrono::seconds(10)) {
        lastSkipLog_ = now;
        automation::receiverState("overload", "the receiver cannot keep up with the sample rate");
        char b[200];
        snprintf(b, sizeof b, "the receiver cannot keep up with the sample rate: skipped %.2f s of signal (%llu skips so far). Close other programs, use a narrower channel or a lower sample rate", skipped / std::max(1.0, srcRate_), (unsigned long long)skipEvents_);
        log(b);
        DIAG(656327, "The receiver cannot keep up with the sample rate and skipped %llu samples of signal, so decoding has a gap (a narrower channel or a lower sample rate helps).", (unsigned long long)skipped);
    }
}

void Engine::holdDelivery(int ms) {
    delivHoldNs_ = steadyNs() + (int64_t)ms * 1000000;
    delivReanchor_ = true;
}

// Samples that never came out of the radio (delivery_check.h), and the rate that did arrive, against the rate the radio was set to.
void Engine::checkDelivery() {
    if (!src_ || !src_->realtimeHardware() || srcRate_ <= 0) return;
    if (radioLost_) { holdDelivery(500); delivRate_.restart(); deliveredPct_ = -1.f; return; }   // unplugged: that is reported as such, and the gap is not a loss of the USB link
    uint64_t off; int64_t ns; size_t block;
    if (!ring_.lastWrite(off, ns, block)) return;
    if (delivReanchor_.exchange(false)) deliv_.restart(delivHoldNs_.load());
    const double step = deliv_.observe(off, ns, srcRate_, (double)block);
    if (step > 0) {
        radioMissing_ = radioMissing_.load() + step;
        radioMissEvents_++;
        const int64_t now = steadyNs();
        lastRadioMissNs_ = now;
        if (now - lastRadioLogNs_.load() > 10000000000LL) {
            lastRadioLogNs_ = now;
            char b[240];
            if (lastNetwork_.load())
                snprintf(b, sizeof b, "the radio did not deliver %.0f ms of signal (%.2f s in all): its network link or server could not keep up. Try a wired connection or a lower sample rate",
                         1000 * step / srcRate_, radioMissing_.load() / srcRate_);
            else
                snprintf(b, sizeof b, "the radio did not deliver %.0f ms of signal (%.2f s in all): its USB connection or driver could not keep up. Try another USB port (not a hub), a shorter cable or a lower sample rate",
                         1000 * step / srcRate_, radioMissing_.load() / srcRate_);
            log(b);
        }
    }
    // Samples the library actually handed over, over about two seconds, as a percent of the rate the radio is set to (DeliveryRate:
    // between the radio's own write stamps, so a radio that hands over large transfers at a low rate reads 100 % and not 78 %). offered
    // counts every sample the callback brought, including ones the ring then dropped, so this is the radio's rate and not the receiver's
    // load. A start or retune pauses the radio; that gap is not a short delivery.
    const int64_t now = steadyNs();
    if (now < delivHoldNs_.load()) { delivRate_.restart(); deliveredPct_ = -1.f; return; }
    deliveredPct_ = delivRate_.observe(off, ns, block, now, srcRate_);
}

// New drops in the ring: the part the source reported as never received (IqRing::noteLost: a network radio's link was down) is the
// radio side's; the rest is overflow, and the last two seconds of this thread's figures say why (overflow_cause.h).
void Engine::noteRingLoss() {
    const uint64_t d = ring_.dropped(), lost = ring_.lostAtSource();
    const uint64_t dd = d - ringDroppedSeen_.load(), dl = lost >= ringLostSeen_.load() ? lost - ringLostSeen_.load() : 0;
    ringDroppedSeen_ = d;
    ringLostSeen_ = lost;
    const int64_t now = steadyNs();
    if (dl) { srcLostEvents_++; lastSrcLostNs_ = now; }
    if (dd <= dl) return;
    cpuDropEvents_++;
    lastCpuDropNs_ = now;
    const OverflowWindow w = loopMarks_.window(rate_.load(), rxStartNs_, now);
    const OverflowCause c = classifyOverflow(w);
    cpuCause_ = (int)c;
    if (getenv("ONAIR_TRACE_OVERFLOW")) {
        fprintf(stderr, "ring overflow: %llu samples, %s (window %.2f s: %.2f s of signal, %.2f s in the receive path, %.2f s of it on the CPU, %.1f s after the start)\n",
                (unsigned long long)(dd - dl), overflowCauseName(c), w.wallSec, w.signalSec, w.busyWallSec, w.busyCpuSec, w.sinceStartSec);
        fflush(stderr);
    }
}

SampleLoss Engine::sampleLoss() const {
    SampleLoss l;
    l.live = live_.load();
    l.network = lastNetwork_.load();
    const double r = std::max(1.0, srcRate_);   // the ring holds the radio's samples (before any resampling)
    l.radioSec = radioMissing_.load() / r;
    const uint64_t lost = ring_.lostAtSource(), dropped = ring_.dropped();
    l.cpuSec = (double)(dropped > lost ? dropped - lost : 0) / r;
    l.sourceLostSec = (double)lost / r;
    l.sourceLostEvents = srcLostEvents_.load();
    if (lastSrcLostNs_.load()) l.sinceSourceLostSec = (double)(steadyNs() - lastSrcLostNs_.load()) * 1e-9;
    l.cpuCause = (OverflowCause)cpuCause_.load();
    l.radioEvents = radioMissEvents_.load();
    l.cpuEvents = cpuDropEvents_.load();
    const int64_t now = steadyNs();
    if (lastRadioMissNs_.load()) l.sinceRadioSec = (double)(now - lastRadioMissNs_.load()) * 1e-9;
    if (lastCpuDropNs_.load()) l.sinceCpuSec = (double)(now - lastCpuDropNs_.load()) * 1e-9;
    l.loadPct = loadPct_.load();
    l.cpuPct = cpuPct_.load();
    l.deliveredPct = deliveredPct_.load();
    l.rateLow = l.live && l.deliveredPct >= 0.f && l.deliveredPct < 97.f;   // under about 97% of the rate the radio was set to
    return l;
}

}   // namespace dect2
