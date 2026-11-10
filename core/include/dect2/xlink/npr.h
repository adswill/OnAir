// New Packet Radio (NPR-70, F4HDK) sync detection on 70 cm. Si4463 4(G)FSK at 50, 100, 180, 300 or 500 ksym/s (modem configurations 20 .. 24; the 2(G)FSK configurations 11 .. 14 use the same layout)
// with a standard preamble and a 3-byte sync word in 4FSK. The sync word carries the radio network ID. Structure only: the length byte and the payload are whitened by the chip's PN generator, whose
// exact bit arrangement is not in the published documentation, so frames are detected and counted but not read. See docs/LEFTOVERS-3.8.md.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2::xlink::npr {

// The 3-byte sync word of a radio network ID 0 .. 15 (SI4463.cpp hash_netID): 0x45 0xD2 then the hash
uint32_t syncWord(int networkId);
// netID hash table
int networkOfHash(uint8_t hash);            // -1: not a known network
extern const double kSymbolRates[5];        // symbols per second of configurations 20 .. 24

struct Hit { int rateIndex = 0; int network = -1; double offsetHz = 0; float metric = 0; int64_t atSample = 0; };

// Slides over one symbol-rate chain (input at 4 samples per symbol, the discriminator phase differences) and reports sync hits.
class SyncFinder {
public:
    using Cb = std::function<void(const Hit&)>;
    explicit SyncFinder(int rateIndex) : rate_(rateIndex) {}
    void setCallback(Cb cb) { cb_ = std::move(cb); }
    void feed(const float* phaseDiff, size_t n);   // radians per sample, at 4 samples per symbol
private:
    int rate_;
    Cb cb_;
    std::vector<float> h_;                         // history
    int64_t base_ = 0, n_ = 0, last_ = -100000;
    Hit pend_;                                     // best candidate of the current peak
};

// Test helper: one frame as complex baseband at sps samples per symbol: preamble, sync, then 'payload' whitened bytes.
std::vector<cf32> txFrame(int rateIndex, int network, const std::vector<uint8_t>& payload, double sps = 8.0);

} // namespace dect2::xlink::npr
