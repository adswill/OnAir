// Channel plan and access rules of the trunk pool: parsing, lookup and CSV (see trunkpool.h).
#include "dect2/trunkpool/trunkpool.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {
namespace tpool {

const char* protoName(Proto p) {
    switch (p) {
    case Proto::P25p1: return "P25 Phase 1";
    case Proto::P25p2: return "P25 Phase 2";
    case Proto::DmrTier3: return "DMR Tier III";
    case Proto::DmrCapPlus: return "DMR Capacity Plus";
    case Proto::DmrConnectPlus: return "DMR Connect Plus";
    case Proto::Tetra: return "TETRA";
    case Proto::Mpt1327: return "MPT 1327";
    case Proto::Edacs: return "EDACS";
    case Proto::Ltr: return "LTR";
    default: return "other";
    }
}
bool protoDigitalVoice(Proto p) { return p != Proto::Mpt1327 && p != Proto::Ltr && p != Proto::Other; }   // EDACS: the analog calls are followed as carriers; both need the radio only

namespace {

std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
// Quoted fields, "" escapes; the line is one record
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (q) {
            if (c == '"') { if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; } else q = false; }
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',' || c == '\t' || c == ';') { out.push_back(trim(cur)); cur.clear(); }
        else cur += c;
    }
    out.push_back(trim(cur));
    return out;
}
std::vector<std::string> lines(const std::string& t) {
    std::vector<std::string> v;
    std::string cur;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\r' || t[i] == '\n') { if (t[i] == '\r' && i + 1 < t.size() && t[i + 1] == '\n') i++; v.push_back(cur); cur.clear(); }
        else cur += t[i];
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}
bool num(const std::string& s, double& v) {
    if (s.empty() || s.size() > 24) return false;
    char* e = nullptr;
    v = std::strtod(s.c_str(), &e);
    return e && *e == 0 && std::isfinite(v);
}
bool integer(const std::string& s, long long& v) {   // decimal, or 0x / trailing h hex
    if (s.empty() || s.size() > 20) return false;
    char* e = nullptr;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = std::strtoll(s.c_str() + 2, &e, 16);
    else v = std::strtoll(s.c_str(), &e, 10);
    return e && *e == 0;
}
// A frequency written as Hz (>= 100000) or MHz (below)
bool freqHz(const std::string& s, double& hz) {
    double v;
    if (!num(s, v) || v <= 0) return false;
    hz = v >= 1e5 ? v : v * 1e6;
    return hz >= 1e5 && hz <= 1e11;
}
// A spacing / bandwidth written as Hz (>= 1000) or kHz (below)
bool stepHz(const std::string& s, double& hz) {
    double v;
    if (!num(s, v) || v <= 0) return false;
    hz = v >= 1000 ? v : v * 1e3;
    return hz >= 100 && hz <= 1e7;
}
// A signed offset written as Hz (|v| >= 1000) or MHz
bool offsetHz(const std::string& s, double& hz) {
    double v;
    if (!num(s, v)) return false;
    hz = std::fabs(v) >= 1000 ? v : v * 1e6;
    return std::fabs(hz) <= 1e10;
}
void addErr(std::string* err, int* skipped, int line, const char* why) {
    if (skipped) (*skipped)++;
    if (err && err->size() < 300) *err += "line " + std::to_string(line) + ": " + why + "; ";
}
int col(const std::vector<std::string>& h, std::initializer_list<const char*> names) {
    for (size_t i = 0; i < h.size(); i++) for (const char* n : names) if (lower(h[i]) == n) return (int)i;
    return -1;
}

} // namespace

// ------------------------------------------------------------------------------------------------ channel plan

void ChannelPlan::setIden(const IdenRow& r) { iden_[r.id & 15] = r; iden_[r.id & 15].id = r.id & 15; }
const IdenRow* ChannelPlan::iden(int id) const { auto i = iden_.find(id & 15); return i == iden_.end() ? nullptr : &i->second; }
double ChannelPlan::p25Freq(int channel) const {
    if (channel < 0) return 0;
    const IdenRow* r = iden((channel >> 12) & 15);
    if (!r || !(r->spacingHz > 0) || !(r->baseHz > 0)) return 0;
    const int n = channel & 0xFFF;
    const double f = r->baseHz + (double)(r->slots > 1 ? n / r->slots : n) * r->spacingHz;   // a TDMA channel number is carrier x slots + slot
    return f > 0 ? f : 0;
}
void ChannelPlan::setChannel(int number, double hz) { if (hz > 0) chan_[number] = hz; else chan_.erase(number); }
double ChannelPlan::channelHz(int number) const { auto i = chan_.find(number); return i == chan_.end() ? 0 : i->second; }
void ChannelPlan::clear() { iden_.clear(); chan_.clear(); name.clear(); wacn = sysId = nac = -1; }

