// Worked-before and needed (4.0): see ham_needed.h.
#include "dect2/ham/ham_needed.h"
#include <algorithm>
#include <cctype>

namespace dect2::ham {

namespace {
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
bool hasDigit(const std::string& s) { return std::any_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; }); }
bool hasAlpha(const std::string& s) { return std::any_of(s.begin(), s.end(), [](char c) { return std::isalpha((unsigned char)c) != 0; }); }
// the prefix of a plain call: up to and including the last digit before the suffix letters; no digit: the first two letters and 0
std::string plainPrefix(const std::string& c) {
    size_t last = std::string::npos;
    for (size_t i = 0; i < c.size(); i++) if (std::isdigit((unsigned char)c[i])) last = i;
    if (last == std::string::npos) return c.size() >= 2 ? c.substr(0, 2) + "0" : "";
    if (last == 0) return "";   // "1ABC" is not a call
    return c.substr(0, last + 1);
}
std::string grid4(const std::string& g) { return g.size() >= 4 ? upper(g.substr(0, 4)) : ""; }
}  // namespace

std::string wpxPrefix(const std::string& call0) {
    const std::string call = upper(call0);
    std::vector<std::string> parts;
    std::string cur;
    for (char c : call) { if (c == '/') { parts.push_back(cur); cur.clear(); } else if (std::isalnum((unsigned char)c)) cur += c; else return ""; }
    parts.push_back(cur);
    static const char* kSuffix[] = {"P", "M", "MM", "AM", "QRP", "A", "LH", "R", "B"};
    std::vector<std::string> keep;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        bool sfx = false;
        for (const char* s : kSuffix) if (p == s) sfx = true;
        if (!sfx) keep.push_back(p);
    }
    if (keep.empty()) return "";
    // the call itself: the longest part with letters (a digit too when there is one)
    size_t bi = 0;
    for (size_t i = 1; i < keep.size(); i++) {
        const bool better = (hasDigit(keep[i]) && hasAlpha(keep[i]) && !(hasDigit(keep[bi]) && hasAlpha(keep[bi]))) || keep[i].size() > keep[bi].size();
        if (better) bi = i;
    }
    const std::string base = keep[bi];
    if (!hasAlpha(base) || base.size() < 3) return "";
    const std::string bp = plainPrefix(base);
    if (bp.empty()) return "";
    if (keep.size() == 1) return bp;
    const std::string other = keep[bi == 0 ? 1 : 0];
    if (other.size() == 1 && std::isdigit((unsigned char)other[0])) {   // K1ABC/4: the call area changes
        std::string letters = bp;
        while (!letters.empty() && std::isdigit((unsigned char)letters.back())) letters.pop_back();
        return letters + other;
    }
    if (!hasAlpha(other)) return bp;
    return std::isdigit((unsigned char)other.back()) ? other : other + "0";   // PJ4/K1ABC -> PJ4, DL/LY1ABC -> DL0
}

void NeedIndex::build(const std::vector<LogQso>& log) {
    *this = NeedIndex();
    for (const auto& q : log) add(q);
}
void NeedIndex::add(const LogQso& q) {
    const std::string c = baseCall(q.call);
    if (c.empty()) return;
    const std::string band = q.band, mode = upper(q.mode);
    call_.insert(c);
    callBand_.insert(c + "|" + band);
    callSlot_.insert(c + "|" + band + "|" + mode);
    const std::string dk = awardKey(q, Award::Dxcc);
    if (!dk.empty()) {
        dxcc_.insert(dk);
        dxccBand_.insert(dk + "|" + band);
        dxccClass_.insert(dk + "|" + modeClass(mode));
        if (q.confirmed()) dxccConf_.insert(dk);
    }
    if (q.cqz > 0) cqz_.insert(std::to_string(q.cqz));
    if (q.ituz > 0) ituz_.insert(std::to_string(q.ituz));
    const std::string st = awardKey(q, Award::Was);   // US states only (WAS)
    if (!st.empty()) state_.insert(st);
    const std::string g = grid4(q.grid);
    if (!g.empty()) { grid_.insert(g); gridBand_.insert(g + "|" + band); }
    const std::string p = wpxPrefix(q.call);
    if (!p.empty()) prefix_.insert(p);
}

