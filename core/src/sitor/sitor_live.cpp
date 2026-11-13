// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SITOR for the engine. See sitor_live.h.
#include "dect2/sitor/sitor_live.h"

#include <algorithm>

namespace dect2 {

namespace {
constexpr size_t kKeepChars = 4000;
}

SitorLiveReceiver::SitorLiveReceiver() {
    for (int i = 0; i < 2; i++) {
        dec_[i].setCharCallback([this, i](int ch, bool err) {
            std::string& t = text_[i];
            t.push_back(err ? '?' : static_cast<char>(ch));
            if (t.size() > kKeepChars) t.erase(0, t.size() - kKeepChars);
        });
        arq_[i].setEventSink([this, i](const std::string& e) {
            std::string& l = arqLog_[i];
            l += e;
            l += '\n';
            if (l.size() > kKeepChars) l.erase(0, l.size() - kKeepChars);
            if (log_) log_(e);
        });
    }
}

void SitorLiveReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(mu_);
    rate_ = inputRateHz;
    ready_ = rate_ >= sitorTuning().minSampleRate - 1;
    if (ready_) { front_.configure(rate_); front_.setOffsetHz(offsetHz_); }
    spec_.reset(); fsk_.reset();
    for (auto& d : dec_) d.reset();
    for (auto& m : arq_) m.reset();
    text_[0].clear(); text_[1].clear();
    arqLog_[0].clear(); arqLog_[1].clear();
    snap_ = SitorTelemetry();
    seq_++;
}

void SitorLiveReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(mu_);
    offsetHz_ = hz;
    if (ready_) front_.setOffsetHz(hz);
}

bool SitorLiveReceiver::ready() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ready_;
}

void SitorLiveReceiver::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    spec_.reset(); fsk_.reset();
    for (auto& d : dec_) d.reset();
    for (auto& m : arq_) m.reset();
    text_[0].clear(); text_[1].clear();
    arqLog_[0].clear(); arqLog_[1].clear();
    info_ = marine::Spectrum::Fsk();
    snap_ = SitorTelemetry();
    seq_++;
}

void SitorLiveReceiver::feed(const cf32* x, size_t n) {
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!ready_) return;
    base_.clear();
    front_.process(x, n, base_);
    if (base_.empty()) return;
    if (spec_.push(base_.data(), base_.size())) {
        info_ = spec_.findFsk(170.0, 600.0);
        if (info_.found) fsk_.setCentreHz(info_.centreHz);
    }
    fsk_.process(base_.data(), base_.size(), [&](float v) { dec_[0].pushBit(v); dec_[1].pushBit(v); arq_[0].pushBit(v); arq_[1].pushBit(v); });   // B = 1 = the higher tone
    publish();
    seq_++;
}

void SitorLiveReceiver::publish() {
    // the decoder that has read more valid words is the right polarity
    const int k = dec_[1].validChars() > dec_[0].validChars() ? 1 : 0;
    const marine::NavtexDecoder& d = dec_[k];
    const bool locked = d.locked();
    snap_.state = locked ? 2 : info_.found ? 1 : 0;
    snap_.cfoHz = info_.found ? info_.centreHz : 0;
    snap_.snrDb = static_cast<float>(info_.found ? info_.snrDb : 0);
    snap_.shiftHz = info_.found ? info_.highHz - info_.lowHz : 0;
    snap_.baud = info_.found ? fsk_.baud() : 0;
    snap_.blocksOk = d.validChars();
    snap_.blocksBad = d.badChars();
    snap_.inverted = k == 1;
    snap_.chars = d.charsDecoded();
    snap_.dataValid = snap_.chars > 0;
    snap_.text = text_[k];
    // SITOR-A: the polarity whose monitor has read more blocks
    const int q = arq_[1].stats().blocks > arq_[0].stats().blocks ? 1 : 0;
    const sitor::ArqStats& s = arq_[q].stats();
    snap_.arqLocked = s.locked;
    snap_.arqBlocks = s.blocks; snap_.arqNew = s.newBlocks; snap_.arqRepeats = s.repeats; snap_.arqBad = s.bad;
    snap_.arqCs = s.controlSignals; snap_.arqChars = s.chars; snap_.arqCsOffset = s.csOffsetBits;
    snap_.arqText = arq_[q].text();
    snap_.selcall = arq_[q].selcall();
    snap_.arqLog = arqLog_[q];
    if (s.locked && !locked) { snap_.inverted = q == 1; snap_.dataValid = s.chars > 0 || snap_.dataValid; }
}

bool SitorLiveReceiver::telemetry(SitorTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(mu_);
    if (seq_ == lastSeq) return false;
    out = snap_;
    out.seq = seq_;
    return true;
}

void SitorLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(mu_);
    log_ = std::move(cb);
}

ModeTuning sitorTuning() {
    ModeTuning t;
    t.stdMode = 121; t.id = "sitor"; t.name = "SITOR";
    t.minMhz = 0.1; t.maxMhz = 30; t.defMhz = 0.518;      // NAVTEX is the SITOR-B most heard; coast stations also use HF
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.01;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;
    return t;
}

} // namespace dect2
