// STANAG 4481 FSK (single channel +-425 Hz, multi-channel +-42.5 Hz sub-carriers) receiver and test generator; see hfu_4481.h.
#include "dect2/hfutil/hfu_4481.h"
#include "dect2/hfdig/hfdig_sitor.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>

namespace dect2::hfu {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;
constexpr double kFs = 24000.0;
constexpr int kDec = 4;                    // decisions at 6 kHz
constexpr double kDs = kFs / kDec;
constexpr int kAvg = 240;                  // tone filters: 10 ms (one bit at 100 Bd)
const double kBauds[4] = {45.45, 50.0, 75.0, 100.0};
constexpr uint8_t kLtrs = 31, kFigs = 27;

bool ita2Code(char ch, bool& figs, std::vector<uint8_t>& out) {
    for (int f = 0; f < 2; f++) for (int c = 0; c < 32; c++) {
        if (c == kLtrs || c == kFigs) continue;
        if (sitorIta2Char((uint8_t)c, f == 1) != ch) continue;
        if ((f == 1) != figs) { out.push_back(f ? kFigs : kLtrs); figs = f == 1; }
        out.push_back((uint8_t)c);
        return true;
    }
    return false;
}

struct Tone {
    double f = 0;
    cd rot{1, 0}, st{1, 0};
    std::vector<cd> ring;
    size_t pos = 0;
    cd sum;
    int k = 0;
    void set(double hz) { f = hz; st = cd(std::cos(-2 * kPi * hz / kFs), std::sin(-2 * kPi * hz / kFs)); rot = cd(1, 0); ring.assign(kAvg, cd(0, 0)); pos = 0; sum = cd(0, 0); k = 0; }
    double step(std::complex<float> x) {
        const cd z = cd(x.real(), x.imag()) * rot;
        rot *= st;
        if (++k == 4096) { rot /= std::abs(rot); k = 0; sum = cd(0, 0); for (const cd& v : ring) sum += v; }   // keep the phasor on the unit circle and the sum exact
        sum += z - ring[pos];
        ring[pos] = z;
        pos = (pos + 1) % ring.size();
        return std::norm(sum);
    }
};

// ITA2 start-stop reader at one speed, on the 6 kHz decisions (1 = mark)
struct Async {
    double T = 0;
    int64_t start = -1, earliest = 0;
    int prev = 1;
    bool figs = false;
    double good = 0, bad = 0;             // decaying counts
    uint64_t chars = 0, errors = 0;
    std::vector<uint8_t> bitsAt;          // the decisions since the start bit
    void init(double baud) { T = kDs / baud; start = -1; earliest = 0; prev = 1; figs = false; good = bad = 0; chars = errors = 0; bitsAt.clear(); }
    // returns a character (0 none, -1 framing error)
    int push(int64_t n, int b) {
        int out = 0;
        if (start < 0) {
            if (prev == 1 && b == 0 && n >= earliest) { start = n; bitsAt.clear(); }
        }
        if (start >= 0) {
            bitsAt.push_back((uint8_t)b);
            const double end = 6.5 * T;
            if ((double)(n - start) >= end) {
                auto at = [&](double k) { const size_t i = (size_t)std::lround(k * T); return i < bitsAt.size() ? bitsAt[i] : 1; };
                const bool ok = at(0.5) == 0 && at(6.5) == 1;
                if (ok) {
                    uint8_t c = 0;
                    for (int i = 0; i < 5; i++) c |= (uint8_t)(at(1.5 + i) << i);
                    good = good * 0.97 + 1; chars++;
                    if (c == kLtrs) figs = false;
                    else if (c == kFigs) figs = true;
                    else { const char ch = sitorIta2Char(c, figs); out = ch ? (unsigned char)ch : 0; }
                    earliest = start + (int64_t)(7.0 * T);
                } else { bad = bad * 0.97 + 1; errors++; out = -1; earliest = start + (int64_t)(T); }
                start = -1;
            }
        }
        prev = b;
        return out;
    }
};

struct Chan {
    S4481Channel info;
    Tone m, s;
    double e = 0, dd = 0;                // smoothed energy and |mark - space| contrast
    Async as[4];
    int lastBit = 1;
    int64_t lastEdge = -1;
    double runErr[4] = {0, 0, 0, 0};
    double runN = 0;
    int best = -1;
    void init(int n, double fm, double fs) {
        info = S4481Channel(); info.n = n; info.markHz = fm; info.spaceHz = fs;
        m.set(fm); s.set(fs); e = 0; dd = 0; lastBit = 1; lastEdge = -1; runN = 0; best = -1;
        for (int k = 0; k < 4; k++) { as[k].init(kBauds[k]); runErr[k] = 0; }
    }
};
}  // namespace

