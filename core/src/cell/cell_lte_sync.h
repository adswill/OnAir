// Private header of the LTE cell search: the structures passed between the synchronisation helpers (cell_lte_sync.cpp) and the
// search itself (cell_lte_search.cpp). Not part of the API.
#pragma once
#include "cell_lte_int.h"
#include <array>
#include <vector>

namespace dect2::cell::lte_rx {

constexpr double kSyncRate = 1.92e6;   // the central 6 resource blocks, FFT 128
constexpr int kHalf = 9600;            // a half frame at that rate
constexpr int kNfft6 = 128;
constexpr float kSssSigExtra = 5.0f;    // SSS significance for a second / third cell behind one PSS timing
constexpr float kSssListAlone = 5.5f;   // SSS significance that lists a cell whose MIB did not decode (noise alone stays under 4.5)

struct Cand { int nid2 = 0; double cfo = 0; double t0 = 0; float db = 0; };

struct Track {
    std::vector<double> t;          // PSS position per half frame (fit), sync samples
    double t0 = 0, slope = kHalf;   // t(m) = t0 + slope * m
    double cfo = 0;
    int nHalf = 0;
};

struct SssResult { bool ok = false; int nid1 = 0; bool ext = false, tdd = false; int sf0Parity = 0; float score = 0, sig = 0; };

// a cell already found: its PSS / SSS are taken out before the search for weaker cells behind the same timing
struct KnownSync { int nid1 = 0, nid2 = 0, parity = 0, off = 0; std::vector<double> t; };

struct MibTry { bool ok = false; LteMibInfo mib; int frame = 0; };

struct DciFound { LteDciSi d; Bits payload; int cce = 0, L = 4; Bits coded; };

// a cell whose MIB decoded: a weaker cell at its timing borrows its set-up, and has it cancelled from under itself
struct Decoded {
    int pci = 0; bool ext = false;
    int nRb = 6, ports = 1, phichDur = 0, phichRes = 0;
    int mibTail = 0;       // MIB bits 14 - 23 (LTE-M scheduling of SIB1-BR)
    double fs0 = 0;        // first frame start in the block, sync samples
    double startIn = 0;    // the same, input samples
    double cfo = 0;
    int sfnAt0 = 0;        // SFN of that frame
    double slope = 9600;   // half frame length, sync samples
    int outIdx = -1;       // its result
    bool haveSib1 = false; LteDciSi sib1;   // the allocation of its SIB1
    Bits sib1Tb;                            // and its transport block, when read (rebuilt in every repetition)
    struct DciEv { int fr, sf, cfi, cce; Bits coded; };
    std::vector<DciEv> dcis;                // its SI-RNTI DCIs as decoded (frame of the block, subframe, the coded bits)
};

// PSS peaks: the folded correlation energy over the first half frames, for each NID2 and carrier offset hypothesis. Several peaks per NID2
// (cells of other sites arrive at other times), the strongest first; a peak blocks +-64 samples (33 us: its echoes) around it.
std::vector<Cand> pssSearch(const std::vector<cf32>& y, double maxCfo, float minDb, int halves = 8, int perNid2 = 3);
// follow the PSS through the block: timing per half frame, clock (slope) and fine carrier offset
bool pssTrack(const std::vector<cf32>& y, const Cand& cd, Track& tr);
// the cell identity groups heard behind one PSS timing, the most significant first: time-synchronised cells of the same NID2 share the
// PSS. The known cells at this timing are cancelled first (their sequences and channels, estimated from the received symbols).
std::vector<SssResult> sssDetect(const std::vector<cf32>& ys, int nid2, const Track& tr, const std::vector<KnownSync>& known = {});
std::vector<std::pair<int, int>> pbchRes(int pci, bool ext);
// gather the received values and the per-port channel of a list of REs, and undo transmit diversity
void txdRes(const LteGrid& y, const LteChan& ch, const std::vector<std::pair<int, int>>& res, int ports, std::vector<float>& llr);
uint32_t mibMask(int ports);
// soft agreement of a decoded word, re-encoded, with the received values (1 = all agree, about 0 = noise)
float agreement(const float* e, const Bits& enc);
MibTry mibDecode(const std::vector<std::array<std::vector<float>, 3>>& llr, int pci, bool ext);
MibTry mibCombine(const std::vector<std::array<std::vector<float>, 3>>& llr, int pci, bool ext);
int pcfichDecode(const LteGrid& g, const LteChan& ch, const LteCellCfg& c, int sf);
// the known transmissions of a decoded cell in one subframe of the SI stage: its SIB1 PDSCH (subframe 5 of even frames) and its decoded DCIs
void knownParts(const Decoded& d, const LteCellCfg& c, int fr, int sf, int sfn, int cfi, std::vector<LteTxPart>& parts);
LteCellCfg cfgOf(const Decoded& d, int nRbGrid);
void addCrs(std::vector<LteGrid>& port, const LteCellCfg& c, int sf);

}   // namespace dect2::cell::lte_rx
