// A dictionary of the IDs actually heard on this channel (DMR radio / group IDs, NXDN and P25 unit IDs, D-STAR callsigns, pager capcodes) and the
// nearest-neighbour search the receivers use to repair a header that arrived with a few wrong bits.
//
// A repair is only ever offered as a candidate: the caller puts the candidate back into the frame and accepts it only when the frame's own check
// (CRC, Reed-Solomon) then passes, so a wrong ID cannot be made up. The dictionary just turns "uncorrectable" into "correct, with a known neighbour".
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::voice {

class HeardIds {
public:
    struct Cand { uint32_t id; int flips; int count; };
    // an ID that decoded cleanly (counts how often)
    void note(uint32_t id);
    // IDs within maxFlips bit flips of rx (rx itself excluded), nearest first, then the most often heard; at most maxN
    std::vector<Cand> nearIds(uint32_t rx, int maxFlips = 2, size_t maxN = 8) const;
    bool known(uint32_t id) const { return ids_.count(id) != 0; }
    size_t size() const { return ids_.size(); }
    void clear() { ids_.clear(); texts_.clear(); }

    // text fields of fixed width (callsigns): candidates that differ from rx in at most maxSubs characters (same length), nearest first
    struct TextCand { std::string text; int subs; int count; };
    void noteText(const std::string& s);
    std::vector<TextCand> nearText(const std::string& rx, int maxSubs = 2, size_t maxN = 6) const;
    size_t textSize() const { return texts_.size(); }

    // chance that a wrong ID of `bits` bits lies within maxFlips of some entry just by accident: how much a candidate alone proves (the frame check
    // does the proving; this is for the "repaired" flag's confidence)
    double accidentalChance(int bits, int maxFlips) const;

private:
    std::map<uint32_t, int> ids_;
    std::map<std::string, int> texts_;
    static constexpr size_t kMax = 4096;   // the oldest quietest entries go first
};

} // namespace dect2::voice
