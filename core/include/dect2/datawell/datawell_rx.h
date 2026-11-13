// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell Waverider buoy HF link monitor: receive only. Datawell wave buoys send their data to a shore receiver on HF (25.5 - 35.5 MHz, up
// to 45 MHz on request) as FSK heard in USB: 2FSK ("HXV" format: DWR-MkIII, DWR-G, WR-SG; 81.92 bit/s) or 4FSK ("HVA" format: DWR4 with the
// acoustic current meter option; 163.84 bit/s), both at 81.92 symbols per second (Datawell RX-C4 manual; tones measured on the sigidwiki recordings).
//  - HXV is decoded for real, from the Datawell Waverider Reference Manual (DWR-MkIII, section 5.7): 64-bit vectors with heave / north / west
//    displacements, a block structure with system words (wave height, period, temperatures, position, battery) and a 64 band directional spectrum
//    (datawell_hxv.h). The vector parity code is not in the manual: it was found from a real recording and the RX-C4 manual's examples.
//  - HVA over the air is NOT public (the DWTP specification starts after the receiver: "BCH / Reed-Solomon concatenated"): for 4FSK the mode demodulates
//    the symbols and shows measurements (bits, repetition period, constant pattern, CRC attempts), nothing more. Nothing is decrypted or transmitted.
//
// Chain: mix by the radio offset, two integrate-and-dump stages to ~16 kHz, a Hann FFT averaged over a few seconds finds the strongest cluster
// of energy (centre, width of 90% of its power); its width tells the format. A narrow chain (mix to the centre, FIR to ~4 kHz, low pass of
// 400 Hz) feeds a frequency discriminator: the mean frequency over a symbol is the tone (continuous phase FSK), a PI loop over 17 sampling
// phases tracks the symbol clock, the tones adapt to the signal.
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
#include <vector>

namespace dect2 {

struct DatawellTelemetry;   // datawell_tel.h

class DatawellReceiver {
public:
    DatawellReceiver();
    ~DatawellReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);          // the radio's centre, for the absolute frequency and the channel name (0: unknown)
    void setFormat(int fmt);              // 0 automatic from the bandwidth, 1 2FSK, 2 4MFSK
    bool ready() const;
    void reset();
    void clearResults();                  // the frames and the bit stream
    void feed(const cf32* x, size_t n);
    bool telemetry(DatawellTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning datawellTuning();

namespace datawell {
struct Channel { const char* name; double mhz; };
const Channel* channels(int& count);                    // the buoy channels sigidwiki lists
std::string channelNear(double mhz, double tolKhz = 6); // "" none
// Both formats send 81.92 symbols per second: 2FSK is 81.92 bit/s, 4FSK 163.84 bit/s (RX-C4 manual, specifications). The tone distances are from the
// sigidwiki sample recordings, measured here: 2FSK tones about 129 Hz apart (sigidwiki's "65 Hz" is the deviation either side of the centre),
// 4FSK tones about 78 Hz apart (sigidwiki: "about 85 Hz"; the measured mean frequency of a symbol sits a little inside its tone).
constexpr double kBaud = 81.92;
constexpr double kBaud2Fsk = kBaud, kShift2Fsk = 129.0;
constexpr double kBaud4Mfsk = kBaud, kSpacing4Mfsk = 78.0;
} // namespace datawell

} // namespace dect2
