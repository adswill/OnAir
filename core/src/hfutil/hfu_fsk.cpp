// HF utility FSK / MFSK decoders and test generators, see hfu_fsk.h.
#include "dect2/hfutil/hfu_fsk.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <deque>
#include <map>

namespace dect2::hfu {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 8000.0;
constexpr int kDecim = 3;
using cf = std::complex<float>;

// ---- ITA2 (LSB = first element) ----
const char* kLtr = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\x0fMXV\x0e";
const char* kFig = "\0" "3\n- \a87\r$4',!:(5\")2#6019?&\x0f./;\x0e";
struct Ita2 {
    bool fig = false;
    std::string put(int c) {
        c &= 31;
        if (c == 31) { fig = false; return ""; }
        if (c == 27) { fig = true; return ""; }
        if (c == 0) return "";
        const char ch = (fig ? kFig : kLtr)[c];
        if (ch == '\x0f' || ch == '\x0e') return "";
        if (ch == '\a') return "";
        return std::string(1, ch);
    }
};
int encodeIta(char ch, bool& fig, std::vector<int>& out) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    for (int v = 1; v < 32; v++) {
        if (v == 27 || v == 31) continue;
        if (kLtr[v] == ch) { if (fig) { out.push_back(31); fig = false; } out.push_back(v); return 1; }
    }
    for (int v = 1; v < 32; v++) {
        if (v == 27 || v == 31) continue;
        if (kFig[v] == ch) { if (!fig) { out.push_back(27); fig = true; } out.push_back(v); return 1; }
    }
    return 0;
}

inline int weight5(int v) { return __builtin_popcount((unsigned)v & 31); }
// Bauer code word: 5 bits, then their repetition, inverted when the five have even parity
inline int bauerCheck(int d) { return weight5(d) % 2 == 0 ? (~d) & 31 : d & 31; }

std::vector<cf32> toneBurst(double f0, const std::vector<double>& freq, double symSec) {
    // freq per symbol, continuous phase, 24 kHz complex
    std::vector<cf32> o;
    double ph = 0;
    const double fs = 24000;
    for (double f : freq) {
        const int n = (int)std::round(symSec * fs);
        const double dp = 2 * kPi * (f0 + f) / fs;
        for (int i = 0; i < n; i++) { o.push_back(cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.3f); ph += dp; if (ph > 2 * kPi) ph -= 2 * kPi; }
    }
    return o;
}

const char* kPicc[6][6] = {{"", "E", "\n", " ", "S", "A"}, {"\r", "D", "R", "N", "F", "J"}, {"T", "Z", "L", "H", "Y", "W"}, {"I", "U", "P", "", "", "Q"}, {"C", "K", "V", "", "", ""}, {"O", "B", "G", "M", "X", ""}};
// kPicc[tone1 A..F][tone2 A..F] per the published matrix; LTR is E/F (4,5), FIG is F/F (5,5); SYNC is D/E and E/D, MI is E/E
const int kCoq1[8] = {7, 3, 1, 4, 5, 6, 2, 0};   // tone 1..8 of group 1: bits 3,4,5 (bit3 = 4)
const int kCoq2[4] = {3, 2, 1, 0};               // tone 5..8 of group 2: bits 1,2 (bit1 = 2)
inline int bitsFromCoq(int t1, int t2) { return (kCoq2[t2] * 8) | kCoq1[t1]; }   // value: b1 b2 b3 b4 b5 (b1 is the first element, stored as bit 4 here)
inline int coqToIta(int v) { int r = 0; for (int i = 0; i < 5; i++) if (v & (1 << (4 - i))) r |= 1 << i; return r; }  // to the LSB-first element order
}  // namespace

// G-TOR
void gtorGolayTable(std::vector<uint16_t>& g) {
    static const unsigned b[12] = {0xDC5, 0xB8B, 0x717, 0xE2D, 0xC5B, 0x8B7, 0x16F, 0x2DD, 0x5B9, 0xB71, 0x6E3, 0xFFE};
    g.assign(4096, 0);
    for (unsigned i = 0; i < 4096; i++) { unsigned v = 0; for (int j = 0; j < 12; j++) if (i & (0x800u >> j)) v ^= b[j]; g[i] = (uint16_t)v; }
}
uint16_t gtorCrc(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int b = 0; b < 8; b++) c = (uint16_t)((c & 1) ? (c >> 1) ^ 0x8408 : c >> 1); }
    return (uint16_t)~c;
}

