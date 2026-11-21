// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 OnAir contributors
#include "../app.h"
#include "dect2/engine/wide_session.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace {
struct Hit {
    double hz = 0; // channel-grid/listening frequency, optionally fine-tuned
    double carrierHz = 0; // actual detector frequency for spectrum placement
    unsigned transmissions = 0;
    double lastDuration = 0, totalDuration = 0;
    double started = 0, lastHeard = 0;
    bool active = false;
    double measuredSumHz = 0;
    unsigned measuredCount = 0;
    int displayId = 0;
    dect2::WsKind kind = dect2::WsKind::Unknown;
    std::string identity;
};
struct Scout {
    double fromMHz = 145.0, toMHz = 145.8;
    float stepKHz = 12.5f, minDurationMs = 1000, hangMs = 500;
    bool scanning = false;
    bool wasOpen = false;
    bool hopping = false;
    bool fineTune = false;
    double scanStarted = 0;
    int acquireCount = 2;
    float minDetectionSnrDb = 6.0f;
    bool interactionPaused = false;
    bool restartPending = false;
    bool externalRetune = false; // A memory/band change owns the new centre, not the old plot view.
    double externalCentreMHz = 0; // Explicit memory recall target, independent of stale spectrum axes.
    bool draggingSpectrum = false;
    double lastInteraction = 0;
    float monitorSeconds = 3.0f;
    float expireSeconds = 60.0f;
    double lastHop = 0;
    bool priorWide = false;
    int receiverId = -1;
    double lockedCentre = 0, lockedRate = 0;
    double selectedHz = 0;
    double sourceRateHz = 0;
    int listenManualId = 0;
    double listenManualHz = 0;
    dect2::WsKind listenKind = dect2::WsKind::Unknown;
    std::string message;
    int nextDisplayId = 1;
    std::map<long long, Hit> hits;
    std::map<long long, int> candidates;
    uint64_t lastSnapshot = 0;
};
Scout H;
int allocateDisplayId() {
    for (int attempt = 0; attempt < 99; ++attempt) {
        const int candidate = H.nextDisplayId;
        H.nextDisplayId = (H.nextDisplayId % 99) + 1;
        const bool occupied = std::any_of(H.hits.begin(), H.hits.end(),
            [candidate](const auto& entry) { return entry.second.displayId == candidate; });
        if (!occupied) return candidate;
    }
    return 0;
}
double tunerRate(const App& a) {
    if (a.chosenRateHz > 0) return a.chosenRateHz;
    if (a.autoRateHz > 0) return a.autoRateHz;
    if (a.tune.sampleRate > 0) return a.tune.sampleRate;
    return a.engine.running() ? a.engine.sampleRate() : 0;
}
void initializeRange(const App& a, double centreMHz) {
    double lo = 0, hi = 0;
    specFreqViewGet(lo, hi);
    if (!(std::isfinite(lo) && std::isfinite(hi) && hi > lo && lo > 0)) {
        const double rate = tunerRate(a);
        if (!std::isfinite(rate) || rate <= 0 || centreMHz <= 0) return;
        lo = std::max(0.000001, centreMHz - rate * 0.5e-6);
        hi = centreMHz + rate * 0.5e-6;
    }
    H.fromMHz = lo;
    H.toMHz = hi;
    H.sourceRateHz = tunerRate(a);
}

void followSpectrumRange() {
    double lo = 0, hi = 0;
    specFreqViewGet(lo, hi);
    if (std::isfinite(lo) && std::isfinite(hi) && lo > 0 && hi > lo) {
        H.fromMHz = lo;
        H.toMHz = hi;
    }
}

bool voiceKind(dect2::WsKind kind) {
    using K = dect2::WsKind;
    switch (kind) {
    case K::Am: case K::Nfm:
    case K::Dmr: case K::P25: case K::Nxdn: case K::Dpmr:
    case K::Dstar: case K::Fusion: case K::M17:
        return true;
    default: return false;
    }
}