std::vector<cf32> genStanag4481(const S4481GenCfg& c, const std::vector<S4481Content>& chans, double seconds) {
    const size_t N = (size_t)((c.leadSec + seconds + c.tailSec) * kFs);
    std::vector<cf32> out(N, cf32(0, 0));
    const double T = kFs / c.baud;
    for (const S4481Content& ch : chans) {
        double fm, fs;
        if (c.multi) { fm = s4481ChannelHz(ch.n) - 42.5; fs = s4481ChannelHz(ch.n) + 42.5; }
        else { fm = c.singleMarkHz; fs = c.singleMarkHz + 850; }
        // the keying as (level, length in samples)
        std::vector<std::pair<int, double>> seg;
        seg.push_back({1, 0.5 * kFs});   // idle mark
        if (ch.syncBits > 0) {
            std::mt19937 r(ch.seed);
            for (int i = 0; i < ch.syncBits; i++) seg.push_back({(int)(r() & 1), T});
        } else {
            std::vector<uint8_t> codes = {kLtrs};
            bool figs = false;
            for (char x : ch.text) ita2Code(x, figs, codes);
            for (uint8_t code : codes) {
                seg.push_back({0, T});
                for (int i = 0; i < 5; i++) seg.push_back({(code >> i) & 1, T});
                seg.push_back({1, 1.5 * T});
            }
        }
        seg.push_back({1, 0.3 * kFs});
        double t = c.leadSec * kFs, ph = 0;
        for (auto& sg : seg) {
            const double f = sg.first ? fm : fs;
            const size_t a = (size_t)t, b = std::min(N, (size_t)(t + sg.second));
            for (size_t i = a; i < b; i++) { ph += 2 * kPi * f / kFs; out[i] += cf32((float)std::cos(ph), (float)std::sin(ph)); }
            t += sg.second;
            if (ph > 1e6) ph = std::fmod(ph, 2 * kPi);
        }
    }
    const float g = 0.3f / (float)std::sqrt((double)std::max<size_t>(1, chans.size()));
    for (auto& v : out) v *= g;
    return out;
}

struct Stanag4481Rx::Impl {
    Chan multi[16];
    Chan single;
    bool singleOn = false;
    Tone ref[16];                          // between the sub-carriers: the noise floor
    double refE[16] = {};
    std::vector<cf32> hist;                // the last 0.1 s, for the single-channel search
    int64_t n = 0, nd = 0;
    int mode = 0;
    std::function<void(int, const std::string&)> textCb;
    std::function<void(const std::string&)> evCb;

    Impl() {
        for (int i = 0; i < 16; i++) { multi[i].init(i + 1, s4481ChannelHz(i + 1) - 42.5, s4481ChannelHz(i + 1) + 42.5); ref[i].set(s4481ChannelHz(i + 1) + 85); }
    }
    void event(const std::string& s) { if (evCb) evCb(s); }

    double noiseFloor() const {
        double v[16];
        for (int i = 0; i < 16; i++) v[i] = refE[i];
        std::nth_element(v, v + 8, v + 16);
        return std::max(v[8], 1e-15);
    }

    // the single channel: the strongest pair of lines 850 Hz apart (Goertzel on the last 0.1 s, 10 Hz steps)
    void searchSingle() {
        if (hist.size() < 2400) return;
        const size_t L = 2400, a = hist.size() - L;
        std::vector<double> p;
        for (double f = 300; f <= 3300; f += 10) {
            const double w = -2 * kPi * f / kFs;
            cd acc(0, 0), r(1, 0), st(std::cos(w), std::sin(w));
            for (size_t i = 0; i < L; i++) { acc += cd(hist[a + i].real(), hist[a + i].imag()) * r; r *= st; }
            p.push_back(std::norm(acc));
        }
        std::vector<double> sorted = p;
        std::nth_element(sorted.begin(), sorted.begin() + (long)sorted.size() / 2, sorted.end());
        const double med = std::max(sorted[sorted.size() / 2], 1e-15);
        int bi = -1;
        double bv = 0;
        for (size_t i = 0; i + 85 < p.size(); i++) {
            const double lo = p[i], hi = p[i + 85];
            const double v = std::min(lo, hi);
            if (v > 30 * med && v > bv) { bv = v; bi = (int)i; }
        }
        if (bi < 0) return;
        // refine each line to 1 Hz with a parabola
        auto refine = [&](int i) { if (i <= 0 || i + 1 >= (int)p.size()) return 300.0 + 10 * i; const double y0 = p[(size_t)i - 1], y1 = p[(size_t)i], y2 = p[(size_t)i + 1]; const double d = y0 - 2 * y1 + y2; return 300.0 + 10 * (i + (d != 0 ? 0.5 * (y0 - y2) / d : 0)); };
        const double fm = refine(bi), fs = refine(bi + 85);
        if (!singleOn || std::fabs(fm - single.info.markHz) > 6) {
            single.init(0, fm, fs);
            singleOn = true;
            char b[120]; snprintf(b, sizeof b, "single channel: mark %.0f Hz, space %.0f Hz (shift %.0f Hz)", fm, fs, fs - fm);
            event(b);
        }
    }

