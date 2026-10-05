// ISDB-T demodulator core: from the FFT output of consecutive OFDM symbols (carriers in band order, already corrected for common phase
// and timing slope) to transport stream packets, layer by layer: channel estimate from the scattered pilots, differential detection
// of the DQPSK segments, de-interleaving, bit de-interleaving, Viterbi, byte de-interleaving, energy dispersal removal, Reed-Solomon.
#pragma once
#include "isdbt.h"
#include <memory>
#include <vector>

namespace dect2 {
namespace isdbt {

struct LayerStats {
    uint64_t packets = 0, rsClean = 0, rsCorrected = 0, rsFailed = 0;
    double viterbiMargin = 0;      // normalised path metric of the last block (1 = noiseless)
    bool synced = false;           // Reed-Solomon works: the byte alignment is right
};

class Demod {
public:
    Demod();
    ~Demod();
    // Parameters of the signal (from the TMCC). Starts over.
    void configure(const Params& p);
    // The TMCC information bits (102) once they are known: the TMCC carriers then serve as pilots for the common phase and timing slope too
    void setTmccInfo(const uint8_t info[kTmccInfoBits]);
    const Params& params() const { return p_; }
    // One symbol: Y has totalCarriers(mode) entries ordered by carrier number; symIdx is the symbol's place in the frame (0..203).
    // Decoding starts at the first symbol with symIdx == 0.
    void pushSymbol(const cf32* Y, int symIdx);
    // Packets decoded so far for a layer (188 bytes each; transport_error_indicator set on those Reed-Solomon could not repair)
    void takePackets(int layer, std::vector<uint8_t>& out);
    // Packets of all layers of the frames decoded since the last call, merged in the order of the model receiver (a packet is ready when its
    // layer has received the corresponding share of the frame). Returns the number of packets.
    size_t takeMerged(std::vector<uint8_t>& out);
    const LayerStats& layerStats(int layer) const;
    double noiseVariance() const;                 // of the carriers, in the units of the received values
    double snrDb() const;                         // of the data carriers after equalisation (from the pilots)
    // for the display: equalised data cells of the last symbol (subsampled), and channel magnitude per carrier
    const std::vector<cf32>& eqCells() const;
    // channel estimate over the whole band (K entries; stretches without scattered pilots hold the nearest value), and the equalised
    // scattered pilots of the last symbol (about +-1), for the display
    void channel(std::vector<cf32>& H) const;
    const std::vector<cf32>& pilotCells() const;
    // the last symbol after the common phase and timing correction (K carriers)
    const std::vector<cf32>& corrected() const;
    const std::vector<float>& channelDb() const;
    uint64_t symbolsDone() const;
    // centre of the delay window of the channel estimate, in samples relative to the start of the FFT window (see GridInterpolator)
    void setDelayCentre(double tau0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Params p_;
};

} // namespace isdbt
} // namespace dect2
