#include "dect2/ease/nit.h"
#include <algorithm>
#include <cmath>

namespace dect2::ease {

std::vector<NitRow> nitRows(const SiSnapshot& si, int kind, int ownTsid, double ownTune, bool inverted) {
    std::vector<NitRow> out;
    const SiTransponder* own = nullptr;
    auto wanted = [&](int k) { return kind == 1 ? (k == 1 || k == 5) : k == kind; };
    for (const auto& n : si.networks) {
        if (!n.actual) continue;
        for (const auto& t : n.transponders) if (wanted(t.kind) && t.tsid == ownTsid) own = &t;
    }
    for (const auto& n : si.networks) {
        if (!n.actual) continue;
        for (const auto& t : n.transponders) {
            if (!wanted(t.kind) || t.freqMhz <= 0) continue;
            if (std::any_of(out.begin(), out.end(), [&](const NitRow& r) { return std::fabs(r.mhz - t.freqMhz) < 0.0005 && r.pol == t.pol; })) continue;
            NitRow r;
            r.mhz = t.freqMhz; r.pol = t.pol; r.modulation = t.modulation; r.fec = t.fecInner; r.services = (int)t.services.size(); r.kind = t.kind;
            r.symbolRateK = t.symbolRateK; r.s2 = t.s2; r.current = own && t.tsid == own->tsid;
            if (kind == 2) r.tuneMhz = t.freqMhz;
            else if (own) r.tuneMhz = inverted ? ownTune + own->freqMhz - t.freqMhz : ownTune + t.freqMhz - own->freqMhz;
            out.push_back(r);
        }
    }
    std::sort(out.begin(), out.end(), [](const NitRow& a, const NitRow& b) { return a.mhz < b.mhz; });
    return out;
}

} // namespace dect2::ease
