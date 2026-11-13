// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL beacon (port of kaal_beacon_v2.py and kaal_beacon_slice.py): the station identity frame (no UTC time, no icon, text up to 100
// characters) and its narrow side channel: BPSK 1000 sym/s, RRC 0.35, centred +4500 Hz from the voice channel, rate 1/4 K=7 convolutional
// code (generators 171 133 165 117 octal), a 64-symbol sync word in front. Header-only frames carry 170 bits, full frames add the text.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace kaal {

struct BeaconFields {
    std::string callsign;   // up to 9 characters of " 0-9A-Z/-"
    std::string grid;       // 6 characters, e.g. KO26ab
    double lat = 0, lon = 0;
    double powerDbm = 0;    // -40 .. 87.5 dBm in 0.5 dB steps
    int linkQuality = 0;
    int version = 1;
    int frameType = 1;      // 0 = header only (no text), 1 = with text
};

constexpr int kBeaconHeaderBits = 170;        // 154 field bits + CRC-16
constexpr size_t kBeaconMaxText = 100;
constexpr double kBeaconOffsetHz = 4500.0;
constexpr int kBeaconSymRate = 1000;
constexpr double kBeaconHeaderPeriodS = 2.0;  // a header-only frame is started every 2 s ...
constexpr double kBeaconFullPeriodS = 10.0;   // ... and a full one with text every 10 s

// Header 170 bits + (when text is not empty) 8 bits per character + CRC-32, one bit (0 or 1) per byte. The text is cut at 100 bytes.
// An unusable field (callsign longer than 9 or with other characters, a grid that is not 6 characters) gives an empty vector.
std::vector<uint8_t> encodeBeaconBits(const BeaconFields&, const std::string& text);
// false on a CRC failure, a short input or a bad text length.
bool decodeBeaconBits(const std::vector<uint8_t>& bits, BeaconFields& f, std::string& text);

// One frame as complex baseband at sampleRate (a multiple of 1000 Hz, 48000 normally), unit RMS, with the slice already at +4500 Hz.
// An empty text gives a header-only frame (frame type 0).
std::vector<std::complex<float>> beaconSliceModulate(const BeaconFields&, const std::string& text, double sampleRate);

// Streaming receiver of the slice. feed() any chunk size; poll() returns true once for every valid frame found.
class BeaconSliceRx {
public:
    BeaconSliceRx();
    ~BeaconSliceRx();
    BeaconSliceRx(const BeaconSliceRx&) = delete;
    BeaconSliceRx& operator=(const BeaconSliceRx&) = delete;
    void configure(double sampleRate);
    void feed(const std::complex<float>* x, size_t n);
    bool poll(BeaconFields& f, std::string& text);
private:
    struct Impl;
    Impl* p_;
};

// Soft-decision Viterbi for the rate 1/4 K=7 code. soft: 4 values per trellis step, positive = bit 0 more likely. Decodes nInfo bits;
// with nInfo + 6 steps the tail is used to end in state 0, with fewer the best end state.
std::vector<uint8_t> beaconViterbi(const float* soft, size_t nSteps, size_t nInfo);
std::vector<uint8_t> beaconConvEncode(const std::vector<uint8_t>& bits);   // appends the 6 tail zeros

}  // namespace kaal
}  // namespace dect2
