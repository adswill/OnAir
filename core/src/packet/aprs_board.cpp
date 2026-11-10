// APRS weather stations and messages (aprs_board.h).
#include "dect2/packet/aprs_board.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dect2 {

namespace {
bool has(float v) { return !std::isnan(v); }
float f(double v) { return std::isnan(v) ? NAN : (float)v; }
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string baseCall(const std::string& c) { const size_t d = c.find('-'); return d == std::string::npos ? c : c.substr(0, d); }
bool sameCall(const std::string& a, const std::string& b) { return upper(trim(a)) == upper(trim(b)); }
}

bool AprsWx::any() const {
    return has(tempC) || has(windDirDeg) || has(windKmh) || has(gustKmh) || has(rain1hMm) || has(rain24hMm) || has(rainMidnightMm) || has(humidityPct) || has(pressureHpa);
}

AprsWx aprsWxFromReport(const aprs::Weather& w, double timeSec) {
    constexpr double kKmhPerMph = 1.609344, kMmPerIn = 25.4;
    AprsWx o;
    o.timeSec = timeSec;
    o.tempC = std::isnan(w.tempF) ? NAN : (float)((w.tempF - 32.0) * 5.0 / 9.0);
    o.windDirDeg = f(w.windDirDeg);
    o.windKmh = std::isnan(w.windMph) ? NAN : (float)(w.windMph * kKmhPerMph);
    o.gustKmh = std::isnan(w.gustMph) ? NAN : (float)(w.gustMph * kKmhPerMph);
    o.rain1hMm = std::isnan(w.rain1hIn) ? NAN : (float)(w.rain1hIn * kMmPerIn);
    o.rain24hMm = std::isnan(w.rain24hIn) ? NAN : (float)(w.rain24hIn * kMmPerIn);
    o.rainMidnightMm = std::isnan(w.rainMidnightIn) ? NAN : (float)(w.rainMidnightIn * kMmPerIn);
    o.humidityPct = f(w.humidityPct);
    o.pressureHpa = f(w.pressureMbar);
    // values outside what a weather station can measure are reading errors (a station without the sensor sometimes sends zeros: those stay)
    if (has(o.tempC) && (o.tempC < -90 || o.tempC > 65)) o.tempC = NAN;
    if (has(o.humidityPct) && (o.humidityPct < 0 || o.humidityPct > 100)) o.humidityPct = NAN;
    if (has(o.pressureHpa) && (o.pressureHpa < 800 || o.pressureHpa > 1100)) o.pressureHpa = NAN;
    if (has(o.windDirDeg) && (o.windDirDeg < 0 || o.windDirDeg > 360)) o.windDirDeg = NAN;
    return o;
}

void aprsWxAdd(AprsWx& last, std::vector<AprsWx>& hist, const AprsWx& r) {
    if (!r.any()) return;
    const bool keep = last.any() && r.timeSec - last.timeSec >= 0 && r.timeSec - last.timeSec < 7200;
    AprsWx m = r;
    auto merge = [&](float AprsWx::*p) { if (!has(r.*p) && keep) m.*p = last.*p; };
    merge(&AprsWx::tempC); merge(&AprsWx::windDirDeg); merge(&AprsWx::windKmh); merge(&AprsWx::gustKmh); merge(&AprsWx::rain1hMm);
    merge(&AprsWx::rain24hMm); merge(&AprsWx::rainMidnightMm); merge(&AprsWx::humidityPct); merge(&AprsWx::pressureHpa);
    last = m;
    // the history holds what was sent, not the merged values: the graph shows a gap rather than a repeated old value
    if (!hist.empty() && r.timeSec >= hist.back().timeSec && r.timeSec - hist.back().timeSec < kAprsWxStepSec) {
        const double t0 = hist.back().timeSec;   // the point keeps its time slot; the newer values replace the older ones
        AprsWx p = r;
        auto keepOld = [&](float AprsWx::*q) { if (!has(r.*q)) p.*q = hist.back().*q; };
        keepOld(&AprsWx::tempC); keepOld(&AprsWx::windDirDeg); keepOld(&AprsWx::windKmh); keepOld(&AprsWx::gustKmh); keepOld(&AprsWx::rain1hMm);
        keepOld(&AprsWx::rain24hMm); keepOld(&AprsWx::rainMidnightMm); keepOld(&AprsWx::humidityPct); keepOld(&AprsWx::pressureHpa);
        p.timeSec = t0;
        hist.back() = p;
    } else if (hist.empty() || r.timeSec >= hist.back().timeSec) {
        hist.push_back(r);
    } else {   // out of order (a delayed copy): into its place
        auto it = std::upper_bound(hist.begin(), hist.end(), r.timeSec, [](double t, const AprsWx& x) { return t < x.timeSec; });
        hist.insert(it, r);
    }
    const double newest = hist.back().timeSec;
    size_t drop = 0;
    while (drop < hist.size() && newest - hist[drop].timeSec > kAprsWxHistorySec) drop++;
    if (drop) hist.erase(hist.begin(), hist.begin() + (long)drop);
    const size_t cap = (size_t)(kAprsWxHistorySec / kAprsWxStepSec) + 16;
    if (hist.size() > cap) hist.erase(hist.begin(), hist.begin() + (long)(hist.size() - cap));
}

