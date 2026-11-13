// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// COSPAS-SARSAT 406 MHz distress beacon receiver: burst finder (a narrow carrier that appears for half a second), carrier frequency estimate from the
// 160 ms unmodulated preamble, then 400 bit/s Manchester phase demodulation (+-1.1 rad) with sync search over timing, bit rate and polarity, and the
// message layer in sarsat_proto.h. Receive only.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread,
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/sarsat/sarsat_proto.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SarsatTelemetry;

namespace sarsat {
constexpr double kFs = 32000.0;   // the rate the burst processing works at
// The demodulator on its own: x holds a burst at kFs with its carrier at carrierHz; fills the message when a sync pattern and the first BCH check pass.
bool demodBurst(const std::vector<cf32>& x, double carrierHz, Beacon& out);
// Repeat combining (4.0, docs/SENSITIVITY-4.0-X1.md): a beacon sends the same identification every 50 s (+-2.5 s). A burst whose checks failed keeps its
// soft bits (kLongBits values, > 0 is a 1, sync polarity applied); the soft bits of the same beacon's next bursts are added to them and the sum is decoded.
struct SoftBurst {
    std::vector<float> soft;
    bool test = false;          // the self-test sync word matched better
    float conf = 0;             // the sync word's correlation
    // further timings of the same burst (best first after `soft`): a weak burst's best-looking timing is often half a bit or one rate step off, so
    // the combiner aligns bursts by the correlation of their soft bits (the same message correlates, a wrong timing does not)
    std::vector<std::vector<float>> alt;
};
// As demodBurst; when nothing decodes and `failed` is set, it gets the soft bits of the best sync candidate (returns false either way).
bool demodBurst(const std::vector<cf32>& x, double carrierHz, Beacon& out, SoftBurst* failed, bool coherent = true);
// 4.0: a repair counts only when the soft confidence of the repaired bits stays under costPerBit x the mean confidence per repaired bit (a bit the
// noise flipped had a small soft value; a decoder that landed on a wrong codeword flips ordinary ones). Applied with the coherent bits.
constexpr double kRepairCostPerBit = 1.0;
// 4.0: the coherent path decodes by soft decision (Chase: 128 test patterns on the 7 least reliable bits) and keeps a repaired message only when the
// winning codeword is at least this much closer to the soft bits than the next codeword (in mean |soft| units). A cost limit of 1e8 or more turns both off.
constexpr double kRepairMargin = 1.5;
// Decodes summed soft bits by soft decision with the margin check, and a perfect or 23 of 24 sync word, the second BCH intact when more than one bit
// was repaired in a long message, a country code that exists.
bool decodeCombined(const std::vector<float>& soft, Beacon& out, double costPerBit = kRepairCostPerBit);
}

class SarsatReceiver {
public:
    SarsatReceiver();
    ~SarsatReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(SarsatTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // Search the whole capture (issue #214: the satellites' L-band downlink, each satellite somewhere in 1544 - 1545 MHz): a bank of
    // burst finders 90 kHz apart across up to 1.1 MHz of the input (as much as the sample rate holds), merged into one beacon list.
    // CPU guard: seconds of burst search that bursts which do not look like a beacon may use before they are skipped (paid back 0.15 s per s of signal)
    void setFalseBurstBudget(double seconds);
    uint64_t falseBurstsSkipped() const;
    void setWideSearch(bool on);
    // Repeat combining (on by default): soft bits of bursts that did not decode are kept up to 4 minutes and summed with the next bursts on the same
    // carrier (+-150 Hz) at the beacon's repeat interval (47.5 to 52.5 s, or a multiple of it when bursts were missed). period* = the interval accepted.
    void setRepeatCombining(bool on, double periodMinSec = 47.5, double periodMaxSec = 52.5);
    // 4.0 single-burst path (both on by default). fullWindow: a burst is processed over at least 0.62 s from its first carrier line, so that a weak
    // burst (line visible only in the preamble) keeps its whole message; false is the 3.9 end of burst (6 blocks of 32 ms without a line).
    // coherentBits: the message bits are detected against the burst's own residual carrier (see sarsat_rx.cpp); false is the 3.9 differential slicer.
    void setWeakBurstPath(bool fullWindow, bool coherentBits);
    // the repair-cost limit of the coherent path (kRepairCostPerBit; a large value turns the check off: measurement only)
    void setRepairCostLimit(double perBit);
    bool wideSearch() const;
    double wideSpanHz() const;          // what the bank covers now (0: off)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning sarsatTuning();

} // namespace dect2
