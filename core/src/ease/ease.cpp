#include "dect2/ease/ease.h"
#include "dect2/inmsat/inmsat_satid.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dect2::ease {

namespace {
constexpr int64_t kHalfDay = 43200;
constexpr int64_t kLaunch = 11 * 3600 + 15 * 60;   // 11:15 UTC, and twelve hours later
constexpr int64_t kBefore = 30 * 60, kAfter = 2 * 3600;
int64_t floorDiv(int64_t a, int64_t b) { int64_t q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }
}

SondeWindow sondeWindow(int64_t u) {
    SondeWindow w;
    const int64_t k = floorDiv(u - (kLaunch - kBefore), kHalfDay);    // the last window that has opened
    const int64_t launch = kLaunch + k * kHalfDay;
    if (u < launch + kAfter) { w.inWindow = true; w.secsToLaunch = (int)(launch - u); w.id = k; }
    else { w.inWindow = false; w.secsToLaunch = (int)(launch + kHalfDay - u); w.id = k + 1; }
    return w;
}

bool sondeAutoStartDue(const SondeWindow& w, int64_t lastFired, bool optionOn, bool anythingRunning) {
    return optionOn && w.inWindow && w.id != lastFired && !anythingRunning;
}

GeoPick highestGeo(double lat, double lon, const double* slots, int n, double minEl) {
    GeoPick best;
    double top = minEl;
    for (int i = 0; i < n; i++) {
        double az = 0, el = 0;
        inmsat::geoLook(lat, lon, slots[i], az, el);
        if (el >= top && (best.index < 0 || el > best.elDeg)) { best.index = i; best.azDeg = az; best.elDeg = el; top = el; }
    }
    return best;
}

const char* compass8(double az) {
    static const char* const k[8] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
    double a = std::fmod(az, 360.0); if (a < 0) a += 360.0;
    return k[(int)std::floor(a / 45.0 + 0.5) % 8];
}

Preselect preselectAction(bool prefExists, bool positionKnown, bool running, double freqMhz, double defMhz) {
    if (prefExists) return Preselect::Done;
    if (!positionKnown || running) return Preselect::Wait;
    if (std::fabs(freqMhz - defMhz) > 0.001) return Preselect::LeaveAlone;
    return Preselect::Pick;
}

bool modeNotUsedIn(const std::string& name, const std::string& country) {
    if (country.empty()) return false;
    static const struct { const char* name; const char* usedIn; } k[] = {   // space-separated: broadcasters' published deployments, not complete lists of every small user
        {"ATSC", "US CA MX KR DO SV HN GT BS"}, {"ATSC 3.0", "US KR JM"},
        {"ISDB-T", "JP BR AR CL PE EC VE BO PY UY CR GT HN NI PH BW MV AO LK"}, {"ISDB-S (BS)", "JP"},
        {"DTMB", "CN HK MO CU KH LA PK"},
        {"HD Radio", "US CA MX PH"}, {"CDR", "CN"},
        {"UAT", "US"}, {"NOAA Weather Radio SAME", "US CA"},
        {"LBJ", "CN"}, {"KLW", "CN"}, {"TDCS", "CN"},
    };
    for (const auto& e : k) {
        if (name != e.name) continue;
        const std::string list = std::string(" ") + e.usedIn + " ";
        return list.find(" " + country + " ") == std::string::npos;
    }
    return false;
}

int wefaxFollowPick(const std::vector<WefaxSlot>& slots, const std::string& id, long long now, int lateS) {
    for (int i = 0; i < (int)slots.size(); i++) {
        if (slots[i].startUtc + lateS < now) continue;
        if (!id.empty() && slots[i].station != id) continue;
        return i;
    }
    return -1;
}

bool wefaxFollowDue(long long start, long long now, int marginS, long long tunedFor, bool chartUnderWay) {
    return !chartUnderWay && tunedFor != start && start - now <= marginS;
}

double dscWorkingKhz(const std::string& t) {
    if (t.size() < 5 || t.compare(t.size() - 3, 3, "kHz") != 0) return 0;
    char* end = nullptr;
    const double k = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return 0;
    return k >= 1605.0 && k <= 27500.0 ? k : 0;
}

} // namespace dect2::ease
