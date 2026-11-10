// DVB-SH OFDM (SH-A) receiver: blind FFT-size and guard detection from the cyclic prefix, fractional and integer frequency offset,
// TPS frame synchronisation with the DVB-SH fields, channel estimation from the scattered and continual pilots with phase and timing
// tracking, the 16-QAM / QPSK demapper (hierarchical HP or LP), the time de-interleaver, the turbo decoder and the EFRAME CRCs.
// Input at the native OFDM rate (dvbsh::sampleRate). Output: 188-byte transport packets (transport_error_indicator on failed CRCs).
#pragma once
#include "dvbsh.h"
#include <memory>

namespace dect2 {
namespace dvbsh {

struct RxStatus {
    int level = 0;            // 0 searching, 1 OFDM symbols found, 2 TPS locked (parameters known), 3 transport stream flowing
    Params p;                 // valid from level 2
    int sfInSh = 0;
    double cfoHz = 0, snrDb = 0;
    long codewordsOk = 0, codewordsBad = 0, packetsOk = 0, packetsBad = 0, tpsFrames = 0;
    double avgIterations = 0;
    int tiLmax = 0;           // time de-interleaver depth in cycles of 48 IU
    size_t tiBytes = 0;       // its memory
    bool tiTooLarge = false;  // the signalled interleaver needs more memory than the receiver allows
};

class Receiver {
public:
    Receiver();
    ~Receiver();
    void configure(double fs);                 // input rate (Hz); the OFDM elementary rate of the channel
    void reset();
    void setLowPriority(bool lp);              // hierarchical modulation: decode the LP stream
    void setMaxInterleaverBytes(size_t b);     // default 768 MB
    void setPacketCallback(std::function<void(const uint8_t* pkts, size_t count)> cb);
    void feed(const cf32* x, size_t n);
    RxStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace dvbsh
} // namespace dect2
