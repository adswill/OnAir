// Throb, ThrobX (fldigi, after gmfsk): 9 tones 8 Hz apart (Throb1 / Throb2, symbols of 1.024 s and 0.512 s) or 16 Hz apart (Throb4, 0.256 s), 11 tones
// 7.8125 / 15.625 Hz apart for ThrobX. A symbol is one or two tones sounding together under a raised-cosine pulse; the character is the pair. Throb has a shift
// symbol for ? @ = and the line feed, ThrobX has 55 symbols and alternates two symbols for space and idle. Tone pair tables are from fldigi's throb.cxx (GPL-3.0).
// The receiver finds the tone comb in the spectrum (4 comb types), tries every variant and symbol timing, and reads the two strongest tones per symbol.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_int.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace hamsci {
using namespace detail;

namespace {
const int kThrobPairs[45][2] = {{5,5},{4,5},{1,2},{1,3},{1,4},{4,6},{1,5},{1,6},{1,7},{3,7},{1,8},{2,3},{2,4},{2,8},{2,5},{5,6},{2,6},{2,9},{3,4},{3,5},{1,9},{3,6},{8,9},{3,8},{3,3},{2,2},{1,1},{3,9},{4,7},{4,8},{4,9},{5,7},{5,8},{5,9},{6,7},{6,8},{6,9},{7,8},{7,9},{8,8},{7,7},{6,6},{4,4},{9,9},{2,7}};
const int kThrobXPairs[55][2] = {{6,11},{1,6},{2,6},{2,5},{2,7},{2,8},{5,6},{2,9},{2,10},{4,8},{4,6},{2,11},{3,4},{3,5},{3,6},{6,9},{6,10},{3,7},{3,8},{3,9},{6,8},{6,7},{3,10},{3,11},{4,5},{4,7},{4,9},{4,10},{1,2},{1,3},{1,4},{1,5},{1,7},{1,8},{1,9},{1,10},{2,3},{2,4},{4,11},{5,7},{5,8},{5,9},{5,10},{5,11},{7,8},{7,9},{7,10},{7,11},{8,9},{8,10},{8,11},{9,10},{9,11},{10,11},{1,11}};
const char kThrobChars[45] = {'\0','A','B','C','D','\0','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','1','2','3','4','5','6','7','8','9','0',',','.','\'','/',')','(','E',' '};
const char kThrobXChars[55] = {'\0',' ','A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','1','2','3','4','5','6','7','8','9','0',',','.','\'','/',')','(','#','"','+','-',';',':','?','!','@','=','\n'};

struct Var { int nt; double sym; double sp; bool x; bool full; };
const Var kVar[6] = {{9, 1.024, 8.0, false, false}, {9, 0.512, 8.0, false, false}, {9, 0.256, 16.0, false, true},
                     {11, 1.024, 7.8125, true, false}, {11, 0.512, 7.8125, true, false}, {11, 0.256, 15.625, true, true}};
double toneHz(const Var& v, int k) { return (k - (v.nt - 1) / 2.0) * v.sp; }   // offset from the centre tone

double pulse(const Var& v, double u) {   // u in 0..1
    if (v.full) return 0.5 * (1 - std::cos(2 * kPi * u));
    if (u < 0.2) return 0.5 * (1 - std::cos(kPi * u / 0.2));
    if (u >= 0.8) return 0.5 * (1 + std::cos(kPi * (u - 0.8) / 0.2));
    return 1.0;
}
int findSymbol(const Var& v, int t1, int t2) {   // zero-based sorted tones -> symbol index, -1 none
    const int n = v.x ? 55 : 45;
    for (int i = 0; i < n; i++) {
        const int a = (v.x ? kThrobXPairs[i][0] : kThrobPairs[i][0]) - 1, b = (v.x ? kThrobXPairs[i][1] : kThrobPairs[i][1]) - 1;
        if (a == t1 && b == t2) return i;
    }
    return -1;
}
int charSymbol(const Var& v, char c) {   // symbol index of a character for the generator, -1 none
    c = (char)std::toupper((unsigned char)c);
    const int n = v.x ? 55 : 45;
    for (int i = 0; i < n; i++) {
        const char t = v.x ? kThrobXChars[i] : kThrobChars[i];
        if (t == c && c != '\0' && !(!v.x && i == 5)) return i;
    }
    return -1;
}
}  // namespace

