// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Elektro-L N3 / N4 / N5 L-band receiver. Receive only.
// HRIT / LRIT: the same BPSK + Viterbi + Reed-Solomon + CCSDS + xRIT path as GOES (1691 MHz).
// GGAK: 5 ksym/s PCM/PM space-weather telemetry on 1693 MHz (224-byte CADU, no convolutional code).
#pragma once
#include "elektro_ggak.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {

struct ElektroTelemetry;

class ElektroReceiver {
public:
    ElektroReceiver();
    ~ElektroReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCraft(int craft);               // 0 N3, 1 N4, 2 N5
    void setLink(int link);                 // 0 HRIT 665.4k, 1 HRIT 1M, 2 LRIT 128k, 3 LRIT 64k, 4 GGAK
    int craft() const { return craft_; }
    int link() const { return link_; }
    bool ready() const;
    void reset();
    void clearProducts();
    void feed(const cf32* x, size_t n);
    bool telemetry(ElektroTelemetry& out, uint64_t lastSeq);
    void clearResults();
    std::string saveResults() const;
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setFileCallback(std::function<void(const std::vector<uint8_t>&, int vcid, int apid)> cb);
    GoesReceiver& inner() { return inner_; }
    GgakReceiver& ggak() { return ggak_; }

private:
    GoesReceiver inner_;
    GgakReceiver ggak_;
    int craft_ = 0;
    int link_ = 0;
    double rate_ = 0;
};

ModeTuning elektroTuning();

} // namespace dect2
