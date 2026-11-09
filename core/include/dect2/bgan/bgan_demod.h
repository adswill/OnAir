// BGAN forward-bearer demodulator for one carrier: matched filter, pilot comb and unique word acquisition, pilot-aided channel estimate, soft
// demapping and the turbo decoder, then the signalling parser. Input: complex samples of the carrier near 0 Hz (any rate of at least 2.2 samples per symbol).
// Designed around the frame structure: every 80 ms frame is measured and decoded on its own (bgan_phy.h).
#pragma once
#include "dect2/bgan/bgan_phy.h"
#include "dect2/bgan/bgan_sig.h"
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace dect2::bgan {

struct DemodStatus {
    int state = 0;                 // 0 searching, 1 frame sync (pilots and unique word), 2 signalling decoded
    int bearer = -1;               // index into bearer(), -1 not known
    int subtype = -1;              // subtype of the unique word
    double cfoHz = 0;
    double esn0Db = 0, cn0Dbhz = 0;   // measured from the pilots
    uint64_t frames = 0, framesLost = 0, blocksOk = 0, blocksBad = 0, blocksEmpty = 0;
    float blockOkRate = 0;         // recent decode success
    float occupancy = -1;          // recent share of non-empty FEC blocks, -1 before any decode
    bool structureOnly = false;    // frame sync holds but the bearer has no published tables / decoders never verified
    double seenBb = -1;            // seconds of signal since the last bulletin board, -1 never
    CarrierSig sig;
};

class BearerDemod {
public:
    BearerDemod();
    ~BearerDemod();
    // symRate selects the candidate bearers; fs = input rate. Returns false if the rate is too low.
    bool configure(double fs, int symRate);
    void reset();
    void feed(const cf32* x, size_t n);
    const DemodStatus& status() const;
    void takeLines(std::vector<std::string>& out);      // new plain-language lines since the last call
    void setDecodeAll(bool on);                         // decode every FEC block (default) or only the first
    void setForceBearer(int idx);                       // Advanced: -1 automatic
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::bgan