void finish(Hit& h, double now) {
    if (!h.active) return;
    const double duration = std::max(0.0, now - h.started);
    if (duration * 1000 >= H.minDurationMs) {
        // The first confirmed transmission establishes the channel. Only
        // subsequent transmissions may refine its carrier when opted in.
        if (H.fineTune && h.transmissions > 0 && h.measuredCount > 0) {
            const double step = H.stepKHz * 1000.0;
            const double measured = h.measuredSumHz / h.measuredCount;
            const double grid = std::round(h.hz / step) * step;
            if (std::fabs(measured - grid) <= step * 0.45)
                h.hz = std::clamp(0.5 * h.hz + 0.5 * measured,
                                  grid - step * 0.45, grid + step * 0.45);
        }
        ++h.transmissions;
        h.lastDuration = duration;
        h.totalDuration += duration;
        h.lastHeard = now;
    }
    h.active = false;
    h.measuredSumHz = 0;
    h.measuredCount = 0;
}
// Keep confirmed detections across automatic retunes. They remain idle until
// refreshed by a new capture, or until their normal idle timeout expires.
void pruneHits(double now) {
    for (auto it = H.hits.begin(); it != H.hits.end();) {
        const Hit& h = it->second;
        const bool unconfirmed = !h.active && h.transmissions == 0;
        const bool expired = !h.active && H.expireSeconds > 0 &&
            h.lastHeard > 0 && now - h.lastHeard >= H.expireSeconds;
        if (unconfirmed || expired) {
            if (H.selectedHz > 0 && std::fabs(H.selectedHz - h.hz) < 1.0)
                H.selectedHz = 0;
            it = H.hits.erase(it);
        } else ++it;
    }
}

void stop(App& a, bool preserveSelection = false) {
    if (!H.scanning) {
        H.restartPending = false;
        return;
    }
    const double now = ImGui::GetTime();
    for (auto& kv : H.hits) finish(kv.second, now);
    H.scanning = false;
    H.restartPending = false;
    H.externalRetune = false;
    H.externalCentreMHz = 0;
    H.hopping = false;
    H.interactionPaused = false;
    H.draggingSpectrum = false;
    H.candidates.clear();
    if (a.engine.running()) {
        a.engine.wideAllSession().setAudioSolo(0);
        a.engine.wideAllSession().setVoiceAudioFocus(-1, 0);
        a.engine.wideAllSession().setListen(0);
        a.wideAllListen = 0;
        a.engine.setVoiceScoutExclusiveAudio(preserveSelection);
    }
    if (H.listenManualId && a.engine.running()) a.engine.wideAllSession().remove(H.listenManualId);
    H.listenManualId = 0;
    H.listenManualHz = 0;
    H.listenKind = dect2::WsKind::Unknown;
    if (!preserveSelection && !H.priorWide && a.engine.running()) a.engine.setWideAll(false);
    if (!preserveSelection) H.selectedHz = 0;
}