int ChannelPlan::importCsv(const std::string& text, std::string* err, int* skipped) {
    int taken = 0, lineNo = 0;
    int cChan = -1, cFreq = -1;   // a table with a header row
    for (const std::string& raw : lines(text)) {
        lineNo++;
        const std::string ln = trim(raw);
        if (ln.empty() || ln[0] == '#') continue;
        const std::vector<std::string> f = splitCsv(ln);
        const std::string k = lower(f[0]);
        if (k == "system") {
            long long w = -1, s = -1, n = -1;
            if (f.size() < 2) { addErr(err, skipped, lineNo, "system row needs a name"); continue; }
            name = f[1];
            if (f.size() > 2 && !f[2].empty() && !integer(f[2], w)) { addErr(err, skipped, lineNo, "bad WACN"); continue; }
            if (f.size() > 3 && !f[3].empty() && !integer(f[3], s)) { addErr(err, skipped, lineNo, "bad system id"); continue; }
            if (f.size() > 4 && !f[4].empty() && !integer(f[4], n)) { addErr(err, skipped, lineNo, "bad NAC"); continue; }
            wacn = (int)w; sysId = (int)s; nac = (int)n;
            taken++;
        } else if (cChan < 0 && (col(f, {"channel", "lcn", "number", "chan"}) >= 0) && (col(f, {"frequency", "freq", "mhz", "hz"}) >= 0)) {
            cChan = col(f, {"channel", "lcn", "number", "chan"});
            cFreq = col(f, {"frequency", "freq", "mhz", "hz"});
        } else if (k == "iden" || k == "iden_up") {
            long long id;
            IdenRow r;
            if (f.size() < 5 || !integer(f[1], id) || id < 0 || id > 15 || !freqHz(f[2], r.baseHz) || !stepHz(f[3], r.spacingHz) || !offsetHz(f[4], r.txOffsetHz)) { addErr(err, skipped, lineNo, "bad iden row"); continue; }
            if (f.size() > 5 && !f[5].empty() && !stepHz(f[5], r.bwHz)) { addErr(err, skipped, lineNo, "bad bandwidth"); continue; }
            long long sl = 1;
            if (f.size() > 6 && !f[6].empty() && (!integer(f[6], sl) || sl < 1 || sl > 8)) { addErr(err, skipped, lineNo, "bad slot count"); continue; }
            r.id = (int)id; r.slots = (int)sl;
            setIden(r);
            taken++;
        } else if (k == "chan" || k == "channel" || k == "lcn") {
            long long n; double hz;
            if (f.size() < 3 || !integer(f[1], n) || n < 0 || n > 0xFFFFF || !freqHz(f[2], hz)) { addErr(err, skipped, lineNo, "bad channel row"); continue; }
            setChannel((int)n, hz);
            taken++;
        } else if (cChan >= 0 && (size_t)std::max(cChan, cFreq) < f.size()) {
            long long n; double hz;
            if (!integer(f[cChan], n) || n < 0 || n > 0xFFFFF || !freqHz(f[cFreq], hz)) { addErr(err, skipped, lineNo, "bad table row"); continue; }
            setChannel((int)n, hz);
            taken++;
        } else addErr(err, skipped, lineNo, "unknown row");
    }
    return taken;
}

std::string ChannelPlan::exportCsv() const {
    std::string o = "# OnAir trunk channel plan\n";
    char b[160];
    if (!name.empty() || wacn >= 0 || sysId >= 0) {
        snprintf(b, sizeof b, "system,%s,0x%X,%d,%d\n", name.c_str(), wacn < 0 ? 0 : wacn, sysId, nac);
        o += b;
    }
    for (const auto& i : iden_) {
        const IdenRow& r = i.second;
        snprintf(b, sizeof b, "iden,%d,%.0f,%.0f,%.0f,%.0f,%d\n", r.id, r.baseHz, r.spacingHz, r.txOffsetHz, r.bwHz, r.slots);
        o += b;
    }
    for (const auto& c : chan_) { snprintf(b, sizeof b, "chan,%d,%.0f\n", c.first, c.second); o += b; }
    return o;
}

// ------------------------------------------------------------------------------------------------ access list

bool AccessList::remove(size_t i) { if (i >= rules_.size()) return false; rules_.erase(rules_.begin() + (long)i); return true; }

