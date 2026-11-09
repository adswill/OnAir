#include "dect2/general/sw_ident.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

namespace dect2 {

// EiBi's language letters that the speech model's ISO codes correspond to (the common ones)
static std::string eibiLang(const std::string& l) {
    static const std::map<std::string, std::string> m = {{"A", "ar"}, {"E", "en"}, {"F", "fr"}, {"G", "de"}, {"I", "it"}, {"J", "ja"}, {"K", "ko"}, {"M", "zh"}, {"P", "pt"}, {"R", "ru"}, {"S", "es"}, {"T", "th"}, {"TU", "tr"}, {"RO", "ro"}, {"PL", "pl"}, {"CZ", "cs"}, {"HN", "hi"}};
    auto it = m.find(l);
    return it == m.end() ? l : it->second;
}

static uint8_t eibiDays(const std::string& s) {
    if (s.empty()) return 0x7F;
    static const char* names[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    uint8_t mask = 0;
    bool digit = std::isdigit((unsigned char)s[0]);
    if (digit) { for (char c : s) if (c >= '1' && c <= '7') mask |= (uint8_t)(1 << (c - '1')); return mask ? mask : 0x7F; }
    const size_t dash = s.find('-');
    if (dash != std::string::npos && dash == 2 && s.size() >= 5) {
        int a = -1, b = -1;
        for (int i = 0; i < 7; i++) { if (s.compare(0, 2, names[i]) == 0) a = i; if (s.compare(3, 2, names[i]) == 0) b = i; }
        if (a >= 0 && b >= 0) { for (int i = a;; i = (i + 1) % 7) { mask |= (uint8_t)(1 << i); if (i == b) break; } return mask; }
    }
    for (int i = 0; i < 7; i++) if (s.find(names[i]) != std::string::npos) mask |= (uint8_t)(1 << i);
    return mask ? mask : 0x7F;
}

std::vector<SwSchedEntry> parseEibi(const std::string& text) {
    std::vector<SwSchedEntry> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::string cur;
        for (char c : line) { if (c == ';') { f.push_back(cur); cur.clear(); } else cur += c; }
        f.push_back(cur);
        if (f.size() < 7) continue;
        char* end = nullptr;
        const double khz = std::strtod(f[0].c_str(), &end);
        if (end == f[0].c_str() || khz <= 0) continue;
        SwSchedEntry e; e.khz = khz;
        int a = 0, b = 0;
        if (sscanf(f[1].c_str(), "%d-%d", &a, &b) == 2) { e.startMin = a / 100 * 60 + a % 100; e.endMin = b / 100 * 60 + b % 100; }
        e.days = eibiDays(f[2]);
        e.itu = f[3]; e.station = f[4]; e.lang = eibiLang(f[5]); e.target = f[6];
        out.push_back(std::move(e));
    }
    return out;
}

static bool onAir(const SwSchedEntry& e, int minute, int day) {
    if (!((e.days >> (day & 7)) & 1)) return false;
    return e.endMin > e.startMin ? minute >= e.startMin && minute < e.endMin : minute >= e.startMin || minute < e.endMin;
}

void SwIdent::addSchedule(std::vector<SwSchedEntry> e) { sched_.insert(sched_.end(), std::make_move_iterator(e.begin()), std::make_move_iterator(e.end())); }

std::vector<SwGuess> SwIdent::guess(const SwIdentInput& in) const {
    std::vector<SwGuess> out;
    // from the schedule: on this frequency, on the air now; the language raises or lowers it
    for (const auto& e : sched_) {
        if (std::fabs(e.khz - in.khz) > 2.5 || !onAir(e, in.utcMinute, in.weekdayMon0)) continue;
        SwGuess g; g.station = e.station;
        g.score = 0.6f;
        g.why = "schedule";
        if (!in.language.empty() && !e.lang.empty()) {
            if (e.lang == in.language) { g.score += 0.3f * std::min(1.f, in.languageProb + 0.2f); g.why += ", language " + in.language; }
            else g.score -= 0.3f;
        }
        out.push_back(std::move(g));
    }
    // taught interval signals
    if (in.intervalSignal && !in.intervalPrint.empty()) for (const auto& t : taught_) {
        const float m = IntervalDetector::match(t.print, in.intervalPrint);
        if (m >= 0.75f) { SwGuess g; g.station = t.name; g.score = 0.4f + 0.5f * m; g.why = "interval signal"; out.push_back(std::move(g)); }
    }
    // nothing named: the language alone is still worth saying
    if (out.empty() && !in.language.empty()) { SwGuess g; g.station = "broadcast in " + in.language; g.score = 0.2f; g.why = "language"; out.push_back(std::move(g)); }
    std::sort(out.begin(), out.end(), [](const SwGuess& a, const SwGuess& b) { return a.score > b.score; });
    return out;
}

// --- interval signal

void IntervalDetector::configure(int r) { rate_ = r; fftN_ = 1 << 13; hop_ = fftN_ / 2; buf_.assign((size_t)fftN_, 0.f); reset(); }
void IntervalDetector::reset() { fill_ = 0; chroma_.clear(); period_ = 0; strength_ = 0; periodFrames_ = 0; since_ = 0; }

void IntervalDetector::push(const float* x, size_t n) {
    if (buf_.empty()) return;
    while (n) {
        const size_t t = std::min(n, (size_t)fftN_ - fill_);
        std::memcpy(buf_.data() + fill_, x, t * sizeof(float));
        fill_ += t; x += t; n -= t;
        if ((int)fill_ < fftN_) continue;
        // the chroma of this frame: FFT power folded into the 12 notes, 100 Hz .. 2.5 kHz
        std::vector<cf32> z((size_t)fftN_);
        for (int i = 0; i < fftN_; i++) z[(size_t)i] = cf32(buf_[(size_t)i] * (0.5f - 0.5f * (float)std::cos(2 * M_PI * i / fftN_)), 0);
        Fft f(fftN_); f.forward(z.data());
        std::vector<float> c(12, 0.f);
        const double bin = (double)rate_ / fftN_;
        for (int k = (int)(100 / bin); k < (int)(2500 / bin) && k < fftN_ / 2; k++) {
            const double midi = 69 + 12 * std::log2(k * bin / 440.0);
            const int nn = ((int)std::lround(midi) % 12 + 12) % 12;
            c[(size_t)nn] += std::norm(z[(size_t)k]);
        }
        double s = 0; for (float v : c) s += v * v;
        const float nrm = (float)std::sqrt(std::max(s, 1e-30));
        for (auto& v : c) v /= nrm;
        chroma_.push_back(std::move(c));
        const size_t cap = (size_t)(60.0 * rate_ / hop_);
        while (chroma_.size() > cap) chroma_.pop_front();
        std::memmove(buf_.data(), buf_.data() + hop_, (size_t)(fftN_ - hop_) * sizeof(float));
        fill_ = (size_t)(fftN_ - hop_);
        if (++since_ % (uint64_t)(rate_ / hop_ * 2) == 0) analyse();
    }
}

void IntervalDetector::analyse() {
    const int n = (int)chroma_.size();
    const double fr = (double)rate_ / hop_;
    if (n < (int)(8 * fr)) return;
    // the frame-to-frame similarity at each lag; the best lag above 1 s that is a repeat of the whole recent past
    const int lo = (int)(1.0 * fr), hi = std::min(n / 2, (int)(30 * fr));
    std::vector<float> sim((size_t)hi + 1, 0.f);
    float best = 0;
    for (int k = lo; k <= hi; k++) {
        double sum = 0; int cnt = 0;
        for (int i = k; i < n; i += 2) {
            double d = 0; const auto& a = chroma_[(size_t)i], & b = chroma_[(size_t)(i - k)];
            for (int m = 0; m < 12; m++) d += a[(size_t)m] * b[(size_t)m];
            sum += d; cnt++;
        }
        sim[(size_t)k] = cnt ? (float)(sum / cnt) : 0.f;
        best = std::max(best, sim[(size_t)k]);
    }
    int bk = 0;   // a melody repeats at every multiple of its loop too: the shortest lag that is (nearly) as good as the best, at a local top
    for (int k = lo; k <= hi; k++) {
        const bool top = (k == lo || sim[(size_t)k] >= sim[(size_t)k - 1]) && (k == hi || sim[(size_t)k] >= sim[(size_t)k + 1]);
        if (top && sim[(size_t)k] >= best - 0.04f) { bk = k; break; }
    }
    if (bk) best = sim[(size_t)bk];
    // the mean similarity between unrelated frames (a melody has a level of it too): strength is above that
    double base = 0; int cnt = 0;
    for (int i = n / 2; i < n; i += 3) for (int j = 0; j < n / 2; j += 7) {
        double d = 0; for (int m = 0; m < 12; m++) d += chroma_[(size_t)i][(size_t)m] * chroma_[(size_t)j][(size_t)m];
        base += d; cnt++;
    }
    base = cnt ? base / cnt : 0;
    strength_ = bk ? (float)std::max(0.0, std::min(1.0, (best - base) / std::max(1.0 - base, 1e-3))) : 0.f;
    if (bk) { period_ = (float)(bk / fr); periodFrames_ = bk; } else { period_ = 0; periodFrames_ = 0; }
}

std::vector<float> IntervalDetector::fingerprint() const {
    std::vector<float> fp;
    if (!repeating() || periodFrames_ <= 0 || (int)chroma_.size() < periodFrames_ * 2) return fp;
    const int p = periodFrames_, n = (int)chroma_.size();
    fp.assign((size_t)p * 12, 0.f);
    int reps = 0;
    for (int e = n; e - p >= 0; e -= p) { for (int i = 0; i < p; i++) for (int m = 0; m < 12; m++) fp[(size_t)(i * 12 + m)] += chroma_[(size_t)(e - p + i)][(size_t)m]; reps++; }
    for (auto& v : fp) v /= (float)reps;
    return fp;
}

float IntervalDetector::match(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() % 12 || b.size() % 12) return 0;
    const int na = (int)a.size() / 12, nb = (int)b.size() / 12;
    if (std::abs(na - nb) * 4 > std::max(na, nb)) return 0;      // a very different length is another melody
    double bestV = 0;
    for (int sh = 0; sh < nb; sh += std::max(1, nb / 256)) {
        double ab = 0, aa = 0, bb = 0;
        const int n = std::min(na, nb);
        for (int i = 0; i < n; i++) for (int m = 0; m < 12; m++) { const double x = a[(size_t)(i * 12 + m)], y = b[(size_t)(((i + sh) % nb) * 12 + m)]; ab += x * y; aa += x * x; bb += y * y; }
        const double v = ab / std::sqrt(std::max(aa * bb, 1e-30));
        if (v > bestV) bestV = v;
    }
    return (float)bestV;
}

} // namespace dect2