void start(App& a, bool preserveHits = false, bool adoptReceiver = false) {
    if (H.scanning) stop(a);
    H.message.clear();
    if (!std::isfinite(H.fromMHz) || !std::isfinite(H.toMHz) ||
        H.fromMHz <= 0 || H.toMHz <= H.fromMHz ||
        !std::isfinite(H.stepKHz) || H.stepKHz < 2.5f) {
        H.message = "Enter a valid frequency range and a step of at least 2.5 kHz.";
        return;
    }
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[a.devIdx].isRadio()) {
        H.message = "Select an SDR radio first."; return;
    }
    const double span = (H.toMHz - H.fromMHz) * 1e6;
    const double step = H.stepKHz * 1000.0;
    if (span / step > 8192) { H.message = "Too many channels (maximum 8192)."; return; }
    const double needed = span;
    const double rate = (adoptReceiver && a.engine.running()) ? a.engine.sampleRate() : tunerRate(a);
    if (!(rate > 0) || rate < needed - 1) {
        H.message = "Visible spectrum does not fit the current SDR bandwidth. Zoom in or adjust bandwidth manually.";
        return;
    }
    const double mid = (H.fromMHz + H.toMHz) * 0.5;
    if (!preserveHits) H.priorWide = a.engine.wideAll();
    const double oldRfCentre = radioCenterMhz(a);
    // External frequency/mode/bandwidth changes have already tuned the radio.
    // Do not derive a new dial value from an old capture or retune it again:
    // that is how a memory recall on 2 m got pulled back to 70 cm.
    if (!adoptReceiver) {
        const double offsetMHz = oldRfCentre - a.freqMhz;
        a.freqMhz = mid - offsetMHz;
        a.tune.centerHz = a.freqMhz * 1e6; // OnAir applies the RF offset once.
        if (a.engine.running()) {
            if (std::fabs(oldRfCentre - mid) > 1e-7) a.engine.retune(a.tune);
        } else startReceiver(a);
    } else if (!a.engine.running()) {
        // A mode/memory recall may still be restarting the receiver. Wait
        // for its new capture rather than starting the old configuration.
        H.message = "Waiting for the receiver after external tuning.";
        H.restartPending = true;
        H.externalRetune = true;
        H.lastInteraction = ImGui::GetTime();
        return;
    }
    if (!a.engine.running()) { H.message = "Could not start the radio: " + a.engine.startError(); return; }
    if (a.engine.sampleRate() < needed) {
        H.message = "SDR sample rate is smaller than the capture required; adjust the rate and retry.";
        return;
    }
    a.engine.setWideAll(true);
    a.engine.wideAllSession().setOnlyManual(false);
    if (!preserveHits) {
        H.hits.clear();
        H.nextDisplayId = 1;
    }
    H.candidates.clear();
    H.lastSnapshot = a.engine.wideAllSession().status().snapshotSerial;
    H.receiverId = a.rxId;
    H.lockedCentre = radioCenterMhz(a) * 1e6;
    H.lockedRate = a.engine.sampleRate();
    H.sourceRateHz = tunerRate(a);
    if (!preserveHits) H.selectedHz = 0;
    H.listenManualId = 0;
    H.listenManualHz = 0;
    H.listenKind = dect2::WsKind::Unknown;
    H.hopping = false;
    H.interactionPaused = false;
    H.restartPending = false;
    H.externalRetune = false;
    H.externalCentreMHz = 0;
    H.lastHop = ImGui::GetTime();
    H.scanStarted = H.lastHop;
    a.engine.wideAllSession().setAudioSolo(-1);
    a.engine.wideAllSession().setVoiceAudioFocus(0, 0);
    // No channel is audible until Scout has an explicitly selected signal.
    // In particular, do not inherit a listen ID from the previous capture.
    a.engine.wideAllSession().setListen(0);
    a.wideAllListen = 0;
    a.engine.setVoiceScoutExclusiveAudio(true);
    H.scanning = true;
}

void syncListening(App& a, const std::vector<dect2::WsEntry>& entries) {
    if (!H.scanning) return;
    auto& ws = a.engine.wideAllSession();
    if (H.selectedHz <= 0 || H.selectedHz < H.fromMHz * 1e6 || H.selectedHz > H.toMHz * 1e6) {
        ws.setAudioSolo(-1);
        ws.setVoiceAudioFocus(0, 0);
        ws.setListen(0);
        a.wideAllListen = 0;
        return;
    }
    const dect2::WsEntry* match = nullptr;
    const double tolerance = std::max(2500.0, H.stepKHz * 500.0);
    for (const auto& e : entries) {
        if (e.id >= dect2::kFirstManualId || !voiceKind(e.kind)) continue;
        const double hz = e.absHz > 0 ? e.absHz : H.lockedCentre + e.offsetHz;
        if (std::fabs(hz - H.selectedHz) > tolerance) continue;
        if (!match || (e.state == dect2::WsState::Running && match->state != dect2::WsState::Running) ||
            (e.kind == H.listenKind && match->kind != H.listenKind)) match = &e;
    }
    if (match) H.listenKind = match->kind;
    const int digitalMode = dect2::wsKindStdMode(H.listenKind);
    const bool digital = digitalMode != 0;
    int listenId = match && match->state == dect2::WsState::Running ? match->id : 0;
    if (!digital && !listenId) {
        if (H.listenManualId && std::fabs(H.listenManualHz - H.selectedHz) > 1.0) {
            ws.remove(H.listenManualId);
            H.listenManualId = 0;
            H.listenManualHz = 0;
        }
        if (!H.listenManualId) {
            const auto kind = H.listenKind == dect2::WsKind::Am ? dect2::WsKind::Am : dect2::WsKind::Nfm;
            H.listenManualId = ws.addManual(H.selectedHz - H.lockedCentre, H.stepKHz * 1000.0, kind);
            H.listenManualHz = H.selectedHz;
        }
        listenId = H.listenManualId;
    } else if (H.listenManualId) {
        ws.remove(H.listenManualId);
        H.listenManualId = 0;
        H.listenManualHz = 0;
    }
    const bool onAir = match && match->active && std::isfinite(match->snrDb) &&
                       match->snrDb >= H.minDetectionSnrDb;
    if (!onAir) {
        ws.setAudioSolo(-1);
        ws.setVoiceAudioFocus(0, 0);
        ws.setListen(0);
        a.wideAllListen = 0;
        return;
    }
    ws.setAudioSolo(digital ? -1 : listenId);
    ws.setVoiceAudioFocus(digital && listenId ? digitalMode : 0, digital && listenId ? listenId : 0);
    ws.setListen(listenId);
    a.wideAllListen = listenId;
}

