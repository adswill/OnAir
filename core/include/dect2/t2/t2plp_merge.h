// Annex D of EN 302 755: rebuilding one transport stream from a data PLP and the common PLP of its group. The two partial streams are co-timed: where the data PLP has a null
// packet and the common PLP a real one, the common packet goes in. This relies on the two streams lining up packet for packet per T2 frame: the common PLP's frame goes in first.
#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace dect2 {

class T2PlpMerge {
public:
    // the packets of one T2 frame of the common PLP; what is left of the frame before is dropped
    void commonFrame(const std::vector<uint8_t>& packets) { q_.clear(); for (size_t i = 0; i + 188 <= packets.size(); i += 188) q_.emplace_back(packets.begin() + (long)i, packets.begin() + (long)i + 188); }
    // a data PLP packet: replaced in place by the co-timed common packet where the rule applies (returns true if it was)
    bool step(uint8_t* pkt) {
        if (q_.empty()) return false;
        const bool nul = ((pkt[1] & 0x1F) << 8 | pkt[2]) == 0x1FFF;
        const std::vector<uint8_t> c = std::move(q_.front()); q_.pop_front();
        const bool cNull = ((c[1] & 0x1F) << 8 | c[2]) == 0x1FFF;
        if (nul && !cNull) { memcpy(pkt, c.data(), 188); replaced_++; return true; }
        return false;
    }
    uint64_t replaced() const { return replaced_; }
    void reset() { q_.clear(); replaced_ = 0; }
private:
    std::deque<std::vector<uint8_t>> q_;
    uint64_t replaced_ = 0;
};

}  // namespace dect2
