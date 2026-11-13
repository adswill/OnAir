// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/specx/scan_propose.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace specx {

const char* surenessName(int s) {
    return s == SureSure ? "sure" : s == SureLikely ? "likely" : s == SureGuess ? "guess" : "unknown";
}

static Proposal fromResult(const sigid::Result& r, double rfHz, const sigid::Region* reg) {
    Proposal p;
    p.result = r;
    p.rfHz = r.f.valid ? r.f.rfHz : rfHz + (reg ? reg->centreHz : 0);
    p.bwHz = r.f.valid ? r.f.obwHz : reg ? reg->bwHz : 0;
    p.snrDb = r.f.valid ? r.f.snrDb : reg ? reg->snrDb : 0;
    if (!r.f.valid) return p;
    p.summary = r.summary;
    for (const sigid::Candidate& c : r.cands) {
        if (p.guesses.size() >= 4) break;
        p.guesses.push_back({c.name, c.modeId, c.confidence});
    }
    if (r.verdict != sigid::VerdictUnknown && !r.cands.empty()) {
        const sigid::Candidate& c = r.cands[0];
        p.name = c.name; p.modeId = c.modeId; p.confidence = c.confidence;
        const bool verified = c.verify == sigid::VerifyConfirmed;
        p.sureness = r.verdict == sigid::VerdictLikely ? (verified || c.confidence >= 0.8 ? SureSure : SureLikely) : SureGuess;
        if (c.verify == sigid::VerifyFailed && p.sureness > SureGuess) p.sureness = SureGuess;
        p.memory = sigid::toMemory(c, r.f);
        p.canOpen = !p.memory.mode.empty();
    }
    return p;
}

Proposal proposeOne(const cf32* x, size_t n, double rateHz, double loHz, double hiHz, double rfHz) {
    sigid::Options o;
    o.rfHz = rfHz; o.loHz = loHz; o.hiHz = hiHz;
    return fromResult(sigid::identify(x, n, rateHz, o), rfHz, nullptr);
}

std::vector<Proposal> proposeAll(const cf32* x, size_t n, double rateHz, const ScanOptions& so) {
    std::vector<Proposal> out;
    if (!x || n < 1024 || rateHz <= 0) return out;
    for (const sigid::Region& g : sigid::findRegions(x, n, rateHz, so.maxSignals, so.thresholdDb)) {
        sigid::Options o;
        o.rfHz = so.rfHz;
        o.loHz = g.loHz; o.hiHz = g.hiHz;
        Proposal p = fromResult(sigid::identify(x, n, rateHz, o), so.rfHz, &g);
        if (p.sureness == SureNone && !so.keepUnknown) continue;
        out.push_back(std::move(p));
    }
    // the ones the app can open first (sure before guess), then the strongest
    std::stable_sort(out.begin(), out.end(), [](const Proposal& a, const Proposal& b) {
        if (a.sureness != b.sureness) return a.sureness > b.sureness;
        if (std::fabs(a.confidence - b.confidence) > 1e-9) return a.confidence > b.confidence;
        return a.snrDb > b.snrDb;
    });
    return out;
}

static std::string width(double hz) {
    char b[32];
    if (hz >= 1e6) snprintf(b, sizeof b, "%.3g MHz", hz / 1e6); else snprintf(b, sizeof b, "%.3g kHz", hz / 1e3);
    return b;
}

std::string proposalLine(const Proposal& p) {
    if (p.sureness == SureNone) return "Unknown signal, " + width(p.bwHz) + " wide";
    if (p.sureness == SureSure) return "Sure: " + p.name;
    if (p.sureness == SureLikely) return "Probably: " + p.name;
    std::string s = "Could be: " + p.name;
    if (p.guesses.size() > 1) s += " or " + p.guesses[1].name;
    return s;
}

} // namespace specx
} // namespace dect2
