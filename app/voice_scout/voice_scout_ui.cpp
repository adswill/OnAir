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
    double hz = 0;
    unsigned transmissions = 0;
    double lastDuration = 0, totalDuration = 0;
    double started = 0, lastHeard = 0;
    bool active = false;
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
    bool muteIfIdle = true;
    int acquireCount = 2;
    float minDetectionSnrDb = 6.0f;
    bool interactionPaused = false;
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
        ++h.transmissions;
        h.lastDuration = duration;
        h.totalDuration += duration;
        h.lastHeard = now;
    }
    h.active = false;
}
void stop(App& a) {
    if (!H.scanning) return;
    const double now = ImGui::GetTime();
    for (auto& kv : H.hits) finish(kv.second, now);
    H.scanning = false;
    H.hopping = false;
    H.interactionPaused = false;
    H.draggingSpectrum = false;
    H.candidates.clear();
    if (a.engine.running()) {
        a.engine.wideAllSession().setAudioSolo(0);
        a.engine.wideAllSession().setVoiceAudioFocus(-1, 0);
        a.engine.wideAllSession().setListen(0);
        a.wideAllListen = 0;
        a.engine.setVoiceScoutExclusiveAudio(false);
    }
    if (H.listenManualId && a.engine.running()) a.engine.wideAllSession().remove(H.listenManualId);
    H.listenManualId = 0;
    H.listenManualHz = 0;
    H.listenKind = dect2::WsKind::Unknown;
    if (!H.priorWide && a.engine.running()) a.engine.setWideAll(false);
    H.selectedHz = 0;
}

void start(App& a) {
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
    const double rate = tunerRate(a);
    if (!(rate > 0) || rate < needed - 1) {
        H.message = "Visible spectrum does not fit the current SDR bandwidth. Zoom in or adjust bandwidth manually.";
        return;
    }
    const double mid = (H.fromMHz + H.toMHz) * 0.5;
    H.priorWide = a.engine.wideAll();
    const double oldRfCentre = radioCenterMhz(a);
    const double offsetMHz = oldRfCentre - a.freqMhz;
    a.freqMhz = mid - offsetMHz;
    a.tune.centerHz = mid * 1e6;
    if (a.engine.running()) {
        if (std::fabs(oldRfCentre - mid) > 1e-7)
            a.engine.retune(a.tune);
    } else {
        startReceiver(a);
    }
    if (!a.engine.running()) { H.message = "Could not start the radio: " + a.engine.startError(); return; }
    if (a.engine.sampleRate() < needed) {
        H.message = "SDR sample rate is smaller than the capture required; adjust the rate and retry.";
        return;
    }
    a.engine.setWideAll(true);
    a.engine.wideAllSession().setOnlyManual(false);
    H.hits.clear();
    H.candidates.clear();
    H.lastSnapshot = a.engine.wideAllSession().status().snapshotSerial;
    H.nextDisplayId = 1;
    H.receiverId = a.rxId;
    H.lockedCentre = radioCenterMhz(a) * 1e6;
    H.lockedRate = a.engine.sampleRate();
    H.sourceRateHz = tunerRate(a);
    H.selectedHz = 0;
    H.listenManualId = 0;
    H.listenManualHz = 0;
    H.listenKind = dect2::WsKind::Unknown;
    H.hopping = false;
    H.interactionPaused = false;
    H.lastHop = ImGui::GetTime();
    a.engine.wideAllSession().setAudioSolo(-1);
    a.engine.wideAllSession().setVoiceAudioFocus(0, 0);
    a.engine.setVoiceScoutExclusiveAudio(true);
    H.scanning = true;
}

