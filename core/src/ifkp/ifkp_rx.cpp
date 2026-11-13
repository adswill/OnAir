// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IFKP receiver (see ifkp_rx.h): the common station of kbd33_station.h with this modem.
#include "dect2/ifkp/ifkp_rx.h"
#include "dect2/fsq/kbd33_station.h"

namespace dect2 {

struct IfkpReceiver::Impl {
    Kbd33Station st{true, ifkpTuning().minSampleRate};
};

IfkpReceiver::IfkpReceiver() : p_(std::make_unique<Impl>()) {}
IfkpReceiver::~IfkpReceiver() = default;
void IfkpReceiver::configure(double r) { p_->st.configure(r); }
void IfkpReceiver::setSignalOffset(double hz) { p_->st.setSignalOffset(hz); }
bool IfkpReceiver::ready() const { return p_->st.ready(); }
void IfkpReceiver::reset() { p_->st.reset(); }
void IfkpReceiver::feed(const cf32* x, size_t n) { p_->st.feed(x, n); }
bool IfkpReceiver::telemetry(IfkpTelemetry& out, uint64_t lastSeq) { return p_->st.telemetry(out, lastSeq); }
void IfkpReceiver::clearResults() { p_->st.clearResults(); }
void IfkpReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->st.setLogCallback(std::move(cb)); }
void IfkpReceiver::setSpeed(int speed) { p_->st.setSpeed(speed); }
void IfkpReceiver::setMyCall(const std::string& call) { p_->st.setMyCall(call); }
void IfkpReceiver::setListen(bool on) { p_->st.setListen(on); }
bool IfkpReceiver::listening() const { return p_->st.listening(); }
void IfkpReceiver::setVolume(float v) { p_->st.setVolume(v); }
void IfkpReceiver::setMuted(bool m) { p_->st.setMuted(m); }
void IfkpReceiver::setSilent(bool s) { p_->st.setSilent(s); }

ModeTuning ifkpTuning() {
    ModeTuning t;
    t.stdMode = 118; t.id = "ifkp"; t.name = "IFKP";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.0830;   // a usual 20 m dial frequency (upper sideband)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;                              // as PSK and HF digital: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

const IfkpPreset* ifkpPresets(int& count) {
    static const IfkpPreset kP[] = {{"80 m", 3.5835}, {"40 m", 7.0385}, {"20 m", 14.0830}, {"15 m", 21.0830}, {"10 m", 28.0830}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

} // namespace dect2