AccessDecision AccessList::decide(int tg, int src) const {
    AccessDecision d;
    d.allow = allowByDefault;
    d.reason = allowByDefault ? "" : "not on the allow list";
    bool matched = false;
    for (const AccessRule& r : rules_) {
        if (r.kind == AccessRule::Unit) {
            if (src >= r.lo && src <= r.hi && r.act == AccessRule::Deny) { d.allow = false; d.priority = 0; d.reason = r.label.empty() ? "unit denied" : "unit denied: " + r.label; return d; }
        }
    }
    for (const AccessRule& r : rules_) {   // later rules win
        if (r.kind != AccessRule::Talkgroup || tg < r.lo || tg > r.hi) continue;
        matched = true;
        d.allow = r.act == AccessRule::Allow;
        d.priority = d.allow ? r.priority : 0;
        d.record = r.record;
        d.reason = d.allow ? "" : (r.label.empty() ? "talkgroup denied" : "talkgroup denied: " + r.label);
    }
    (void)matched;
    return d;
}

int AccessList::importCsv(const std::string& text, std::string* err, int* skipped) {
    int taken = 0, lineNo = 0;
    int cKind = -1, cId = -1, cFrom = -1, cTo = -1, cAct = -1, cPri = -1, cRec = -1, cLab = -1, cMode = -1;
    bool header = false;
    for (const std::string& raw : lines(text)) {
        lineNo++;
        const std::string ln = trim(raw);
        if (ln.empty() || ln[0] == '#') continue;
        const std::vector<std::string> f = splitCsv(ln);
        if (!header) {   // the first row names the columns
            header = true;
            cKind = col(f, {"kind", "type"});
            cId = col(f, {"id", "decimal", "dec", "tgid", "talkgroup"});
            cFrom = col(f, {"from", "low"});
            cTo = col(f, {"to", "high"});
            cAct = col(f, {"action", "act"});
            cPri = col(f, {"priority", "pri"});
            cRec = col(f, {"record", "rec"});
            cLab = col(f, {"label", "alpha tag", "name", "alpha"});
            cMode = col(f, {"mode"});
            if (cId >= 0 || cFrom >= 0) continue;
            addErr(err, skipped, lineNo, "no id column in the header row");
            return taken;
        }
        auto cell = [&](int c) -> std::string { return c >= 0 && (size_t)c < f.size() ? f[(size_t)c] : std::string(); };
        long long lo, hi;
        if (cFrom >= 0) { if (!integer(cell(cFrom), lo)) { addErr(err, skipped, lineNo, "bad id"); continue; } hi = lo; if (cTo >= 0 && !cell(cTo).empty() && !integer(cell(cTo), hi)) { addErr(err, skipped, lineNo, "bad range end"); continue; } }
        else if (!integer(cell(cId), lo)) { addErr(err, skipped, lineNo, "bad id"); continue; }
        else hi = lo;
        if (lo < 0 || hi < lo || hi > 0xFFFFFF) { addErr(err, skipped, lineNo, "id out of range"); continue; }
        AccessRule r;
        r.lo = (int)lo; r.hi = (int)hi;
        const std::string kind = lower(cell(cKind));
        r.kind = (kind == "unit" || kind == "radio" || kind == "id") ? AccessRule::Unit : AccessRule::Talkgroup;
        const std::string act = lower(cell(cAct));
        if (!act.empty() && act != "allow" && act != "deny") { addErr(err, skipped, lineNo, "action is allow or deny"); continue; }
        r.act = act == "deny" ? AccessRule::Deny : AccessRule::Allow;
        const std::string mode = cell(cMode);
        if (!mode.empty() && mode.find('E') != std::string::npos && act.empty()) r.act = AccessRule::Deny;   // RadioReference: an encrypted talkgroup
        long long pr = 0;
        if (!cell(cPri).empty() && (!integer(cell(cPri), pr) || pr < 0 || pr > 10)) { addErr(err, skipped, lineNo, "priority is 0 to 10"); continue; }
        r.priority = (int)pr;
        const std::string rec = lower(cell(cRec));
        r.record = !(rec == "0" || rec == "no" || rec == "false" || rec == "off");
        r.label = cell(cLab);
        add(r);
        taken++;
    }
    return taken;
}

std::string AccessList::exportCsv() const {
    std::string o = "Kind,From,To,Action,Priority,Record,Label\n";
    for (const AccessRule& r : rules_) {
        std::string lab = r.label;
        std::string q;
        for (char c : lab) { if (c == '"') q += "\"\""; else q += c; }
        o += std::string(r.kind == AccessRule::Unit ? "Unit" : "TG") + "," + std::to_string(r.lo) + "," + std::to_string(r.hi) + "," + (r.act == AccessRule::Allow ? "Allow" : "Deny") + "," +
             std::to_string(r.priority) + "," + (r.record ? "yes" : "no") + ",\"" + q + "\"\n";
    }
    return o;
}

} // namespace tpool
} // namespace dect2
