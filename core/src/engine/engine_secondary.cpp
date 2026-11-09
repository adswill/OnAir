// The second mode that runs at the same time (setSecondary): capture shifter, start/stop/refresh of the secondary receiver and
// its thread, and applySubMode().
#include "dect2/engine/engine.h"
#include "dect2/common/threadpool.h"
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

// ---------------------------------------------------------------- two modes at once (setSecondary)

// The second mode's shift of the whole capture (a few Msps, so it must be cheap): a table of one period of 4096 phase steps, turned
// by the phase at the start of each period, in plain float arithmetic that the compiler vectorises (std::complex products are not)
namespace {
struct CaptureShifter {
    static constexpr size_t L = 4096;
    double shift = 0, rate = 0;
    std::vector<float> re, im;
    std::complex<double> ph{1, 0}, stepL{1, 0};
    size_t pos = 0;
    void set(double s, double r) {
        if (s == shift && r == rate && !re.empty()) return;
        shift = s; rate = r; pos = 0; ph = 1;
        re.resize(L); im.resize(L);
        const double w = r > 0 ? -2 * M_PI * s / r : 0.0;
        for (size_t k = 0; k < L; k++) { re[k] = (float)std::cos(w * (double)k); im[k] = (float)std::sin(w * (double)k); }
        stepL = std::polar(1.0, w * (double)L);
    }
    void mix(cf32* x, size_t n) {
        if (shift == 0 || rate <= 0) return;
        float* v = reinterpret_cast<float*>(x);
        size_t i = 0;
        while (i < n) {
            const size_t seg = std::min(n - i, L - pos);
            const float pr = (float)ph.real(), pi = (float)ph.imag();
            const float* tr = re.data() + pos;
            const float* ti = im.data() + pos;
            float* o = v + 2 * i;
            for (size_t k = 0; k < seg; k++) {
                const float lr = tr[k] * pr - ti[k] * pi, li = tr[k] * pi + ti[k] * pr;
                const float xr = o[2 * k], xi = o[2 * k + 1];
                o[2 * k] = xr * lr - xi * li;
                o[2 * k + 1] = xr * li + xi * lr;
            }
            i += seg; pos += seg;
            if (pos == L) { pos = 0; ph *= stepL; ph /= std::abs(ph); }
        }
    }
};
}   // namespace

// Shifts the second mode's frequency to where its receiver expects the channel: the centre of a radio tuned for that mode alone (its
// frequency plus its tuneOffsetHz, the receiver mixing the offset out as for the first mode); a recording has the channel on the centre
bool Engine::placeSecondary(std::string& why) {
    const int sm = secStd_.load();
    const double hz = secHz_.load(), centre = captureCentreHz();
    why = secondaryCheck(stdMode_.load(), sm, centre, rate_.load(), hz, secChanHz_.load());
    if (why.empty() && activeStdFor(sm) == activeStd_.load()) why = "the first mode runs the same receiver";
    if (!why.empty()) return false;
    const ModeTuning* mt = modeTuning(sm);
    DeviceInfo::Kind k;
    { std::lock_guard<std::mutex> lk(tuneMu_); k = lastDev_.kind; }
    const bool file = k == DeviceInfo::File;
    const double toff = file || k == DeviceInfo::Audio || !mt ? 0.0 : mt->tuneOffsetHz;
    const double vc = hz + toff;
    secShift_ = vc - centre;
    placeReceiver(activeStdFor(sm), -toff, file ? 0.0 : vc, file ? 0.0 : hz, hz);
    return true;
}

bool Engine::setSecondary(int stdMode, double offsetHz) { return setSecondaryHz(stdMode, stdMode ? captureCentreHz() + offsetHz : 0.0); }

bool Engine::setSecondaryHz(int sm, double hz) { return setSecondaryImpl(sm, hz, 0, 0, 0); }

// The carrier alone: a DVB-S/S2 signal of symbolRate takes (1 + roll-off) times that of the spectrum; the receiver runs at 3.2 times the symbol
// rate (2 Msps at least, what it needs), a whole division of the capture's rate where there is one
bool Engine::setSecondaryDvbs(double hz, double sym) {
    sym = std::max(sym, 10e3);
    return setSecondaryImpl(8, hz, sym * 1.4, std::max(2e6, sym * 3.2), sym);
}

bool Engine::setSecondaryImpl(int sm, double hz, double chanHz, double wantRate, double sym) {
    std::lock_guard<std::mutex> ck(secCtlMu_);
    stopSecondaryLocked();
    secStd_ = sm; secHz_ = hz;
    secChanHz_ = chanHz; secWantRate_ = wantRate; secSym_ = sym;
    { std::lock_guard<std::mutex> lk(rx2Mu_); rxTel2_ = RxTelemetry(); }
    if (!sm) { setSecNote(""); return true; }
    if (!running_) {   // nothing to measure against yet: the mode now, the fit when the radio starts
        if (!secondaryAllowed(sm, stdMode_.load()) || sm == stdMode_.load()) { setSecNote(secondaryCheck(stdMode_.load(), sm, 0, 0, hz)); secStd_ = 0; return false; }
        setSecNote("starts with the radio");
        return true;
    }
    return startSecondaryLocked(false);
}