// ---- Generator ----
std::vector<cf32> genFsk(const FskGenCfg& c, const std::string& text) {
    std::vector<int> ita;
    { bool fig = false; ita.push_back(31); for (char ch : text) encodeIta(ch, fig, ita); }
    std::vector<int> bits;
    double baud = c.baud, shift = c.shift, centre = c.centre;
    auto pushWord = [&](int w, int n) { for (int i = 0; i < n; i++) bits.push_back((w >> i) & 1); };
    switch (c.kind) {
    case FskKind::ArqE:
    case FskKind::ArqN: {
        std::vector<int> words;
        for (int i = 0; i < c.idleChars; i++) words.push_back(-1);
        for (int v : ita) words.push_back(v);
        for (int i = 0; i < c.idleChars; i++) words.push_back(-1);
        for (size_t i = 0; i < words.size(); i++) {
            int w;
            if (words[i] < 0) w = 1 | (0x15 << 1);                    // id = stop polarity, idle pattern (value is arbitrary for the test)
            else w = (words[i] & 31) << 1;                            // id = 0 (traffic), then ITA2
            int ones = __builtin_popcount((unsigned)w & 0x3F);
            if (ones % 2 == 0) w |= 1 << 6;                           // total number of stop polarity elements odd
            if (c.kind == FskKind::ArqE && c.cycle > 0 && (i % (size_t)c.cycle) == (size_t)c.cycle - 1) w = (~w) & 0x7F;   // every 4th / 8th character inverted
            pushWord(w, 7);
        }
        break;
    }
    case FskKind::Autospec:
    case FskKind::Spread: {
        const int S = c.kind == FskKind::Autospec ? 1 : c.depth;
        std::vector<int> chars;
        for (int i = 0; i < c.idleChars; i++) chars.push_back(21);
        for (int v : ita) chars.push_back(v);
        for (int i = 0; i < c.idleChars; i++) chars.push_back(21);
        const size_t n = chars.size();
        std::vector<int> stream(n * 10 + 9 * (size_t)S + 1, 0);
        for (size_t ch = 0; ch < n; ch++) {
            const int d = chars[ch], w = d | (bauerCheck(d) << 5);
            for (int k = 0; k < 10; k++) stream[10 * ch + (size_t)k * S] = (w >> k) & 1;
        }
        for (int v : stream) bits.push_back(v);
        break;
    }
    case FskKind::GTor: {
        baud = c.gtorBaud; shift = 170;
        const int nBytes = (int)(baud * 0.24);          // 24 / 48 / 72 byte frames
        std::vector<uint16_t> g;
        gtorGolayTable(g);
        std::vector<cf32> out(0);
        std::vector<uint8_t> data;
        for (size_t i = 0; i < text.size(); i++) data.push_back((uint8_t)text[i]);
        const int payload = nBytes - 3;
        size_t pos = 0;
        std::vector<double> allF;
        std::vector<double> seq;
        for (int fr = 0; pos < data.size() || fr < 2; fr++) {
            std::vector<uint8_t> f;
            for (int i = 0; i < payload; i++) f.push_back(pos < data.size() ? data[pos++] : 0x1E);
            f.push_back((uint8_t)(fr & 3));       // status: data, no compression, block number
            const uint16_t crc = gtorCrc(f.data(), f.size());
            f.push_back((uint8_t)(crc >> 8)); f.push_back((uint8_t)crc);
            std::vector<int> tri;
            for (size_t i = 0; i + 3 <= f.size(); i += 3) { tri.push_back((f[i] << 4) | (f[i + 1] >> 4)); tri.push_back(((f[i + 1] & 15) << 8) | f[i + 2]); }
            if (c.gtorGolay) for (auto& t : tri) t = g[(size_t)t];
            const int nT = (int)tri.size();
            for (int b = 11; b >= 0; b--) for (int t = 0; t < nT; t++) seq.push_back(((tri[(size_t)t] >> b) & 1) ? 1.0 : 0.0);
            for (int i = 0; i < (int)(baud * 0.48); i++) seq.push_back(-1.0);   // the 0.48 s pause of the 2.4 s cycle (silent)
        }
        std::vector<double> sequ;
        for (double v : seq) sequ.push_back(v);
        const double fs = 24000;
        const int nSym = (int)(fs / baud);
        double ph = 0;
        std::vector<cf32> sig;
        sig.assign((size_t)(c.leadSec * fs), cf32(0, 0));
        for (double v : sequ) {
            for (int i = 0; i < nSym; i++) {
                if (v < 0) { sig.push_back(cf32(0, 0)); continue; }
                const double f = centre + (v > 0.5 ? shift / 2 : -shift / 2);
                ph += 2 * kPi * f / fs;
                if (ph > 2 * kPi) ph -= 2 * kPi;
                sig.push_back(cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.3f);
            }
        }
        sig.resize(sig.size() + (size_t)(c.tailSec * fs), cf32(0, 0));
        return sig;
    }
    case FskKind::Efr: {
        baud = 200; shift = 340;
        auto addByte = [&](int b) {
            int p = __builtin_popcount((unsigned)b) & 1;
            bits.push_back(0);
            for (int i = 0; i < 8; i++) bits.push_back((b >> i) & 1);
            bits.push_back(p);
            bits.push_back(1);
        };
        std::vector<uint8_t> msg;
        for (char ch : text) msg.push_back((uint8_t)ch);
        for (int rep = 0; rep < 2; rep++) {   // messages are sent twice
            int mn = 37, a1 = 12, a2 = 34;
            std::vector<uint8_t> payload;
            payload.push_back((uint8_t)mn); payload.push_back((uint8_t)a1); payload.push_back((uint8_t)a2);
            payload.insert(payload.end(), msg.begin(), msg.begin() + (long)std::min<size_t>(16, msg.size()));
            for (int i = 0; i < 30; i++) bits.push_back(1);        // idle mark between frames
            addByte(0x68); addByte((int)payload.size()); addByte((int)payload.size()); addByte(0x68);
            int sum = 0;
            for (auto b : payload) { addByte(b); sum = (sum + b) & 255; }
            addByte(sum);
            addByte(0x16);
            for (int i = 0; i < 120; i++) bits.push_back(1);
        }
        break;
    }
    case FskKind::Piccolo6:
    case FskKind::Coquelet8: {
        const bool picc = c.kind == FskKind::Piccolo6;
        const double base = picc ? 460.0 : 773.0, sp = picc ? 20.0 : 26.67;
        std::vector<double> tones;
        auto tone = [&](int n) { tones.push_back(base + n * sp - centre); };
        for (int i = 0; i < c.idleChars; i++) {
            if (picc) { if (i % 2 == 0) { tone(3); tone(4); } else { tone(4); tone(3); } }   // SYNC and the barred SYNC, so the pair D/D never appears
            else { tone(0); tone(7); }                      // idle: tones 1 and 8 alternate
        }
        if (picc) {
            for (int v : ita) {
                int a = -1, b = -1;
                const std::string ch(1, kLtr[v]);
                if (v == 31) { a = 4; b = 5; }                   // LTR
                else if (v == 27) { a = 5; b = 5; }              // FIG
                else for (int t1 = 0; t1 < 6 && a < 0; t1++) for (int t2 = 0; t2 < 6; t2++) { if (ch == kPicc[t1][t2] && !ch.empty()) { a = t1; b = t2; break; } }
                if (a >= 0) { tone(a); tone(b); }
            }
        } else {
            for (int v : ita) {
                int wire = 0;
                for (int i = 0; i < 5; i++) if (v & (1 << i)) wire |= 1 << (4 - i);   // b1 first
                int t1 = 0, t2 = 0;
                for (int i = 0; i < 8; i++) if (kCoq1[i] == (wire & 7)) t1 = i;
                for (int i = 0; i < 4; i++) if (kCoq2[i] == ((wire >> 3) & 3)) t2 = i;
                tone(t1); tone(4 + t2);
            }
        }
        for (int i = 0; i < c.idleChars; i++) { if (picc) { if (i % 2 == 0) { tone(3); tone(4); } else { tone(4); tone(3); } } else { tone(0); tone(7); } }
        std::vector<cf32> s = toneBurst(centre, tones, c.toneMs * 1e-3);
        std::vector<cf32> o;
        o.assign((size_t)(c.leadSec * 24000), cf32(0, 0));
        o.insert(o.end(), s.begin(), s.end());
        o.resize(o.size() + (size_t)(c.tailSec * 24000), cf32(0, 0));
        return o;
    }
    }
    const double fs = 24000;
    const double ns = fs / baud;
    std::vector<cf32> o;
    o.assign((size_t)(c.leadSec * fs), cf32(0, 0));
    double ph = 0;
    double acc = 0;
    for (int v : bits) {
        const double f = centre + (v ? shift / 2 : -shift / 2);
        acc += ns;
        const int n = (int)std::floor(acc);
        acc -= n;
        for (int i = 0; i < n; i++) { ph += 2 * kPi * f / fs; if (ph > 2 * kPi) ph -= 2 * kPi; o.push_back(cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.3f); }
    }
    o.resize(o.size() + (size_t)(c.tailSec * fs), cf32(0, 0));
    return o;
}