const char* throbName(int v) {
    static const char* n[6] = {"Throb1", "Throb2", "Throb4", "ThrobX1", "ThrobX2", "ThrobX4"};
    return v >= 0 && v < 6 ? n[v] : "?";
}
double throbSymbolSec(int v) { return v >= 0 && v < 6 ? kVar[v].sym : 0; }
double throbToneSpacing(int v) { return v >= 0 && v < 6 ? kVar[v].sp : 0; }

std::vector<float> throbGenerate(const std::string& text, int variant, double fs, double centreHz, double amp, int preambleSymbols) {
    const Var& v = kVar[std::clamp(variant, 0, 5)];
    std::vector<std::pair<int, int>> syms;   // zero-based tone pairs
    int idleSym = 0, spaceSym = v.x ? 1 : 44;
    auto flip = [&] { if (v.x) { std::swap(idleSym, spaceSym); } };
    auto put = [&](int sym) {
        if (v.x) syms.push_back({kThrobXPairs[sym][0] - 1, kThrobXPairs[sym][1] - 1});
        else syms.push_back({kThrobPairs[sym][0] - 1, kThrobPairs[sym][1] - 1});
    };
    for (int i = 0; i < preambleSymbols; i++) { put(idleSym); flip(); }
    for (char ch : text) {
        char c = (char)std::toupper((unsigned char)ch);
        if (!v.x) {
            if (c == '?') { put(5); put(20); continue; }
            if (c == '@') { put(5); put(13); continue; }
            if (c == '=') { put(5); put(9); continue; }
            if (c == '\n') { put(5); put(0); continue; }
            if (c == '\r') continue;
        }
        if (c == ' ') { put(spaceSym); flip(); continue; }
        int s = charSymbol(v, c);
        if (s < 0) { put(spaceSym); flip(); continue; }
        put(s);
    }
    put(idleSym);
    const double S = v.sym * fs;
    const size_t n = (size_t)std::ceil((double)syms.size() * S);
    std::vector<float> y(n, 0.f);
    for (size_t k = 0; k < syms.size(); k++) {
        const size_t a = (size_t)std::llround((double)k * S), b = std::min(n, (size_t)std::llround((double)(k + 1) * S));
        const double w1 = 2 * kPi * (centreHz + toneHz(v, syms[k].first)) / fs, w2 = 2 * kPi * (centreHz + toneHz(v, syms[k].second)) / fs;
        for (size_t i = a; i < b; i++) {
            const double u = (double)(i - a) / (double)(b - a);
            y[i] = (float)(amp * pulse(v, u) * (std::sin(w1 * (double)i) + std::sin(w2 * (double)i)) / 2.0);
        }
    }
    return y;
}

