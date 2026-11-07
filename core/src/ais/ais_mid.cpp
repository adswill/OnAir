#include "dect2/ais/ais_mid.h"
#include <algorithm>
#include <iterator>

namespace dect2 {
namespace {
struct MidRow { int mid; const char* iso; const char* name; };
const MidRow kMids[] = {
#include "ais_mid_table.inc"
};
const MidRow* findMid(int mid) {
    const MidRow* e = std::end(kMids);
    const MidRow* r = std::lower_bound(std::begin(kMids), e, mid, [](const MidRow& x, int m) { return x.mid < m; });
    return r != e && r->mid == mid ? r : nullptr;
}
} // namespace

const char* aisMidIso(int mid) { const MidRow* r = findMid(mid); return r ? r->iso : nullptr; }
const char* aisMidCountry(int mid) { const MidRow* r = findMid(mid); return r ? r->name : nullptr; }

AisMmsiInfo aisMmsiInfo(uint32_t m) {
    AisMmsiInfo o;
    int mid = 0;
    if (m >= 200000000u && m < 800000000u) { o.kind = AIS_MMSI_SHIP; mid = (int)(m / 1000000u); }
    else if (m >= 2000000u && m < 8000000u) { o.kind = AIS_MMSI_COAST; mid = (int)(m / 10000u); }          // 00MIDxxxx
    else if (m >= 20000000u && m < 80000000u) { o.kind = AIS_MMSI_GROUP; mid = (int)(m / 100000u); }       // 0MIDxxxxx
    else if (m >= 111200000u && m < 111800000u) { o.kind = AIS_MMSI_SAR_AIRCRAFT; mid = (int)(m / 1000u) - 111000; }   // 111MIDxxx
    else if (m >= 982000000u && m < 988000000u) { o.kind = AIS_MMSI_AUXILIARY; mid = (int)(m / 10000u) - 98000; }      // 98MIDxxxx
    else if (m >= 992000000u && m < 998000000u) { o.kind = AIS_MMSI_ATON; mid = (int)(m / 10000u) - 99000; }           // 99MIDxxxx
    else if (m >= 800000000u && m < 900000000u) { o.kind = AIS_MMSI_HANDHELD; mid = (int)(m / 100000u) - 8000; }       // 8MIDxxxxx
    else if (m / 1000000u == 970) o.kind = AIS_MMSI_SART;
    else if (m / 1000000u == 972) o.kind = AIS_MMSI_MOB;
    else if (m / 1000000u == 974) o.kind = AIS_MMSI_EPIRB;
    if (mid >= 200 && mid < 800) {
        if (const MidRow* r = findMid(mid)) { o.mid = mid; o.iso = r->iso; o.country = r->name; }
        else o.mid = mid;
    }
    return o;
}

const char* aisMmsiKindText(int k) {
    static const char* const n[AIS_MMSI_KINDS] = {"-", "Ship", "Coast station", "Group call", "SAR aircraft", "Auxiliary craft", "Aid to navigation", "Handheld VHF", "AIS-SART", "Man overboard", "EPIRB-AIS"};
    return k >= 0 && k < AIS_MMSI_KINDS ? n[k] : "-";
}

} // namespace dect2