// ---- Receiver ----
struct FskRx::Impl {
    FskKind kind;
    double forcedBaud = 0, forcedShift = 0;
    std::function<void(const std::string&)> textCb, evCb;
    FskStatus st;
    std::vector<cf> y;              // 8 kHz complex
    int decCnt = 0;
    cf decAcc{0, 0};
    double ph = 0;
    size_t consumed = 0;

    // ---- tone search ----
    double f0 = 0, f1 = 0, baud = 0;
    bool haveTones = false, haveMfsk = false, refined = false;
    double mfBase = 0, slotSamples = 0;

    // ---- symbol processing ----
    double tNext = 0;
    double phase0 = 0, phase1 = 0;
    std::vector<float> softs;       // one per symbol: > 0 means tone 1 (mark)
    size_t softBase = 0;

    explicit Impl(FskKind k) : kind(k) { clear(); }

    void clear() {
        y.clear(); decCnt = 0; decAcc = {0, 0}; ph = 0; consumed = 0;
        haveTones = false; haveMfsk = false; refined = false; pairPhase = -1; mfReady = false; f0 = f1 = 0; baud = 0; tNext = 0; softs.clear(); softBase = 0;
        lockOff = -1; lockPol = 0; lockCycle = 0; lockPhase = 0; lockDepth = 1;
        text.clear(); ita = Ita2(); words = 0; mbits.clear(); mPos = 0;
        st = FskStatus();
    }
    std::string text;
    Ita2 ita;
    int lockOff = -1, lockPol = 0, lockCycle = 0, lockPhase = 0, lockDepth = 1;
    uint64_t words = 0;

    void out(const std::string& s) { if (!s.empty() && textCb) textCb(s); if (!s.empty()) { text += s; st.state = 2; st.good += s.size(); } }
    void ev(const std::string& s) { if (evCb) evCb(s); }