namespace {
struct Dec { double valid = 1, usage = 0; double score = 0, snr = 0; std::string text; std::vector<double> times; int symbols = 0; double dup = 0; double centre = 0; };

// The channel around `centre` as complex samples at fs / D (a boxcar of D samples, then every D-th): the tones are all within +-100 Hz of it
struct Base { std::vector<cd> y; double rate = 0; };
Base baseband(const float* x, size_t n, double fs, double centre) {
    Base b;
    const int D = std::max(1, (int)std::floor(fs / 480.0));
    b.rate = fs / D;
    const size_t m = n / (size_t)D;
    b.y.resize(m);
    const double w = 2 * kPi * centre / fs;
    for (size_t k = 0; k < m; k++) {
        cd acc(0, 0);
        for (int i = 0; i < D; i++) { const size_t idx = k * (size_t)D + (size_t)i; acc += (double)x[idx] * cd(std::cos(w * (double)idx), -std::sin(w * (double)idx)); }
        b.y[k] = acc / (double)D;
    }
    return b;
}

// Energies of the tones in the central 60 % of each symbol (t0 and the symbol length in seconds); extra tones on each side (nt + 2 * extra values per symbol,
// the first one `extra` tone steps below the lowest tone)
void symbolEnergies(const Base& b, const Var& v, double t0Sec, std::vector<std::vector<float>>& E, int extra = 0) {
    E.clear();
    const double S = v.sym * b.rate;   // samples per symbol
    const double t0 = t0Sec * b.rate;
    for (double s = t0; s + S <= (double)b.y.size(); s += S) {
        const size_t a = (size_t)std::floor(s + 0.2 * S), e = (size_t)std::floor(s + 0.8 * S);
        std::vector<float> en((size_t)(v.nt + 2 * extra));
        for (int k = -extra; k < v.nt + extra; k++) {
            const double w = 2 * kPi * toneHz(v, k) / b.rate;
            const cd r(std::cos(w), -std::sin(w));
            cd z(0, 0), p(std::cos(w * (double)a), -std::sin(w * (double)a));
            for (size_t i = a; i < e; i++) { z += b.y[i] * p; p *= r; }
            en[(size_t)(k + extra)] = (float)std::norm(z);
        }
        E.push_back(std::move(en));
    }
}

Dec decodeVariant(const float* x, size_t n, double fs, int vi, double centre) {
    const Var& v = kVar[vi];
    Dec best;
    const double S = v.sym * fs;
    if ((double)n < 6 * S) return best;
    const Base base = baseband(x, n, fs, centre);
    // timing: the offset with the most energy in the central windows (the pulses are zero at the edges)
    std::vector<std::vector<float>> E;
    double bestT = 0, bestE = -1;
    const int steps = 24;
    const double symSec = v.sym;
    for (int t = 0; t < steps; t++) {
        symbolEnergies(base, v, t * symSec / steps, E);
        double tot = 0;
        for (auto& e : E) for (float f : e) tot += f;
        if (tot > bestE) { bestE = tot; bestT = t * symSec / steps; }
    }
    for (int dt = -3; dt <= 3; dt++) {
        if (dt == 0) continue;
        double t0 = bestT + dt * symSec / (steps * 4);
        if (t0 < 0) t0 += symSec;
        symbolEnergies(base, v, t0, E);
        double tot = 0;
        for (auto& e : E) for (float f : e) tot += f;
        if (tot > bestE) { bestE = tot; bestT = t0; }
    }
    // which tone is the first one: the alignment of the comb that holds the most energy (two extra tones on each side are looked at)
    constexpr int kExtra = 6;
    int align = kExtra;
    double capture = 1;
    {
        std::vector<std::vector<float>> W;
        symbolEnergies(base, v, bestT, W, kExtra);
        double tot = 0;
        const int nW = v.nt + 2 * kExtra;
        std::vector<double> A((size_t)nW, 0.0);
        for (auto& e : W) for (int k = 0; k < nW; k++) { A[(size_t)k] += e[(size_t)k]; tot += e[(size_t)k]; }
        double mx = 0;
        for (double a : A) mx = std::max(mx, a);
        std::vector<double> srtA(A);
        std::sort(srtA.begin(), srtA.end());
        const double noiseRef = (srtA[0] + srtA[1] + srtA[2]) / 3.0;
        const double thr = std::max(0.08 * mx, 4.0 * noiseRef);
        int lo = -1, hi = -1;
        for (int k = 0; k < nW; k++) if (A[(size_t)k] > thr) { if (lo < 0) lo = k; hi = k; }
        align = kExtra;
        if (lo >= 0) align = (int)std::lround((lo + hi) / 2.0 - (v.nt - 1) / 2.0);   // the span of tones in use, centred on the comb
        align = std::clamp(align, 0, 2 * kExtra);
        double bestIn = 0;
        for (int k = 0; k < v.nt; k++) bestIn += A[(size_t)(align + k)];
        capture = bestIn / std::max(1e-30, tot);
        E.clear();
        for (auto& e : W) E.push_back(std::vector<float>(e.begin() + align, e.begin() + align + v.nt));
        best.centre = centre + (align - kExtra) * v.sp;
    }
    bestT *= fs;   // samples, as used below
    const size_t ns = E.size();
    if (ns < 4) return best;
    std::vector<double> tot(ns);
    for (size_t i = 0; i < ns; i++) { double t = 0; for (float f : E[i]) t += f; tot[i] = t; }
    std::vector<double> srt(tot);
    std::sort(srt.begin(), srt.end());
    double med = 0;   // typical energy of an active symbol: the mean of the strongest quarter
    { size_t q = std::max<size_t>(1, ns / 4); for (size_t k = ns - q; k < ns; k++) med += srt[k]; med /= (double)q; }
    // symbols
    std::vector<int> sym(ns, -1);
    double conc = 0, wsum = 0, snrAcc = 0;
    int active = 0;
    std::vector<std::pair<int, int>> pr(ns);
    for (size_t i = 0; i < ns; i++) {
        int k1 = 0;
        for (int k = 1; k < v.nt; k++) if (E[i][(size_t)k] > E[i][(size_t)k1]) k1 = k;
        int k2 = k1 == 0 ? 1 : 0;
        for (int k = 0; k < v.nt; k++) if (k != k1 && E[i][(size_t)k] > E[i][(size_t)k2]) k2 = k;
        const double e1 = E[i][(size_t)k1], e2 = E[i][(size_t)k2];
        if (!v.x && std::sqrt(e1) > 2.0 * std::sqrt(e2)) k2 = k1;
        const double q = (e1 + e2) / std::max(1e-30, tot[i]);
        conc += q * tot[i]; wsum += tot[i];
        snrAcc += (e1 + e2) / 2.0 / std::max(1e-30, (tot[i] - e1 - e2) / (v.nt - 2));
        if (tot[i] > 3.0 * med) active++;
        pr[i] = {std::min(k1, k2), std::max(k1, k2)};
        sym[i] = findSymbol(v, pr[i].first, pr[i].second);
    }
    best.score = conc / std::max(1e-30, wsum) * capture;
    {   // how many symbols are a defined pair, and how many different tones are in use
        int ok = 0, used = 0;
        std::vector<int> use((size_t)v.nt, 0);
        int act = 0;
        for (size_t i = 0; i < ns; i++) {
            if (tot[i] < 0.1 * med) continue;
            act++;
            if (sym[i] >= 0) ok++;
            use[(size_t)pr[i].first]++; use[(size_t)pr[i].second]++;
        }
        for (int k = 0; k < v.nt; k++) if (act > 0 && use[(size_t)k] * 20 >= act) used++;
        best.valid = act ? (double)ok / act : 0;
        best.usage = used;
    }
    best.snr = 10 * std::log10(std::max(1e-6, snrAcc / (double)ns));
    best.symbols = (int)ns;
    // duplicates for the Throb1 / Throb2 decision: pairs of equal consecutive symbols at the better parity
    {
        int dupPar[2] = {0, 0}, cnt[2] = {0, 0};
        for (size_t i = 0; i + 1 < ns; i++) { const int p = (int)(i & 1); cnt[p]++; if (pr[i] == pr[i + 1]) dupPar[p]++; }
        best.dup = std::max(cnt[0] ? (double)dupPar[0] / cnt[0] : 0.0, cnt[1] ? (double)dupPar[1] / cnt[1] : 0.0);
    }
    // text
    bool shift = false;
    char lastc = 0;
    int idleSym = 0, spaceSym = v.x ? 1 : 44;
    for (size_t i = 0; i < ns; i++) {
        if (sym[i] < 0 || tot[i] < 0.1 * med) continue;
        const double tsec = (bestT + ((double)i + 0.5) * S) / fs;
        char out = 0;
        if (!v.x) {
            const int a = pr[i].first, b = pr[i].second;
            if (shift) {
                if (a == 0 && b == 8) out = '?';
                else if (a == 1 && b == 7) out = '@';
                else if (a == 2 && b == 6) out = '=';
                else if (a == 4 && b == 4) out = '\n';
                shift = false;
                if (out) { best.text += out; best.times.push_back(tsec); }
                continue;
            }
            if (a == 3 && b == 5) { shift = true; continue; }
            const char c = kThrobChars[sym[i]];
            if (c) { best.text += c; best.times.push_back(tsec); }
        } else {
            (void)idleSym; (void)spaceSym;
            if (sym[i] == 0 || sym[i] == 1) {
                if (lastc != 0 && lastc != ' ') { best.text += ' '; best.times.push_back(tsec); lastc = ' '; }
                else lastc = 0;
            } else {
                const char c = kThrobXChars[sym[i]];
                best.text += c; best.times.push_back(tsec); lastc = c;
            }
        }
    }
    if (std::getenv("HS_DEBUG")) std::fprintf(stderr, "throb v%d centre %.1f -> %.1f score %.2f dup %.2f text '%s'\n", vi, centre, best.centre, best.score, best.dup, best.text.c_str());
    (void)active;
    return best;
}
}  // namespace

