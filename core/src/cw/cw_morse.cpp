// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW (Morse) code table, callsign pattern and timing decoder (see cw_morse.h).
#include "dect2/cw/cw_morse.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>

namespace dect2 {
namespace cw {

namespace {
struct Sym { const char* code; const char* text; };
// ITU-R M.1677-1, and the prosigns as operators send them (a prosign is shown, not the character that shares its code)
const Sym kTable[] = {
    {".-", "A"}, {"-...", "B"}, {"-.-.", "C"}, {"-..", "D"}, {".", "E"}, {"..-.", "F"}, {"--.", "G"}, {"....", "H"}, {"..", "I"},
    {".---", "J"}, {"-.-", "K"}, {".-..", "L"}, {"--", "M"}, {"-.", "N"}, {"---", "O"}, {".--.", "P"}, {"--.-", "Q"}, {".-.", "R"},
    {"...", "S"}, {"-", "T"}, {"..-", "U"}, {"...-", "V"}, {".--", "W"}, {"-..-", "X"}, {"-.--", "Y"}, {"--..", "Z"},
    {"-----", "0"}, {".----", "1"}, {"..---", "2"}, {"...--", "3"}, {"....-", "4"}, {".....", "5"}, {"-....", "6"}, {"--...", "7"},
    {"---..", "8"}, {"----.", "9"},
    {".-.-.-", "."}, {"--..--", ","}, {"..--..", "?"}, {"-..-.", "/"}, {".----.", "'"}, {"-.-.--", "!"}, {"-.--.-", ")"},
    {"---...", ":"}, {"-.-.-.", ";"}, {"-....-", "-"}, {"..--.-", "_"}, {".-..-.", "\""}, {"...-..-", "$"}, {".--.-.", "@"},
    {".-.-.", "<AR>"}, {"...-.-", "<SK>"}, {"-...-", "<BT>"}, {"-.--.", "<KN>"}, {".-...", "<AS>"}, {"........", "<HH>"},
    {"...---...", "<SOS>"}, {"-.-.-", "<KA>"}, {"...-.", "<VE>"},
};
} // namespace

const char* decodeSymbol(const std::string& code) {
    for (const Sym& s : kTable) if (code == s.code) return s.text;
    return nullptr;
}

std::string encodeSymbol(const std::string& tok) {
    std::string t = tok;
    for (char& c : t) c = (char)std::toupper((unsigned char)c);
    if (t == "+") t = "<AR>";
    if (t == "=") t = "<BT>";
    if (t == "(") t = "<KN>";
    if (t == "&") t = "<AS>";
    for (const Sym& s : kTable) if (t == s.text) return s.code;
    return "";
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == ' ' || c == '\n' || c == '\t') { if (!out.empty() && out.back() != " ") out.push_back(" "); continue; }
        if (c == '<') {
            const size_t e = text.find('>', i);
            if (e != std::string::npos) { std::string t = text.substr(i, e - i + 1); for (char& ch : t) ch = (char)std::toupper((unsigned char)ch); out.push_back(t); i = e; continue; }
        }
        out.push_back(std::string(1, (char)std::toupper((unsigned char)c)));
    }
    return out;
}

bool isCallsign(const std::string& word) {
    // prefix (one or two letters, a figure and a letter, or a letter and a figure), one or two figures, a suffix of one to four letters;
    // an optional portable / prefix part after a "/"
    static const std::regex re("^(?:[A-Z0-9]{1,4}/)?(?:[A-Z]{1,2}|[0-9][A-Z]|[A-Z][0-9])[0-9]{1,2}[A-Z]{1,4}(?:/[A-Z0-9]{1,4})?$");
    if (word.size() < 3 || word.size() > 14) return false;
    return std::regex_match(word, re);
}

std::vector<std::string> callsigns(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') i++;
        size_t e = i;
        while (e < text.size() && text[e] != ' ') e++;
        if (e > i) {
            const std::string w = text.substr(i, e - i);
            if (isCallsign(w) && std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
        }
        i = e;
    }
    return out;
}

