// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Decode help: see decode_progress.h.
#include "dect2/doctor/decode_progress.h"
#include "dect2/source/gain.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace dect2::doctor {

namespace {
bool hasCode(const Report& r, Code c) { return std::any_of(r.findings.begin(), r.findings.end(), [&](const Finding& f) { return f.code == c; }); }

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o;
}
}  // namespace

Progress progress(const Report& rep, const StageInput& in) {
    Progress p;
    p.signal = in.samples ? StageState::Yes : StageState::No;
    if (rep.valid) {
        if (hasCode(rep, Code::NoSignal)) p.carrier = StageState::No;
        else if (hasCode(rep, Code::WeakSignal)) p.carrier = StageState::Partial;
        else p.carrier = StageState::Yes;
    }
    if (in.syncState == 0) p.sync = StageState::No;
    else if (in.syncState == 1) p.sync = StageState::Partial;
    else if (in.syncState >= 2) p.sync = StageState::Yes;
    if (in.blocksOk > 0) p.fec = in.blocksBad > in.blocksOk * 4 ? StageState::Partial : StageState::Yes;
    else if (in.blocksOk == 0 && in.blocksBad > 0) p.fec = StageState::No;
    else if (in.blocksOk == 0) p.fec = p.sync == StageState::Yes ? StageState::No : StageState::Unknown;
    // a later stage proves the earlier ones
    const bool frames = p.fec == StageState::Yes || p.fec == StageState::Partial;
    if (frames && p.sync != StageState::Yes) p.sync = in.syncState < 0 ? StageState::Yes : p.sync == StageState::No ? StageState::Partial : p.sync;
    if ((frames || p.sync == StageState::Yes || p.sync == StageState::Partial) && p.carrier != StageState::Yes) p.carrier = StageState::Yes;
    if (p.carrier == StageState::Yes || p.sync != StageState::Unknown || frames) { if (p.signal == StageState::No && (frames || p.sync == StageState::Yes)) p.signal = StageState::Yes; }
    const StageState* order[4] = {&p.signal, &p.carrier, &p.sync, &p.fec};
    for (int i = 0; i < 4; i++) if (*order[i] == StageState::Yes || *order[i] == StageState::Partial) p.furthest = i;
    return p;
}

bool AutoOpen::update(double now, bool running, bool decoding, const std::string& mode) {
    if (mode != mode_) { mode_ = mode; since_ = -1; done_ = false; }
    if (!running) { since_ = -1; done_ = false; return false; }
    if (done_) return false;
    if (decoding) { done_ = true; return false; }   // it worked in this run: no need
    if (since_ < 0) since_ = now;
    if (now - since_ < kAfterSec) return false;
    done_ = true;
    return true;
}

size_t clipBytes(size_t samples) { return std::min(samples, kMaxClipSamples) * 2; }

std::string clipMeta(const ClipInfo& info) {
    char b[120];
    std::string m = "{\n  \"global\": {\n    \"core:datatype\": \"ci8\",\n";
    snprintf(b, sizeof b, "    \"core:sample_rate\": %.0f,\n", info.rateHz);
    m += b;
    m += "    \"core:version\": \"1.0.0\",\n";
    m += "    \"core:description\": \"" + jsonEsc("OnAir failed-signal clip, mode " + info.modeId + (info.note.empty() ? "" : ": " + info.note)) + "\"\n  },\n";
    snprintf(b, sizeof b, "  \"captures\": [ { \"core:sample_start\": 0, \"core:frequency\": %.0f } ],\n", info.freqHz);
    m += b;
    m += "  \"annotations\": []\n}\n";
    return m;
}

bool writeClip(const std::string& basePath, const cf32* x, size_t n, const ClipInfo& info, std::string* err) {
    n = std::min(n, kMaxClipSamples);
    std::vector<int8_t> d(n * 2);
    // scale to the strongest sample so that a weak signal keeps its bits
    float peak = 1e-6f;
    for (size_t i = 0; i < n; i++) peak = std::max({peak, std::fabs(x[i].real()), std::fabs(x[i].imag())});
    const float g = 127.0f / peak;
    for (size_t i = 0; i < n; i++) {
        d[2 * i] = (int8_t)std::lround(std::clamp(x[i].real() * g, -127.0f, 127.0f));
        d[2 * i + 1] = (int8_t)std::lround(std::clamp(x[i].imag() * g, -127.0f, 127.0f));
    }
    {
        std::ofstream f(u8file(basePath + ".sigmf-data"), std::ios::binary);
        f.write((const char*)d.data(), (std::streamsize)d.size());
        if (!f) { if (err) *err = "could not write the data file"; return false; }
    }
    std::ofstream m(u8file(basePath + ".sigmf-meta"), std::ios::binary);
    m << clipMeta(info);
    if (!m) { if (err) *err = "could not write the meta file"; return false; }
    return true;
}

Coaching coach(const Report& rep, const Context& c, bool antennaKnown, bool antennaForBand) {
    Coaching out;
    if (!rep.valid) { out.advice.push_back(Advice::NoAdvice); return out; }
    if (!c.live) { out.advice.push_back(Advice::NoAdvice); return out; }   // a recording has no gain or antenna to change
    const Measure& m = rep.m;
    bool gainSaid = false;
    const double headroom = c.gainMaxDb > 0 ? c.gainMaxDb - c.gainDb : 20;
    if (m.clipPct > 0.1 || m.peak >= 0.98) {
        out.advice.push_back(Advice::GainDown);
        out.gainDeltaDb = m.clipPct > 2 ? 12 : 6;
        gainSaid = true;
    } else if (m.rmsDbfs < -45 && headroom > 3 && !adcLevelCompensated()) {   // (AirspyHF+: the level does not follow the gain)
        out.advice.push_back(Advice::GainUp);
        out.gainDeltaDb = std::min(headroom, 10.0);
        gainSaid = true;
    }
    // no signal above the noise: gain does not help beyond making the noise visible
    const bool nothing = std::any_of(rep.findings.begin(), rep.findings.end(), [](const Finding& f) { return f.code == Code::NoSignal || f.code == Code::WeakSignal; });
    if (antennaKnown && !antennaForBand) out.advice.push_back(Advice::NeedsAntennaForBand);
    else if (nothing) {
        if (headroom <= 3) out.advice.push_back(Advice::LnaHelps);
        out.advice.push_back(Advice::MoveAntenna);
    } else if (antennaKnown) out.advice.push_back(Advice::AntennaOk);
    if (!gainSaid && !nothing) out.advice.insert(out.advice.begin(), Advice::GainGood);
    if (out.advice.empty()) out.advice.push_back(Advice::NoAdvice);
    return out;
}

} // namespace dect2::doctor
