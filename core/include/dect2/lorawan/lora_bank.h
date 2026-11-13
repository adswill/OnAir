// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A bank of LoRa channels for a wide capture: for every wanted channel (an offset in the input and a bandwidth) one channel filter, and for
// every spreading factor / sync word one demodulator of the mesh receiver's LoRa physical layer (mesh_lora.h). Optionally a second set per
// channel for inverted IQ (LoRaWAN gateway downlinks send their chirps the other way round: the channel samples are conjugated for it).
// Shared by the LoRaWAN monitor and the LoRa satellite receiver. Not thread safe: one thread feeds, configure() is called by it too.
#pragma once
#include "dect2/mesh/mesh_lora.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {
namespace lorabank {

struct Channel {
    double offsetHz = 0;             // where the channel centre sits in the input samples
    double bwHz = 125e3;
    double filterBwHz = 0;           // > bwHz: the channel filter is wider than the LoRa band (a satellite's Doppler moves the signal across it); 0 = bwHz
    std::vector<int> sfs;            // spreading factors searched (5..12)
    std::vector<int> syncs;          // sync words searched (0x34 LoRaWAN public, 0x12 private)
    bool normalIq = true;            // decode the normal chirps
    bool invertedIq = false;         // and the inverted ones (LoRaWAN downlinks)
    double carrierHz = 0;            // satellites: the nominal carrier frequency (the symbol clock stretches with the Doppler shift), 0 unknown
    double cfoTrack = 0;             // > 0: follow a drifting carrier through the frame (satellites), see lora::Params::cfoTrack
    int preamble = 8;                // symbols (only places the frame start)
    double marginHz = 25e3;          // room for the carrier offset the channel must still pass inside the captured band
    int implicitLen = 0;             // > 0: implicit header frames of this length, CR 4/5, no payload CRC (Class B beacons)
};

struct Decoded {
    size_t chan = 0;                 // index in the configured list
    int sf = 0;
    int sync = 0;
    double bwHz = 0;
    double offsetHz = 0;             // channel centre in the input
    bool inverted = false;
    double sec = 0;                  // signal time of the frame's start
    lora::RxFrame f;
};

struct DecoderStat {
    size_t chan = 0;
    int sf = 0, sync = 0;
    bool inverted = false;
    bool inBand = false;
    uint64_t preambles = 0, frames = 0, crcBad = 0, headerBad = 0, syncBad = 0;
};

class Bank {
public:
    Bank();
    ~Bank();
    // Starts afresh (drops what was buffered). Channels outside the captured band are listed in stats() with inBand = false and not decoded.
    void configure(double inputRate, const std::vector<Channel>& chans);
    // The samples (any amount); the finished frames are appended to out. Equal results whatever the chunking.
    void feed(const cf32* x, size_t n, std::vector<Decoded>& out);
    // At end of input, process the Bank's last partial block and append any finished frames to out.
    // Uses only samples already fed. Does not add silence or flush the channelizer's own filter/FFT lookahead.
    // No effect if the Bank has no pending samples.
    void flushPending(std::vector<Decoded>& out);
    double timeSec() const;          // samples fed / rate
    int64_t samples() const;
    const std::vector<DecoderStat>& stats() const;
    size_t decoders() const;         // running (in band)
    size_t channelsInBand() const;
    // Whether a channel fits the captured band
    static bool fits(double inputRate, const Channel& c);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace lorabank
} // namespace dect2