NeedResult NeedIndex::check(const NeedQuery& q) const {
    NeedResult r;
    const std::string c = baseCall(q.call);
    if (c.empty()) return r;
    const std::string band = q.band, mode = upper(q.mode);
    if (!call_.count(c)) r.bits |= NeedCall;
    if (!callBand_.count(c + "|" + band)) r.bits |= NeedCallBand;
    if (!callSlot_.count(c + "|" + band + "|" + mode)) r.bits |= NeedCallSlot;
    if (!q.dxccKey.empty()) {
        if (!dxcc_.count(q.dxccKey)) r.bits |= NeedDxcc;
        else if (!dxccConf_.count(q.dxccKey)) r.bits |= NeedDxccQsl;
        if (!band.empty() && !dxccBand_.count(q.dxccKey + "|" + band)) r.bits |= NeedDxccBand;
        const std::string mc = modeClass(mode);
        if (!mc.empty() && !dxccClass_.count(q.dxccKey + "|" + mc)) r.bits |= NeedDxccClass;
    }
    if (q.cqz > 0 && !cqz_.count(std::to_string(q.cqz))) r.bits |= NeedCqz;
    if (q.ituz > 0 && !ituz_.count(std::to_string(q.ituz))) r.bits |= NeedItuz;
    if (!q.state.empty() && !state_.count(upper(q.state))) r.bits |= NeedState;
    const std::string g = grid4(q.grid);
    if (!g.empty()) {
        if (!grid_.count(g)) r.bits |= NeedGrid;
        if (!band.empty() && !gridBand_.count(g + "|" + band)) r.bits |= NeedGridBand;
    }
    const std::string p = wpxPrefix(q.call);
    if (!p.empty() && !prefix_.count(p)) r.bits |= NeedPrefix;

    if (r.has(NeedDxcc)) { r.level = 6; r.label = "NEW DXCC"; }
    else if (r.has(NeedDxccBand)) { r.level = 5; r.label = "new band"; }
    else if (r.has(NeedDxccClass)) { r.level = 4; r.label = "new mode"; }
    else if (r.has(NeedCqz) || r.has(NeedItuz)) { r.level = 3; r.label = "new zone"; }
    else if (r.has(NeedState)) { r.level = 3; r.label = "new state"; }
    else if (r.has(NeedGrid)) { r.level = 2; r.label = "new grid"; }
    else if (r.has(NeedPrefix)) { r.level = 2; r.label = "new prefix"; }
    else if (r.has(NeedCall)) { r.level = 1; r.label = "new call"; }
    else if (r.has(NeedCallSlot)) { r.level = 1; r.label = "new slot"; }
    else { r.level = 0; r.label = "B4"; }
    return r;
}

bool wildMatch(const std::string& pat0, const std::string& txt0) {
    const std::string p = upper(pat0), t = upper(txt0);
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) { pi++; ti++; }
        else if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ti; }
        else if (star != std::string::npos) { pi = star + 1; ti = ++mark; }
        else return false;
    }
    while (pi < p.size() && p[pi] == '*') pi++;
    return pi == p.size();
}

bool NeedAlerter::check(const NeedQuery& q, const NeedResult& r, double now, std::string& why) {
    why.clear();
    const std::string c = upper(q.call);
    if (c.empty()) return false;
    for (const auto& w : rules.watch) if (!w.empty() && wildMatch(w, c)) { why = "Watched call: " + c; break; }
    if (why.empty()) {
        if (rules.newDxcc && r.has(NeedDxcc)) why = "New DXCC: " + c;
        else if (rules.newDxccBand && r.has(NeedDxccBand)) why = "DXCC new on " + q.band + ": " + c;
        else if (rules.newDxccClass && r.has(NeedDxccClass)) why = "DXCC new in " + modeClass(q.mode) + ": " + c;
        else if (rules.newZone && r.has(NeedCqz)) why = "New CQ zone " + std::to_string(q.cqz) + ": " + c;
        else if (rules.newZone && r.has(NeedItuz)) why = "New ITU zone " + std::to_string(q.ituz) + ": " + c;
        else if (rules.newState && r.has(NeedState)) why = "New state " + upper(q.state) + ": " + c;
        else if (rules.newGrid && r.has(NeedGrid)) why = "New grid " + grid4(q.grid) + ": " + c;
        else if (rules.newPrefix && r.has(NeedPrefix)) why = "New prefix " + wpxPrefix(c) + ": " + c;
    }
    if (why.empty()) return false;
    const std::string key = c + "|" + q.band;
    const auto it = last_.find(key);
    if (it != last_.end() && now - it->second < rules.quietSec) { why.clear(); return false; }
    last_[key] = now;
    return true;
}

}  // namespace dect2::ham
