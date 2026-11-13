// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Argos receiver (see argos_proto.h for what is public and what is not). Two links in one mode, picked from the dial or by hand:
//   uplink, 401.650 MHz: burst finder (the 160 ms unmodulated carrier), PTT-A2 Manchester phase demodulator (+-1.1 rad, 400 bit/s) with a
//     search over bit timing and rate, message fields (28 bit ID, user data), and a per-platform list of the burst frequencies (the Doppler
//     and oscillator drift curve). Bursts with another modulation are counted and listed, not decoded.
//   downlink, 465.9875 MHz: carrier acquisition on the squared signal over +-14 kHz, a squaring phase-locked loop that follows the
//     satellite's Doppler, 400 bit/s Manchester bit decisions, HDLC deframing (NRZ, inverted, NRZI) with the FCS, and the Doppler curve for
//     the pass tools to compare with the predicted one.
// Receive only.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters
// from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct ArgosTelemetry;

namespace argos {
constexpr double kFs = 32000.0;   // the uplink burst demodulator's rate
struct DemodResult { int kind = 0; float carrierMs = 0, quality = 0; std::vector<uint8_t> bits; };
// The A2 demodulator on its own: x holds a burst at kFs with its carrier near 0 Hz, starting a little before the carrier
// hintStart: where the burst starts in x when the line detector knows (used only where the power gate cannot see the burst), -1 none
bool demodUplinkBurst(const std::vector<cf32>& x, DemodResult& out, long hintStart = -1, bool legacy = false);
}

class ArgosReceiver {
public:
    ArgosReceiver();
    ~ArgosReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setDialHz(double hz);       // the link follows the dial when the setting is automatic (0: unknown, a file: uplink)
    void setLink(int link);          // 0 automatic, 1 uplink, 2 downlink
    void setWeakSignalPath(bool on); // 4.0 weak-signal uplink path (default on); off = the 3.9 receiver, for before / after measurements
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(ArgosTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning argosTuning();

}  // namespace dect2