void tick(App& a) {
    if (!H.scanning || H.interactionPaused) return;
    if (!a.engine.running()) {
        H.message = "Receiver stopped; Scout scan stopped.";
        stop(a);
        return;
    }
    if (a.rxId != H.receiverId || !a.engine.wideAll() ||
        std::abs(radioCenterMhz(a) * 1e6 - H.lockedCentre) > 1000 ||
        std::abs(a.engine.sampleRate() - H.lockedRate) > 100) {
        // External tuning (including memory recall) is an automatic relock,
        // not a user request to disable Scout.
        stop(a, true);
        H.restartPending = true;
        H.externalRetune = true;
        H.externalCentreMHz = radioCenterMhz(a);
        H.lastInteraction = ImGui::GetTime();
        return;
    }
    const double now = ImGui::GetTime();
    // Retuning invalidates the old detector state. Do not turn its cached
    // entries into new signals until a fresh capture has settled.
    if (now - H.scanStarted < 1.25) return;
    const double step = H.stepKHz * 1000.0, lo = H.fromMHz * 1e6, hi = H.toMHz * 1e6;
    // Do not drop historical hits merely because the new view no longer
    // covers them. Their idle age, not viewport coordinates, owns expiry.
    pruneHits(now);
    auto& ws = a.engine.wideAllSession();
    const auto before = ws.status().snapshotSerial;
    const std::vector<dect2::WsEntry> entries = ws.entries();
    const auto after = ws.status().snapshotSerial;
    if (before != after) return;
    if (after == H.lastSnapshot) {
        syncListening(a, entries);
        return;
    }
    H.lastSnapshot = after;
    struct Detection { dect2::WsKind kind; std::string identity; double hz; };
    std::map<long long, Detection> present;
    for (const auto& e : entries) {
        if (e.id >= dect2::kFirstManualId || !voiceKind(e.kind) || !e.active ||
            !std::isfinite(e.snrDb) || e.snrDb < H.minDetectionSnrDb) continue;
        // The detector is asynchronous. After a retune, a published entry
        // can still refer to the previous IQ capture. Do not relocate that
        // entry using the new centre: accept only internally consistent RF
        // coordinates belonging to the current capture epoch.
        const double expectedHz = H.lockedCentre + e.offsetHz;
        if (!(e.absHz > 0 && std::isfinite(e.absHz) && std::isfinite(e.offsetHz)) ||
            std::fabs(e.absHz - expectedHz) > 250.0 ||
            std::fabs(e.offsetHz) > H.lockedRate * 0.5 + 1.0) continue;
        const double hz = e.absHz;
        if (hz < lo - step * 0.4 || hz > hi + step * 0.4) continue;
        const long long channel = llround(hz / step);
        if (channel < 0 || channel * step > hi + 1) continue;
        auto found = present.find(channel);
        if (found == present.end() || e.state == dect2::WsState::Running)
            present[channel] = {e.kind, e.identity, hz};
    }
    for (auto& kv : H.hits) {
        Hit& h = kv.second;
        if (h.active && present.find(kv.first) == present.end() && now - h.lastHeard >= H.hangMs / 1000.0) finish(h, now);
    }
    for (auto it = H.candidates.begin(); it != H.candidates.end();) {
        if (present.count(it->first) == 0 || H.hits.count(it->first) != 0)
            it = H.candidates.erase(it);
        else ++it;
    }
    for (const auto& p : present) {
        auto it = H.hits.find(p.first);
        if (it == H.hits.end()) {
            int& count = H.candidates[p.first];
            count = std::min(H.acquireCount, count + 1);
            if (count < H.acquireCount) continue;
            H.candidates.erase(p.first);
        }
        if (it == H.hits.end()) {
            const int displayId = allocateDisplayId();
            if (displayId == 0) continue;
            it = H.hits.try_emplace(p.first).first;
            it->second.displayId = displayId;
        }
        Hit& h = it->second;
        // Default: the absolute channel-step grid drives markers and tuning.
        // A previously confirmed channel may drift off-grid only if the user
        // enables fine tuning, based on measurements from a later transmission.
        if (h.hz <= 0) h.hz = p.first * step;
        // The channel grid is an address for grouping/listening, not the
        // spectrum coordinate. OnAir draws this detection at WsEntry.absHz;
        // preserve exactly that absolute RF coordinate for the scout overlay.
        // Never derive its position from a zoom level, axis range, or bin ID.
        if (std::isfinite(p.second.hz) && p.second.hz > 0)
            h.carrierHz = p.second.hz;
        if (!h.active) {
            h.active = true;
            h.started = now;
            h.measuredSumHz = 0;
            h.measuredCount = 0;
        }
        if (std::fabs(p.second.hz - p.first * step) <= step * 0.45) {
            h.measuredSumHz += p.second.hz;
            ++h.measuredCount;
        }
        h.lastHeard = now;
        h.kind = p.second.kind;
        h.identity = p.second.identity;
    }
    pruneHits(now);
    syncListening(a, entries);
}

