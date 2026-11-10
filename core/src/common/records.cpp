// Records board: see records.h.
#include "dect2/common/records.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {

namespace {
std::string clean(std::string s) { for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }
}

bool RecordsBoard::offer(const std::string& cat, const std::string& band, double km, const std::string& id, const std::string& detail, int64_t utc) {
    if (cat.empty() || !std::isfinite(km) || km <= 0 || km > 20100) return false;   // half the earth's circumference at most
    Record& x = r_[cat + "|" + band];
    if (!x.category.empty() && km < x.km + 0.1) return false;
    x.category = cat; x.band = band; x.km = km; x.id = clean(id); x.detail = clean(detail); x.utc = utc;
    dirty_ = true;
    return true;
}

std::vector<Record> RecordsBoard::all() const { std::vector<Record> v; for (const auto& kv : r_) v.push_back(kv.second); return v; }
const Record* RecordsBoard::get(const std::string& c, const std::string& b) const { auto it = r_.find(c + "|" + b); return it == r_.end() ? nullptr : &it->second; }
void RecordsBoard::clear(const std::string& c) {
    for (auto it = r_.begin(); it != r_.end();) { if (c.empty() || it->second.category == c) it = r_.erase(it); else ++it; }
    dirty_ = true;
}

std::string RecordsBoard::serialize() const {
    std::ostringstream o;
    for (const auto& kv : r_) {
        const Record& x = kv.second;
        char km[32]; std::snprintf(km, sizeof km, "%.3f", x.km);
        o << clean(x.category) << '\t' << clean(x.band) << '\t' << km << '\t' << x.id << '\t' << x.detail << '\t' << x.utc << '\n';
    }
    return o.str();
}

void RecordsBoard::parse(const std::string& text) {
    r_.clear();
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) {
        std::vector<std::string> f;
        size_t a = 0;
        for (;;) { const size_t b = l.find('\t', a); f.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
        if (f.size() < 6 || f[0].empty()) continue;
        Record x; x.category = f[0]; x.band = f[1]; x.km = std::atof(f[2].c_str()); x.id = f[3]; x.detail = f[4]; x.utc = std::atoll(f[5].c_str());
        if (x.km > 0) r_[x.category + "|" + x.band] = x;
    }
    dirty_ = false;
}

}  // namespace dect2
