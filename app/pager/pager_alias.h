// Pagers: capcode names (#5). A CSV maps capcodes (POCSAG RICs and FLEX capcodes, single or a range) to a name and a category, so the message list
// says "Fire Station 3" instead of 1234567. Entries are per protocol (POCSAG, FLEX or both). Pure code: the UI imports, edits and exports the table.
//
// File format (UTF-8, comma, semicolon or tab separated; a header line is optional and its names pick the columns):
//   capcode,name,category,protocol
//   1234567,Fire Station 3,Fire,POCSAG
//   1000-1099,Hospital pagers,Medical,           (a range; protocol empty or "any" = both)
// Without a header the columns are capcode, name, category, protocol in that order. Lines that start with # are comments.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {

enum PagerAliasProto { kAliasAny = 0, kAliasPocsag = 1, kAliasFlex = 2 };

struct PagerAlias {
    uint32_t first = 0, last = 0;        // one capcode (first == last) or a range
    int proto = kAliasAny;               // PagerAliasProto
    std::string name, category;
};

struct PagerAliasImport {
    int added = 0, replaced = 0, skipped = 0;   // rows taken, rows that replaced an entry with the same capcode and protocol, lines that could not be used
};

class PagerAliases {
public:
    const std::vector<PagerAlias>& rows() const { return rows_; }
    bool empty() const { return rows_.empty(); }
    void clear() { rows_.clear(); reindex(); dirty_ = true; }

    // Adds or replaces the entry with the same range and protocol. An empty name and category removes it.
    void set(const PagerAlias& a) {
        if (a.last < a.first || a.proto < 0 || a.proto > 2) return;
        const uint64_t k = ((uint64_t)a.first << 32) | a.last;
        auto it = exact_[a.proto].find(k);
        if (it != exact_[a.proto].end()) {
            if (a.name.empty() && a.category.empty()) { rows_.erase(rows_.begin() + (long)it->second); reindex(); }
            else rows_[it->second] = a;
            dirty_ = true;
            return;
        }
        if (a.name.empty() && a.category.empty()) return;
        if (rows_.size() >= kMaxRows) return;
        exact_[a.proto][k] = rows_.size();
        rows_.push_back(a);
        dirty_ = true;
    }
    bool has(uint32_t first, uint32_t last, int proto) const {
        return proto >= 0 && proto <= 2 && exact_[proto].count(((uint64_t)first << 32) | last) != 0;
    }

    // The entry for a capcode heard on POCSAG or FLEX: an entry for this protocol beats one for both, a single capcode beats a range, a narrower range beats a wider one.
    const PagerAlias* find(uint32_t addr, bool flex) const {
        if (rows_.empty()) return nullptr;
        if (dirty_) rebuild();
        const int mine = flex ? kAliasFlex : kAliasPocsag;
        const PagerAlias* best = nullptr;
        auto it = single_.find(key(addr, mine));
        if (it != single_.end()) return &rows_[it->second];
        it = single_.find(key(addr, kAliasAny));
        if (it != single_.end()) return &rows_[it->second];
        uint64_t bestSpan = ~0ull;
        for (size_t i : ranges_) {
            const PagerAlias& r = rows_[i];
            if ((r.proto != kAliasAny && r.proto != mine) || addr < r.first || addr > r.last) continue;
            const uint64_t span = (uint64_t)r.last - r.first;
            if (!best || span < bestSpan || (span == bestSpan && r.proto == mine && best->proto == kAliasAny)) { best = &r; bestSpan = span; }
        }
        return best;
    }
    std::string name(uint32_t addr, bool flex) const { const PagerAlias* a = find(addr, flex); return a ? a->name : std::string(); }

    // ---- CSV
    PagerAliasImport importCsv(const std::string& text) {
        PagerAliasImport res;
        std::vector<std::vector<std::string>> lines = parse(text);
        int cCap = 0, cName = 1, cCat = 2, cProto = 3;
        size_t start = 0;
        if (!lines.empty()) {   // a header: the first line has no capcode in its first column that reads as a number
            bool numeric = false;
            { uint32_t a, b; numeric = !lines[0].empty() && range(lines[0][0], a, b); }
            if (!numeric) {
                start = 1;
                cCap = cName = cCat = cProto = -1;
                for (size_t i = 0; i < lines[0].size(); i++) {
                    const std::string h = lower(trim(lines[0][i]));
                    if (h == "capcode" || h == "ric" || h == "address" || h == "id" || h == "cap" || h == "capcodes") cCap = (int)i;
                    else if (h == "name" || h == "alias" || h == "label" || h == "description" || h == "desc") cName = (int)i;
                    else if (h == "category" || h == "group" || h == "type" || h == "tag") cCat = (int)i;
                    else if (h == "protocol" || h == "mode" || h == "system") cProto = (int)i;
                }
                if (cCap < 0) { res.skipped = (int)lines.size(); return res; }       // nothing recognisable
            }
        }
        auto field = [](const std::vector<std::string>& l, int c) { return c >= 0 && (size_t)c < l.size() ? trim(l[(size_t)c]) : std::string(); };
        for (size_t i = start; i < lines.size(); i++) {
            const auto& l = lines[i];
            PagerAlias a;
            if (!range(field(l, cCap), a.first, a.last)) { res.skipped++; continue; }
            a.name = unguard(clip(field(l, cName)));
            a.category = unguard(clip(field(l, cCat)));
            const std::string p = lower(field(l, cProto));
            if (p == "pocsag" || p == "p") a.proto = kAliasPocsag;
            else if (p == "flex" || p == "f") a.proto = kAliasFlex;
            else if (p.empty() || p == "any" || p == "both" || p == "*") a.proto = kAliasAny;
            else { res.skipped++; continue; }
            if (a.name.empty() && a.category.empty()) { res.skipped++; continue; }
            const size_t before = rows_.size();
            const bool existed = has(a.first, a.last, a.proto);
            set(a);
            if (existed) res.replaced++; else if (rows_.size() > before) res.added++; else res.skipped++;
        }
        return res;
    }
    std::string exportCsv() const {
        std::string o = "capcode,name,category,protocol\n";
        for (const PagerAlias& a : rows_) {
            o += std::to_string(a.first);
            if (a.last != a.first) o += "-" + std::to_string(a.last);
            o += "," + quote(guard(a.name)) + "," + quote(guard(a.category)) + ",";
            o += a.proto == kAliasPocsag ? "POCSAG" : a.proto == kAliasFlex ? "FLEX" : "any";
            o += "\n";
        }
        return o;
    }

