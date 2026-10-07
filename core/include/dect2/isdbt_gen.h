// ISDB-T signal generator (transmitter): transport stream packets per layer in, baseband IQ at 512/63 Msamples/s out.
// It follows ARIB STD-B31 chapter 3: Reed-Solomon, energy dispersal per frame, byte interleaving, convolutional coding, bit interleaving
// and mapping, layer combining, time and frequency interleaving, pilots, TMCC and AC, IFFT with guard interval. The delay adjustments
// that line the layers up are left out (they change nothing for a receiver that decodes each layer on its own).
#pragma once
#include "isdbt.h"
#include "dvbt.h"
#include "isdbt_resample.h"
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {
class Fft;
namespace isdbt {

// Packet source: fills one 188-byte transport stream packet for a layer (0..2).
using PacketSource = std::function<void(int layer, uint8_t* pkt188)>;
// Packets with a counter and pseudo-random payload, a different PID per layer (0x100 + layer): the receiver can check them exactly.
PacketSource countingSource(unsigned seed = 1);
bool checkCountingPacket(const uint8_t* pkt188, int* layer = nullptr, unsigned* counter = nullptr);
// Wraps a single-stream source (the demo programme, for instance): all packets go to layer `layer`, the others carry null packets.
PacketSource singleLayerSource(int layer, std::function<void(uint8_t*)> src);

class Generator {
public:
    Generator(const Params& p, PacketSource src, unsigned seed = 1);
    ~Generator();
    const Params& params() const { return p_; }
    int symbolSamples() const { return N_ + G_; }
    int frameSamples() const { return (N_ + G_) * kSymbolsPerFrame; }
    // One OFDM frame (204 symbols with guard interval), unit mean power
    void nextFrame(std::vector<cf32>& out);
    // Frequency-domain carriers of every symbol of the last frame (204 x totalCarriers), for tests
    const std::vector<cf32>& lastCarriers() const { return carriers_; }
    long frameCount() const { return frames_; }

private:
    struct LayerTx;
    void produceFrame(std::vector<cf32>* out);
    Params p_;
    int N_, G_, K_;
    SegmentInfo seg_[kSegments];
    PacketSource src_;
    std::vector<std::unique_ptr<LayerTx>> tx_;
    std::vector<std::vector<cf32>> fifo_;       // time interleaver delay lines, per segment and data carrier
    std::vector<size_t> fifoPos_;
    std::vector<cf32> carriers_;
    std::vector<uint8_t> chain_;                // differential state of the TMCC and AC carriers
    ::dect2::Fft* fft_ = nullptr;
    long frames_ = 0;
};

} // namespace isdbt
} // namespace dect2
