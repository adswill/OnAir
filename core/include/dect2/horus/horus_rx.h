// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus balloon telemetry receiver: Horus Binary v1 / v2 (100 baud 4FSK, Golay) and Horus RTTY (50 / 100 / 300 baud,
// 7N2 / 8N2, UKHAS sentences with CRC16). A channel follows the dial; with a wide input the whole span is searched and further
// balloons get channels of their own. Works on radio IQ and on sound-card audio (the audio source gives analytic samples).
#pragma once
#include "horus_tel.h"
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {
class HorusReceiver {
public:
    HorusReceiver();
    ~HorusReceiver();
    void configure(double rate);
    void setSignalOffset(double hz);   // where the dial sits in the input
    void setCenterHz(double hz);       // the dial frequency itself (for the packets' frequency), 0 when unknown
    bool ready() const;
    void reset();                      // after a retune: the payloads heard stay
    void clearResults();               // forget the payloads and packets
    void feed(const cf32* iq, size_t n);
    bool telemetry(HorusTelemetry& out, uint64_t lastSeq);
    // the user's tables (horus_proto.h formats); empty text keeps the built-in default. Returns "" or what could not be read.
    std::string setTables(const std::string& idList, const std::string& customFieldsJson);
    void setMaxChannels(int n);        // 1 to 8 channels (1: the dial only)
    void setWideSearch(bool on);       // look for further balloons across the captured span
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    bool busy() const { return false; }
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
ModeTuning horusTuning();
} // namespace dect2