void syncListening(App& a, const std::vector<dect2::WsEntry>& entries) {
    if (!H.scanning) return;
    auto& ws = a.engine.wideAllSession();
    if (H.selectedHz <= 0) {
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
    if (H.muteIfIdle && !onAir) {
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
    if (a.rxId != H.receiverId || !a.engine.running() || !a.engine.wideAll() ||
        std::abs(radioCenterMhz(a) * 1e6 - H.lockedCentre) > 1000 ||
        std::abs(a.engine.sampleRate() - H.lockedRate) > 100) {
        H.message = "Capture changed or receiver stopped. Voice scout stopped; press Scan to relock the band.";
        stop(a);
        return;
    }
    const double now = ImGui::GetTime();
    const double step = H.stepKHz * 1000.0, lo = H.fromMHz * 1e6, hi = H.toMHz * 1e6;
    for (auto it = H.hits.begin(); it != H.hits.end();) {
        if (it->second.hz < lo || it->second.hz > hi) {
            finish(it->second, now);
            it = H.hits.erase(it);
        } else ++it;
    }
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
    struct Detection { dect2::WsKind kind; std::string identity; };
    std::map<long long, Detection> present;
    for (const auto& e : entries) {
        if (e.id >= dect2::kFirstManualId || !voiceKind(e.kind) || !e.active ||
            !std::isfinite(e.snrDb) || e.snrDb < H.minDetectionSnrDb) continue;
        const double hz = e.absHz > 0 ? e.absHz : H.lockedCentre + e.offsetHz;
        if (hz < lo - step * 0.4 || hz > hi + step * 0.4) continue;
        const long long channel = llround(hz / step);
        if (channel < 0 || channel * step > hi + 1) continue;
        auto found = present.find(channel);
        if (found == present.end() || e.state == dect2::WsState::Running)
            present[channel] = {e.kind, e.identity};
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
        h.hz = p.first * step;
        if (!h.active) { h.active = true; h.started = now; }
        h.lastHeard = now;
        h.kind = p.second.kind;
        h.identity = p.second.identity;
    }
    {
        for (auto it = H.hits.begin(); it != H.hits.end();) {
            const Hit& h = it->second;
            if (!h.active && (h.transmissions == 0 ||
                (h.lastHeard > 0 && now - h.lastHeard > H.expireSeconds)))
                it = H.hits.erase(it);
            else ++it;
        }
    }
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

std::vector<VoiceScoutMarker> voiceScoutMarkers() {
    std::vector<VoiceScoutMarker> result;
    if (!H.scanning) return result;
    result.reserve(H.hits.size());
    for (const auto& kv : H.hits) {
        const Hit& h = kv.second;
        if (h.hz <= 0 || (h.transmissions == 0 && (!h.active ||
            (ImGui::GetTime() - h.started) * 1000.0 < H.minDurationMs))) continue;
        result.push_back({h.hz / 1e6, h.active, std::fabs(H.selectedHz - h.hz) < 1.0, h.displayId});
    }
    return result;
}

bool voiceScoutSelectFrequency(App& a, double mhz) {
    if (!H.scanning) return false;
    if (!std::isfinite(mhz)) return true;
    Hit* closest = nullptr;
    const double toleranceHz = std::max(2500.0, H.stepKHz * 500.0);
    double distance = toleranceHz;
    for (auto& kv : H.hits) {
        Hit& h = kv.second;
        if (h.hz <= 0 || (h.transmissions == 0 && (!h.active ||
            (ImGui::GetTime() - h.started) * 1000.0 < H.minDurationMs))) continue;
        const double d = std::fabs(mhz * 1e6 - h.hz);
        if (d <= distance) { closest = &h; distance = d; }
    }
    if (closest) {
        H.hopping = false;
        listen(a, *closest);
    }
    return true;
}

void voiceScoutSpectrumInteraction(App& a) {
    if (!H.scanning) return;
    H.interactionPaused = true;
    H.lastInteraction = ImGui::GetTime();
    H.draggingSpectrum |= ImGui::IsMouseDragging(0, 3.f) ||
                          ImGui::IsMouseDragging(1, 3.f) ||
                          ImGui::IsMouseDragging(2, 3.f);
    if (a.engine.running()) {
        a.engine.wideAllSession().setAudioSolo(-1);
        a.engine.wideAllSession().setVoiceAudioFocus(0, 0);
        a.engine.wideAllSession().setListen(0);
        a.wideAllListen = 0;
    }
}

void voiceScoutWindow(App& a) {
    if (!a.voiceScoutOpen) H.wasOpen = false;
    if (!H.wasOpen) {
        if (!H.scanning) initializeRange(a, a.freqMhz);
        H.wasOpen = true;
    }
    const double rateNow = tunerRate(a);
    if (!H.scanning && rateNow > 0) H.sourceRateHz = rateNow;
    followSpectrumRange();
    if (H.scanning && H.interactionPaused && H.draggingSpectrum &&
        !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2)) {
        H.draggingSpectrum = false;
        const double mid = (H.fromMHz + H.toMHz) * 0.5;
        if (std::isfinite(mid) && mid > 0 &&
            (H.toMHz - H.fromMHz) * 1e6 <= tunerRate(a) + 1.0 &&
            a.engine.running()) {
            const double oldRfCentre = radioCenterMhz(a);
            const double offsetMHz = oldRfCentre - a.freqMhz;
            a.freqMhz = mid - offsetMHz;
            a.tune.centerHz = mid * 1e6;
            if (std::fabs(oldRfCentre - mid) > 1e-7)
                a.engine.retune(a.tune);
        }
    }
    if (H.scanning && rateNow > 0 && std::fabs(rateNow - H.sourceRateHz) > 100) {
        H.interactionPaused = true;
        H.lastInteraction = ImGui::GetTime();
        H.sourceRateHz = rateNow;
    }
    if (H.scanning && H.interactionPaused &&
        !ImGui::IsMouseDown(0) && !ImGui::IsMouseDown(1) && !ImGui::IsMouseDown(2) &&
        ImGui::GetTime() - H.lastInteraction >= 1.0) {
        const double lo = H.fromMHz, hi = H.toMHz;
        for (auto& kv : H.hits) finish(kv.second, ImGui::GetTime());
        auto savedHits = std::move(H.hits);
        const int savedNextId = H.nextDisplayId;
        const bool savedHopping = H.hopping;
        const double savedSelection = H.selectedHz;
        const auto savedKind = H.listenKind;
        stop(a);
        if (lo > 0 && hi > lo && std::isfinite(lo) && std::isfinite(hi)) {
            H.fromMHz = lo;
            H.toMHz = hi;
            start(a);
            if (H.scanning) {
                H.hits = std::move(savedHits);
                H.nextDisplayId = savedNextId;
                H.hopping = savedHopping;
                if (savedSelection >= lo * 1e6 && savedSelection <= hi * 1e6) {
                    H.selectedHz = savedSelection;
                    H.listenKind = savedKind;
                }
            }
        }
    }
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
    ImGui::Text("Spectrum range: %.6f - %.6f MHz (zoom/pan the spectrum to change)", H.fromMHz, H.toMHz);
    ImGui::BeginDisabled(H.scanning);
    ImGui::SetNextItemWidth(125 * gUi);
    ImGui::InputFloat("Channel step (kHz)", &H.stepKHz, 0, 0, "%.2f");
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(125 * gUi);
    ImGui::DragFloat("Min transmission (ms)", &H.minDurationMs, 10, 1000, 30000, "%.0f");
    H.minDurationMs = std::max(1000.0f, H.minDurationMs);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(125 * gUi);
    ImGui::DragFloat("End hang (ms)", &H.hangMs, 10, 100, 3000, "%.0f");
    if (ImGui::Button(H.scanning ? "Stop" : "Scan")) { if (H.scanning) stop(a); else start(a); }
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
    if (ImGui::Checkbox("Mute if idle", &H.muteIfIdle) && H.scanning && !H.interactionPaused)
        syncListening(a, a.engine.wideAllSession().entries());
    ImGui::SetNextItemWidth(115 * gUi);
    ImGui::DragFloat("Remove idle after (s), 0=off", &H.expireSeconds, 1.0f, 0, 86400, "%.0f");
    H.expireSeconds = std::max(0.0f, H.expireSeconds);
    ImGui::TextDisabled("%s | %zu frequencies", H.scanning ? (H.interactionPaused ? "Paused (spectrum interaction)" : "Scanning") : "Stopped", H.hits.size());
    ImGui::TextDisabled("Digital voice uses the selected wideband decoder when a codec is available.");
    if (!H.message.empty()) ImGui::TextWrapped("%s", H.message.c_str());
    if (H.scanning) ImGui::Text("Locked centre: %.6f MHz | rate: %.3f MS/s | listening: %.6f MHz", H.lockedCentre / 1e6, H.lockedRate / 1e6, H.selectedHz / 1e6);
    ImGui::Separator();
    if (ImGui::BeginTable("##voice_scout_hits", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 55 * gUi);
        ImGui::TableSetupColumn("Frequency (MHz)");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Transmissions");
        ImGui::TableSetupColumn("Last TX (s)");
        ImGui::TableSetupColumn("Total (s)");
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
            ImGui::TableNextColumn(); ImGui::TextUnformatted(h.active ? "On air" : "Idle");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
    if (!a.voiceScoutOpen) H.wasOpen = false;
}
