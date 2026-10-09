// DAB Transmitter Identification Information (TII), ETSI EN 300 401 clause 14.8, transmission mode I.
// Every transmitter of a network switches on 4 pairs of adjacent carriers in the null symbol of every other frame (the frames whose CIF
// count is 0 - 3 modulo 8), repeated in the four quarters of the band: 32 carriers. Which 4 of the 8 pair positions of a comb are on is the
// pattern, the MainId (0 - 69); which comb, the SubId (0 - 23). A receiver that sees several transmitters of a single-frequency network
// finds several of these, and with a list of the transmitters (where each MainId / SubId stands) can put them on a map.
#pragma once
#include <complex>
#include <cstdint>
#include <vector>

namespace dect2::dabtii {

using cf32 = std::complex<float>;
constexpr int kMainIds = 70, kSubIds = 24, kPairs = 8;

// a_p(b) of table 26 as a byte, b = 0 in the top bit: the 70 bytes with exactly four bits set, in ascending order
uint8_t pattern(int mainId);
int mainIdOfPattern(uint8_t bits);   // the inverse; -1 for a byte that is not a pattern
// The first carrier of each active pair (k and k + 1 are on; k in -768 .. 767, never 0): 16 values
void pairCarriers(int mainId, int subId, std::vector<int>& k);

struct Found {
    int mainId = 0, subId = 0;
    float levelDb = 0;     // the four pairs above the noise of the null symbol (per carrier)
    float marginDb = 0;    // the weakest of the four pairs against the strongest of the other four
};

// Finds the transmitters from the spectra of the null symbols. The TII is sent in every other frame only: the frames without it are the
// reference (the noise and interference of the null symbol at every carrier), which is what the standard leaves them empty for.
class Detector {
public:
    void reset();
    // The spectrum of one null symbol (2048 bins, bin = k mod 2048, carrier offset already removed) and the frame's index (counts up by
    // one per frame; only its parity matters).
    void addNull(const std::vector<cf32>& spectrum, uint64_t frame);
    // The transmitters heard, strongest first. Empty until both kinds of frame have been seen a few times.
    std::vector<Found> found() const;
    int framesSeen() const { return n_[0] + n_[1]; }

private:
    // pair powers averaged per frame parity: [parity][comb][pair position], summed over the four quarters and the two carriers of a pair
    double p_[2][kSubIds][kPairs] = {};
    int n_[2] = {};
};

} // namespace dect2::dabtii
