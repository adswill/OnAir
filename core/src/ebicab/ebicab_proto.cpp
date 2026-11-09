// Eurobalise telegram coding, see ebicab_proto.h (UNISIG SUBSET-036 issue 4.0.0, clause 4.3)
#include "dect2/ebicab/ebicab_proto.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>

namespace dect2 {
namespace ebicab {

namespace {

const uint16_t kWords[1024] = {
#include "ebicab_table.inc"
};

struct Tables {
    int inv[2048];            // 11-bit word -> 10-bit block, -1: not valid
    Tables() {
        std::fill(inv, inv + 2048, -1);
        for (int i = 0; i < 1024; i++) inv[kWords[i]] = i;
    }
};
const Tables& tables() { static const Tables t; return t; }

// polynomials as bitsets, bit k = coefficient of x^k
using Poly = std::bitset<128>;
Poly fromExp(std::initializer_list<int> e) { Poly p; for (int k : e) p.set((size_t)k); return p; }

const Poly& polyF(bool lng) {
    static const Poly L = fromExp({10, 9, 7, 6, 4, 3, 2, 1, 0}), S = fromExp({10, 8, 7, 5, 3, 1, 0});
    return lng ? L : S;
}
const Poly& polyG(bool lng) {
    static const Poly L = fromExp({75, 73, 72, 71, 67, 62, 61, 60, 57, 56, 55, 52, 51, 49, 46, 45, 44, 43, 41, 37, 35, 34, 33, 31, 30, 28, 26, 24, 21, 17, 16, 15, 13, 12, 11, 9, 4, 1, 0});
    static const Poly S = fromExp({75, 72, 71, 70, 69, 68, 66, 65, 64, 63, 60, 55, 54, 49, 47, 46, 45, 44, 43, 42, 41, 39, 38, 37, 36, 34, 33, 32, 31, 30, 27, 25, 22, 19, 17, 13, 12, 11, 10, 6, 3, 1, 0});
    return lng ? L : S;
}
Poly mulPoly(const Poly& a, const Poly& b) {
    Poly r;
    for (size_t i = 0; i < 128; i++) if (a[i]) r ^= (b << i);
    return r;
}
int degree(const Poly& p) { for (int i = 127; i >= 0; i--) if (p[(size_t)i]) return i; return -1; }

// r = r * x mod m (deg m = dm)
inline void mulx(Poly& r, const Poly& m, int dm) { r <<= 1; if (r[(size_t)dm]) r ^= m; }

Poly modPoly(const Poly& a, const Poly& m) {
    Poly r = a;
    const int dm = degree(m);
    for (int i = degree(r); i >= dm; i = degree(r)) r ^= (m << (size_t)(i - dm));
    return r;
}

uint32_t scrambleSeed(uint16_t B) { return (uint32_t)((2801775573ull * (uint64_t)B) & 0xFFFFFFFFull); }
constexpr uint32_t kH = (1u << 31) | (1u << 30) | (1u << 29) | (1u << 27) | (1u << 25) | 1u;   // x^32 mod h(x)

int wordAt(const std::vector<uint8_t>& t, size_t at) { int w = 0; for (size_t i = 0; i < 11; i++) w = w << 1 | t[at + i]; return w; }

} // namespace

Header parseHeader(const std::vector<uint8_t>& u) {
    Header h;
    if (u.size() < 58) return h;
    size_t p = 0;
    auto get = [&](int n) { int v = 0; for (int i = 0; i < n; i++) v = v << 1 | u[p++]; return v; };
    h.qUpdown = get(1); h.mVersion = get(7); h.qMedia = get(1); h.nPig = get(3); h.nTotal = get(3); h.mDup = get(2); h.mMcount = get(8);
    h.nidC = get(10); h.nidBg = get(14); h.qLink = get(1);
    h.firstPacket = get(8);
    h.valid = true;
    return h;
}

namespace {
struct Br {
    const std::vector<uint8_t>& u; size_t p, end; bool ok = true;
    int get(int n) { int v = 0; for (int i = 0; i < n; i++) { if (p >= end) { ok = false; v <<= 1; continue; } v = v << 1 | u[p++]; } return v; }
};
std::string distText(int scale, int raw) {
    if (raw == 32767) return "now/unknown";
    static const double m[4] = {0.1, 1.0, 10.0, 0.0};
    char b[32];
    if (m[scale & 3] == 0.0) return "?";
    snprintf(b, sizeof b, "%.1f m", raw * m[scale & 3]);
    return b;
}
std::string speedText(int v) { if (v == 127) return "end"; char b[24]; snprintf(b, sizeof b, "%d km/h", v * 5); return b; }

void packetBody(int nid, Br& r, Packet& p) {
    char b[160];
    const int sc = r.get(2);
    if (nid == 5) {
        int d = r.get(15); const int newC = r.get(1); int nidC = newC ? r.get(10) : -1; int bg = r.get(14); r.get(1); r.get(2); r.get(6);
        std::string t;
        int nIter = 0;
        if (r.ok) nIter = r.get(5);
        snprintf(b, sizeof b, "linking: next group %d", bg); t = b;
        if (nidC >= 0) { snprintf(b, sizeof b, " country %d", nidC); t += b; }
        t += " at " + distText(sc, d);
        for (int i = 0; i < nIter && r.ok; i++) { d = r.get(15); const int nc = r.get(1); if (nc) r.get(10); bg = r.get(14); r.get(1); r.get(2); r.get(6); snprintf(b, sizeof b, ", %d", bg); t += b; t += " at +" + distText(sc, d); }
        p.text = t;
    } else if (nid == 21) {
        int d = r.get(15), dir = r.get(1), ga = r.get(8);
        std::string t = "gradient: at " + distText(sc, d) + (dir ? " +" : " -") + std::to_string(ga) + " per mil";
        const int n = r.get(5);
        for (int i = 0; i < n && r.ok; i++) { d = r.get(15); dir = r.get(1); ga = r.get(8); t += ", +" + distText(sc, d) + (ga == 255 ? " end" : (dir ? " +" : " -") + std::to_string(ga)); }
        p.text = t;
    } else if (nid == 27) {
        int d = r.get(15), v = r.get(7); r.get(1);
        auto diffs = [&](int cnt) { for (int i = 0; i < cnt && r.ok; i++) { const int q = r.get(2); if (q == 0 || q == 1 || q == 2) r.get(4); r.get(7); } };
        diffs(r.get(5));
        std::string t = "speed: " + speedText(v) + " at " + distText(sc, d);
        const int n = r.get(5);
        for (int i = 0; i < n && r.ok; i++) { d = r.get(15); v = r.get(7); r.get(1); diffs(r.get(5)); t += ", " + speedText(v) + " at +" + distText(sc, d); }
        p.text = t;
    } else if (nid == 41) {
        int d = r.get(15), m = r.get(3);
        if (m == 1) { r.get(8); r.get(15); }
        static const char* const lv[8] = {"level 0", "NTC", "level 1", "level 2", "level 3", "?", "?", "?"};
        p.text = "level transition to " + std::string(lv[m & 7]) + " at " + distText(sc, d);
    } else if (nid == 65) {
        const int id = r.get(8); int d = r.get(15), l = r.get(15); r.get(1); int v = r.get(7);
        snprintf(b, sizeof b, "temporary speed %d: ", id);
        p.text = std::string(b) + speedText(v) + " at " + distText(sc, d) + " for " + distText(sc, l);
    } else if (nid == 68) {
        const int init = r.get(1);
        if (init) { p.text = "track conditions: initial state at " + distText(sc, r.get(15)); return; }
        int d = r.get(15), l = r.get(15), m = r.get(4);
        static const char* const cond[16] = {"non-stopping area", "tunnel stopping area", "sound horn", "powerless (lower pantograph)", "radio hole", "air tightness",
                                             "regenerative brake off", "eddy current service brake off", "magnetic shoe brake off", "powerless (main switch off)", "eddy current emergency brake off",
                                             "spare", "spare", "spare", "spare", "spare"};
        snprintf(b, sizeof b, "track condition: %s at %s for %s", cond[m & 15], distText(sc, d).c_str(), distText(sc, l).c_str());
        p.text = b;
    } else if (nid == 72) {
        r.get(2); r.get(1); r.get(15); r.get(4); const int lv = r.get(3); if (lv == 1) r.get(8);
        r.get(15); r.get(10); r.get(4); const int lv2 = r.get(3); if (lv2 == 1) r.get(8);
        const int conf = r.get(2);
        if (conf) { r.get(1); const int rep = r.get(1); if (rep) { r.get(8); r.get(10); r.get(14); } }
        const int n = r.get(8);
        std::string t = "text: ";
        for (int i = 0; i < n && r.ok; i++) { const int c = r.get(8); t += c >= 32 && c < 127 ? (char)c : ' '; }
        p.text = t;
    } else if (nid == 79) {
        const int nc = r.get(1); if (nc) r.get(10);
        const int bg = r.get(14), off = r.get(15); const int qm = r.get(1); const int pos = r.get(24);
        snprintf(b, sizeof b, "geographical position: group %d %s from it, km %d.%03d counting %s", bg, distText(sc, off).c_str(), pos / 1000, pos % 1000, qm ? "up" : "down");
        p.text = b;
    }
}
} // namespace

std::vector<Packet> parsePackets(const std::vector<uint8_t>& u) {
    std::vector<Packet> out;
    Br r{u, 50, u.size()};
    if (u.size() < 58) return out;
    for (int guard = 0; guard < 64; guard++) {
        Br rd{u, r.p, u.size()};
        const int nid = rd.get(8);
        if (!rd.ok || nid == 255) break;
        Packet p; p.nid = nid; p.qDir = rd.get(2);
        const size_t start = r.p;
        p.lenBits = rd.get(13);
        if (!rd.ok || p.lenBits < 23 || start + (size_t)p.lenBits > u.size()) break;
        Br body{u, rd.p, start + (size_t)p.lenBits};
        packetBody(nid, body, p);
        if (!body.ok) p.text.clear();
        if (p.text.empty()) { char b[40]; snprintf(b, sizeof b, "packet %d, %d bits", nid, p.lenBits); p.text = b; }
        out.push_back(p);
        r.p = start + (size_t)p.lenBits;
    }
    return out;
}

std::vector<uint8_t> userBitsFromHeader(const Header& h, int total) {
    std::vector<uint8_t> u;
    auto put = [&](int v, int n) { for (int i = n - 1; i >= 0; i--) u.push_back((uint8_t)((v >> i) & 1)); };
    put(h.qUpdown, 1); put(h.mVersion, 7); put(h.qMedia, 1); put(h.nPig, 3); put(h.nTotal, 3); put(h.mDup, 2); put(h.mMcount, 8);
    put(h.nidC, 10); put(h.nidBg, 14); put(h.qLink, 1);
    put(h.firstPacket >= 0 ? h.firstPacket : 255, 8);
    u.resize((size_t)total, 0);
    return u;
}

std::vector<uint8_t> encode(const std::vector<uint8_t>& user, bool lng, uint16_t scrambling) {
    const int n = lng ? kLong : kShort, m = lng ? 830 : 210, k = m / 10;
    if ((int)user.size() != m) return {};
    // step 1: the first ten bits become the sum of all ten-bit blocks
    std::vector<int> blk((size_t)k);
    int sum = 0;
    for (int i = 0; i < k; i++) { int v = 0; for (int j = 0; j < 10; j++) v = v << 1 | user[(size_t)(i * 10 + j)]; blk[(size_t)i] = v; sum += v; }
    blk[0] = sum & 1023;
    // step 3: the scrambler
    uint32_t sig = scrambleSeed((uint16_t)(scrambling & 0xFFF));
    std::vector<uint8_t> s((size_t)m);
    for (int i = 0; i < m; i++) {
        const int u = (blk[(size_t)(i / 10)] >> (9 - i % 10)) & 1;
        const int f = (int)(sig >> 31) ^ u;
        s[(size_t)i] = (uint8_t)f;
        sig = (sig << 1) ^ (f ? kH : 0u);
    }
    // 10 to 11 bit words
    std::vector<uint8_t> t((size_t)n, 0);   // t[0] = b(n-1)
    for (int i = 0; i < k; i++) {
        int v = 0; for (int j = 0; j < 10; j++) v = v << 1 | s[(size_t)(i * 10 + j)];
        const int w = kWords[v];
        for (int j = 0; j < 11; j++) t[(size_t)(i * 11 + j)] = (uint8_t)((w >> (10 - j)) & 1);
    }
    const size_t base = (size_t)(n - 110);          // index of b109
    t[base] = 0; t[base + 1] = 0; t[base + 2] = 1;  // control bits b109 b108 b107
    for (int j = 0; j < 12; j++) t[base + 3 + (size_t)j] = (uint8_t)((scrambling >> (11 - j)) & 1);
    if (tables().inv[wordAt(t, base)] < 0) return {};
    const Poly p = mulPoly(polyF(lng), polyG(lng));
    const Poly& g = polyG(lng);
    for (int esb = 0; esb < 1024; esb++) {
        for (int j = 0; j < 10; j++) t[base + 15 + (size_t)j] = (uint8_t)((esb >> (9 - j)) & 1);
        Poly r;
        for (size_t i = 0; i < (size_t)(n - 85); i++) { mulx(r, p, 85); if (t[i]) r.flip(0); }
        for (int i = 0; i < 85; i++) mulx(r, p, 85);
        r ^= g;
        for (int i = 0; i < 85; i++) t[(size_t)(n - 85 + i)] = (uint8_t)r[(size_t)(84 - i)];
        bool ok = true;
        for (size_t w = base; w + 11 <= (size_t)n; w += 11)
            if (tables().inv[wordAt(t, w)] < 0) { ok = false; break; }
        if (ok) return t;
    }
    return {};
}

bool decodeWindow(const uint8_t* bits, size_t count, bool lng, Telegram& out) {
    const int n = lng ? kLong : kShort, r0 = lng ? 77 : 121;
    if (count < (size_t)(n + r0)) return false;
    const int r = (int)count - n;   // every extra bit given is checked
    // 2. parity check: the first n bits divisible by g(x)
    const Poly& g = polyG(lng);
    Poly rem;
    for (int i = 0; i < n; i++) { mulx(rem, g, 75); if (bits[i]) rem.flip(0); }
    if (rem.any()) return false;
    // 3. the r extra bits repeat the first r
    for (int i = 0; i < r; i++) if (bits[i] != bits[n + i]) return false;
    // 4. start position from the syndrome modulo f(x)
    const Poly& f = polyF(lng);
    Poly sy;
    for (int i = 0; i < n; i++) { mulx(sy, f, 10); if (bits[i]) sy.flip(0); }
    const unsigned key = (unsigned)(sy.to_ulong() & 1023);
    Poly cur = modPoly(g, f);
    int shift = -1;
    for (int s = 0; s < n; s++) {
        if ((unsigned)(cur.to_ulong() & 1023) == key) { shift = s; break; }
        mulx(cur, f, 10);
    }
    if (shift < 0) return false;
    shift = (n - shift) % n;               // window[0] is telegram bit s; the telegram starts at window position n - s
    std::vector<uint8_t> t((size_t)n);   // b(n-1-k) = window[(shift + k) mod n]
    for (int k = 0; k < n; k++) t[(size_t)k] = bits[(shift + k) % n];
    // 5. all words valid (also after inverting everything)
    auto allValid = [&](const std::vector<uint8_t>& v) { for (int w = 0; w < n / 11; w++) if (tables().inv[wordAt(v, (size_t)w * 11)] < 0) return false; return true; };
    if (!allValid(t)) return false;
    const size_t base = (size_t)(n - 110);
    out = Telegram();
    out.longFmt = lng; out.shift = shift;
    out.inversionBit = t[base] != 0;
    out.controlBits = t[base] << 2 | t[base + 1] << 1 | t[base + 2];
    if (out.inversionBit) {   // received inverted: every bit is inverted; the control bits then read 1 and the rest of them 0 / 1 swapped
        for (auto& b : t) b ^= 1;
        out.inverted = true;
        out.controlBits = t[base] << 2 | t[base + 1] << 1 | t[base + 2];
    }
    out.formatKnown = t[base + 1] == 0 && t[base + 2] == 1;
    if (!out.formatKnown) return false;
    uint16_t B = 0;
    for (int j = 0; j < 12; j++) B = (uint16_t)(B << 1 | t[base + 3 + (size_t)j]);
    out.scrambling = B;
    // 9. inverse transformation, 10. descramble
    const int k = lng ? 83 : 21, m = k * 10;
    std::vector<uint8_t> sc((size_t)m);
    for (int i = 0; i < k; i++) {
        const int blk = tables().inv[wordAt(t, (size_t)i * 11)];
        for (int j = 0; j < 10; j++) sc[(size_t)(i * 10 + j)] = (uint8_t)((blk >> (9 - j)) & 1);
    }
    uint32_t sig = scrambleSeed(B);
    std::vector<uint8_t> u((size_t)m);
    for (int i = 0; i < m; i++) {
        const int f1 = sc[(size_t)i];
        u[(size_t)i] = (uint8_t)(f1 ^ (int)(sig >> 31));
        sig = (sig << 1) ^ (f1 ? kH : 0u);
    }
    // undo step 1: the first block was replaced by the sum of all
    int first = 0, rest = 0;
    for (int i = 0; i < k; i++) { int v = 0; for (int j = 0; j < 10; j++) v = v << 1 | u[(size_t)(i * 10 + j)]; if (i == 0) first = v; else rest += v; }
    const int orig = (first - rest) & 1023;
    for (int j = 0; j < 10; j++) u[(size_t)j] = (uint8_t)((orig >> (9 - j)) & 1);
    out.userBits = u;
    out.header = parseHeader(u);
    out.packets = parsePackets(u);
    static const char* hx = "0123456789ABCDEF";
    for (size_t i = 0; i < u.size(); i += 8) {
        int v = 0; for (size_t j = 0; j < 8; j++) v = v << 1 | (i + j < u.size() ? u[i + j] : 0);
        out.hex += hx[v >> 4]; out.hex += hx[v & 15];
    }
    return true;
}

// ---------------------------------------------------------------- streaming

struct Stream::Impl {
    static constexpr size_t kHist = 8192;     // power of two
    std::vector<uint8_t> hist = std::vector<uint8_t>(kHist, 0);
    uint64_t total = 0, burstBits = 0, parityHits = 0;
    Telegram last;
    struct Fmt {
        bool lng;
        int n, r;
        Poly rem, xp;
        std::vector<uint8_t> zero = std::vector<uint8_t>(kHist, 0);   // the window ending at bit t passed the parity check
        uint64_t lastFound = 0;
    } fm[2];
    Impl() {
        for (int i = 0; i < 2; i++) {
            Fmt& f = fm[i];
            f.lng = i == 0; f.n = f.lng ? kLong : kShort; f.r = f.lng ? 77 : 121;
            f.xp.reset(); f.xp.set(0);
            for (int j = 0; j < f.n - 1; j++) mulx(f.xp, polyG(f.lng), 75);
        }
    }
    void reset() { burstBits = 0; }
    bool bit(int b) {
        bool found = false;
        const uint64_t t = total;
        hist[t & (kHist - 1)] = (uint8_t)b;
        total++; burstBits++;
        for (int i = 0; i < 2; i++) {
            Fmt& f = fm[i];
            const Poly& g = polyG(f.lng);
            if (t >= (uint64_t)f.n) {   // the oldest bit of the window leaves
                if (hist[(t - (uint64_t)f.n) & (kHist - 1)]) f.rem ^= f.xp;
            }
            mulx(f.rem, g, 75);
            if (b) f.rem.flip(0);
            const bool z = t + 1 >= (uint64_t)f.n && f.rem.none();
            f.zero[t & (kHist - 1)] = z;
            if (z) parityHits++;
            const int r = burstBits > 7500 ? f.n : f.r;
            if (t + 1 < (uint64_t)(f.n + r)) continue;
            if (t + 1 - f.lastFound < (uint64_t)f.n) continue;   // one telegram per n bits
            if (!f.zero[(t - (uint64_t)r) & (kHist - 1)]) continue;
            // window = bits t-n-r+1 .. t
            std::vector<uint8_t> w((size_t)(f.n + r));
            const uint64_t s0 = t + 1 - (uint64_t)(f.n + r);
            for (size_t j = 0; j < w.size(); j++) w[j] = hist[(s0 + j) & (kHist - 1)];
            Telegram tg;
            if (decodeWindow(w.data(), w.size(), f.lng, tg)) { last = tg; f.lastFound = t + 1; found = true; }
        }
        return found;
    }
};

Stream::Stream() : p_(std::make_unique<Impl>()) {}
Stream::~Stream() = default;
void Stream::reset() { p_->reset(); }
bool Stream::bit(int b) { return p_->bit(b); }
const Telegram& Stream::last() const { return p_->last; }
uint64_t Stream::bitsSeen() const { return p_->total; }
uint64_t Stream::parityHits() const { return p_->parityHits; }

} // namespace ebicab
} // namespace dect2
