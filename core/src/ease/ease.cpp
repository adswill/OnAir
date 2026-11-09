#include "dect2/ease/ease.h"
#include "dect2/inmsat/inmsat_satid.h"
#include <algorithm>
#include <cmath>
#include <utility>
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

std::vector<double> ismBandsFor(const std::string& c) {
    static const char* const k915[] = {"US", "CA", "MX", "BR", "AR", "CL", "CO", "PE", "AU", "NZ"};
    for (const char* x : k915) if (c == x) return {433.92, 915.0};
    return {433.92, 868.3};
}

AutoBand::AutoBand(std::vector<double> b, double dwellS, double stayS) : bands_(std::move(b)), scores_(bands_.size(), 0.0), dwell_(dwellS), stay_(stayS) {}

int AutoBand::best() const {
    int b = 0;
    for (int i = 1; i < (int)scores_.size(); i++) if (scores_[(size_t)i] > scores_[(size_t)b]) b = i;
    return b;
}

int AutoBand::update(double now, uint64_t total) {
    if (bands_.empty()) return 0;
    if (since_ < 0) since_ = now;
    if (!haveLast_ || total < last_) { last_ = total; haveLast_ = true; }   // a receiver restart resets its counter
    scores_[(size_t)cur_] += (double)(total - last_);
    last_ = total;
    const int n = (int)bands_.size();
    if (n < 2) return 0;
    const double elapsed = now - since_;
    if (!settled_) {                                // searching: a band per dwell until one of them decodes
        if (elapsed >= dwell_) {
            const int b = best();
            if (scores_[(size_t)b] > 0) { settled_ = true; cur_ = b; sampleNext_ = (b + 1) % n; }
            else cur_ = (cur_ + 1) % n;
            since_ = now;
        }
        return cur_;
    }
    if (sampling_) {                                // a short look elsewhere, then back to the best band
        if (elapsed >= dwell_) { sampling_ = false; cur_ = best(); since_ = now; sampleNext_ = (cur_ + 1) % n; }
        return cur_;
    }
    if (elapsed >= stay_) {
        for (double& s : scores_) s *= 0.7;         // old decodes count for less, so a device that went away stops holding the radio
        int i = sampleNext_;
        if (i == cur_) i = (i + 1) % n;
        sampleNext_ = (i + 1) % n;
        cur_ = i; sampling_ = true; since_ = now;
    }
    return cur_;
}

int FreqHunt::update(double now, uint64_t total) {
    if (mhz_.empty()) { finished_ = true; return 0; }
    if (!started_) { started_ = true; since_ = now; base_ = total; return cur_; }
    if (found_ || finished_) return cur_;
    if (total < base_) base_ = total;
    if (total > base_) { found_ = true; return cur_; }
    if (now - since_ >= dwell_) {
        if (cur_ + 1 >= (int)mhz_.size()) { finished_ = true; return cur_; }
        cur_++; since_ = now; base_ = total;
    }
    return cur_;
}

bool freqRegionApplies(const std::string& r, const std::string& c) {
    if (c.empty() || r.empty() || r == "GLOBAL" || r == c) return true;
    if (r == "UK") return c == "GB";
    if (r == "NA") return c == "US" || c == "CA" || c == "MX";
    if (r == "EU") {
        static const char* const k[] = {"AT","BE","BG","HR","CY","CZ","DK","EE","FI","FR","DE","GR","HU","IE","IT","LV","LT","LU","MT","NL","PL","PT","RO","SK","SI","ES","SE","IS","NO","CH","LI"};
        for (const char* x : k) if (c == x) return true;
    }
    return false;
}

void addDrmHit(std::vector<DrmHit>& h, double khz, float snr) {
    bool seen = false;
    for (auto& x : h) if (std::fabs(x.khz - khz) < 0.5) { x.snrDb = std::max(x.snrDb, snr); seen = true; }
    if (!seen) h.push_back({khz, snr});
    std::sort(h.begin(), h.end(), [](const DrmHit& a, const DrmHit& b) { return a.snrDb > b.snrDb; });
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