bool throbDecode(const float* x, size_t n, double fs, ThrobResult& out, int variant) {
    if (n < (size_t)(8 * fs)) return false;
    // averaged spectrum
    const size_t N = std::max<size_t>(8192, nextPow2((size_t)(fs * 1.2)));   // about 0.7 Hz per bin
    const double binHz = fs / (double)N;
    std::vector<double> P(N / 2, 0.0);
    std::vector<cd> a(N);
    int frames = 0;
    for (size_t s = 0; s + N <= n; s += N / 2, frames++) {
        for (size_t i = 0; i < N; i++) a[i] = cd((double)x[s + i] * (0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)N)), 0);
        fft(a);
        for (size_t k = 0; k < N / 2; k++) P[k] += std::norm(a[k]);
    }
    if (frames < 2) return false;
    const size_t kLo = (size_t)(300.0 / binHz), kHi = (size_t)(std::min(3500.0, fs / 2 - 200) / binHz);
    // smoothed log spectrum and its median
    std::vector<double> L(N / 2);
    for (size_t k = 1; k < N / 2; k++) L[k] = std::log10(P[k] + 1e-20);
    std::vector<double> tmp(L.begin() + (long)kLo, L.begin() + (long)kHi);
    std::nth_element(tmp.begin(), tmp.begin() + (long)tmp.size() / 2, tmp.end());
    const double medL = tmp[tmp.size() / 2];
    struct Cand { double score, fc; };
    std::vector<Cand> loc;
    const int combs[4] = {0, 2, 3, 5};   // variants that define the four tone spacings
    for (int ci = 0; ci < 4; ci++) {
        const Var& v = kVar[combs[ci]];
        for (double fc = 300 + 5 * v.sp; fc < 3500 - 5 * v.sp; fc += binHz / 2) {
            double sc = 0;
            for (int m = 0; m < v.nt; m++) {
                const double f = fc + toneHz(v, m);
                const size_t k = (size_t)std::llround(f / binHz);
                double best = L[k];
                if (k > 0) best = std::max(best, L[k - 1]);
                if (k + 1 < N / 2) best = std::max(best, L[k + 1]);
                sc += best - medL;
            }
            loc.push_back({sc / v.nt, fc});
        }
    }
    std::sort(loc.begin(), loc.end(), [](const Cand& p, const Cand& q) { return p.score > q.score; });
    std::vector<Cand> cands;
    for (const Cand& c : loc) {
        bool near = false;
        for (const Cand& q : cands) if (std::fabs(q.fc - c.fc) < 40) near = true;
        if (near) continue;
        cands.push_back(c);
        if (cands.size() == 2) break;
    }
    Dec bestDec;
    int bestV = -1;
    for (const Cand& c : cands) {
        for (int vi = 0; vi < 6; vi++) {
            if (variant >= 0 && vi != variant && !(variant == 0 && vi == 1) && !(variant == 3 && vi == 4)) continue;
            Dec d = decodeVariant(x, n, fs, vi, c.fc);
            if (d.score <= 0) continue;
            {   // energy of the averaged spectrum near the comb positions against all of it in the band of the comb
                const Var& v = kVar[vi];
                const double hw = std::min(v.sp / 3.0, 1.5 / v.sym);
                double in = 0, all = 0;
                const double lo = d.centre + toneHz(v, 0) - v.sp * 0.7, hi = d.centre + toneHz(v, v.nt - 1) + v.sp * 0.7;
                for (size_t k = (size_t)std::max(1.0, lo / binHz); k < N / 2 && (double)k * binHz <= hi; k++) {
                    const double f = (double)k * binHz;
                    all += P[k];
                    for (int m = 0; m < v.nt; m++) if (std::fabs(f - (d.centre + toneHz(v, m))) <= hw) { in += P[k]; break; }
                }
                d.score *= all > 0 ? in / all : 0;
            }
            if (kVar[vi].x) d.score *= d.valid;
            if (d.usage < 4) d.score *= 0.5;   // fewer than four tones in use: not Throb
            d.score -= 0.002 * kVar[vi].nt;   // equal readings: the variant with fewer tones
            if (bestV < 0 || d.score > bestDec.score) { bestDec = d; bestV = vi; }
        }
    }
    if (bestV < 0 || bestDec.score < 0.55) return false;
    // Throb1 / Throb2 (and X1 / X2): a short-symbol reading that is all repeated pairs is the long variant
    if (variant < 0 && (bestV == 1 || bestV == 4) && bestDec.dup > 0.8) {
        Dec d = decodeVariant(x, n, fs, bestV - 1, bestDec.centre);
        if (d.score > 0.5) { bestDec = d; bestV = bestV - 1; }
    }
    std::string t;
    for (char c : bestDec.text) t += c;
    if (t.empty()) return false;
    out.text = t;
    out.variant = bestV;
    out.centreHz = bestDec.centre;
    out.snrDb = bestDec.snr;
    out.score = bestDec.score;
    out.symbols = bestDec.symbols;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------ streaming
