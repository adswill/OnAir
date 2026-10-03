// BB frames -> MPEG transport stream packets (ETSI EN 302 755 clause 5.1: mode adaptation, normal and high-efficiency mode).
#pragma once
#include "t2fec.h"
#include <cstdint>
#include <functional>
#include <vector>
#include <utility>

namespace dect2 {

struct BbStats {
    uint64_t frames = 0, framesLost = 0, packets = 0, nullInserted = 0;
    uint64_t resyncs = 0, crcErrors = 0, unsupported = 0;
    bool hem = false;
    int issyi = 0, npd = 0, tsGs = 0;
};

class BbUnpacker {
public:
    using Sink = std::function<void(const uint8_t* pkt188)>;
    void setSink(Sink s) { sink_ = std::move(s); }
    // A BB frame (descrambled bits, one per byte) or a gap: call lost() for every FEC block that could not be decoded,
    // and also when whole T2 frames are missing.
    void push(const std::vector<uint8_t>& bits, const BbHeader& h);
    void lost();
    const BbStats& stats() const { return st_; }

private:
    void emit(const uint8_t* payload187);
    Sink sink_;
    BbStats st_;
    std::vector<uint8_t> partial_;
    bool havePartial_ = false;
    bool gap_ = true;
    uint8_t lastCrc_ = 0;
    bool haveCrc_ = false;
    uint8_t cc_null_ = 0;
};

} // namespace dect2
