// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) network monitor: the networks around, from the S1G beacons they broadcast openly in the sub-GHz bands (863 - 868 MHz EU,
// 902 - 928 MHz US and the other regions of halow_chan.h), as a Wi-Fi list shows them (SSID, BSSID and vendor, channel, bandwidth, MCS, streams,
// security, level). Passive: it never transmits, probes, associates or joins; the security type is shown as information only and nothing is decrypted.
//
// PHY (IEEE 802.11ah, S1G OFDM; 1, 2, 4, 8 and 16 MHz channels, 31.25 kHz tones, 40 us symbols): the input is mixed to 0 Hz and handed to up to
// three receive chains (halow_chain.h): 1 / 2 / 4 MHz on a 4 Msps grid, 8 MHz on 8 Msps (radio rate from 8 Msps, 10 recommended), 16 MHz on 16 Msps
// (from 16 Msps, 20 recommended). Each finds preambles with a delay-and-correlate detector, tries every width, reads the SIG field of the S1G_1M,
// S1G_SHORT and S1G_LONG preambles, and decodes the data field of one-stream BCC frames at MCS 0 - 10 (BPSK to 256-QAM). The FCS (CRC-32) must pass.
// PPDUs with several spatial streams, STBC, LDPC, A-MPDU or no data (NDP) are counted from their SIG field only.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct HalowTelemetry;   // halow_tel.h

class HalowReceiver {
public:
    HalowReceiver();
    ~HalowReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);          // the radio's centre: the channel shown with each network (0: unknown)
    void setWidthMask(int mask);          // bit w (0 = 1 MHz ... 4 = 16 MHz): the widths tried; 31 = all (auto-detect), one bit = a manual width
    int widthMask() const;
    bool ready() const;
    void reset();                         // the demodulator only: the tables stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    void flush();                         // from the feed thread: work on what was fed and publish the telemetry now (tests; feed() does it every 0.25 s of signal)
    bool telemetry(HalowTelemetry& out, uint64_t lastSeq);
    // The tables and the frame log stay through reset(), configure() and retunes: only clearResults() empties them
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning halowTuning();

} // namespace dect2
