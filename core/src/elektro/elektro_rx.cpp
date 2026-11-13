// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Elektro-L HRIT / LRIT / GGAK receiver (elektro_rx.h).
#include "dect2/elektro/elektro_rx.h"
#include "dect2/elektro/elektro_tel.h"
#include <algorithm>

namespace dect2 {

ElektroReceiver::ElektroReceiver() { inner_.setSymbolRate(elektroSymbolRate(0)); }
ElektroReceiver::~ElektroReceiver() = default;

void ElektroReceiver::configure(double r) {
    rate_ = r;
    inner_.configure(r);
    ggak_.configure(r);
}
void ElektroReceiver::setSignalOffset(double hz) {
    inner_.setSignalOffset(hz);
    ggak_.setSignalOffset(hz);
}
void ElektroReceiver::setCraft(int craft) { craft_ = std::clamp(craft, 0, 2); }
void ElektroReceiver::setLink(int link) {
    link_ = std::clamp(link, 0, 6);
    if (!elektroIsGgak(link_)) { inner_.setSymbolRate(elektroSymbolRate(link_)); inner_.setOqpsk(link_ == 6); }
}
bool ElektroReceiver::ready() const { return elektroIsGgak(link_) ? ggak_.ready() : inner_.ready(); }
void ElektroReceiver::reset() { inner_.reset(); ggak_.reset(); }
void ElektroReceiver::clearProducts() { inner_.clearProducts(); ggak_.clearResults(); }
void ElektroReceiver::feed(const cf32* x, size_t n) {
    if (elektroIsGgak(link_)) ggak_.feed(x, n);
    else inner_.feed(x, n);
}
bool ElektroReceiver::telemetry(ElektroTelemetry& out, uint64_t lastSeq) {
    out.craft = craft_;
    out.link = link_;
    if (elektroIsGgak(link_)) {
        if (!ggak_.telemetry(out.ggak, lastSeq)) return false;
        return true;
    }
    if (!inner_.telemetry(out.hrit, lastSeq)) return false;
    return true;
}
void ElektroReceiver::clearResults() { inner_.clearResults(); ggak_.clearResults(); }
std::string ElektroReceiver::saveResults() const { return elektroIsGgak(link_) ? ggak_.saveResults() : inner_.saveResults(); }
bool ElektroReceiver::loadResults(const std::string& blob) { return inner_.loadResults(blob) || ggak_.loadResults(blob); }
void ElektroReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    inner_.setLogCallback(cb);
    ggak_.setLogCallback(std::move(cb));
}
void ElektroReceiver::setFileCallback(std::function<void(const std::vector<uint8_t>&, int, int)> cb) { inner_.setFileCallback(std::move(cb)); }

ModeTuning elektroTuning() {
    ModeTuning t;
    t.stdMode = 82; t.id = "elektro"; t.name = "Elektro-L";   // 81 is TETRAPOL
    t.minMhz = 1670; t.maxMhz = 1710; t.defMhz = 1691.0;
    t.sampleRate = 2400000;
    t.basebandHz = 0;
    t.bandwidthMhz = 1.0;
    t.minSampleRate = 200000;               // GGAK works from ~20 ksps; HRIT needs ~1.4 Msps and says so when the rate is short
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
