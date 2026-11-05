#include "rec_tags.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace rectags {

static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::vector<std::string> parseTags(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        std::string t = lower(trim(cur));
        if (!t.empty() && t[0] == '#') t = trim(t.substr(1));
        cur.clear();
        if (!t.empty() && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
    };
    for (char c : text) { if (c == ',' || c == ';' || c == '\n' || c == '\t') flush(); else cur += c; }
    flush();
    return out;
}

std::string joinTags(const std::vector<std::string>& tags) {
    std::string s;
    for (size_t i = 0; i < tags.size(); i++) { if (i) s += ", "; s += tags[i]; }
    return s;
}

const char* fileName() { return ".onair-tags.tsv"; }

// notes may hold tabs and line breaks: \t \n \\ escapes
static std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '\\') o += "\\\\"; else if (c == '\t') o += "\\t"; else if (c == '\n') o += "\\n"; else if (c == '\r') {} else o += c; }
    return o;
}
static std::string unesc(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) { char n = s[++i]; o += n == 't' ? '\t' : n == 'n' ? '\n' : n; }
        else o += s[i];
    }
    return o;
}

bool load(const std::string& dir, Store& out) {
    out.clear();
    std::ifstream f(std::filesystem::path(dir) / fileName());
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t a = line.find('\t');
        if (a == std::string::npos) continue;
        const size_t b = line.find('\t', a + 1);
        Entry e;
        e.tags = parseTags(b == std::string::npos ? line.substr(a + 1) : line.substr(a + 1, b - a - 1));
        if (b != std::string::npos) e.note = unesc(line.substr(b + 1));
        const std::string name = unesc(line.substr(0, a));
        if (!name.empty() && (!e.tags.empty() || !e.note.empty())) out[name] = e;
    }
    return true;
}

bool save(const std::string& dir, const Store& s) {
    const auto path = std::filesystem::path(dir) / fileName();
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# OnAir recording tags and notes: file name, tags, note (tab separated)\n";
    for (const auto& kv : s) {
        if (kv.second.tags.empty() && kv.second.note.empty()) continue;
        f << esc(kv.first) << '\t' << joinTags(kv.second.tags) << '\t' << esc(kv.second.note) << '\n';
    }
    return (bool)f;
}

void set(Store& s, const std::string& file, const std::string& tagsText, const std::string& note) {
    Entry e; e.tags = parseTags(tagsText); e.note = note;
    if (e.tags.empty() && trim(e.note).empty()) s.erase(file); else s[file] = e;
}

bool matches(const std::string& file, const Entry* e, const std::string& query) {
    std::istringstream words(lower(query));
    std::string w;
    const std::string lf = lower(file), ln = e ? lower(e->note) : std::string();
    while (words >> w) {
        if (w[0] == '#') {
            const std::string t = w.substr(1);
            if (t.empty()) continue;
            if (!e || std::find(e->tags.begin(), e->tags.end(), t) == e->tags.end()) return false;
            continue;
        }
        bool ok = lf.find(w) != std::string::npos || ln.find(w) != std::string::npos;
        if (!ok && e) for (const auto& t : e->tags) if (t.find(w) != std::string::npos) { ok = true; break; }
        if (!ok) return false;
    }
    return true;
}

std::vector<std::pair<std::string, int>> tagCounts(const Store& s) {
    std::map<std::string, int> m;
    for (const auto& kv : s) for (const auto& t : kv.second.tags) m[t]++;
    std::vector<std::pair<std::string, int>> v(m.begin(), m.end());
    std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    return v;
}

}  // namespace rectags
