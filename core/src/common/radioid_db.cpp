#include "dect2/common/radioid_db.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace dect2 {
namespace {

std::string norm(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += (char)std::tolower(c);
    return o;
}

// one record split into fields; quotes ("" is a quote); returns false at the end of text
bool nextRecord(const std::string& t, size_t& p, char sep, std::vector<std::string>& f) {
    f.clear();
    if (p >= t.size()) return false;
    std::string cur; bool q = false, any = false;
    while (p < t.size()) {
        const char c = t[p++];
        if (q) {
            if (c == '"') { if (p < t.size() && t[p] == '"') { cur += '"'; p++; } else q = false; }
            else cur += c;
        } else if (c == '"') { q = true; any = true; }
        else if (c == sep) { f.push_back(cur); cur.clear(); any = true; }
        else if (c == '\n') break;
        else if (c == '\r') {}
        else { cur += c; any = true; }
    }
    if (any || !cur.empty()) f.push_back(cur);
    return true;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool allDigits(const std::string& s) {
    if (s.empty() || s.size() > 9) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    return true;
}

}  // namespace

size_t RadioIdDb::parse(const std::string& csvIn) {
    clear();
    std::string csv = csvIn;
    if (csv.size() >= 3 && (unsigned char)csv[0] == 0xEF && (unsigned char)csv[1] == 0xBB && (unsigned char)csv[2] == 0xBF) csv.erase(0, 3);
    // the separator: the most frequent one in the first line
    const size_t nl = csv.find('\n');
    const std::string first = csv.substr(0, nl);
    char sep = ',';
    size_t best = 0;
    for (char c : {',', ';', '\t'}) { const size_t n = (size_t)std::count(first.begin(), first.end(), c); if (n > best) { best = n; sep = c; } }

    int cId = 0, cCall = 1, cFirst = 2, cLast = 3, cCity = 4, cState = 5, cCountry = 6;
    size_t p = 0;
    std::vector<std::string> f;
    bool firstRow = true;
    struct Rec { uint32_t id; std::string s; };
    std::vector<Rec> recs;
    while (nextRecord(csv, p, sep, f)) {
        if (f.empty()) continue;
        if (firstRow) {
            firstRow = false;
            if (!allDigits(trim(f[0]))) {   // a header
                cId = cCall = cFirst = cLast = cCity = cState = cCountry = -1;
                for (size_t i = 0; i < f.size(); i++) {
                    const std::string n = norm(f[i]);
                    if (n == "radioid" || n == "id" || n == "dmrid" || n == "nxdnid" || n == "userid" || n == "unitid" || n == "dmrradioid") cId = (int)i;
                    else if (n == "callsign" || n == "call") cCall = (int)i;
                    else if (n == "firstname" || n == "name" || n == "fname" || n == "alias") cFirst = (int)i;
                    else if (n == "lastname" || n == "surname" || n == "lname") cLast = (int)i;
                    else if (n == "city" || n == "town") cCity = (int)i;
                    else if (n == "state" || n == "region" || n == "province") cState = (int)i;
                    else if (n == "country") cCountry = (int)i;
                }
                if (cId < 0) { skipped_++; return 0; }   // no id column: not a radio id list
                continue;
            }
        }
        auto get = [&](int c) { return c >= 0 && (size_t)c < f.size() ? trim(f[(size_t)c]) : std::string(); };
        const std::string ids = get(cId);
        if (!allDigits(ids)) { skipped_++; continue; }
        const uint32_t id = (uint32_t)std::strtoul(ids.c_str(), nullptr, 10);
        if (id == 0) { skipped_++; continue; }
        std::string name = get(cFirst), last = get(cLast);
        if (!last.empty()) name += (name.empty() ? "" : " ") + last;
        std::string place;
        for (const std::string& s : {get(cCity), get(cState), get(cCountry)}) if (!s.empty()) place += (place.empty() ? "" : ", ") + s;
        std::string call = get(cCall);
        if (call.empty() && name.empty()) { skipped_++; continue; }
        for (std::string* s : {&call, &name, &place}) for (char& c : *s) if (c == '\x1f' || c == '\0') c = ' ';
        recs.push_back({id, call + '\x1f' + name + '\x1f' + place});
    }
    std::stable_sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) { return a.id < b.id; });
    for (size_t i = 0; i < recs.size(); i++) {
        if (i + 1 < recs.size() && recs[i + 1].id == recs[i].id) continue;   // a repeated id: the later row wins
        ids_.push_back(recs[i].id);
        offs_.push_back((uint32_t)pool_.size());
        pool_ += recs[i].s; pool_ += '\0';
    }
    ids_.shrink_to_fit(); offs_.shrink_to_fit(); pool_.shrink_to_fit();
    return ids_.size();
}

bool RadioIdDb::loadFile(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open " + path; return false; }
    std::ostringstream ss; ss << in.rdbuf();
    if (parse(ss.str()) == 0) { err = "no radio ids found in the file (it needs a column with the radio id, usually the first)"; return false; }
    return true;
}

bool RadioIdDb::find(uint32_t id, Entry& out) const {
    auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
    if (it == ids_.end() || *it != id) return false;
    const char* r = pool_.c_str() + offs_[(size_t)(it - ids_.begin())];
    const std::string s(r);
    const size_t a = s.find('\x1f'), b = s.find('\x1f', a + 1);
    out.callsign = s.substr(0, a);
    out.name = s.substr(a + 1, b - a - 1);
    out.place = s.substr(b + 1);
    return true;
}

std::string RadioIdDb::label(uint32_t id) const {
    Entry e;
    if (!find(id, e)) return std::string();
    return e.callsign.empty() ? e.name : e.name.empty() ? e.callsign : e.callsign + " " + e.name;
}

namespace {
struct Slot { std::mutex mu; std::shared_ptr<RadioIdDb> db; };
Slot& slot(RadioIdKind k) { static Slot s[2]; return s[k == RadioIdKind::Nxdn ? 1 : 0]; }
}

std::string radioIdLabel(RadioIdKind k, uint32_t id) {
    std::shared_ptr<RadioIdDb> d;
    { Slot& s = slot(k); std::lock_guard<std::mutex> lk(s.mu); d = s.db; }
    return d ? d->label(id) : std::string();
}
void radioIdInstall(RadioIdKind k, std::shared_ptr<RadioIdDb> db) { Slot& s = slot(k); std::lock_guard<std::mutex> lk(s.mu); s.db = std::move(db); }
size_t radioIdCount(RadioIdKind k) { Slot& s = slot(k); std::lock_guard<std::mutex> lk(s.mu); return s.db ? s.db->size() : 0; }
const char* radioIdUrl(RadioIdKind k) { return k == RadioIdKind::Nxdn ? "https://radioid.net/static/nxdn.csv" : "https://radioid.net/static/user.csv"; }

}  // namespace dect2