// ---------------------------------------------------------------- timing

void Timing::reset() { *this = Timing(); }

void Timing::mark(double ms) { push(true, ms); }
void Timing::space(double ms) { push(false, ms); }

void Timing::push(bool on, double ms) {
    if (ms <= 0) return;
    if (!locked()) {
        if (backlog_.size() > 400) backlog_.erase(backlog_.begin(), backlog_.begin() + 100);
        backlog_.push_back({on, ms});
        if (on && ms >= 10) { marks_.push_back(ms); if (marks_.size() > 40) marks_.erase(marks_.begin()); }
        estimate();
        if (locked()) replay();
        return;
    }
    if (on) { cur_.push_back({true, ms}); charOpen_ = true; if (cur_.size() >= 15) resync(); return; }
    if (ms < 2 * dot_) { if (charOpen_) cur_.push_back({false, ms}); return; }
    if (charOpen_) endChar();
    const double wordThr = wordThresholdMs();
    if (ms < 120 * dot_) { gaps_.push_back(ms); if (gaps_.size() > 40) gaps_.erase(gaps_.begin()); gapClusters(); }
    if (ms >= wordThr) wordSpace();
}

// the gap between characters: most key-ups of two dots and more end a character (a word has several), so a low percentile of them is
// the character gap; Farnsworth spacing stretches it and the word gap alike (7 / 3 of it), long pauses do not move it
void Timing::gapClusters() {
    if (gaps_.size() < 6) return;
    std::vector<double> g = gaps_;
    std::sort(g.begin(), g.end());
    charGap_ = g[g.size() * 3 / 10];
}

double Timing::wordThresholdMs() const {
    if (dot_ <= 0) return 0;
    if (charGap_ < 1.8 * dot_ || charGap_ > 60 * dot_) return 5.0 * dot_;   // not known, or from another speed
    return std::max(4.2 * dot_, 1.67 * charGap_);
}

void Timing::idle(double ms) {
    if (!locked()) return;
    if (charOpen_ && ms >= 2 * dot_) endChar();
    if (!spaced_ && ms >= wordThresholdMs()) wordSpace();
}

void Timing::wordSpace() {
    if (!spaced_) out_ += ' ';
    spaced_ = true;
}

void Timing::endChar() {
    charOpen_ = false;
    std::vector<Ev> e;
    e.swap(cur_);
    while (!e.empty() && !e.back().on) e.pop_back();
    const double g = 0.35 * dot_;
    // drop-outs inside a mark: mark + short space + mark -> one mark
    for (size_t i = 1; i + 1 < e.size();) {
        if (!e[i].on && e[i].ms < g && e[i - 1].on && e[i + 1].on) { e[i - 1].ms += e[i].ms + e[i + 1].ms; e.erase(e.begin() + (long)i, e.begin() + (long)i + 2); }
        else i++;
    }
    // spikes: a short mark is part of the gap around it
    for (size_t i = 0; i < e.size();) {
        if (e[i].on && e[i].ms < g) {
            if (i > 0 && i + 1 < e.size()) { e[i - 1].ms += e[i].ms + e[i + 1].ms; e.erase(e.begin() + (long)i, e.begin() + (long)i + 2); }
            else if (i + 1 < e.size()) e.erase(e.begin(), e.begin() + 2);
            else { e.erase(e.begin() + (long)i); if (i > 0) e.erase(e.begin() + (long)i - 1); }
        } else i++;
    }
    std::string code;
    const double thr = dash_ > 0 ? 0.5 * (dot_ + dash_) : 2 * dot_;
    for (const Ev& v : e) {
        if (!v.on) continue;
        code += v.ms < thr ? '.' : '-';
        marks_.push_back(v.ms);
    }
    while (marks_.size() > 30) marks_.erase(marks_.begin());
    if (code.empty()) return;
    estimate();
    const char* t = code.size() <= 9 ? decodeSymbol(code) : nullptr;
    if (t) out_ += t; else { out_ += '*'; unknown_++; }
    chars_++;
    spaced_ = false;
}

