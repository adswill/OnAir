// Band hopping for one narrow radio (WSJT-X Band Hopping): a band per 2-minute slot, chosen from the bands switched on for each UTC hour,
// optionally following the coordinated hopping table (the same band at every station in the same slot). docs/HFDIG-HOP-3.9.md.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace dect2 {

struct HopPlan {
    bool coordinated = true;                 // the 20-minute cycle 160 80 60 40 30 20 17 15 12 10 m, a band per 2-minute slot
    std::array<uint16_t, 24> hours{};        // per UTC hour: bit b = band b (multiband band numbers: 0 = 160 m ... 10 = 6 m) switched on
    int slotSec = 120;
    static HopPlan allHours(uint16_t bands) { HopPlan p; p.hours.fill(bands); return p; }
};

// the band for the slot holding utc; -1 when no band is on in that hour. Coordinated: the table's band for the slot, or when that band is off,
// the next band on in the table's order. Not coordinated: the bands on in that hour in turn, one per slot.
int hopBand(const HopPlan& p, int64_t utc);
int64_t hopNextChange(const HopPlan& p, int64_t utc);   // the start of the next slot
// settings text: "c=1;h=<24 hex masks separated by ,>"
std::string hopToText(const HopPlan& p);
HopPlan hopFromText(const std::string& s);

}  // namespace dect2
