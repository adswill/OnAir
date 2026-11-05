// Demodulation chains of the Smart CubeSat decoder. A chain takes one carrier brought down to a 96 kHz complex baseband stream and tries ONE
// modulation hypothesis (and every framing that belongs to it) on it; the frames that validate (CRC, Reed-Solomon, Golay, Viterbi + RS, Morse
// structure) come out as ChainFrame, the counts of every trial as TrialStats.
//   FSK  (2-FSK, GFSK, GMSK, MSK at 1200 .. 19200 baud): discriminator, bit slicer, then in parallel AX.25 behind the G3RUH descrambler (with the
//        AX100 Mode 6 Reed-Solomon fallback), plain NRZI AX.25, AX100 Mode 5 (ASM + Golay + RS), CCSDS ASM + RS, ASCII text.
//   AFSK (Bell 202, 1200 baud): discriminator audio, the AFSK demodulators of the APRS decoder, AX.25.
//   BPSK (1200 .. 9600 baud, differential): carrier recovery by squaring and a Costas loop, Gardner timing, then AX.25 (NRZI, with and without the
//        G3RUH descrambler), AX100 / CCSDS ASM, ASCII text and the AO-40 FEC of FUNcube / Nayif-1 (soft Viterbi + Reed-Solomon).
//   CW   (OOK keying, a beacon): envelope, key-down / key-up timing, Morse.
#pragma once
#include "cubesat_proto.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace cubesat {

constexpr double kChanRate = 96000.0;      // the rate of the channel stream the chains work on

struct ChainFrame {
    int framing = 0;                // cubesat::Framing, or kFramingAo40 / kFramingText / kFramingCw (cubesat_telem.h)
    std::string framingName;        // "AX.25 G3RUH", "AO-40 FEC", ...
    int mod = 0;                    // ModKind of cubesat_db.h
    int baud = 0;
    int rsErrors = -1;              // Reed-Solomon byte errors put right (-1: no RS code in this framing)
    std::string how;                // what validated it: "FCS", "Golay + Reed-Solomon", "Viterbi + Reed-Solomon", "Morse structure", ...
    std::string from, to, path, type;
    int pid = -1;
    bool hasCsp = false;
    CspHeader csp;
    std::vector<uint8_t> data, payload;
    std::string text;               // CW text, ASCII beacon
    int64_t pos = 0;                // sample index in the channel stream
    int variant = 0;
};

struct TrialStats {
    std::string label;              // "FSK 9600 baud, AX.25 G3RUH"
    int mod = 0, baud = 0;
    uint64_t heard = 0;             // frames that looked like frames and failed their checks
    uint64_t valid = 0;
};

class Chain {
public:
    virtual ~Chain() {}
    virtual void process(const cf32* y, size_t n) = 0;                 // channel stream at kChanRate
    virtual void reset() = 0;
    virtual void stats(std::vector<TrialStats>& out) const = 0;
    virtual int mod() const = 0;
    virtual int baud() const = 0;
    virtual double snrDb() const { return 0; }
    std::function<void(ChainFrame&)> sink;
};

std::unique_ptr<Chain> makeFskChain(int baud);
std::unique_ptr<Chain> makeAfskChain();
std::unique_ptr<Chain> makeBpskChain(int baud);
std::unique_ptr<Chain> makeCwChain();

// Morse decoding of key-down / key-up durations (seconds), exposed for tests. Returns the text ("" when it is not Morse); `unit` the dot length.
std::string morseDecode(const std::vector<std::pair<bool, double>>& runs, double* unit = nullptr);

} // namespace cubesat
} // namespace dect2
