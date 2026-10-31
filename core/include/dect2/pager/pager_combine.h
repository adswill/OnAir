// Pagers: multi-copy combining (#20). Paging networks repeat messages; each copy is damaged in a different place, so the copies are voted
// code word by code word BEFORE the BCH repair decides a word is lost. Pure functions plus a small history, no receiver state.
#pragma once
#include <cstdint>
#include <vector>

namespace dect2 {
namespace pager {

// The best 32-bit code word out of n received copies of the same transmitted word (n >= 1).
//  - copies that pass the BCH check vote with their repaired data; the most votes win, ties go to the copy that needed fewer repairs;
//  - when no copy passes: three or more copies are voted bit by bit and the result is repaired; two copies are merged by trying the
//    bit positions in which they differ (at most 10) and accepting only a unique closest valid word.
struct CombinedWord {
    uint32_t raw = 0;             // the chosen code word (a valid code word when ok)
    bool ok = false;              // false: no valid word could be built, raw is the plain bit vote
    bool voted = false;           // the result needed the other copies (no single copy was good, or the copies disagreed and a vote decided)
    int agree = 0;                // how many copies decode to the chosen word
};
CombinedWord combineWord(const uint32_t* raw, int n);

// A group of copies of one POCSAG message: the code words after the sync, the address word first.
struct PocsagCopy {
    std::vector<uint32_t> words;
    bool hasTerm = false;          // the word that ended the message (an idle word or the next address word) is known
    uint32_t term = 0;             // ... as received
};

// How well two copies match, over the positions both have: both words valid and equal / valid and different. Position 0 (the address word) is ignored.
struct CopyMatch { int equal = 0, differ = 0, overlap = 0; };
CopyMatch comparePocsagCopies(const PocsagCopy& a, const PocsagCopy& b);
// true when b is a plausible repeat of a: they have the same address word and enough message words agree, almost none contradict
bool pocsagSameMessage(const PocsagCopy& a, const PocsagCopy& b);
// Merges copies position by position; copies[0] is the one being decoded. Each position is voted over all copies (the first copy wins a tie, a word
// that needed no repair beats one that did), so a word the BCH code "repaired" into a wrong word is outvoted too. The merged message is longer than
// the first copy only when the first copy ended on a doubtful word (cut off, or ended by an idle / address word that itself needed repair): a real
// short message that is the start of a longer one stays short. valid[i] tells whether word i is a valid code word.
struct MergedCopy { std::vector<uint32_t> words; std::vector<uint8_t> valid; int recovered = 0; };   // recovered: positions that differ from the first copy as decoded
MergedCopy mergePocsagCopies(const std::vector<const PocsagCopy*>& copies);

// FLEX: a phase is 88 code words, bit-interleaved (bits[blk*256 + j*8 + w] = bit j of word blk*8+w). The same page set repeated in the same
// frame of a later cycle lines up word for word, so the phases can be voted like POCSAG copies.
void flexPhaseWords(const uint8_t* bits, uint32_t* words88);
void flexPhaseFromWords(const uint32_t* words88, uint8_t* bits);
class FlexPhaseCombiner {
public:
    int windowSec = 900;          // copies older than this are forgotten
    int maxCopies = 4;
    // Looks at one phase (2816 bits). If earlier phases of the same stream (key: speed and phase) match word for word, the damaged words
    // are replaced by the voted words, in place. Returns the number of code words that were rebuilt from other copies.
    int process(int key, uint8_t* bits, int64_t nowSec);
    int phasesCombined = 0, wordsRecovered = 0;
    void clear() { hist_.clear(); }
private:
    struct Entry { int key; int64_t wall; std::vector<uint32_t> words; };
    std::vector<Entry> hist_;
};

} // namespace pager
} // namespace dect2
