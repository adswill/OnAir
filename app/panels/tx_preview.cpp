// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The preview of what step 3 shows before anything is sent: a short piece of the planned signal, made on a worker thread (some generators take a second
// to start) a moment after the last change of a setting, at the sample rate the radio would run. While a transmission or a test runs, the session's own
// tap (the IQ really handed to the radio) is shown instead. The preview never opens a microphone, a camera or a radio.
#include "dect2/common/thread_name.h"
#include "dect2/common/diag.h"
#include "tx_state.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace txui {

namespace {
struct Job { std::future<PreviewData> fut; std::string key; };
PreviewData gData;
Job gJob;
std::string gShownKey, gWantKey;
double gWantSince = 0;

// the settings that change the signal, as one string (a change starts a new preview)
std::string keyOf(const TxRequest& r, double rate) {
    char b[256];
    snprintf(b, sizeof b, "%s|%.0f|%.2f|%s|%s|%s|%s|%s|%s|%s|%s|%s|", r.modeId.c_str(), rate, r.bandwidthMhz, r.callsign.c_str(), r.grid.c_str(), r.text.c_str(), r.dest.c_str(), r.path.c_str(), r.file.c_str(), r.name.c_str(), r.provider.c_str(), r.network.c_str());
    std::string k = b;
    for (const auto& sv : r.services) k += sv + ",";
    snprintf(b, sizeof b, "|%d %d %d %d %d %d %d %d %u %.1f %.4f %.4f %d %.0f %.0f %d %d %d %d %d %d %d", r.kind, r.baudIdx, r.shiftIdx, r.variant, r.wpm, r.dbm, r.function, r.serviceId, r.address, r.toneHz, r.lat, r.lon, (int)r.reverse,
             r.deviationHz, r.preemphUs, r.rdsPi, r.slot, r.colorCode, r.talkgroup, (int)r.analogVoice, r.audioSrc, r.videoSrc);
    k += b;
    for (const auto& kv : r.opt) { snprintf(b, sizeof b, " %s=%.4f", kv.first.c_str(), kv.second); k += b; }
    for (int v : r.synth.modeOpt) k += " " + std::to_string(v);
    for (double v : r.synth.modeVal) k += " " + std::to_string(v);
    return k;
}

// makes about a quarter of a second of the signal (at most 2^18 samples) off the UI thread
PreviewData make(TxRequest r, const TxModeSpec* mode, double rate) {
    PreviewData d;
    d.rateHz = rate;
    r.audioSrc = r.audioSrc == 0 ? 2 : r.audioSrc;   // a preview never opens the microphone
    r.videoSrc = 0;
    r.pullAudio = nullptr;
    r.pullVideo = nullptr;
    try {
        std::string err;
        auto g = makeTxGenerator(*mode, r, rate, err);
        if (!g) { d.error = err; d.ready = true; return d; }
        const size_t n = (size_t)std::max(8192.0, std::min(262144.0, rate * 0.25));
        d.iq.resize(n);
        g->generate(d.iq.data(), n);
        txOutputStage(r, d.iq.data(), n, 1u << 30, rate);   // the preview shows the trim and the mirrored spectrum (not the fade-in)
        size_t lim = 0;
        txSoftLimit(d.iq.data(), n, &lim);   // the preview shows what the radio would get: after the limiter
    } catch (const std::exception& e) {   // a generator that throws must not take the program down through future::get()
        d.iq.clear();
        DIAG(257228, "The transmit preview could not be made (%s), so no preview is shown.", e.what());
        d.error = std::string("the preview could not be made: ") + e.what();
    }
    d.ready = true;
    return d;
}
}

void previewDrop() { gShownKey.clear(); gWantKey.clear(); gData = PreviewData(); }

// The IQ to show: the running session's tap, else the planned signal (made in the background; `busy` while it is being made)
const PreviewData& previewFor(TxUi& s, const TxModeSpec& mode, const DeviceInfo* radio) {
    if (s.session.running()) {
        gData.ready = false;
        static PreviewData live;
        live.busy = false; live.error.clear();
        s.session.copyTap(live.iq, live.rateHz);
        live.ready = !live.iq.empty();
        return live;
    }
    TxRequest r = makeReq(s, mode, false);
    const double rate = txRequestRate(mode, r, radio ? TxRateLimits{radio->txMinRateHz, radio->txMaxRateHz} : TxRateLimits{});
    const std::string key = keyOf(r, rate);
    const double now = ImGui::GetTime();
    if (key != gWantKey) { gWantKey = key; gWantSince = now; }
    if (gJob.fut.valid() && gJob.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        gData = gJob.fut.get();
        gShownKey = gJob.key;
    }
    if (!gJob.fut.valid() && gShownKey != gWantKey && now - gWantSince > 0.35) {
        gJob.key = gWantKey;
        gJob.fut = std::async(std::launch::async, [r, &mode, rate] { dect2::setThreadName("onair-txprev"); return make(r, &mode, rate); });
    }
    gData.busy = gJob.fut.valid() || gShownKey != gWantKey;
    return gData;
}

} // namespace txui