const char* aprsCompass(float deg) {
    if (!has(deg)) return "";
    static const char* k[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int i = (int)std::floor(std::fmod(deg + 22.5f + 360.f, 360.f) / 45.f) & 7;
    return k[i];
}

std::string aprsWxLabel(const AprsWx& w) {
    std::string o;
    char b[48];
    if (has(w.tempC)) { snprintf(b, sizeof b, "%.1f \xC2\xB0""C", w.tempC); o += b; }
    if (has(w.windKmh)) {
        if (!o.empty()) o += "  ";
        if (has(w.windDirDeg) && w.windKmh > 0.5f) snprintf(b, sizeof b, "%s %.0f km/h", aprsCompass(w.windDirDeg), w.windKmh);
        else snprintf(b, sizeof b, "%.0f km/h", w.windKmh);
        o += b;
    }
    if (o.empty() && has(w.humidityPct)) { snprintf(b, sizeof b, "%.0f %%", w.humidityPct); o += b; }
    if (o.empty() && has(w.pressureHpa)) { snprintf(b, sizeof b, "%.1f hPa", w.pressureHpa); o += b; }
    return o;
}

// ---- messages

const char* aprsMsgKindName(AprsMsgKind k) {
    switch (k) {
    case AprsMsgKind::Message: return "message";
    case AprsMsgKind::Bulletin: return "bulletin";
    case AprsMsgKind::Announcement: return "announcement";
    case AprsMsgKind::GroupBulletin: return "group bulletin";
    case AprsMsgKind::Nws: return "weather service";
    }
    return "message";
}

AprsMsgKind aprsAddresseeKind(const std::string& addressee) {
    const std::string a = upper(trim(addressee));
    if (a.size() >= 4 && a.compare(0, 3, "BLN") == 0) {
        const char c = a[3];
        if (a.size() == 4) {
            if (c >= '0' && c <= '9') return AprsMsgKind::Bulletin;
            if (c >= 'A' && c <= 'Z') return AprsMsgKind::Announcement;
        } else if (c >= '0' && c <= '9') return AprsMsgKind::GroupBulletin;   // BLN<n><group name of up to 5 characters>
        else if (c >= 'A' && c <= 'Z') return AprsMsgKind::Announcement;
    }
    if (a.compare(0, 4, "NWS-") == 0 || a.compare(0, 4, "NWS_") == 0) return AprsMsgKind::Nws;
    if (a.size() >= 4 && (a.compare(0, 3, "SKY") == 0 || a.compare(0, 3, "CWA") == 0 || a.compare(0, 3, "BOM") == 0) &&
        (a[3] == '-' || a[3] == '_' || std::isalpha((unsigned char)a[3]))) return AprsMsgKind::Nws;
    return AprsMsgKind::Message;
}

bool aprsIsForCall(const std::string& addressee, const std::string& myCall) {
    const std::string a = upper(trim(addressee)), m = upper(trim(myCall));
    if (m.empty() || a.empty()) return false;
    if (m.find('-') != std::string::npos) return a == m || (m.size() > 2 && m.compare(m.size() - 2, 2, "-0") == 0 && a == baseCall(m));
    return baseCall(a) == m;
}

AprsMessageLog::Added AprsMessageLog::add(double t, const std::string& from, const aprs::Info& a) {
    if (a.type != "Message" || a.name.empty()) return Added::No;
    const std::string to = trim(a.name);
    if (a.isAck || a.isRej) {
        if (a.msgNo.empty()) return Added::No;
        // the ack goes from the addressee back to the sender: the message is the one from `to` to `from` with this number
        for (auto it = list_.rbegin(); it != list_.rend(); ++it) {
            if (it->kind != AprsMsgKind::Message || it->msgNo != a.msgNo) continue;
            if (!sameCall(it->from, to) || !sameCall(it->to, from)) continue;
            if (a.isAck) it->acked = true; else it->rejected = true;
            return Added::Ack;
        }
        return Added::No;
    }
    // telemetry definitions are messages a station sends to itself (APRS101 chapter 13): not conversation
    static const char* const kTel[] = {"PARM.", "UNIT.", "EQNS.", "BITS."};
    for (const char* p : kTel) if (a.text.compare(0, 5, p) == 0) return Added::No;
    const AprsMsgKind kind = aprsAddresseeKind(to);
    const double window = kind == AprsMsgKind::Message ? 3600.0 : 12 * 3600.0;   // bulletins are sent again for hours
    for (auto it = list_.rbegin(); it != list_.rend(); ++it) {
        if (t - it->lastSec > window) continue;
        if (!sameCall(it->from, from) || !sameCall(it->to, to) || it->msgNo != a.msgNo || it->text != a.text) continue;
        it->copies++;
        it->lastSec = std::max(it->lastSec, t);
        last_ = (int)(list_.rend() - it) - 1;
        return Added::Again;
    }
    AprsMessage m;
    m.timeSec = m.lastSec = t;
    m.from = from; m.to = to; m.text = a.text; m.msgNo = a.msgNo; m.kind = kind;
    m.toMe = kind == AprsMsgKind::Message && aprsIsForCall(to, my_);
    m.fromMe = aprsIsForCall(from, my_);
    list_.push_back(std::move(m));
    if (list_.size() > max_) list_.erase(list_.begin(), list_.begin() + (long)(list_.size() - max_));
    last_ = (int)list_.size() - 1;
    return Added::New;
}

void AprsMessageLog::setMyCall(const std::string& call) {
    my_ = call;
    for (auto& m : list_) { m.toMe = m.kind == AprsMsgKind::Message && aprsIsForCall(m.to, my_); m.fromMe = aprsIsForCall(m.from, my_); }
}

void AprsMessageLog::shiftTimes(double dt) {
    for (auto& m : list_) { m.timeSec += dt; m.lastSec += dt; }
}

std::string aprsThreadKey(const AprsMessage& m) {
    if (m.kind != AprsMsgKind::Message) return upper(m.to) + " from " + upper(m.from);
    std::string a = upper(m.from), b = upper(m.to);
    if (b < a) std::swap(a, b);
    return a + " <> " + b;
}

} // namespace dect2
