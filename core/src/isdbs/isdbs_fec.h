// ISDB-S: the coding of a frame (internal). One K = 7 (171,133) convolutional code runs over the whole transmitted bit stream of the frames
// (header, then the bytes of the slots that carry packets, in slot order; BO.1408 Fig. 12 and 6.3), the puncturing starts again at every
// group of slots (Table 2 note), TC8PSK sends one bit of each pair through the code and the other uncoded (Fig. 14). Super-frame side:
// RS(204,188), energy dispersal over the whole super-frame, byte interleaving across its 8 frames (Figs. 1, 5, 6).
#pragma once
#include "dect2/isdbs/isdbs.h"
#include <array>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace isdbs {

// Where everything of one TMCC layout sits in a frame
struct Layout {
    struct Group { int mod = kUnused, firstSlot = 0, count = 0, step0 = 0, steps = 0, sym0 = 0; };   // sym0: data symbol index (bursts not counted)
    std::vector<Group> groups;
    int totalSteps = kHeaderBits;          // trellis steps of a frame, header included
    std::array<int, kSlots> group{};       // group of each slot, -1 none
    std::array<int, kSlots> ordinal{};     // which packet of its group the slot carries, -1 for a dummy slot
    bool ok = false;
    void build(const Tmcc& t);
};

// position of data symbol d (0..38975) in the frame
inline int dataPos(int d) { return kHeaderSyms + (d / kBlockData) * kBlockSyms + d % kBlockData; }

// ---- encoder: keeps the state of the code across frames
class FrameEncoder {
public:
    // header (96 bits) and the bytes of each group's packet-carrying slots in order (groupBytes[g], 203 * packets bytes); writes the
    // frame's 39936 symbols, bursts included
    void encode(const Layout& L, const uint8_t* header96, const std::vector<std::vector<uint8_t>>& groupBytes, cf32* frame, const float* burst768 = nullptr);
private:
    unsigned state_ = 0;
};
// the header bits of frame `f` (0..7) of a super-frame: W1, 64 TMCC bits (energy dispersed), W2 or W3
void headerBits(const uint8_t* tmcc512, int f, uint8_t* out96);

// ---- decoder
// Branch metrics of a trellis step: m[(x << 1) | y], larger is better (log-likelihood scale, 0 for a bit that was not sent)
struct Branch { float m[4] = {0, 0, 0, 0}; };
// Max-log Viterbi over steps [0, n): startState / endState -1 when unknown; forced[i] (when given) 0 / 1 forces the input of step i
// (-1 free). Returns the decided inputs and, when `states` is given, the state before each step (n + 1 values).
float viterbi(const Branch* b, int n, int startState, int endState, const int8_t* forced, uint8_t* bits, uint8_t* states = nullptr);
unsigned convOut(unsigned state, int in);  // (x << 1) | y of a step
inline unsigned convNext(unsigned state, int in) { return ((state >> 1) | ((unsigned)in << 5)) & 63; }
// state after the first 6 bits of W1 (the start of every frame)
unsigned w1State();

// Branch metrics of a frame's steps from its symbols (carrier removed, unit power): the header, and the groups in `L`
void headerBranches(const cf32* frame, Branch* out96, int nbits = kHeaderBits);   // nbits: only that many (the next frame's W1 has just 12 symbols in the buffer)
void groupBranches(const Layout& L, int g, const cf32* frame, Branch* out);   // out has L.groups[g].steps entries
// TC8PSK: the uncoded bit of each symbol once the coded path is known (states from viterbi())
void tc8Uncoded(const Layout& L, int g, const cf32* frame, const uint8_t* states, const uint8_t* bits, int stepFrom, int stepTo, uint8_t* w);

// ---- super-frame side
// energy dispersal of byte b (1..203) of slot s in frame f: the PRBS byte to add
uint8_t prbsByte(int f, int s, int b);
const std::vector<uint8_t>& tmccPrbs();    // 512 bits
// interleaving of one slot across the 8 frames: byte j of frame f goes to byte k of frame F
inline void interleavePos(int f, int j, int& F, int& k) { const int n = j * kFramesPerSuper + f; F = n / kSlotBytes; k = n % kSlotBytes; }

}  // namespace isdbs
}  // namespace dect2