    // FFT size N of a real-valued power spectrum of the last n samples
    static void fftc(std::vector<cf>& a) {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; i++) { size_t b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b; if (i < j) std::swap(a[i], a[j]); }
        for (size_t len = 2; len <= n; len <<= 1) {
            const double ang = -2 * kPi / (double)len;
            const cf wl((float)std::cos(ang), (float)std::sin(ang));
            for (size_t i = 0; i < n; i += len) { cf w(1, 0); for (size_t k = 0; k < len / 2; k++) { cf u = a[i + k], v = a[i + k + len / 2] * w; a[i + k] = u + v; a[i + k + len / 2] = u - v; w *= wl; } }
        }
    }
    std::vector<double> power(size_t startFromEnd, size_t N) {
        std::vector<cf> b(N, cf(0, 0));
        const size_t s = y.size() - startFromEnd;
        for (size_t i = 0; i < N; i++) { const double w = 0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)N); b[i] = y[s + i] * (float)w; }
        fftc(b);
        std::vector<double> p(N / 2);
        for (size_t i = 0; i < N / 2; i++) p[i] = std::norm(b[i]);   // positive audio frequencies only
        return p;
    }

    bool findFskTones() {
        const size_t N = 8192;
        if (y.size() < N) return false;
        auto p = power(N, N);
        const double bin = kFs / (double)N;
        // two strongest bins apart by a plausible shift
        std::vector<int> peaks;
        for (int i = 2; i + 2 < (int)p.size(); i++) { if (p[(size_t)i] > p[(size_t)i - 1] && p[(size_t)i] >= p[(size_t)i + 1]) peaks.push_back(i); }
        std::sort(peaks.begin(), peaks.end(), [&](int a, int b) { return p[(size_t)a] > p[(size_t)b]; });
        double mean = 0;
        for (auto v : p) mean += v;
        mean /= (double)p.size();
        double wantS = forcedShift > 0 ? forcedShift : kind == FskKind::Efr ? 340 : 0;
        for (size_t a = 0; a < std::min<size_t>(peaks.size(), 12); a++)
            for (size_t b = a + 1; b < std::min<size_t>(peaks.size(), 12); b++) {
                double hzA = peaks[a] * bin, hzB = peaks[b] * bin;
                double sh = std::fabs(hzA - hzB);
                if (sh < 60 || sh > 1100) continue;
                if (wantS > 0 && std::fabs(sh - wantS) > 40) continue;
                if (p[(size_t)peaks[b]] < 0.015 * p[(size_t)peaks[a]] || p[(size_t)peaks[b]] < 12 * mean) continue;
                if (hzA < 100 || hzB < 100 || hzA > 3900 || hzB > 3900) continue;
                f0 = std::min(hzA, hzB); f1 = std::max(hzA, hzB);
                haveTones = true;
                st.state = std::max(st.state, 1);
                st.shiftHz = f1 - f0; st.centreHz = (f0 + f1) / 2;
                return true;
            }
        return false;
    }

    // baud candidates of this mode
    std::vector<double> bauds() {
        if (forcedBaud > 0) return {forcedBaud};
        switch (kind) {
        case FskKind::ArqE: return {46.2, 48, 50, 64, 72, 86, 96, 144, 184.6, 192, 288};
        case FskKind::ArqN: return {48, 64, 72, 86, 96, 192};
        case FskKind::Autospec: return {62.3, 68.5, 102.63, 137};
        case FskKind::Spread: return {68.5, 102.7, 137, 218};
        case FskKind::GTor: return {100, 200, 300};
        case FskKind::Efr: return {200};
        default: return {};
        }
    }

    // The baud from the eye instead (3.9 sensitivity pass D): noise adds transitions, so the clock line above fell under a quarter of
    // them in noise that the slicer copes with (G-TOR 100 needed 6 dB in 3 kHz with it, -2 dB with the baud given). For each candidate the
    // two tone energies are integrated over one of its symbols and the decisions sampled at 12 clock phases: at the right baud (and at its
    // multiples) the best phase gives decisions far sharper than the worst; at a fraction of it the windows straddle symbols. The lowest
    // candidate with an eye within 15 % of the best wins, and only an eye of at least kEyeMin counts (noise alone: about 1.1). It goes
    // first; the clock line is the fallback (it also picked a multiple of the G-TOR 100 baud at 7 dB, one trial in three).
    static constexpr double kEyeMin = 1.6;
    bool findBaudEye() {
        auto cand = bauds();
        const size_t n = std::min<size_t>(y.size(), 32768);
        if (n < 8192) return false;
        const size_t st0 = y.size() - n;
        std::vector<cf> m0(n), m1(n);
        {
            const cf w0((float)std::cos(-2 * kPi * f0 / kFs), (float)std::sin(-2 * kPi * f0 / kFs));
            const cf w1((float)std::cos(-2 * kPi * f1 / kFs), (float)std::sin(-2 * kPi * f1 / kFs));
            cf r0(1, 0), r1(1, 0);
            for (size_t i = 0; i < n; i++) {
                m0[i] = y[st0 + i] * r0; m1[i] = y[st0 + i] * r1; r0 *= w0; r1 *= w1;
                if ((i & 255) == 0) { r0 /= std::abs(r0); r1 /= std::abs(r1); }
            }
        }
        std::vector<double> eye(cand.size(), 0), d(n);
        for (size_t c = 0; c < cand.size(); c++) {
            const double Ts = kFs / cand[c];
            const int L = std::max(2, (int)std::lround(Ts));
            if ((size_t)L * 40 > n) continue;
            cf a0(0, 0), a1(0, 0);
            for (size_t i = 0; i < n; i++) {
                a0 += m0[i]; a1 += m1[i];
                if (i >= (size_t)L) { a0 -= m0[i - L]; a1 -= m1[i - L]; }
                d[i] = std::norm(a1) - std::norm(a0);   // the symbol that ends at i
            }
            double hi = 0, lo = 1e300;
            for (int k = 0; k < 12; k++) {
                double sum = 0; int cnt = 0;
                for (double t = L + Ts * k / 12.0; t < (double)n; t += Ts) { sum += std::fabs(d[(size_t)t]); cnt++; }
                if (!cnt) continue;
                sum /= cnt;
                hi = std::max(hi, sum); lo = std::min(lo, sum);
            }
            eye[c] = lo > 0 ? hi / lo : 0;
        }
        double best = 0;
        for (double e : eye) best = std::max(best, e);
        if (best < kEyeMin) return false;
        for (size_t c = 0; c < cand.size(); c++)
            if (eye[c] >= 0.85 * best) { baud = cand[c]; st.baud = baud; return true; }
        return false;
    }
    bool findBaud() {
        if (forcedBaud <= 0 && findBaudEye()) return true;
        // the transition signal of the demodulated bits: power at the baud against nearby frequencies
        auto cand = bauds();
        const size_t n = std::min<size_t>(y.size(), 32768);
        const size_t st0 = y.size() - n;
        std::vector<float> disc(n);
        double p0 = 0, p1 = 0;
        const cf w0((float)std::cos(-2 * kPi * f0 / kFs), (float)std::sin(-2 * kPi * f0 / kFs));
        const cf w1((float)std::cos(-2 * kPi * f1 / kFs), (float)std::sin(-2 * kPi * f1 / kFs));
        cf r0(1, 0), r1(1, 0), a0(0, 0), a1(0, 0);
        const int win = 16;
        std::vector<cf> la0(win, cf(0, 0)), la1(win, cf(0, 0));
        for (size_t i = 0; i < n; i++) {
            const cf s = y[st0 + i];
            const cf m0 = s * r0, m1 = s * r1;
            r0 *= w0; r1 *= w1;
            if ((i & 255) == 0) { r0 /= std::abs(r0); r1 /= std::abs(r1); }
            a0 += m0 - la0[i % win]; la0[i % win] = m0;
            a1 += m1 - la1[i % win]; la1[i % win] = m1;
            disc[i] = std::norm(a1) - std::norm(a0);
            p0 += std::fabs(disc[i]);
        }
        (void)p1;
        // transitions
        double mean = p0 / (double)n;
        std::vector<float> trans(n, 0);
        int last = 0;
        for (size_t i = 1; i < n; i++) { int cur = disc[i] > 0.2 * mean ? 1 : disc[i] < -0.2 * mean ? -1 : last; if (last && cur != last) trans[i] = 1; if (cur) last = cur; }
        double bestB = 0, bestV = 0;
        std::vector<std::pair<double, double>> vals;
        for (double b : cand) {
            cf s(0, 0);
            for (size_t i = 0; i < n; i++) if (trans[i] != 0) { const double a = -2 * kPi * b * (double)i / kFs; s += cf((float)std::cos(a), (float)std::sin(a)); }
            const double v = std::abs(s);
            vals.push_back({b, v});
            if (v > bestV) bestV = v;
        }
        for (const auto& kv : vals) if (kv.second >= 0.7 * bestV) { bestB = kv.first; break; }   // a harmonic of the clock is not the clock
        double total = 0;
        for (auto v : trans) total += v;
        if (bestB <= 0 || total < 20 || bestV < 0.25 * total) return false;
        baud = bestB;
        st.baud = baud;
        return true;
    }

    // decide symbols from the stored 8 kHz signal (integrate over a symbol, noncoherent)
    void slice() {
        const double Ts = kFs / baud;
        const double period = Ts;
        while (tNext + period + 2 < (double)y.size() + (double)consumed) {
            const double start = tNext - (double)consumed;
            if (start < 0) { tNext += period; continue; }
            const size_t a = (size_t)start, b = std::min<size_t>(y.size(), (size_t)(start + period));
            cf e0(0, 0), e1(0, 0);
            double pa = phase0, pb = phase1;
            const double da = -2 * kPi * f0 / kFs, db = -2 * kPi * f1 / kFs;
            for (size_t i = a; i < b; i++) { e0 += y[i] * cf((float)std::cos(pa), (float)std::sin(pa)); e1 += y[i] * cf((float)std::cos(pb), (float)std::sin(pb)); pa += da; pb += db; }
            phase0 = std::remainder(pa, 2 * kPi); phase1 = std::remainder(pb, 2 * kPi);
            softs.push_back(std::norm(e1) - std::norm(e0));
            tNext += period;
        }
    }

    // symbol clock: over the available symbols pick the sub-symbol offset with the sharpest decisions
    bool timing() {
        const double Ts = kFs / baud;
        const size_t want = (size_t)(160 * Ts);
        if (y.size() < want + (size_t)Ts * 2) return false;
        const size_t a = y.size() - want;
        double bestS = -1, bestPh = 0;
        for (int k = 0; k < 12; k++) {
            const double off = Ts * k / 12.0;
            double metric = 0;
            for (double t = off; t + Ts < (double)want; t += Ts) {
                cf e0(0, 0), e1(0, 0);
                const size_t i0 = (size_t)t, i1 = (size_t)(t + Ts);
                double pa = -2 * kPi * f0 * ((double)a + (double)i0) / kFs, pb = -2 * kPi * f1 * ((double)a + (double)i0) / kFs;
                const double da = -2 * kPi * f0 / kFs, db = -2 * kPi * f1 / kFs;
                for (size_t i = i0; i < i1; i++) { e0 += y[a + i] * cf((float)std::cos(pa), (float)std::sin(pa)); e1 += y[a + i] * cf((float)std::cos(pb), (float)std::sin(pb)); pa += da; pb += db; }
                metric += std::fabs(std::norm(e1) - std::norm(e0));
            }
            if (metric > bestS) { bestS = metric; bestPh = off; }
        }
        if (!refined) {
            // the spectrum peaks of keyed tones sit near but not on the tones: search the pair around them for the sharpest symbol decisions
            refined = true;
            double bestM = -1, bf0 = f0, bf1 = f1;
            const size_t i0 = (size_t)bestPh;
            auto metricAt = [&](double fa, double fb) {
                double total = 0;
                for (double t = (double)i0; t + Ts < (double)want; t += Ts) {
                    cf e0(0, 0), e1(0, 0);
                    const size_t q0 = (size_t)t, q1 = (size_t)(t + Ts);
                    double pa = -2 * kPi * fa * ((double)a + (double)q0) / kFs, pb = -2 * kPi * fb * ((double)a + (double)q0) / kFs;
                    const double da = -2 * kPi * fa / kFs, db = -2 * kPi * fb / kFs;
                    for (size_t i = q0; i < q1; i++) { e0 += y[a + i] * cf((float)std::cos(pa), (float)std::sin(pa)); e1 += y[a + i] * cf((float)std::cos(pb), (float)std::sin(pb)); pa += da; pb += db; }
                    total += std::fabs(std::norm(e1) - std::norm(e0));
                }
                return total;
            };
            for (int d0 = -36; d0 <= 36; d0 += 6) for (int d1 = -36; d1 <= 36; d1 += 6) {
                const double m = metricAt(f0 + d0, f1 + d1);
                if (m > bestM) { bestM = m; bf0 = f0 + d0; bf1 = f1 + d1; }
            }
            f0 = bf0; f1 = bf1;
            st.shiftHz = f1 - f0; st.centreHz = (f0 + f1) / 2;
            return timing();
        }
        const double rel = (double)a + bestPh - Ts * std::floor(((double)a + bestPh) / Ts);   // back to the oldest buffered symbol with this clock phase
        tNext = (double)consumed + rel;
        phase0 = -2 * kPi * f0 * rel / kFs;
        phase1 = -2 * kPi * f1 * rel / kFs;
        softs.clear();
        return true;
    }

    // ---- ARQ-E / ARQ-N words ----
    std::vector<uint8_t> mbits;
    size_t mPos = 0;

    void wordsArq() {
        const size_t W = 7;
        const size_t N = 120;
        while (lockOff < 0 && mbits.size() >= (N + 2) * W) {
            // score all alignments, polarities, cycle hypotheses
            int bo = -1, bp = 0, bc = 0, bph = 0;
            double bs = 0;
            for (int pol = 0; pol < 2; pol++) for (int off = 0; off < 7; off++) {
                // word pass/fail
                std::vector<int> passes(N);
                for (size_t w = 0; w < N; w++) {
                    int ones = 0;
                    for (size_t i = 0; i < W; i++) ones += mbits[(size_t)off + w * W + i] ^ (pol ? 1 : 0);
                    passes[w] = ones % 2;           // odd = 1
                }
                for (int cyc : {0, 4, 8}) {
                    if (kind == FskKind::ArqN && cyc) continue;
                    for (int phi = 0; phi < std::max(1, cyc); phi++) {
                        int ok = 0;
                        for (size_t w = 0; w < N; w++) { const bool inv = cyc && (int)(w % (size_t)cyc) == phi; ok += inv ? !passes[w] : passes[w]; }
                        double s = (double)ok / (double)N;
                        if (cyc == 0) s -= 0.02;     // a plain odd check also passes by chance on inverted streams: prefer the cycle when it fits equally
                        if (s > bs + 1e-9 && s >= 0.93) { bs = s; bo = off; bp = pol; bc = cyc; bph = phi; }
                    }
                }
            }
            if (bo >= 0) {
                // an idle pattern passes the parity check at any rotation (and noise varies a few words): only lock on real traffic, varied words whose
                // identification element says "traffic"
                std::map<int, int> seen;
                int traffic = 0;
                for (size_t w = 0; w < N; w++) {
                    int v = 0;
                    for (size_t i = 0; i < W; i++) v |= (mbits[(size_t)bo + w * W + i] ^ (bp ? 1 : 0)) << i;
                    if (bc && (int)(w % (size_t)bc) == bph) v = (~v) & 0x7F;
                    if (!(v & 1)) { traffic++; seen[v]++; }
                }
                if ((int)seen.size() < 10 || traffic < (int)N / 5) bo = -1;
            }
            if (bo < 0) { mbits.erase(mbits.begin(), mbits.begin() + 7); continue; }
            lockOff = bo; lockPol = bp; lockCycle = bc; lockPhase = bph;
            st.detail = bc ? "cycle " + std::to_string(bc) : "no inversion";
            st.state = 2;
            char m[120]; std::snprintf(m, sizeof m, "%s word sync: %.1f Bd, shift %.0f Hz, %s", kind == FskKind::ArqE ? "ARQ-E" : "ARQ-N", baud, st.shiftHz, st.detail.c_str());
            ev(m);
            wordIndex = 0;
            mbits.erase(mbits.begin(), mbits.begin() + lockOff);
            lockOff = 0;
        }
        if (lockOff < 0) return;
        int ctrl = 0;
        while (mbits.size() >= W) {
            int w = 0;
            for (size_t i = 0; i < W; i++) w |= (mbits[i] ^ (lockPol ? 1 : 0)) << i;
            mbits.erase(mbits.begin(), mbits.begin() + (long)W);
            const bool inv = lockCycle && (int)(wordIndex % (uint64_t)lockCycle) == lockPhase;
            wordIndex++;
            if (inv) w = (~w) & 0x7F;
            const int ones = __builtin_popcount((unsigned)w);
            if (ones % 2 == 0) { st.bad++; lockErr = lockErr * 0.95 + 1; if (lockErr > 8) { ev("word sync lost"); lockOff = -1; lockErr = 0; return; } continue; }
            lockErr *= 0.95;
            if (w & 1) { ctrl++; continue; }                     // the identification element at stop polarity: a control character (idle, RQ)
            out(ita.put((w >> 1) & 31));
        }
        st.good += (uint64_t)ctrl;
    }
    uint64_t wordIndex = 0;
    double lockErr = 0;

    // ---- Bauer (Autospec, Spread) ----
    int bauerDecode(int w, int& dist) {
        int best = 0, bd = 11;
        for (int d = 0; d < 32; d++) { const int c = d | (bauerCheck(d) << 5); const int dd = __builtin_popcount((unsigned)(c ^ w)); if (dd < bd) { bd = dd; best = d; } }
        dist = bd;
        return best;
    }

    void wordsBauer() {
        std::vector<int> depths = kind == FskKind::Autospec ? std::vector<int>{1} : std::vector<int>{11, 21, 51};
        const size_t N = 90;
        auto wordAt = [&](size_t ch, int off, int S, int pol) {
            int w = 0;
            for (int k = 0; k < 10; k++) {
                const size_t t = (size_t)off + 10 * ch + (size_t)k * (size_t)S;
                w |= (mbits[t] ^ (pol ? 1 : 0)) << k;
            }
            return w;
        };
        while (lockOff < 0 && mbits.size() >= 10 * N + 9 * 51 + 12) {
            int bo = -1, bpol = 0, bS = 1;
            double bs = 0;
            for (int S : depths) for (int pol = 0; pol < 2; pol++) for (int off = 0; off < 10; off++) {
                int ok = 0;
                for (size_t ch = 0; ch < N; ch++) {
                    const int w = wordAt(ch, off, S, pol);
                    if ((w >> 5) == bauerCheck(w & 31)) ok++;
                }
                const double s = (double)ok / (double)N;
                if (s > bs) { bs = s; bo = off; bpol = pol; bS = S; }
            }
            bool varied = false;
            if (bs >= 0.80) {
                std::map<int, int> seen;
                for (size_t ch = 0; ch < N; ch++) seen[wordAt(ch, bo, bS, bpol) & 31]++;
                int top = 0; for (auto& kv : seen) top = std::max(top, kv.second);
                varied = seen.size() >= 10 && top <= (int)N * 3 / 5;
            }
            if (bs < 0.80 || !varied) { mbits.erase(mbits.begin(), mbits.begin() + 10); continue; }   // an idle-only window is ambiguous
            lockOff = 0;
            mbits.erase(mbits.begin(), mbits.begin() + bo);
            lockPol = bpol;
            lockDepth = bS;
            st.detail = kind == FskKind::Autospec ? "Mk 1" : "SPREAD " + std::to_string(bS);
            char m[120]; std::snprintf(m, sizeof m, "%s sync: %.1f Bd, shift %.0f Hz (%.0f %% of the code words consistent)", kind == FskKind::Autospec ? "AUTOSPEC" : st.detail.c_str(), baud, st.shiftHz, 100 * bs);
            ev(m);
            st.state = 2;
        }
        if (lockOff < 0) return;
        while (mbits.size() >= 10 * 3 + 9 * (size_t)lockDepth + 2) {
            const int w = wordAt(0, 0, lockDepth, lockPol);
            mbits.erase(mbits.begin(), mbits.begin() + 10);
            int dist = 0;
            const int d = bauerDecode(w, dist);
            if (dist > 1) { st.bad++; lockErr = lockErr * 0.97 + 1; if (lockErr > 12) { ev("sync lost"); lockOff = -1; lockErr = 0; return; } out("_"); continue; }
            lockErr *= 0.97;
            out(ita.put(d));
        }
    }

    // ---- G-TOR ----
    std::vector<uint16_t> golay;
    std::vector<uint8_t> gtPrev;
    int gtSinceLast = 0;

    bool gtorTry(const std::vector<int>& window) {
        const int nBytes = (int)std::lround(baud * 0.24);
        const int n = nBytes * 8;
        if ((int)window.size() < n) return false;
        const int nT = n / 12;
        std::vector<int> tri((size_t)nT, 0);
        for (int b = 0; b < 12; b++) for (int t = 0; t < nT; t++) { const int bit = window[(size_t)(b * nT + t)]; if (bit) tri[(size_t)t] |= 1 << (11 - b); }
        auto check = [&](const std::vector<int>& ts, bool isGolay, bool invert) {
            std::vector<uint8_t> f;
            for (size_t i = 0; i + 1 < ts.size(); i += 2) {
                const int a = ts[i] & 0xFFF, b = ts[i + 1] & 0xFFF;
                f.push_back((uint8_t)(a >> 4)); f.push_back((uint8_t)(((a & 15) << 4) | (b >> 8))); f.push_back((uint8_t)b);
            }
            const uint16_t got = (uint16_t)((f[f.size() - 2] << 8) | f[f.size() - 1]);
            if (gtorCrc(f.data(), f.size() - 2) != got) return false;
            const int status = f[f.size() - 3];
            int cmd = (status >> 6) & 3, comp = (status >> 2) & 3, blk = status & 3;
            std::string body;
            const size_t end = f.size() - 3;
            if (comp == 0) {
                for (size_t i = 0; i < end; i++) { if (f[i] == 0x1E) break; if (f[i] == 0x1C && i + 1 < end) { i++; body += (char)(f[i] == 0x7E ? 0x1E : 0x1C); continue; } body += (char)f[i]; }
            } else for (size_t i = 0; i < end; i++) { char t[4]; std::snprintf(t, sizeof t, "%02X", f[i]); body += t; }
            static const char* cmds[4] = {"data", "changeover request", "disconnect", "connect"};
            static const char* comps[4] = {"", " Huffman (not expanded)", " swapped Huffman (not expanded)", ""};
            char m[160];
            std::snprintf(m, sizeof m, "G-TOR %.0f Bd %s block %d%s%s%s", baud, cmds[cmd], blk, isGolay ? " (Golay form)" : "", invert ? " (inverted)" : "", comps[comp]);
            ev(m);
            st.detail = std::to_string((int)baud) + " Bd";
            st.state = 2;
            st.good++;
            if (cmd == 0 && comp == 0) out(body);
            else if (cmd == 3 || cmd == 2) {
                // connect / disconnect frames: seven bytes per station with their marker bits; show the raw call letters
                std::string call;
                for (size_t i = 0; i < 20 && i < end; i++) { uint8_t v = f[i]; if (i % 3 == 2) { v = (uint8_t)(((v & 0x0F) << 4) | (v >> 4)); v &= 0x7F; } if (v >= 0x20 && v < 0x7F) call += (char)v; }
                ev("G-TOR " + std::string(cmds[cmd]) + " frame, call field: " + call);
            }
            return true;
        };
        std::vector<int> pos = tri, g = tri, inv = tri, invg = tri;
        for (size_t i = 0; i < g.size(); i++) { g[i] = golay[(size_t)tri[i] & 0xFFF]; inv[i] = (~tri[i]) & 0xFFF; invg[i] = golay[(size_t)inv[i]]; }
        if (check(pos, false, false)) return true;
        if (check(g, true, false)) return true;
        if (check(inv, false, true)) return true;
        if (check(invg, true, true)) return true;
        return false;
    }

    void wordsGTor() {
        if (golay.empty()) gtorGolayTable(golay);
        const size_t n = (size_t)std::lround(baud * 1.92);
        while (mbits.size() > n) {
            std::vector<int> w(mbits.begin(), mbits.begin() + (long)n);
            if (gtorTry(w)) {
                mbits.erase(mbits.begin(), mbits.begin() + (long)n);
            } else mbits.erase(mbits.begin());
        }
    }

    // ---- EFR (DIN 19244 frame) ----
    std::vector<int> efrBytes;

    void wordsEfr() {
        const size_t W = 11;
        // frame sync: start bit 0, stop 1, even parity
        while (mbits.size() >= 200 && lockOff < 0) {
            int bo = -1, bp = 0;
            double bs = 0;
            for (int pol = 0; pol < 2; pol++) for (int off = 0; off < 11; off++) {
                int ok = 0, tot = 0;
                for (size_t w = 0; (size_t)off + (w + 1) * W <= mbits.size() && w < 9; w++) {
                    int v = 0;
                    for (int i = 0; i < 11; i++) v |= (mbits[(size_t)off + w * W + (size_t)i] ^ (pol ? 1 : 0)) << i;
                    if ((v & 1) == 0 && (v >> 10) == 1) ok++;
                    tot++;
                }
                if (tot >= 8) { const double s = (double)ok / tot; if (s > bs + 1e-9) { bs = s; bo = off; bp = pol; } }
            }
            if (bs < 0.9 || bo < 0) { mbits.erase(mbits.begin(), mbits.begin() + 11); continue; }
            mbits.erase(mbits.begin(), mbits.begin() + bo);
            lockOff = 0; lockPol = bp;
        }
        if (lockOff < 0) return;
        while (mbits.size() >= W) {
            int v = 0;
            for (size_t i = 0; i < W; i++) v |= (mbits[i] ^ (lockPol ? 1 : 0)) << i;
            mbits.erase(mbits.begin(), mbits.begin() + (long)W);
            if ((v & 1) != 0 || (v >> 10) != 1) { lockErr = lockErr * 0.9 + 1; if (lockErr > 3) { lockOff = -1; lockErr = 0; efrBytes.clear(); return; } continue; }
            lockErr *= 0.9;
            const int b = (v >> 1) & 255, p = (v >> 9) & 1;
            if ((__builtin_popcount((unsigned)b) & 1) != p) { st.bad++; efrBytes.clear(); continue; }
            efrBytes.push_back(b);
            efrParse();
        }
    }

    void efrParse() {
        for (;;) {
            size_t s = 0;
            while (s < efrBytes.size() && efrBytes[s] != 0x68) s++;
            if (s > 0) efrBytes.erase(efrBytes.begin(), efrBytes.begin() + (long)s);
            if (efrBytes.size() < 4) return;
            if (efrBytes[1] != efrBytes[2] || efrBytes[3] != 0x68) { efrBytes.erase(efrBytes.begin()); continue; }
            const int L = efrBytes[1];
            if (L < 3 || L > 40) { efrBytes.erase(efrBytes.begin()); continue; }
            const size_t total = 4 + (size_t)L + 2;
            if (efrBytes.size() < total) return;
            int sum = 0;
            for (int i = 0; i < L; i++) sum = (sum + efrBytes[(size_t)(4 + i)]) & 255;
            const bool ok = sum == efrBytes[(size_t)(4 + L)] && efrBytes[(size_t)(4 + L + 1)] == 0x16;
            char m[80];
            std::snprintf(m, sizeof m, "EFR telegram %s: message %d, address %d / %d, %d data bytes", ok ? "ok" : "bad checksum", efrBytes[4], efrBytes[5], efrBytes[6], L - 3);
            std::string hex;
            for (int i = 3; i < L; i++) { char t[4]; std::snprintf(t, sizeof t, "%02X", efrBytes[(size_t)(4 + i)]); hex += t; }
            ev(std::string(m) + ": " + hex);
            if (ok) { st.good++; st.state = 2; st.detail = "200 Bd, DIN 19244 frames"; out(hex + "\n"); } else st.bad++;
            efrBytes.erase(efrBytes.begin(), efrBytes.begin() + (long)total);
        }
    }

    // ---- MFSK tone pairs ----
    std::vector<int> slotTones;
    size_t slotPos = 0;
    int nTones() const { return kind == FskKind::Piccolo6 ? 6 : 8; }
    double toneSpacing() const { return kind == FskKind::Piccolo6 ? 20.0 : 26.67; }

    bool findMfsk() {
        const size_t N = 8192;
        if (y.size() < N) return false;
        auto p = power(N, N);
        const double bin = kFs / (double)N, sp = toneSpacing();
        const int K = nTones();
        double best = 0, bestF = 0;
        std::vector<double> sm(p.size());
        for (int i = 1; i + 1 < (int)p.size(); i++) sm[(size_t)i] = p[(size_t)i - 1] + p[(size_t)i] + p[(size_t)i + 1];
        for (double f = 300; f < 3000 - K * sp; f += 1) {
            double e = 0;
            for (int t = 0; t < K; t++) e += sm[(size_t)((f + t * sp) / bin + 0.5)];
            if (e > best) { best = e; bestF = f; }
        }
        double mean = 0;
        for (auto v : sm) mean += v;
        mean /= (double)sm.size();
        if (best < 20 * mean) return false;
        if (kind == FskKind::Piccolo6) {
            // while it idles only the SYNC tones D and E (0-based 3 and 4) are on the air
            std::vector<int> strong;
            for (int t = 0; t < K; t++) { const double v = sm[(size_t)((bestF + t * sp) / bin + 0.5)]; if (v > 0.25 * best) strong.push_back(t); }
            if ((int)strong.size() == 2 && strong[1] == strong[0] + 1) bestF += (strong[0] - 3) * sp;
        }
        mfBase = bestF;
        haveMfsk = true;
        st.centreHz = bestF + (K - 1) * sp / 2;
        st.state = std::max(st.state, 1);
        return true;
    }

    std::vector<double> mfskCandidates() { return kind == FskKind::Piccolo6 ? std::vector<double>{50.0} : std::vector<double>{37.5, 50.0, 75.0}; }

    double mfTs = 0, mfNext = 0;
    bool mfReady = false;

    bool mfTiming() {
        // tone duration and slot phase: maximise how much energy goes to the winning tone in a slot
        double bestV = 0, bestT = 0, bestOff = 0;
        const size_t avail = y.size();
        for (double ms : mfskCandidates()) {
            const double Ts = ms * 1e-3 * kFs;
            const size_t need = (size_t)(Ts * 80);
            if (avail < need) return false;
            for (int k = 0; k < 8; k++) {
                const double off = Ts * k / 8.0;
                double metric = 0;
                for (int s = 0; s < 70; s++) {
                    const double t = (double)(avail - need) + off + s * Ts;
                    double e[12], total = 0, mx = 0;
                    slotEnergy(t, Ts, e);
                    for (int i = 0; i < nTones(); i++) { total += e[i]; mx = std::max(mx, e[i]); }
                    metric += total > 0 ? mx / total : 0;
                }
                if (metric > bestV * 1.05 || (metric > 0.9 * bestV && Ts > bestT)) { bestV = std::max(bestV, metric); bestT = Ts; bestOff = off; }
            }
        }
        if (bestV / 70 < 0.55) return false;
        mfTs = bestT;
        mfNext = (double)consumed + (double)avail - (double)((size_t)(bestT * 80)) + bestOff;
        mfReady = true;
        slotTones.clear();
        st.detail = std::to_string((int)std::lround(mfTs / kFs * 1e3)) + " ms tones";
        return true;
    }

    void slotEnergy(double t, double Ts, double* e) {
        const size_t a = (size_t)t, b = (size_t)(t + Ts);
        for (int k = 0; k < nTones(); k++) {
            const double f = mfBase + k * toneSpacing();
            cf s(0, 0);
            double ph2 = -2 * kPi * f * t / kFs;
            const double dp = -2 * kPi * f / kFs;
            for (size_t i = a; i < b && i < y.size(); i++) { s += y[i] * cf((float)std::cos(ph2), (float)std::sin(ph2)); ph2 += dp; }
            e[k] = std::norm(s);
        }
    }

    void slotsMfsk() {
        while (mfNext + mfTs + 2 < (double)consumed + (double)y.size()) {
            double e[12];
            const double rel = mfNext - (double)consumed;
            if (rel < 0) { mfNext += mfTs; continue; }
            slotEnergy(rel, mfTs, e);
            int bi = 0;
            for (int i = 1; i < nTones(); i++) if (e[i] > e[bi]) bi = i;
            slotTones.push_back(bi);
            mfNext += mfTs;
        }
        decodePairs();
    }

    void decodePairs() {
        // decide which slots are the first tone of a pair: count how often each interpretation is valid
        if (pairPhase < 0) {
            if (slotTones.size() < 64) return;
            double s[2] = {0, 0};
            for (int ph2 = 0; ph2 < 2; ph2++) {
                int v = 0, n = 0;
                for (size_t i = (size_t)ph2; i + 1 < slotTones.size(); i += 2) {
                    n++;
                    const int a = slotTones[i], b = slotTones[i + 1];
                    if (kind == FskKind::Piccolo6) v += (a == 3 && b == 3) ? -5 : isSyncP(a, b) ? 1 : 0;   // the pair D/D is never sent
                    else v += b >= 4 ? 1 : 0;
                }
                s[ph2] = n ? (double)v / n : 0;
            }
            pairPhase = s[0] >= s[1] ? 0 : 1;
            slotPos = (size_t)pairPhase;
            ev(kind == FskKind::Piccolo6 ? "Piccolo tone pairs found" : "Coquelet tone pairs found");
        }
        while (slotPos + 1 < slotTones.size()) {
            const int a = slotTones[slotPos], b = slotTones[slotPos + 1];
            slotPos += 2;
            if (kind == FskKind::Piccolo6) {
                if (isSyncP(a, b) || (a == 4 && b == 4)) continue;   // SYNC or MI
                if (a == 3 && b == 3) continue;
                if (a == 4 && b == 5) { ita.fig = false; continue; }
                if (a == 5 && b == 5) { ita.fig = true; continue; }
                const char* c = kPicc[a][b];
                if (c && *c) {
                    std::string s = c;
                    if (ita.fig) { for (int v = 1; v < 32; v++) if (kLtr[v] == s[0] && (kFig[v])) { s = std::string(1, kFig[v]); break; } }
                    out(s);
                } else st.bad++;
            } else {
                if (a == 0 && b == 7) { idleRun++; continue; }
                if (idleRun > 0 && idleRun < 3) for (int q = 0; q < idleRun; q++) out(ita.put(coqToIta(bitsFromCoq(0, 3))));   // tones 1 and 8 alternate at idle; a short run is the letter with that code
                idleRun = 0;
                if (b < 4) { st.bad++; continue; }
                out(ita.put(coqToIta(bitsFromCoq(a, b - 4))));
            }
        }
        if (slotPos > 4096) { slotTones.erase(slotTones.begin(), slotTones.begin() + (long)slotPos); slotPos = 0; }
    }
    int pairPhase = -1;
    int idleRun = 0;
    bool isSyncP(int a, int b) const { return (a == 3 && b == 4) || (a == 4 && b == 3); }

    void pump() {
        if (kind == FskKind::Piccolo6 || kind == FskKind::Coquelet8) {
            if (!haveMfsk) { if (!findMfsk()) return; }
            if (!mfReady) { if (!mfTiming()) return; }
            slotsMfsk();
            trim();
            return;
        }
        if (!haveTones) { if (!findFskTones()) return; }
        if (baud <= 0) { if (!findBaud()) { if (y.size() > 96000) { haveTones = false; trim(); } return; } if (!timing()) { baud = 0; return; } }
        slice();
        for (auto v : softs) mbits.push_back(v > 0 ? 1 : 0);
        softs.clear();
        switch (kind) {
        case FskKind::ArqE: case FskKind::ArqN: wordsArq(); break;
        case FskKind::Autospec: case FskKind::Spread: wordsBauer(); break;
        case FskKind::GTor: wordsGTor(); break;
        case FskKind::Efr: wordsEfr(); break;
        default: break;
        }
        trim();
    }

    void trim() {
        if (y.size() > 200000) {
            const size_t keep = 100000;
            const size_t drop = y.size() - keep;
            y.erase(y.begin(), y.begin() + (long)drop);
            consumed += drop;
        }
    }

    void push(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            decAcc += x[i];
            if (++decCnt == kDecim) { y.push_back(decAcc); decAcc = {0, 0}; decCnt = 0; }
        }
        pump();
    }
};

FskRx::FskRx(FskKind k) : p_(new Impl(k)) {}
FskRx::~FskRx() = default;
void FskRx::reset() { p_->clear(); }
void FskRx::setBaud(double b) { p_->forcedBaud = b; p_->baud = 0; }
void FskRx::setShift(double hz) { p_->forcedShift = hz; p_->haveTones = false; }
void FskRx::push(const cf32* x, size_t n) { p_->push(x, n); }
void FskRx::setTextSink(std::function<void(const std::string&)> cb) { p_->textCb = std::move(cb); }
void FskRx::setEventSink(std::function<void(const std::string&)> cb) { p_->evCb = std::move(cb); }
FskStatus FskRx::status() const { return p_->st; }

}  // namespace dect2::hfu
