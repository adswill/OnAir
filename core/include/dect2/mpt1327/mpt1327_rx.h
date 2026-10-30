// MPT 1327 control channel decoder: FM audio of the control channel in, codewords and messages out.
// 1200 bit/s FFSK is demodulated as minimum shift keying around 1500 Hz (tones 1200 and 1800 Hz, a phase step of a quarter turn per bit);
// bit timing is found by running a sync search on every sample phase. The bit polarity (which tone is which bit) is taken from the
// sync pattern: SYNC or its complement locks the stream, so the receiver works with either discriminator polarity.
#pragma once
#include "dect2/mpt1327/mpt1327_proto.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2::mpt1327 {

constexpr double kBitRate = 1200.0;

// How codewords are framed. Standard: only the SYNC word (0xC4D7 or its complement) starts a codeword. Tolerant: the sync word is not checked;
// a codeword is taken at any position where the last 64 bits have good check bits, and once a good codeword is found the next ones are
// taken on the 64 bit grid (as TrunkView does on French networks, whose sync word differs from the standard). Auto: standard framing, and
// tolerant framing is added when no exact sync word has been seen after 8 s of audio.
enum class SyncMode { Standard, Tolerant, Auto };

struct Event {
    double t = 0;        // seconds of audio since reset
    Message msg;
};

struct Stats {
    uint64_t good = 0;       // codewords with good check bits (control channel system codewords and address / data codewords after a SYNC)
    uint64_t bad = 0;        // codewords after a SYNC whose check bits fail
    uint64_t ccsc = 0;       // control channel system codewords seen
    uint64_t messages = 0;   // address codewords decoded
    int sys = -1;            // system identity from the last good CCSC
    bool tolerant = false;   // sync word check is off (Tolerant, or Auto after the fallback)
    bool locked = false;     // a good codeword within the last 3 s
    double t = 0;
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    void reset();
    void setSyncMode(SyncMode m);
    SyncMode syncMode() const;
    void process(const float* audio, size_t n, double fs);   // FM discriminator audio at fs Hz (any rate above about 6 kHz)
    const Stats& stats() const;
    std::vector<Event> takeEvents();                          // messages decoded since the last call
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace dect2::mpt1327