// dots and dashes from the last marks: a 2-means on their lengths
void Timing::estimate() {
    if (marks_.size() < 4) return;
    std::vector<double> m = marks_;
    std::sort(m.begin(), m.end());
    double c1 = m[m.size() / 4], c2 = m[m.size() * 9 / 10];
    size_t n1 = 0, n2 = 0;
    for (int it = 0; it < 12; it++) {
        double s1 = 0, s2 = 0; n1 = n2 = 0;
        const double mid = 0.5 * (c1 + c2);
        for (double v : m) { if (v < mid) { s1 += v; n1++; } else { s2 += v; n2++; } }
        if (n1) c1 = s1 / n1;
        if (n2) c2 = s2 / n2;
    }
    auto clampDot = [](double d) { return std::max(24.0, std::min(240.0, d)); };   // 50 to 5 WPM
    const double was = dot_;
    auto changed = [&]() { if (was > 0 && std::fabs(dot_ - was) > 0.3 * was) { gaps_.clear(); charGap_ = 0; } };
    if (n1 && n2 && c2 / c1 >= 2.0) {
        dot_ = clampDot(c1);
        changed();
        dash_ = std::max(2.0 * dot_, c2);
        return;
    }
    double mean = 0;
    for (double v : m) mean += v;
    mean /= (double)m.size();
    if (locked()) {   // one kind of element only: a big change of speed is taken from it
        if (mean < 0.6 * dot_) { dot_ = clampDot(mean); dash_ = 3 * dot_; changed(); }
        else if (dash_ > 0 && mean > 1.5 * dash_) { dot_ = clampDot(mean / 3); dash_ = 3 * dot_; changed(); }
        return;
    }
    if (marks_.size() < 8) return;
    // still no dashes and dots apart: the gaps inside the characters are one dot long
    std::vector<double> sp;
    for (const Ev& v : backlog_) if (!v.on && v.ms >= 10) sp.push_back(v.ms);
    if (sp.size() < 6) return;
    std::sort(sp.begin(), sp.end());
    const double unit = sp[sp.size() / 5];
    if (mean / unit < 1.6) dot_ = clampDot(mean);
    else if (mean / unit > 2.2) dot_ = clampDot(mean / 3);
    else return;
    dash_ = 3 * dot_;
}

// a character far too long: the speed went up (the gaps inside it are the new character gaps). Its own marks give the new speed.
void Timing::resync() {
    if (replaying_) return;
    std::vector<double> mk;
    for (const Ev& v : cur_) if (v.on && v.ms >= 10) mk.push_back(v.ms);
    const double was = dot_;
    const double wasDash = dash_;
    const std::vector<double> keep = marks_;
    marks_ = mk;
    estimate();
    if (std::fabs(dot_ - was) < 0.2 * was) { marks_ = keep; dot_ = was; dash_ = wasDash; return; }
    std::vector<Ev> e;
    e.swap(cur_);
    charOpen_ = false;
    replaying_ = true;
    for (const Ev& v : e) push(v.on, v.ms);
    replaying_ = false;
}

void Timing::replay() {
    if (replaying_) return;
    std::vector<Ev> b;
    b.swap(backlog_);
    marks_.clear();
    replaying_ = true;
    for (const Ev& v : b) push(v.on, v.ms);
    replaying_ = false;
}

void Keyer::sample(bool raw, Timing& tm_) {
    const int g = tm_.locked() ? std::max(3, std::min(25, (int)std::lround(0.33 * tm_.dotMs()))) : 6;
    cnt_ = raw != key_ ? cnt_ + 1 : 0;
    run_++;
    if (cnt_ >= g) {
        const int d = run_ - cnt_;
        if (key_) tm_.mark(d); else if (started_) tm_.space(d);
        started_ = true;
        key_ = raw;
        run_ = cnt_;
        cnt_ = 0;
    }
    if (!key_ && started_ && run_ % 10 == 0) tm_.idle(run_);
}

std::string Timing::take() {
    std::string s;
    s.swap(out_);
    return s;
}

} // namespace cw
} // namespace dect2
