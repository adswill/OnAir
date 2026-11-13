// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi network monitor: the access points around, from the beacons they broadcast openly on 2.4, 5 and 6 GHz, as a phone's Wi-Fi list shows
// them (SSID, BSSID and vendor, band, channel, width, standard, security, capabilities, beacon interval, level). Passive: it never transmits,
// probes, associates or joins, and parses only beacons, probe responses and FILS discovery frames; every other frame (data included) is only
// counted, never decoded.
//
// Two PHYs run side by side on the same 20 Msps stream: the 802.11b DSSS chain below (2.4 GHz, and only there or when the band is unknown) and
// the OFDM receiver of wifi_ofdm.h (every band: 5 and 6 GHz beacons are OFDM at 6 Mbit/s). The band comes from the radio's centre frequency
// (setCenterHz). A capture of 43.5 Msps or more on 5 / 6 GHz is split into the 20 MHz channels it holds, each with its own OFDM receiver (the
// channel filter of channelizer.h); a frame heard on the secondary 20 MHz of a wider network (the non-HT duplicate) is counted, not listed twice.
//
// 802.11b DSSS at 1 Mbit/s (IEEE 802.11 clause 15), how APs send their beacons on 2.4 GHz: DBPSK, each symbol spread by Barker-11 at
// 11 Mchip/s. The input is mixed to 0 Hz and brought to 20 Msps (exactly 20 samples a symbol), correlated with the Barker sequence, and
// each of the 20 sample phases runs its own differential detector and descrambler (z^-7 + z^-4 + 1, self-synchronising) looking for the
// long preamble: 32 descrambled ones then the SFD 0xF3A0. The strongest phase decodes the PLCP header (CRC-16) and the MPDU with an
// early / late timing tracker; the FCS (CRC-32) must pass. 2 Mbit/s (DQPSK, clause 16) is decoded after the long preamble and after the short
// one (56 scrambled zeros, SFD 0x05CF, header at 2 Mbit/s); 5.5 / 11 Mbit/s (CCK) headers are counted, not decoded.
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

struct WifiTelemetry;   // wifi_tel.h

// How a frame was received (setFrameMetaCallback)
struct WifiFrameMeta {
    float rssiDbfs = -200;
    int rateKbps = 1000;                 // DSSS 1000 / 2000, OFDM 6000 / 12000
    bool ofdm = false, shortPreamble = false;
    bool fcsOk = true;
    double cfoHz = 0;                    // carrier offset measured on this frame
    float snrDb = 0;
};

class WifiReceiver {
public:
    WifiReceiver();
    ~WifiReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);          // the radio's centre: the channel shown and noted with each network (0: unknown, a file)
    bool ready() const;
    void reset();                         // the demodulator only: the network table stays
    void setReportInterval(double sec);   // seconds of signal between telemetry reports (default 0.25; for tests)
    void clearNetworks();
    void feed(const cf32* x, size_t n);
    bool telemetry(WifiTelemetry& out, uint64_t lastSeq);
    // The network table and the frame log stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    // Every good frame (FCS passed; n without the FCS), on the feeding thread: for a mode built on this demodulator (Drone Remote ID). Set before feeding.
    void setFrameCallback(std::function<void(const uint8_t* mpdu, size_t n, float rssiDbfs)> cb);
    // Every frame whose PLCP header (DSSS) or SIGNAL and FCS (OFDM) decoded, with its rate and level; a DSSS frame whose FCS failed comes with
    // fcsOk false (n without the four FCS bytes as received). For a mode built on this demodulator (ESP-NOW). Set before feeding.
    void setFrameMetaCallback(std::function<void(const uint8_t* mpdu, size_t n, const WifiFrameMeta& meta)> cb);
    void setOfdmQpsk(bool on);            // also read OFDM frames at 12 Mbit/s (QPSK 1/2); off by default (beacons are 6 Mbit/s)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning wifiTuning();

} // namespace dect2
