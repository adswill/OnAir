// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ receiver (see fsq_rx.h): the common station of kbd33_station.h with this modem.
#include "dect2/fsq/fsq_rx.h"
#include "dect2/fsq/kbd33_station.h"

namespace dect2 {

struct FsqReceiver::Impl {
    Kbd33Station st{false, fsqTuning().minSampleRate};
};

FsqReceiver::FsqReceiver() : p_(std::make_unique<Impl>()) {}
FsqReceiver::~FsqReceiver() = default;
void FsqReceiver::configure(double r) { p_->st.configure(r); }
void FsqReceiver::setSignalOffset(double hz) { p_->st.setSignalOffset(hz); }
bool FsqReceiver::ready() const { return p_->st.ready(); }
void FsqReceiver::reset() { p_->st.reset(); }
void FsqReceiver::feed(const cf32* x, size_t n) { p_->st.feed(x, n); }
bool FsqReceiver::telemetry(FsqTelemetry& out, uint64_t lastSeq) { return p_->st.telemetry(out, lastSeq); }
void FsqReceiver::clearResults() { p_->st.clearResults(); }
void FsqReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->st.setLogCallback(std::move(cb)); }
void FsqReceiver::setSpeed(int speed) { p_->st.setSpeed(speed); }
void FsqReceiver::setMyCall(const std::string& call) { p_->st.setMyCall(call); }
void FsqReceiver::setListen(bool on) { p_->st.setListen(on); }
bool FsqReceiver::listening() const { return p_->st.listening(); }
void FsqReceiver::setVolume(float v) { p_->st.setVolume(v); }
void FsqReceiver::setMuted(bool m) { p_->st.setMuted(m); }
void FsqReceiver::setSilent(bool s) { p_->st.setSilent(s); }

ModeTuning fsqTuning() {
    ModeTuning t;
    t.stdMode = 115; t.id = "fsq"; t.name = "FSQ";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.1050;   // a usual 20 m dial frequency (upper sideband)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;                              // as PSK and HF digital: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

const FsqPreset* fsqPresets(int& count) {
    static const FsqPreset kP[] = {{"80 m", 3.5895}, {"40 m", 7.0435}, {"20 m", 14.1050}, {"15 m", 21.1050}, {"10 m", 28.1050}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

} // namespace dect2
