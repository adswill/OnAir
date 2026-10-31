// What the engine hands out after decoding: the per-frame PLP callback (onPlp, with its lock-timing counters), the EPG and
// BB statistics getters, and the latest receiver telemetry and spectrum frame.
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
    // FRAME_IDX restarts at 0 every super-frame (NUM_T2_FRAMES frames), and a PLP with FRAME_INTERVAL > 1 skips frames: compare the
    // index with the one expected, not with last + 1, or every super-frame boundary (every second frame on a 2-frame super-frame) looks
    // like lost data and drops the half-assembled SDT/NIT/EIT sections.
    if (lastT2Frame_ >= 0) {
        const int mod = r.numT2Frames > 0 ? r.numT2Frames : 256;
        if (r.t2Frame != (lastT2Frame_ + std::max(1, r.frameInterval)) % mod) { unpack_.lost(); demux_.markLoss(); }
    }
    lastT2Frame_ = r.t2Frame;
    for (const auto& f : r.frames) {
        if (f.bits.empty()) { unpack_.lost(); demux_.markLoss(); continue; }
        unpack_.push(f.bits, f.header);
    }
    demux_.advance(r.frameSec);
    tsSnap_ = demux_.snapshot();
    outputs_.packets(frameBuf_.data(), frameBuf_.size() / 188, &tsSnap_);
    if (tap_) tap_(frameBuf_.data(), frameBuf_.size() / 188, &tsSnap_, r.frameSec);
    if (rtap_) rtap_(frameBuf_.data(), frameBuf_.size() / 188, &tsSnap_, r.frameSec);
    if (player_.selected() >= 0 && !playerExternal_) { const auto tp = std::chrono::steady_clock::now(); player_.push(frameBuf_.data(), frameBuf_.size() / 188, tsSnap_); gPlpPushUs += usSince(tp); }
    frameBuf_.clear();
    outputs_.burstDone(r.frameSec);
}

std::string Engine::setT2Export(const T2ExportConfig& c) {
    std::lock_guard<std::mutex> lk(t2ExportMu_);
    if (!t2Export_) t2Export_ = std::make_unique<T2Export>();
    return t2Export_->configure(c) ? std::string() : t2Export_->error();
}

void Engine::setPacketTap(std::function<void(const uint8_t*, size_t, const TsSnapshot*, double)> f) { std::lock_guard<std::mutex> lk(tsMu_); tap_ = std::move(f); }
void Engine::setRecorderTap(std::function<void(const uint8_t*, size_t, const TsSnapshot*, double)> f) { std::lock_guard<std::mutex> lk(tsMu_); rtap_ = std::move(f); }
void Engine::clearServices() { std::lock_guard<std::mutex> lk(tsMu_); demux_.reset(); tsSnap_ = TsSnapshot(); }
TsSnapshot Engine::tsSnapshot() { std::lock_guard<std::mutex> lk(tsMu_); return tsSnap_; }
std::map<int, std::vector<EpgEvent>> Engine::epg() {
    if (mtvActive_.load()) { if (auto m = mtvGet()) return m->epg(m->selected()); }
    std::lock_guard<std::mutex> lk(tsMu_);
    return demux_.epg();
}

BbStats Engine::bbStats() {
    if (mtvActive_.load()) { if (auto m = mtvGet()) return m->bbStats(m->selected()); }
    std::lock_guard<std::mutex> lk(tsMu_);
    return unpack_.stats();
}

bool Engine::latestRx(RxTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(rxMu_);
    if (rxTel_.seq <= lastSeq) return false;
    out = rxTel_;
    return true;
}

bool Engine::latestSpectrum(SpectrumFrame& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(specMu_);
    if (spec_.seq <= lastSeq || spec_.dbfs.empty()) return false;
    out = spec_;
    return true;
}

}   // namespace dect2