    static constexpr size_t kMaxRows = 200000;

private:
    static uint64_t key(uint32_t a, int proto) { return ((uint64_t)proto << 32) | a; }
    void reindex() { for (auto& m : exact_) m.clear(); for (size_t i = 0; i < rows_.size(); i++) exact_[rows_[i].proto][((uint64_t)rows_[i].first << 32) | rows_[i].last] = i; }
    void rebuild() const {
        single_.clear(); ranges_.clear();
        for (size_t i = 0; i < rows_.size(); i++) {
            if (rows_[i].first == rows_[i].last) single_[key(rows_[i].first, rows_[i].proto)] = i; else ranges_.push_back(i);
        }
        dirty_ = false;
    }
    static std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
        return s.substr(a, b - a);
    }
    static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
    static std::string clip(const std::string& s) { return s.size() > 120 ? s.substr(0, 120) : s; }
    // a spreadsheet would run a cell that starts with = + - @; the export prefixes such a cell with an apostrophe, the import takes it off again
    static std::string guard(const std::string& s) { return !s.empty() && (s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@') ? "'" + s : s; }
    static std::string unguard(const std::string& s) { return s.size() > 1 && s[0] == '\'' && (s[1] == '=' || s[1] == '+' || s[1] == '-' || s[1] == '@') ? s.substr(1) : s; }
    static std::string quote(const std::string& s) {
        if (s.find_first_of(",;\t\"\r\n") == std::string::npos) return s;
        std::string o = "\"";
        for (char c : s) { if (c == '"') o += '"'; o += c; }
        return o + "\"";
    }
    // "1234" or "1000-1099"
    static bool range(const std::string& s0, uint32_t& a, uint32_t& b) {
        const std::string s = trim(s0);
        size_t i = 0;
        auto num = [&](uint32_t& v) {
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
            uint64_t x = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') { x = x * 10 + (uint64_t)(s[i] - '0'); if (x > 0xFFFFFFFFull) return false; i++; }
            v = (uint32_t)x;
            return true;
        };
        if (!num(a)) return false;
        b = a;
        while (i < s.size() && s[i] == ' ') i++;
        if (i < s.size() && s[i] == '-') { i++; while (i < s.size() && s[i] == ' ') i++; if (!num(b)) return false; }
        while (i < s.size() && s[i] == ' ') i++;
        if (i != s.size()) return false;
        if (b < a) std::swap(a, b);
        return true;
    }
    // CSV records: quoted fields with "" escapes and newlines inside quotes; the delimiter (, ; or tab) is the commonest one in the first line
    static std::vector<std::vector<std::string>> parse(std::string t) {
        std::vector<std::vector<std::string>> out;
        if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) t.erase(0, 3);
        char delim = ',';
        {
            const size_t nl = t.find('\n');
            const std::string first = t.substr(0, nl);
            size_t c = (size_t)std::count(first.begin(), first.end(), ','), s = (size_t)std::count(first.begin(), first.end(), ';'), tb = (size_t)std::count(first.begin(), first.end(), '\t');
            if (s > c && s >= tb) delim = ';'; else if (tb > c && tb > s) delim = '\t';
        }
        std::vector<std::string> row;
        std::string cur;
        bool inq = false, any = false, quotedField = false;
        auto endField = [&] { row.push_back(cur); cur.clear(); quotedField = false; };
        auto endRow = [&] {
            endField();
            const bool blank = row.size() == 1 && trim(row[0]).empty();
            const bool comment = !row.empty() && !row[0].empty() && trim(row[0])[0] == '#';
            if (!blank && !comment && out.size() < PagerAliases::kMaxRows + 2) out.push_back(row);
            row.clear(); any = false;
        };
        for (size_t i = 0; i < t.size(); i++) {
            const char c = t[i];
            any = true;
            if (inq) {
                if (c == '"') { if (i + 1 < t.size() && t[i + 1] == '"') { cur += '"'; i++; } else inq = false; }
                else cur += c;
            } else if (c == '"' && cur.empty() && !quotedField) { inq = true; quotedField = true; }
            else if (c == delim) endField();
            else if (c == '\n') endRow();
            else if (c == '\r') { /* part of CRLF */ }
            else cur += c;
        }
        if (any) endRow();
        return out;
    }

    std::vector<PagerAlias> rows_;
    mutable std::unordered_map<uint64_t, size_t> single_;
    mutable std::vector<size_t> ranges_;
    std::unordered_map<uint64_t, size_t> exact_[3];
    mutable bool dirty_ = true;
};

}  // namespace dect2
