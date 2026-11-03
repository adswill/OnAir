#include "export.h"

namespace kit {

static int colOf(const resexp::Table& t, const char* a, const char* b) {
    for (size_t i = 0; i < t.cols.size(); i++) {
        std::string c;
        for (char ch : t.cols[i]) c += (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
        if (c == a || c == b) return (int)i;
    }
    return -1;
}

bool hasPositions(const resexp::Table& t) {
    const int la = colOf(t, "lat", "latitude"), lo = colOf(t, "lon", "longitude");
    if (la < 0 || lo < 0) return false;
    for (const auto& r : t.rows)
        if ((size_t)la < r.size() && (size_t)lo < r.size() && !r[la].empty() && !r[lo].empty()) return true;
    return false;
}

std::string clipboardCsv(const resexp::Table& t) {
    const std::string s = resexp::toCsv(t);
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        o += s[i];
    }
    return o;
}

}  // namespace kit
