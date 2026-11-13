// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GK-2A LRIT / HRIT receiver (gk2a_rx.h).
#include "dect2/gk2a/gk2a_rx.h"

namespace dect2 {

Gk2aReceiver::Gk2aReceiver(int link) : link_(link ? 1 : 0) {
    inner_.setSymbolRate(gk2aSymbolRate(link_));
    inner_.setQpsk(link_ == 1);
    inner_.setDialect(1, gk2aLinkName(link_));
}

void Gk2aReceiver::configure(double r) { rate_ = r; inner_.configure(r); }

bool Gk2aReceiver::telemetry(Gk2aTelemetry& out, uint64_t lastSeq) {
    GoesTelemetry g;
    if (!inner_.telemetry(g, lastSeq)) return false;
    static_cast<GoesTelemetry&>(out) = std::move(g);
    out.link = link_;
    out.keysLoaded = keys_;
    int locked = 0;
    for (const auto& p : out.products) { std::lock_guard<std::mutex> lk(p->mu); locked += p->segsLocked; }
    out.segmentsLocked = locked;
    out.hint = gk2aHint(out, true, rate_);
    return true;
}

void Gk2aReceiver::setKeys(const std::vector<std::pair<int, std::array<uint8_t, 8>>>& keys) {
    keys_ = (int)keys.size();
    inner_.setKeys(keys);
}

int Gk2aReceiver::importKeys(const std::vector<uint8_t>& file) {
    const auto k = goes::parseKeyFile(file);
    if (!k.empty()) setKeys(k);
    return (int)k.size();
}

ModeTuning gk2aLritTuning() {
    ModeTuning t;
    t.stdMode = 232; t.id = "gk2alrit"; t.name = "GK-2A LRIT";
    t.minMhz = 1670; t.maxMhz = 1710; t.defMhz = kGk2aLritMhz;
    t.sampleRate = 2400000;                // RTL-SDR friendly: 18 samples a symbol
    t.basebandHz = 0;
    t.bandwidthMhz = 0.3;                  // 128 ksym/s x (1 + 0.5) roll-off = 192 kHz
    t.minSampleRate = 300000;
    t.tuneOffsetHz = 0;
    return t;
}

ModeTuning gk2aHritTuning() {
    ModeTuning t;
    t.stdMode = 233; t.id = "gk2ahrit"; t.name = "GK-2A HRIT";
    t.minMhz = 1670; t.maxMhz = 1710; t.defMhz = kGk2aHritMhz;
    t.sampleRate = 10000000;               // HackRF / Airspy R2 / SDRplay class: 3.3 samples a symbol
    t.basebandHz = 0;
    t.bandwidthMhz = 5.2;                  // KMA lists a 5.2 MHz channel (3 Msym/s x 1.5 = 4.5 MHz occupied)
    t.minSampleRate = 6400000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
