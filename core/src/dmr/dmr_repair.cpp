#include "dect2/dmr/dmr_repair.h"
#include "dect2/dmr/dmr_fec.h"
#include "dect2/dmr/dmr_proto.h"
#include <cstring>

namespace dect2 {
using namespace dmr;

// A voice LC header / terminator whose Reed-Solomon (12,9) check failed: put heard destination / source IDs back (within two bit flips) and keep a
// candidate only when the word then checks out clean. Unique or nothing: a wrong ID cannot be made up.
bool lcRepairHeard(const Bits& info96, bool term, const voice::HeardIds& ids, Bits& fixed) {
    uint8_t w[12];
    bitsToBytes(info96, 0, 96, w);
    const uint32_t dst = be24(w + 3), src = be24(w + 6);
    std::vector<uint32_t> dOpt{dst}, sOpt{src};
    for (auto& c : ids.nearIds(dst, 2, 4)) dOpt.push_back(c.id);
    for (auto& c : ids.nearIds(src, 2, 4)) sOpt.push_back(c.id);
    // an ID heard exactly is as good an option as any (the damage may be elsewhere in the word)
    int found = 0;
    for (uint32_t d : dOpt) for (uint32_t s : sOpt) {
        if (d == dst && s == src) continue;
        uint8_t v[12];
        memcpy(v, w, 12);
        putBe24(v + 3, d); putBe24(v + 6, s);
        Bits b;
        bytesToBits(v, 12, b);
        uint8_t lc[9];
        if (lcBurstDecode(b, term, lc) == 0) { if (!found) fixed = b; found++; }
    }
    return found == 1;
}


} // namespace dect2