void listen(App& a, Hit& h) {
    H.selectedHz = h.hz;
    H.listenKind = h.kind;
    H.lastHop = ImGui::GetTime();
    syncListening(a, a.engine.wideAllSession().entries());
}

void hopNext(App& a) {
    std::vector<Hit*> monitored;
    for (auto& kv : H.hits) if (kv.second.active &&
        (ImGui::GetTime() - kv.second.started) * 1000.0 >= H.minDurationMs) monitored.push_back(&kv.second);
    if (monitored.empty()) { H.lastHop = ImGui::GetTime(); return; }
    auto next = std::find_if(monitored.begin(), monitored.end(), [](const Hit* h) { return h->hz > H.selectedHz + 1.0; });
    if (next == monitored.end()) next = monitored.begin();
    listen(a, **next);
}
}

bool voiceScoutScanning() { return H.scanning; }
bool voiceScoutNavigating() { return H.scanning || H.restartPending; }

// A memory recall may restart the engine and change the mode/sample rate.
// Reacquire the new band after that operation, without losing Scan intent.
void voiceScoutExternalTune(App& a, double recalledMHz) {
    if (!H.scanning && !H.restartPending) return;
    if (H.scanning) stop(a, true);
    H.restartPending = true;
    H.externalRetune = true;
    // The recalled frequency is a dial frequency; the RF axis may also have
    // a mode-dependent offset. Query the already updated OnAir state instead.
    (void)recalledMHz;
    H.externalCentreMHz = radioCenterMhz(a);
    H.lastInteraction = ImGui::GetTime();
}

// Called after the normal mode UI applies its own audio settings. Keep the
// primary decoder and monitoring audio quiet; wideband Scout audio is gated
// independently by syncListening() and only opens for active selected hits.
void voiceScoutEnforceAudio(App& a) {
    if (!voiceScoutNavigating() || !a.engine.running()) return;
    a.engine.setVoiceScoutExclusiveAudio(true);
    a.engine.monitor().setMuted(true);
}

