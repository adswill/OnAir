// Callsigns and CQ calls in decoded text (4.0): for CW, PSK31, RTTY, Olivia, MFSK, JS8 free text. Pure code.
//  - hamFindCallsigns(): every amateur callsign in a text line (prefix 1-2 letters/digit, one digit, 1-4 suffix letters, optional /P /M /QRP
//    or a prefix like DL/), in the order they appear. "DE" is read as the separator, so "CQ CQ DE LY1ABC LY1ABC K" gives LY1ABC.
//  - hamParseCall(): what the line is: a CQ call (who is calling, "CQ DX", "CQ TEST"), a reply (to, from), or nothing; the signal report
//    (599, 5NN, 579 ...) when one is sent.
//  - HamHeard: the stations heard on the decoder with their last time, count and last text, so the user clicks one to start the log entry.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

inline bool hamLooksLikeCall(const std::string& w) {
    // strip a /suffix or prefix/
    std::string c = w;
    for (int pass = 0; pass < 2; pass++) {
        const size_t s = c.find('/');
        if (s == std::string::npos) break;
        const std::string a = c.substr(0, s), b = c.substr(s + 1);
        // keep the longer (the real call), unless the shorter looks like a prefix: both are handled by testing the longer one
        c = a.size() >= b.size() ? a : b;
    }
    if (c.size() < 3 || c.size() > 7) return false;
    for (char ch : c) if (!std::isupper((unsigned char)ch) && !std::isdigit((unsigned char)ch)) return false;
    // find the last digit that is followed only by 1-4 letters, preceded by a prefix of 1-3 chars
    size_t dpos = std::string::npos;
    for (size_t i = c.size(); i-- > 0;) if (std::isdigit((unsigned char)c[i])) { dpos = i; break; }
    if (dpos == std::string::npos || dpos == 0) return false;
    const size_t suf = c.size() - dpos - 1;
    if (suf < 1 || suf > 4) return false;
    for (size_t i = dpos + 1; i < c.size(); i++) if (!std::isupper((unsigned char)c[i])) return false;
    if (dpos > 3) return false;
    bool hasLetter = false;
    for (size_t i = 0; i < dpos; i++) if (std::isupper((unsigned char)c[i])) hasLetter = true;
    // prefixes like "9A1", "4X6", "3DA0": a letter must be in the prefix, or the first char is a digit followed by a letter
    if (!hasLetter) return false;
    return true;
}

inline std::vector<std::string> hamFindCallsigns(const std::string& text) {
    std::vector<std::string> out; std::string cur;
    auto flush = [&]() { if (!cur.empty()) { if (cur != "DE" && hamLooksLikeCall(cur)) out.push_back(cur); cur.clear(); } };
    for (char ch : text) {
        const char u = (char)std::toupper((unsigned char)ch);
        if (std::isalnum((unsigned char)u) || u == '/') cur += u; else flush();
    }
    flush();
    return out;
}

struct HamCallLine {
    enum Kind { None, Cq, Reply } kind = None;
    std::string from, to, cqType;    // cqType: "", "DX", "TEST", "POTA", "SOTA" ...
    std::string report;              // "599", "5NN", ""
};
inline HamCallLine hamParseCall(const std::string& text) {
    HamCallLine r;
    std::vector<std::string> w; std::string cur;
    for (char ch : text) { const char u = (char)std::toupper((unsigned char)ch); if (std::isalnum((unsigned char)u) || u == '/') cur += u; else if (!cur.empty()) { w.push_back(cur); cur.clear(); } }
    if (!cur.empty()) w.push_back(cur);
    for (const auto& x : w) if (x.size() == 3 && (x == "5NN" || x == "599" || x == "579" || x == "589" || x == "559" || x == "569" || x == "449" || x == "579")) { if (r.report.empty()) r.report = x; }
    auto isCall = [](const std::string& s) { return s != "DE" && hamLooksLikeCall(s); };
    size_t cq = w.size();
    for (size_t i = 0; i < w.size(); i++) if (w[i] == "CQ") { cq = i; break; }
    if (cq < w.size()) {
        size_t i = cq;
        while (i < w.size() && (w[i] == "CQ" || w[i] == "DX" || w[i] == "TEST" || w[i] == "POTA" || w[i] == "SOTA" || w[i] == "NA" || w[i] == "EU")) {
            if (w[i] != "CQ" && r.cqType.empty()) r.cqType = w[i];
            i++;
        }
        if (i < w.size() && w[i] == "DE") i++;
        if (i < w.size() && isCall(w[i])) { r.kind = HamCallLine::Cq; r.from = w[i]; return r; }
        // "CQ LY1ABC LY1ABC"
        if (cq + 1 < w.size() && isCall(w[cq + 1])) { r.kind = HamCallLine::Cq; r.from = w[cq + 1]; return r; }
        return r;
    }
    // "LY1ABC DE DL2XYZ": to, from
    for (size_t i = 0; i + 2 < w.size(); i++)
        if (w[i + 1] == "DE" && isCall(w[i]) && isCall(w[i + 2])) { r.kind = HamCallLine::Reply; r.to = w[i]; r.from = w[i + 2]; return r; }
    return r;
}

class HamHeard {
public:
    struct Entry { std::string call; int count = 0; double lastS = 0; bool cq = false; std::string lastText; };
    void feed(const std::string& text, double nowS) {
        const HamCallLine l = hamParseCall(text);
        for (const auto& c : hamFindCallsigns(text)) {
            Entry& e = m_[c]; e.call = c; e.count++; e.lastS = nowS; e.lastText = text.substr(0, 120);
            if (l.kind == HamCallLine::Cq && l.from == c) e.cq = true;
        }
    }
    // the most recently heard first
    std::vector<Entry> list() const { std::vector<Entry> v; for (auto& kv : m_) v.push_back(kv.second); std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.lastS > b.lastS; }); return v; }
    size_t size() const { return m_.size(); }
private:
    std::map<std::string, Entry> m_;
};

} // namespace dect2
