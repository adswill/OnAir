// ATSC 3.0 bootstrap (A/321): the short signal at the start of every frame that tells a receiver the version, the bandwidth, the sample
// rate of the rest of the frame and what the preamble looks like. It is always sent at 6.144 Msamples/s in a 4.5 MHz band.
// The generator is for tests (nothing is transmitted); the detector finds the bootstrap in a stream and reads the signalling.
#pragma once
#include "ring.h"
#include <cstddef>
#include <vector>

namespace dect2 {
namespace atsc3 {

constexpr double kBootstrapRate = 6144000.0;
constexpr int kBootstrapFft = 2048;
constexpr int kBootstrapSymbolLen = 3072;   // samples per bootstrap symbol (500 us)

struct Bootstrap {
    int minorVersion = 0;         // 0..7: selects the pseudo-noise seed (major version 0 only)
    int numSymbols = 4;           // including the first (synchronisation) symbol
    int eaWakeUp = 0;             // 2 bits: bit 1 is sent in symbol 1, bit 0 in symbol 2
    int minTimeToNext = 0;        // 0..30: see minTimeToNextMs()
    int systemBandwidth = 0;      // 0 = 6 MHz, 1 = 7 MHz, 2 = 8 MHz
    int bsrCoefficient = 0;       // 0..80: sample rate of the rest of the frame is (N + 16) * 0.384 MHz
    int preambleStructure = 0;    // 8 bits, defined in A/322 Annex H
    bool operator==(const Bootstrap& o) const {
        return minorVersion == o.minorVersion && numSymbols == o.numSymbols && eaWakeUp == o.eaWakeUp && minTimeToNext == o.minTimeToNext &&
               systemBandwidth == o.systemBandwidth && bsrCoefficient == o.bsrCoefficient && preambleStructure == o.preambleStructure;
    }
};

double postBootstrapRate(const Bootstrap& b);   // samples per second of the part after the bootstrap
int minTimeToNextMs(int x);                     // time to the next frame, lower bound in milliseconds
double bandwidthHz(int systemBandwidth);

// The bootstrap as 6.144 Msamples/s baseband, numSymbols * 3072 samples, unit power.
std::vector<cf32> generateBootstrap(const Bootstrap& b);

struct Detection {
    bool found = false;
    long start = 0;               // index of the first sample of the bootstrap in the input
    float metric = 0;             // 0..1, how well the first symbol matches (noise gives about 0.06)
    Bootstrap info;
    bool valid = false;           // the signalling bits were consistent (the shift pattern of the Gray code fits)
};

// Looks for a bootstrap in `n` samples at 6.144 Msamples/s (the first symbol is correlated against all minor versions) and reads the
// signalling. Needs the whole bootstrap (up to 8 symbols) after the first symbol, so give at least 8 * 3072 samples after a possible start.
// seedOnly (0..7): when the minor version is already known, only that one is tried (8 times less work).
Detection detectBootstrap(const cf32* x, size_t n, int seedOnly = -1);

} // namespace atsc3
} // namespace dect2