std::vector<VoiceScoutMarker> voiceScoutMarkers() {
    std::vector<VoiceScoutMarker> result;
    if (!H.scanning && !H.restartPending) return result;
    result.reserve(H.hits.size());
    for (const auto& kv : H.hits) {
        const Hit& h = kv.second;
        if (h.hz <= 0 || (h.transmissions == 0 && (!h.active ||
            (ImGui::GetTime() - h.started) * 1000.0 < H.minDurationMs))) continue;
        const double rfHz = h.carrierHz > 0 ? h.carrierHz : h.hz;
        result.push_back({rfHz / 1e6, h.active, std::fabs(H.selectedHz - h.hz) < 1.0, h.displayId});
    }
    return result;
}

bool voiceScoutSelectFrequency(App& a, double mhz) {
    if (!H.scanning) return false;
    if (!std::isfinite(mhz)) return true;
    Hit* closest = nullptr;
    const double toleranceHz = std::max(2500.0, H.stepKHz * 500.0) + 1.0;
    double distance = toleranceHz;
    for (auto& kv : H.hits) {
        Hit& h = kv.second;
        if (h.hz <= 0 || (h.transmissions == 0 && (!h.active ||
            (ImGui::GetTime() - h.started) * 1000.0 < H.minDurationMs))) continue;
        const double d = std::fabs(mhz * 1e6 - (h.carrierHz > 0 ? h.carrierHz : h.hz));
        if (d <= distance) { closest = &h; distance = d; }
    }
    if (closest) {
        H.hopping = false;
        listen(a, *closest);
    }
    return true;
}

// The plot may be outside the old capture while the one-second restart is
// pending. Keep its unbounded axis navigation enabled until the new lock.
void voiceScoutSpectrumInteraction(App& a) {
    (void)a;
    if (!voiceScoutNavigating()) return;
    // A gesture on its own does not interrupt acquisition. The new view is
    // assessed after the axis links have updated in voiceScoutWindow().
    if (H.restartPending) H.lastInteraction = ImGui::GetTime();
}