void Engine::startSecondary() {
    std::lock_guard<std::mutex> ck(secCtlMu_);
    if (secStd_.load()) startSecondaryLocked(true);
}

void Engine::stopSecondary() {
    std::lock_guard<std::mutex> ck(secCtlMu_);
    stopSecondaryLocked();
}

bool Engine::startSecondaryLocked(bool keep) {
    const int sm = secStd_.load();
    if (!sm || !running_ || secTh_.joinable()) return false;
    const int a = activeStdFor(sm);
    std::string why;
    bool ok;
    {
        std::lock_guard<std::mutex> fl(secFeedMu_);
        ok = placeSecondary(why);
        if (ok) {   // as a radio tuned for this mode alone: its own sample rate (or the capture's when that is lower), fresh tables
            const ModeTuning* mt = modeTuning(sm);
            double want = mt && mt->sampleRate > 0 ? mt->sampleRate : rate_.load();
            if (secWantRate_.load() > 0) {   // a whole division of the capture's rate that is not below what the mode wants
                want = secWantRate_.load();
                const double k = std::floor(rate_.load() / want + 1e-9);
                if (k >= 2) want = rate_.load() / k;
            }
            secRate_ = rate_.load() > want * 1.001 ? want : rate_.load();
            configureReceiver(a, secRate_.load());
            muteReceiver(a, !secSound_.load());   // the first mode keeps the speakers unless the second is asked to play
            resetReceiver(a);   // the first mode does not use it, the second thread is not running yet
            if (a == 7) {   // DVB-S/S2 beside QO-100: its stream is the stream of the engine, the old one (and what played from it) goes
                rxDvbs_.setSymbolRate(secSym_.load());
                rxDvbs_.setStandardHint(0);
                player_.select(-1);
                std::lock_guard<std::mutex> tl(tsMu_);
                demux_.reset();
                tsSnap_ = TsSnapshot();
            }
        }
    }
    if (!ok && !keep) { secStd_ = 0; setSecNote(why); log("second mode refused: " + why); return false; }
    for (auto& s : modeSeq2_) s = 0;
    {
        std::lock_guard<std::mutex> lk(secQMu_);
        secQ_.clear(); secQN_ = 0; secStop_ = false;
    }
    secDropped_ = 0;
    secFlush_ = true;
    secAct_ = ok ? a : -1;
    setSecNote(ok ? "" : "suspended: " + why);
    secTh_ = std::thread([this, a] { secondaryLoop(a); });
    char b[240];
    const ModeTuning* mt = modeTuning(sm);
    if (ok) snprintf(b, sizeof b, "second mode: %s at %.4f MHz, %.3f MHz from the centre of the capture", mt ? mt->name : "?", secHz_.load() / 1e6, (secHz_.load() - captureCentreHz()) / 1e6);
    else snprintf(b, sizeof b, "second mode suspended: %s", why.c_str());
    log(b);
    return ok;
}

void Engine::stopSecondaryLocked() {
    const bool had = secTh_.joinable();
    const int a = activeStdFor(secStd_.load());
    secAct_ = -1;
    {
        std::lock_guard<std::mutex> lk(secQMu_);
        secStop_ = true;
    }
    secQCv_.notify_all();
    if (secTh_.joinable()) secTh_.join();
    {
        std::lock_guard<std::mutex> lk(secQMu_);
        secQ_.clear(); secQN_ = 0; secStop_ = false;
    }
    if (had && a == 7 && a != activeStd_.load()) {   // DVB-S/S2 beside QO-100: its worker threads stop with the receiver, its stream and picture go
        rxDvbs_.resetNow();
        player_.select(-1);
        std::lock_guard<std::mutex> tl(tsMu_);
        demux_.reset();
        tsSnap_ = TsSnapshot();
    }
    // the receiver back where start() put it, should it become the first mode without a restart (autoSelect does it for the first mode itself)
    if (had && a != activeStd_.load()) { configureReceiver(a, rate_.load()); placeReceiverAsPrimary(a); muteReceiver(a, false); }
}

void Engine::refreshSecondary(bool force) {
    std::lock_guard<std::mutex> ck(secCtlMu_);
    if (!secTh_.joinable()) return;
    const int a = activeStdFor(secStd_.load());
    std::lock_guard<std::mutex> fl(secFeedMu_);
    const bool was = secAct_.load() == a;
    if (was && !force) return;
    std::string why;
    if (placeSecondary(why)) {
        if (!was) { secFlush_ = true; setSecNote(""); log("second mode resumed"); }
        secAct_ = a;
    } else {
        if (was) log("second mode suspended: " + why);
        secAct_ = -1;
        setSecNote("suspended: " + why);
        secQCv_.notify_all();
    }
}