    void chanStep(Chan& c, std::complex<float> x, bool decide, double floor, double maxE) {
        const double em = c.m.step(x), es = c.s.step(x);
        if (!decide) return;
        c.e = 0.995 * c.e + 0.005 * (em + es);
        c.dd = 0.995 * c.dd + 0.005 * std::fabs(em - es) / (em + es + 1e-20);
        // on: well over the noise between the sub-carriers, or (a full band leaves no clean gap) one of the strong channels and keyed
        const bool on = (c.e > 12 * floor && c.e > 0.3 * maxE) || (c.e > 0.3 * maxE && c.dd > 0.5);   // 0.3: a neighbour's leakage is about -15 dB
        if (on != c.info.active) {
            c.info.active = on;
            char b[100];
            if (c.info.n) snprintf(b, sizeof b, "channel %d (%.0f Hz) %s", c.info.n, s4481ChannelHz(c.info.n), on ? "on" : "off");
            else snprintf(b, sizeof b, "single channel %s", on ? "on" : "off");
            event(b);
        }
        c.info.snrDb = 10 * std::log10(c.e / floor + 1e-12);
        if (!on) return;
        const int b = em > es ? 1 : 0;
        c.info.bits++;
        // speed from the edges: the run lengths fit whole bits of the right speed
        if (b != c.lastBit) {
            if (c.lastEdge >= 0) {
                const double r = (double)(nd - c.lastEdge);
                for (int k = 0; k < 4; k++) { const double q = r / (kDs / kBauds[k]); const double e = std::fabs(q - std::max(1.0, std::round(q))); c.runErr[k] = 0.98 * c.runErr[k] + 0.02 * e; }
                c.runN = std::min(c.runN + 1, 1e9);
            }
            c.lastEdge = nd;
        }
        c.lastBit = b;
        std::string out;
        int bestK = 0;
        double bestS = -1e9;
        for (int k = 0; k < 4; k++) { const double s = c.as[k].good - 2 * c.as[k].bad; if (s > bestS) { bestS = s; bestK = k; } }
        for (int k = 0; k < 4; k++) {
            const int ch = c.as[k].push(nd, b);
            if (k == bestK && ch > 0 && c.info.startStop) out.push_back((char)ch);
        }
        c.info.startStop = c.as[bestK].good >= 5 && c.as[bestK].bad < 0.25 * c.as[bestK].good;
        if (c.info.startStop) { c.info.baud = kBauds[bestK]; c.info.chars = c.as[bestK].chars; c.info.framingErrors = c.as[bestK].errors; }
        else if (c.runN > 20) {
            int k = 0;
            for (int i = 1; i < 4; i++) if (c.runErr[i] < c.runErr[k]) k = i;
            c.info.baud = c.runErr[k] < 0.12 ? kBauds[k] : 0;
        }
        if (!out.empty()) {
            c.info.text += out;
            if (c.info.text.size() > 400) c.info.text.erase(0, c.info.text.size() - 400);
            if (textCb) textCb(c.info.n, out);
        }
    }

    void push(const cf32* x, size_t len) {
        for (size_t i = 0; i < len; i++) {
            const bool decide = (n % kDec) == 0;
            for (int k = 0; k < 16; k++) { const double e = ref[k].step(x[i]); if (decide) refE[k] = 0.995 * refE[k] + 0.005 * e; }
            const double floor = noiseFloor();
            double mx = 1e-20;
            for (auto& c : multi) mx = std::max(mx, c.e);
            for (auto& c : multi) chanStep(c, x[i], decide, floor, mx);
            if (singleOn) chanStep(single, x[i], decide, floor, single.e);
            hist.push_back(x[i]);
            n++;
            if (decide) nd++;
            if (n % 6000 == 0) {   // every 0.25 s
                int act = 0;
                for (auto& c : multi) act += c.info.active;
                // a single-channel signal leaks into one or two sub-carrier filters next to its tones; three or more sub-carriers is multi-channel
                if (act < 3) searchSingle();
                const int m = act >= 3 ? 2 : (singleOn && single.info.active ? 1 : (act >= 1 ? 2 : 0));
                if (m != mode) { mode = m; event(m == 2 ? "multi-channel FSK (+-42.5 Hz sub-carriers)" : m == 1 ? "single-channel FSK (+-425 Hz)" : "no FSK broadcast"); }
                if (hist.size() > 4800) hist.erase(hist.begin(), hist.end() - 2400);
            }
        }
    }
};

Stanag4481Rx::Stanag4481Rx() : p_(new Impl) {}
Stanag4481Rx::~Stanag4481Rx() = default;
void Stanag4481Rx::reset() { auto t = std::move(p_->textCb); auto e = std::move(p_->evCb); p_.reset(new Impl); p_->textCb = std::move(t); p_->evCb = std::move(e); }
void Stanag4481Rx::push(const cf32* x, size_t n) { p_->push(x, n); }
void Stanag4481Rx::setTextSink(std::function<void(int, const std::string&)> cb) { p_->textCb = std::move(cb); }
void Stanag4481Rx::setEventSink(std::function<void(const std::string&)> cb) { p_->evCb = std::move(cb); }
S4481Status Stanag4481Rx::status() const {
    S4481Status s;
    s.mode = p_->mode;
    if (p_->mode == 1 && p_->singleOn) s.channels.push_back(p_->single.info);
    else for (auto& c : p_->multi) if (c.info.active) s.channels.push_back(c.info);
    return s;
}

}  // namespace dect2::hfu
