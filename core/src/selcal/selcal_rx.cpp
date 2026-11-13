// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/selcal/selcal_rx.h"
#include "dect2/selcal/selcal_tel.h"
#include "dect2/tones/tones_proto.h"
#include "dect2/tones/tones_rx.h"
#include <algorithm>
#include <atomic>
#include <ctime>
#include <map>
#include <mutex>

namespace dect2 {

struct SelcalReceiver::Impl {
    TonesReceiver tr;
    std::mutex mu;
    double dialHz = 0;
    uint64_t seq = 0, lastToneSeq = 0, serial = 0;
    std::vector<SelcalCall> calls;
    std::vector<SelcalCode> codes;
    std::map<std::string, std::string> aircraft;
    uint64_t seenEvents = 0;          // SELCAL events of the tone receiver already taken
    uint64_t okCount = 0;
    SelcalTelemetry pub;
    bool havePub = false;
    int64_t lastCallUtc = 0;
    std::function<void(const std::string&)> log;

    void absorb(const TonesTelemetry& t) {
        // the tone receiver lists events newest first and counts the SELCAL ones: take the new ones, oldest first
        const uint64_t total = (uint64_t)t.counts[TkSelcal];
        uint64_t fresh = total > seenEvents ? total - seenEvents : 0;
        seenEvents = total;
        std::vector<const ToneEvent*> ev;
        for (const ToneEvent& e : t.events) if (e.kind == TkSelcal) { if (ev.size() < fresh) ev.push_back(&e); }
        for (size_t i = ev.size(); i-- > 0;) {
            const ToneEvent& e = *ev[i];
            SelcalCall c;
            c.serial = ++serial; c.utc = e.wall; c.timeSec = e.timeSec; c.code = e.text; c.levelDb = e.levelDb; c.dialHz = dialHz;
            double hz[4];
            if (selcalCodeTones(c.code, hz)) std::copy(hz, hz + 4, c.hz);
            auto it = aircraft.find(c.code);
            if (it != aircraft.end()) c.aircraft = it->second;
            calls.insert(calls.begin(), c);
            if (calls.size() > kSelcalCallsMax) calls.pop_back();
            auto cd = std::find_if(codes.begin(), codes.end(), [&](const SelcalCode& x) { return x.code == c.code; });
            if (cd == codes.end()) { SelcalCode n; n.code = c.code; n.aircraft = c.aircraft; n.firstSeen = c.utc; codes.insert(codes.begin(), n); cd = codes.begin(); }
            cd->count++; cd->lastSeen = c.utc; cd->lastDialHz = dialHz;
            if (cd != codes.begin()) { SelcalCode keep = *cd; codes.erase(cd); codes.insert(codes.begin(), keep); }
            okCount++; lastCallUtc = c.utc;
            if (log) log("SELCAL " + c.code + (c.aircraft.empty() ? "" : " (" + c.aircraft + ")"));
        }
    }
};

SelcalReceiver::SelcalReceiver() : p_(new Impl) { p_->tr.setSelcalOnly(true); p_->tr.setSilent(false); }
SelcalReceiver::~SelcalReceiver() = default;
void SelcalReceiver::configure(double r) { p_->tr.configure(r); }
void SelcalReceiver::setSignalOffset(double hz) { p_->tr.setSignalOffset(hz); }
void SelcalReceiver::setAudioInput(bool on) { p_->tr.setAudioInput(on); }
void SelcalReceiver::setDemod(int d) { p_->tr.setDemod(d); }
void SelcalReceiver::setDialHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->dialHz = hz; }
bool SelcalReceiver::ready() const { return p_->tr.ready(); }
void SelcalReceiver::reset() { p_->tr.reset(); }
void SelcalReceiver::clearResults() {
    p_->tr.clearResults();
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->calls.clear(); p_->codes.clear(); p_->seenEvents = 0; p_->okCount = 0; p_->havePub = false; p_->seq++;
}
void SelcalReceiver::feed(const cf32* x, size_t n) { p_->tr.feed(x, n); }
void SelcalReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void SelcalReceiver::setListen(bool on) { p_->tr.setListen(on); }
void SelcalReceiver::setVolume(float v) { p_->tr.setVolume(v); }
void SelcalReceiver::setMuted(bool m) { p_->tr.setMuted(m); }
void SelcalReceiver::setSilent(bool s) { p_->tr.setSilent(s); }

void SelcalReceiver::setAircraftTable(const std::string& tsv) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->aircraft.clear();
    size_t i = 0;
    while (i < tsv.size()) {
        size_t e = tsv.find('\n', i); if (e == std::string::npos) e = tsv.size();
        std::string l = tsv.substr(i, e - i); i = e + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.empty() || l[0] == '#') continue;
        const size_t t = l.find('\t'); if (t == std::string::npos) continue;
        std::string code = l.substr(0, t), reg = l.substr(t + 1);
        code.erase(std::remove(code.begin(), code.end(), ' '), code.end());
        std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return (char)std::toupper(c); });
        if (code.size() == 4) code.insert(2, "-");
        if (code.size() == 5 && !reg.empty()) p_->aircraft[code] = reg;
    }
}

bool SelcalReceiver::telemetry(SelcalTelemetry& out, uint64_t lastSeq) {
    TonesTelemetry tt;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tr.telemetry(tt, p_->lastToneSeq)) {
        p_->lastToneSeq = tt.seq;
        p_->absorb(tt);
        SelcalTelemetry s;
        s.seq = ++p_->seq;
        s.state = !p_->calls.empty() && (int64_t)std::time(nullptr) - p_->lastCallUtc < 60 ? 2 : tt.state == 1 ? 1 : 0;
        s.cfoHz = 0; s.snrDb = tt.snrDb; s.levelDb = tt.levelDb;
        s.dataValid = p_->okCount > 0; s.blocksOk = p_->okCount; s.blocksBad = 0;
        s.timeSec = tt.timeSec; s.inputRate = tt.inputRate; s.dialHz = p_->dialHz; s.demod = tt.demod; s.audioInput = tt.audioInput; s.audioDb = tt.audioDb;
        s.toneHz = tt.toneHz; s.toneDb = tt.toneDb; s.spec = tt.spec;
        s.calls = p_->calls; s.codes = p_->codes;
        p_->pub = std::move(s); p_->havePub = true;
    }
    if (!p_->havePub || p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

bool selcalCodeTones(const std::string& code, double hz[4]) {
    std::string c;
    for (char ch : code) if (ch != '-' && ch != ' ') c += ch;
    if (c.size() != 4) return false;
    for (int i = 0; i < 4; i++) {
        int k = -1;
        for (int j = 0; j < tones::kSelcalN; j++) if (tones::selcalLetter(j) == c[(size_t)i]) k = j;
        if (k < 0) return false;
        hz[i] = tones::selcalHz(k);
    }
    return true;
}

ModeTuning selcalTuning() {
    ModeTuning t = tonesTuning();
    t.stdMode = 242; t.id = "selcal"; t.name = "SELCAL (aircraft tone call)";
    t.minMhz = 1.5; t.maxMhz = 400; t.defMhz = 8.891;    // HF aeronautical voice (USB), VHF airband (AM)
    t.bandwidthMhz = 0.006;
    return t;
}

} // namespace dect2