// The first mode's thread hands a copy of every block over; a live radio cannot wait, so a second receiver that falls behind loses
// blocks (counted); a recording or the test signal waits for it, as the source waits for the first receiver
void Engine::pushSecondary(const cf32* x, size_t n) {
    const size_t cap = std::max<size_t>(1u << 18, (size_t)(rate_.load() * 0.5));
    std::unique_lock<std::mutex> lk(secQMu_);
    if (live_.load()) {
        if (secQN_ + n > cap) { secDropped_ += n; return; }
    } else {
        while (!secStop_ && secAct_.load() >= 0 && secQN_ + n > cap && !stopReq_) secQCv_.wait_for(lk, std::chrono::milliseconds(50));
        if (secStop_ || secAct_.load() < 0 || stopReq_) return;
    }
    secQ_.emplace_back(x, x + n);
    secQN_ += n;
    lk.unlock();
    secQCv_.notify_all();
}

void Engine::secondaryLoop(int a) {
    setThreadName(a >= 0 && a < 10 ? ("onair-rx" + std::to_string(a + 1)).c_str() : "onair-rx");   // one analysis thread per secondary receiver
    setThreadPriority(ThreadPriority::Background);   // below the first mode's sample path
    cpu::bindThread(cpuSec_.load());
    CaptureShifter mix;
    ExactResampler rs;   // the capture's rate down to the second mode's own
    double rsIn = 0, rsOut = 0;
    std::vector<cf32> dec;
    std::vector<cf32> b;
    auto nextPoll = std::chrono::steady_clock::now();
    for (;;) {
        b.clear();
        {
            std::unique_lock<std::mutex> lk(secQMu_);
            secQCv_.wait_for(lk, std::chrono::milliseconds(30), [&] { return secStop_ || !secQ_.empty(); });
            if (secStop_) break;
            if (!secQ_.empty()) { b.swap(secQ_.front()); secQ_.pop_front(); secQN_ -= b.size(); }
        }
        cpu::tick();
        if (!b.empty()) {
            secQCv_.notify_all();   // room for the next block
            std::lock_guard<std::mutex> fl(secFeedMu_);
            if (secFlush_.exchange(false)) { mix = CaptureShifter(); rs.reset(); for (auto& s : modeSeq2_) s = 0; }
            if (secAct_.load() == a) {
                mix.set(secShift_.load(), rate_.load());
                mix.mix(b.data(), b.size());
                if (rsIn != rate_.load() || rsOut != secRate_.load()) { rsIn = rate_.load(); rsOut = secRate_.load(); if (!rs.configure(rsIn, rsOut)) rs.configure(1, 1); }
                if (rs.passthrough()) feedReceiver(a, b.data(), b.size());
                else { dec.clear(); rs.process(b.data(), b.size(), dec); if (!dec.empty()) feedReceiver(a, dec.data(), dec.size()); }
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPoll) {
            nextPoll = now + std::chrono::milliseconds(33);
            RxTelemetry t;
            int pm = 0;
            { std::lock_guard<std::mutex> fl(secFeedMu_); if (secAct_.load() == a) pm = pollMode(a, t, modeSeq2_); }
            if (pm == 1) { std::lock_guard<std::mutex> lk(rx2Mu_); t.seq = ++rx2Pub_; rxTel2_ = std::move(t); }
        }
    }
}

bool Engine::latestRx2(RxTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(rx2Mu_);
    if (rxTel2_.seq <= lastSeq) return false;
    out = rxTel2_;
    return true;
}

void applySubMode(Engine& e, const SubMode* s) {
    if (!s) { e.hfdig().setDecoders(31); e.pager().setProtocols(3); e.vor().setFunction(0); return; }
    switch (s->stdMode) {
    case 27:
        e.hfdig().setDecoders(s->decoders);
        if (s->decoders == 32) { KbdConfig k = e.hfdig().kbd().config(); k.family = s->sel; e.hfdig().kbd().setConfig(k); }
        else if (s->sel >= 0) for (int m = 0; m < kFtxModes; m++) e.hfdig().ftx().setEnabled(m, m == s->sel || (s->sel == 7 && m == 9));   // FST4 also listens for FST4W
        break;
    case 22: e.mesh().setProtocols(s->decoders); break;
    case 362: e.rclink().setProtocols(s->decoders); break;
    case 602: e.xlink().setProtocols(s->decoders); break;
    case 32: e.vor().setFunction(s->sel); break;
    case 25: e.pager().setProtocols(s->decoders); break;
    case 17: e.marine().setService(s->sel); break;
    default: break;
    }
}

}   // namespace dect2
