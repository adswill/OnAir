// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SSDV receiver (amateur satellites and high-altitude balloons): one FM / FSK channel (the packet front end, packet_demod.h: offset, decimation to
// 96 kHz, AFC, channel filter, discriminator) and a bank of bit slicers at the same time, each with its own clock loop:
//   - plain bit streams at 1200, 2400, 4800, 9600 and 19200 baud;
//   - bytes framed RTTY style (start bit, 8 data bits LSB first, two stop bits) at 300, 600 and 1200 baud.
// Both frequency polarities are tried. A 0x55 0x66 / 0x55 0x67 sync word (one bit error allowed) starts a packet of 256 bytes; the CRC-32 and the
// Reed-Solomon code of SSDV (third_party/ssdv) decide. Good packets are collected per picture (callsign and image id); the JPEG is rebuilt as
// packets arrive and written to the save folder when it is complete (or after 20 s of silence).
// Wenet (115.2 kbaud with LDPC, or 96 kbaud raw scrambled; wenet.h) has its own wide front end and is tried at the same time at 700 kHz and above.
// Not handled: scrambled / AX.25 / IL2P framing and the 4FSK Horus layers, LoRa. See docs/ssdv.md.
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

struct SsdvTelemetry;   // ssdv_tel.h

class SsdvReceiver {
public:
    SsdvReceiver();
    ~SsdvReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                        // after a retune: the front end only, the pictures stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(SsdvTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setBaud(int baud);              // 0: all speeds at once (default), or one of 300 .. 19200, 115200 (Wenet) or 96000 (Wenet, raw scrambled)
    void setSaveDir(const std::string& dir);   // "" (default): nothing is written
    void flush();                        // tests: write what is pending (the end of a recording)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning ssdvTuning();

struct SsdvPreset { const char* name; double mhz; };
const SsdvPreset* ssdvPresets(int& count);   // starting points: 70 cm balloon / satellite telemetry segments

} // namespace dect2