ThrobDecoder::ThrobDecoder(double fs) : fs_(fs) { reset(); }
void ThrobDecoder::reset() { buf_.clear(); total_ = nextTry_ = 0; lastText_.clear(); level_ = -120; out_.clear(); }
std::vector<ThrobResult> ThrobDecoder::take() { std::vector<ThrobResult> r; r.swap(out_); return r; }

void ThrobDecoder::feed(const float* x, size_t n) {
    buf_.insert(buf_.end(), x, x + n);
    total_ += n;
    double e = 0;
    for (size_t i = 0; i < n; i++) e += (double)x[i] * x[i];
    level_ = 10 * std::log10(std::max(1e-12, e / (double)std::max<size_t>(1, n)));
    const size_t maxN = (size_t)(30 * fs_);
    if (buf_.size() > maxN) buf_.erase(buf_.begin(), buf_.begin() + (long)(buf_.size() - maxN));
    if (total_ < nextTry_ || buf_.size() < (size_t)(14 * fs_)) return;
    nextTry_ = total_ + (size_t)(6 * fs_);
    ThrobResult r;
    if (!throbDecode(buf_.data(), buf_.size(), fs_, r, variant_)) return;
    // report the characters of the window the previous report did not have: the tail of the new text after its longest overlap with the last one
    const std::string& a = lastText_;
    size_t ov = 0;
    for (size_t k = std::min(a.size(), r.text.size()); k > 0; k--) if (a.compare(a.size() - k, k, r.text, 0, k) == 0) { ov = k; break; }
    // a short overlap is chance: fall back to a substring search for the new text inside the last
    std::string fresh = r.text.substr(ov);
    if (ov < 3 && !a.empty() && a.find(r.text) != std::string::npos) fresh.clear();
    lastText_ = (a + fresh);
    if (lastText_.size() > 400) lastText_.erase(0, lastText_.size() - 400);
    if (!fresh.empty()) { r.text = fresh; out_.push_back(r); }
}

}  // namespace hamsci
}  // namespace dect2
