// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/wb/wb_rx.h"
#include "dect2/wb/wb_tel.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace dect2 {

struct WbReceiver::Impl {
    std::mutex mu;
    WbTelemetry pub;
    double rate = 0, power = 0;
    int64_t n = 0, inBlock = 0, blockLen = 1;
};

WbReceiver::WbReceiver() : p_(new Impl) {}
WbReceiver::~WbReceiver() = default;
void WbReceiver::configure(double r) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = r; p_->pub.inputRate = r; p_->blockLen = std::max<int64_t>(1, (int64_t)(0.25 * r)); p_->pub.seq++;
}
void WbReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->n = 0; p_->inBlock = 0; p_->power = 0; }

void WbReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    for (size_t i = 0; i < n; i++) m.power += (double)std::norm(x[i]);
    m.n += (int64_t)n;
    m.inBlock += (int64_t)n;
    if (m.rate > 0 && m.inBlock >= m.blockLen) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.pub.state = 1;
        m.pub.levelDb = (float)(10 * std::log10(m.power / (double)m.inBlock + 1e-20));
        m.pub.dataValid = true;
        m.pub.timeSec = (double)m.n / m.rate;
        m.pub.seq++;
        m.power = 0; m.inBlock = 0;
    }
}

bool WbReceiver::telemetry(WbTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning wbTuning() {
    ModeTuning t;
    t.stdMode = 75; t.id = "wb"; t.name = "Signal workbench";
    t.minMhz = 0.1; t.maxMhz = 6000; t.defMhz = 100;
    t.sampleRate = 2000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 2;
    t.minSampleRate = 8000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