void voiceScoutWindow(App& a) {
    if (!a.voiceScoutOpen) H.wasOpen = false;
    if (!H.wasOpen) {
        if (!H.scanning) initializeRange(a, a.freqMhz);
        H.wasOpen = true;
    }
    const double rateNow = tunerRate(a);
    if (!H.scanning && rateNow > 0) H.sourceRateHz = rateNow;
    // Keep the initialized scan range independent of the viewport. Zooming
    // (or panning) fully inside the interval is a pure UI operation.
    double viewLo = 0, viewHi = 0;
    specFreqViewGet(viewLo, viewHi);
    const bool validView = std::isfinite(viewLo) && std::isfinite(viewHi) &&
                           viewLo > 0 && viewHi > viewLo;
    const double eps = 1e-8; // avoid a restart from axis-link rounding
    const bool outside = validView && (viewLo < H.fromMHz - eps || viewHi > H.toMHz + eps) &&
        // If the user zooms out beyond the SDR bandwidth, another restart
        // cannot make that wider view capturable. Avoid a restart loop.
        ((viewHi - viewLo <= H.toMHz - H.fromMHz + eps) ||
         std::fabs((viewLo + viewHi - H.fromMHz - H.toMHz) * 0.5) > eps);
    const bool dragging = ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1) || ImGui::IsMouseDown(2);
    // External receiver changes outrank plot navigation. The old spectrum
    // view must never be used to derive a range after a memory/mode retune.
    const bool captureChanged = H.scanning &&
        (a.rxId != H.receiverId ||
         std::fabs(radioCenterMhz(a) * 1e6 - H.lockedCentre) > 1000 ||
         (a.engine.running() && std::fabs(a.engine.sampleRate() - H.lockedRate) > 100));
    const bool bandwidthChanged = H.scanning && rateNow > 0 &&
                                  std::fabs(rateNow - H.sourceRateHz) > 100;
    if (captureChanged || bandwidthChanged) {
        stop(a, true);
        H.restartPending = true;
        H.externalRetune = true;
        H.externalCentreMHz = radioCenterMhz(a);
        H.lastInteraction = ImGui::GetTime();
    } else if (H.scanning && outside) {
        stop(a, true);
        H.restartPending = true;
        H.lastInteraction = ImGui::GetTime();
    }
    if (H.restartPending) {
        if (H.externalRetune) {
            // Let OnAir finish an external retune. The capture and plot use
            // the same RF center and the actual sample rate, never the stale
            // pre-recall pan position or memory's un-offset dial frequency.
            const double centre = radioCenterMhz(a);
            const double bandwidthMHz = (a.engine.running() ? a.engine.sampleRate() : tunerRate(a)) / 1e6;
            if (std::isfinite(centre) && centre > 0 && std::isfinite(bandwidthMHz) && bandwidthMHz > 0) {
                H.externalCentreMHz = centre;
                H.fromMHz = std::max(0.000001, centre - bandwidthMHz * 0.5);
                H.toMHz = H.fromMHz + bandwidthMHz;
                specFreqViewCenterOn(a); // new band is visible immediately
            }
        } else if (validView) {
            // Even when zoomed in, take the view's midpoint but extend by
            // half the SDR bandwidth on each side, not by the zoom span.
            const double bandwidthMHz = tunerRate(a) / 1e6;
            if (bandwidthMHz > 0 && std::isfinite(bandwidthMHz)) {
                const double midpoint = (viewLo + viewHi) * 0.5;
                H.fromMHz = std::max(0.000001, midpoint - bandwidthMHz * 0.5);
                H.toMHz = H.fromMHz + bandwidthMHz;
            }
        }
        if (dragging && !H.externalRetune) H.lastInteraction = ImGui::GetTime();
        if ((!dragging || H.externalRetune) && ImGui::GetTime() - H.lastInteraction >= 1.0 &&
            a.engine.running()) {
            const bool adopt = H.externalRetune;
            // start() clears restartPending only after successful reacquisition.
            start(a, true, adopt);
        }
    } else if (!H.scanning) {
        followSpectrumRange();
    }
    // Expire idle history even during the restart delay / capture settling.
    pruneHits(ImGui::GetTime());
    tick(a);
    if (H.scanning && !H.interactionPaused && H.hopping && ImGui::GetTime() - H.lastHop >= std::max(0.5f, H.monitorSeconds)) {
        bool any = false;
        for (const auto& kv : H.hits) if (kv.second.active) { any = true; break; }
        if (any) hopNext(a);
        else H.lastHop = ImGui::GetTime();
    }
    if (!a.voiceScoutOpen) return;
    firstWindowSize(760 * gUi, 485 * gUi);
    if (!ImGui::Begin(trw("Voice scout"), &a.voiceScoutOpen)) { ImGui::End(); return; }
    ImGui::Text("Scan range: %.6f - %.6f MHz (recentered if the view leaves it)", H.fromMHz, H.toMHz);
    ImGui::BeginDisabled(H.scanning || H.restartPending);
    ImGui::SetNextItemWidth(125 * gUi);
    ImGui::InputFloat("Channel step (kHz)", &H.stepKHz, 0, 0, "%.2f");
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Fine tune after first transmission", &H.fineTune)) {
        // Disabling fine tune returns all existing hits to exact grid slots.
        if (!H.fineTune) {
            const double step = H.stepKHz * 1000.0;
            for (auto& kv : H.hits) kv.second.hz = kv.first * step;
            if (H.selectedHz > 0) H.selectedHz = std::round(H.selectedHz / step) * step;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Channel tuning is on-grid by default; subsequent transmissions can refine it (up to +/-45%% of step). Spectrum overlays always show OnAir's measured carrier frequency.");
    ImGui::SetNextItemWidth(125 * gUi);
    ImGui::DragFloat("Min transmission (ms)", &H.minDurationMs, 10, 1000, 30000, "%.0f");
    H.minDurationMs = std::max(1000.0f, H.minDurationMs);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(125 * gUi);
    ImGui::DragFloat("End hang (ms)", &H.hangMs, 10, 100, 3000, "%.0f");
    if (ImGui::Button(H.scanning || H.restartPending ? "Stop" : "Scan")) {
        if (H.scanning || H.restartPending) stop(a); else start(a);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear log")) H.hits.clear();
    ImGui::SameLine();
    ImGui::BeginDisabled(!H.scanning);
    if (ImGui::Button(H.hopping ? "Stop hopping" : "Hop signals")) {
        H.hopping = !H.hopping;
        if (H.hopping) hopNext(a);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::DragFloat("Monitor each (s)", &H.monitorSeconds, 0.5f, 0.5f, 120.0f, "%.1f");
    H.monitorSeconds = std::max(0.5f, H.monitorSeconds);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(115 * gUi);
    const bool countChanged = ImGui::DragInt("Acquiring detection count", &H.acquireCount, 0.1f, 1, 99);
    H.acquireCount = std::clamp(H.acquireCount, 1, 99);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(115 * gUi);
    const bool snrChanged = ImGui::DragFloat("Minimum detection SNR (dB)", &H.minDetectionSnrDb, 0.25f, 0.0f, 60.0f, "%.1f");
    H.minDetectionSnrDb = std::clamp(H.minDetectionSnrDb, 0.0f, 60.0f);
    if (countChanged || snrChanged) H.candidates.clear();
    if (snrChanged && H.scanning && !H.interactionPaused)
        syncListening(a, a.engine.wideAllSession().entries());
    ImGui::TextDisabled("Audio is silent unless the selected signal is on air.");
    ImGui::SetNextItemWidth(115 * gUi);
    ImGui::DragFloat("Remove idle after (s), 0=off", &H.expireSeconds, 1.0f, 0, 86400, "%.0f");
    H.expireSeconds = std::max(0.0f, H.expireSeconds);
    ImGui::TextDisabled("%s | %zu frequencies", H.scanning ? (H.interactionPaused ? "Paused (spectrum interaction)" : "Scanning") : "Stopped", H.hits.size());
    ImGui::TextDisabled("Spectrum markers follow detected RF carriers; the log shows channel tuning frequencies.");
    ImGui::TextDisabled("Digital voice uses the selected wideband decoder when a codec is available.");
    if (!H.message.empty()) ImGui::TextWrapped("%s", H.message.c_str());
    if (H.scanning) ImGui::Text("Locked centre: %.6f MHz | rate: %.3f MS/s | listening: %.6f MHz", H.lockedCentre / 1e6, H.lockedRate / 1e6, H.selectedHz / 1e6);
    ImGui::Separator();
    if (ImGui::BeginTable("##voice_scout_hits", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 55 * gUi);
        ImGui::TableSetupColumn("Frequency (MHz)");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Transmissions");
        ImGui::TableSetupColumn("Last TX (s)");
        ImGui::TableSetupColumn("Total (s)");
        ImGui::TableSetupColumn("Idle (s)");
        ImGui::TableSetupColumn("State");
        ImGui::TableHeadersRow();
        for (auto& kv : H.hits) {
            Hit& h = kv.second;
            if (h.transmissions == 0 && (!h.active ||
                (ImGui::GetTime() - h.started) * 1000.0 < H.minDurationMs)) continue;
            ImGui::PushID((int)kv.first);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%02d", h.displayId);
            ImGui::TableNextColumn();
            char freq[40]; snprintf(freq, sizeof freq, "%.6f", h.hz / 1e6);
            if (ImGui::Selectable(freq, H.selectedHz == h.hz, ImGuiSelectableFlags_SpanAllColumns) && H.scanning) { H.hopping = false; listen(a, h); }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(h.kind == dect2::WsKind::Unknown ? "Unclassified" : dect2::wsKindName(h.kind));
            if (!h.identity.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", h.identity.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", h.transmissions);
            const double liveDuration = h.active ? std::max(0.0, ImGui::GetTime() - h.started) : 0.0;
            ImGui::TableNextColumn(); ImGui::Text("%.0f", std::floor(h.active ? liveDuration : h.lastDuration));
            ImGui::TableNextColumn(); ImGui::Text("%.0f", std::floor(h.totalDuration + liveDuration));
            const double idleSeconds = h.active ? 0.0 : std::max(0.0, ImGui::GetTime() - h.lastHeard);
            ImGui::TableNextColumn(); ImGui::Text("%.0f", std::floor(idleSeconds));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(h.active ? "On air" : "Idle");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
    if (!a.voiceScoutOpen) H.wasOpen = false;
}
