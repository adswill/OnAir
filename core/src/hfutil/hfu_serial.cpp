// MIL-STD-188-110 serial tone and STANAG 4285 receivers and test generators; see hfu_serial.h.
#include "dect2/hfutil/hfu_serial.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <deque>

namespace dect2::hfu {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cf = std::complex<float>;
constexpr double kBaud = 2400.0, kCarrier = 1800.0;
constexpr int kDecim = 5;                    // 24 kHz to 4800 Hz, two samples per symbol

inline cf tribit(int t) { const double a = (t & 7) * kPi / 4; return cf((float)std::cos(a), (float)std::sin(a)); }

// root raised cosine at 24 kHz, 10 samples per symbol, roll-off 0.35, 12 symbols long
std::vector<float> rrc() {
    const int sps = 10, half = 60;
    const double b = 0.35;
    std::vector<float> h(2 * half + 1);
    double e = 0;
    for (int i = -half; i <= half; i++) {
        const double t = (double)i / sps;
        double v;
        if (i == 0) v = 1 - b + 4 * b / kPi;
        else if (std::fabs(std::fabs(4 * b * t) - 1) < 1e-9) v = b / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * b)) + (1 - 2 / kPi) * std::cos(kPi / (4 * b)));
        else v = (std::sin(kPi * t * (1 - b)) + 4 * b * t * std::cos(kPi * t * (1 + b))) / (kPi * t * (1 - 16 * b * b * t * t));
        h[(size_t)(i + half)] = (float)v;
        e += v * v;
    }
    for (auto& v : h) v = (float)(v / std::sqrt(e));
    return h;
}

// ---- MIL-STD-188-110 tables ----
const int kSyncScr[32] = {7, 4, 3, 0, 5, 1, 5, 0, 2, 2, 1, 1, 5, 7, 4, 3, 5, 0, 2, 6, 2, 1, 6, 2, 0, 0, 5, 0, 5, 2, 6, 6};
const int kPre[8][8] = {{0, 0, 0, 0, 0, 0, 0, 0}, {0, 4, 0, 4, 0, 4, 0, 4}, {0, 0, 4, 4, 0, 0, 4, 4}, {0, 4, 4, 0, 0, 4, 4, 0},
                        {0, 0, 0, 0, 4, 4, 4, 4}, {0, 4, 0, 4, 4, 0, 4, 0}, {0, 0, 4, 4, 4, 4, 0, 0}, {0, 4, 4, 0, 4, 0, 0, 4}};
const int kFixed[9] = {0, 1, 3, 0, 1, 3, 1, 2, 0};
inline int preTribit(int ch, int k) { return kPre[ch][k & 7]; }         // Table XIII: the eight numbers are the 4 pairs repeated; period 8
inline int countSym(int twoBits) { return 4 + (twoBits & 3); }          // 5.3.2.3.7.2.1: a 1 is put above the two-bit value (the worked example 23 -> 5, 5, 7); Table XII lists the bits least significant first

struct Mil110Mode { int bps, d1s, d2s, d1l, d2l, bitsPerSym, U, K, rep; };
const Mil110Mode kMil[] = {{4800, 7, 6, -1, -1, 3, 32, 16, 1}, {2400, 6, 4, 4, 4, 3, 32, 16, 1}, {1200, 6, 5, 4, 5, 2, 20, 20, 1},
                           {600, 6, 6, 4, 6, 1, 20, 20, 1}, {300, 6, 7, 4, 7, 1, 20, 20, 2}, {150, 7, 4, 5, 4, 1, 20, 20, 4}, {75, 7, 5, 5, 5, 2, 32, 0, 1}};

const Mil110Mode* findMode(int d1, int d2, bool& longIntl) {
    for (const auto& m : kMil) {
        if (m.d1s == d1 && m.d2s == d2) { longIntl = false; return &m; }
        if (m.d1l == d1 && m.d2l == d2) { longIntl = true; return &m; }
    }
    return nullptr;
}

void intlDims(int bps, bool lng, int& rows, int& cols) {
    rows = 40;
    switch (bps) {
    case 2400: cols = lng ? 576 : 72; break;
    case 1200: cols = lng ? 288 : 36; break;
    case 75: rows = lng ? 20 : 10; cols = lng ? 36 : 9; break;
    default: cols = lng ? 144 : 18; break;
    }
}

// Scrambler for the 110 data phase: 12 stages, x^12 + x^6 + x^4 + x + 1, eight shifts per symbol
struct Scr110 {
    uint32_t r = 0xBAD; int n = 0;
    int next() {
        if (n == 160) { r = 0xBAD; n = 0; }
        for (int i = 0; i < 8; i++) {
            const uint32_t fb = (r >> 11) & 1;
            r = ((r << 1) & 0xFFF) | fb;
            if (fb) r ^= (1u << 1) | (1u << 4) | (1u << 6);
        }
        n++;
        return (((r >> 2) & 1) << 2) | (((r >> 1) & 1) << 1) | (r & 1);
    }
};

// ---- STANAG 4285 ----
std::vector<int> sync4285() {
    uint32_t r = 0b11010;    // x4 .. x0
    std::vector<int> s(80);
    for (int i = 0; i < 80; i++) { s[(size_t)i] = (int)(r & 1); const uint32_t fb = (r ^ (r >> 2)) & 1; r = (r >> 1) | (fb << 4); }
    return s;
}
std::vector<int> scr4285() {
    uint32_t r = 0x1FF;
    std::vector<int> s(176);
    for (int i = 0; i < 176; i++) {
        s[(size_t)i] = (int)(r & 7);
        for (int j = 0; j < 3; j++) { const uint32_t fb = (r ^ (r >> 4)) & 1; r = (r >> 1) | (fb << 8); }
    }
    return s;
}
// frame position: 1 when data, 0 when reference (of the 176 scrambled symbols): blocks 32 data + 16 reference, three times, then 32 data
inline bool isData4285(int k) { return k < 144 ? (k % 48) < 32 : true; }
const int k3bit4285[8] = {1, 0, 2, 3, 6, 7, 5, 4};     // tribit (oldest bit first) to symbol
int bitsPerSym4285(int bps, bool coded) { return coded ? (bps == 2400 ? 3 : bps == 1200 ? 2 : 1) : (bps == 3600 ? 3 : bps == 2400 ? 2 : 1); }

struct Prof4285 { int bps; bool coded; int j; int rep; int phyBits; const char* name; };
// the deinterleaver delay increment is per 4800 / 2400 / 1200 bit/s coded rate; short values: 4 / 2 / 1, long: 48 / 24 / 12
const Prof4285 kP4285[] = {{2400, true, 4, 1, 3, "2400 bit/s coded (8-PSK), short"}, {1200, true, 2, 1, 2, "1200 bit/s coded (QPSK), short"}, {600, true, 1, 1, 1, "600 bit/s coded, short"},
                           {300, true, 1, 2, 1, "300 bit/s coded, short"}, {150, true, 1, 4, 1, "150 bit/s coded, short"}, {75, true, 1, 8, 1, "75 bit/s coded, short"},
                           {3600, false, 0, 1, 3, "3600 bit/s uncoded"}, {2400, false, 0, 1, 2, "2400 bit/s uncoded"}, {1200, false, 0, 1, 1, "1200 bit/s uncoded"},
                           {2400, true, 48, 1, 3, "2400 bit/s coded (8-PSK), long"}, {1200, true, 24, 1, 2, "1200 bit/s coded (QPSK), long"}, {600, true, 12, 1, 1, "600 bit/s coded, long"},
                           {300, true, 12, 2, 1, "300 bit/s coded, long"}, {150, true, 12, 4, 1, "150 bit/s coded, long"}, {75, true, 12, 8, 1, "75 bit/s coded, long"}};
constexpr int kNP4285 = (int)(sizeof kP4285 / sizeof *kP4285);

constexpr uint32_t kSOM = 0x03873C3C, kEOM = 0x4B65A5B2;

// convolutional (de)interleaver of Annex E: 32 rows, the row order on input is 9 n mod 32
struct Delay4285 {
    int j = 0;
    std::vector<std::deque<float>> rows;
    void init(int jj) { j = jj; rows.assign(32, {}); for (int r = 0; r < 32; r++) rows[(size_t)r].assign((size_t)(j * (31 - r)), 0.f); }
    // one cycle of 32 soft bits in row order, out: 32 soft bits in row order after the deinterleaver delay
    void cycle(const float* in, float* out) {
        for (int r = 0; r < 32; r++) {
            auto& q = rows[(size_t)r];
            q.push_back(in[r]);
            out[r] = q.front();
            q.pop_front();
        }
    }
};
}  // namespace

// ---- Viterbi ----
void Viterbi::reset() { m_.assign(64, 0.f); n_.assign(64, 0.f); dec_.clear(); emitted_ = 0; good_ = total_ = 0; }

static inline int par(uint32_t v) { return __builtin_parity(v); }

void Viterbi::push(float a, float b) {
    // state: the six previous bits, the newest in bit 5; the encoder register has the input bit above them
    std::fill(n_.begin(), n_.end(), -1e30f);
    uint64_t d = 0;
    float bestAdd = -1e30f;
    for (int s = 0; s < 64; s++) {
        for (int bit = 0; bit < 2; bit++) {
            const uint32_t reg = ((uint32_t)bit << 6) | (uint32_t)s;   // bit 6 is the input (delay 0), bit 6 - k is delay k
            const int t1 = par(reg & 0b1011011u), t2 = par(reg & 0b1111001u);
            const float add = (t1 ? -a : a) + (t2 ? -b : b);
            const float v = m_[(size_t)s] + add;
            const int ns = (int)(reg >> 1);
            if (v > n_[(size_t)ns]) {
                n_[(size_t)ns] = v;
                if (s & 1) d |= 1ull << ns; else d &= ~(1ull << ns);
            }
            bestAdd = std::max(bestAdd, add);
        }
    }
    const float best = *std::max_element(n_.begin(), n_.end());
    const float lo = std::fabs(a) + std::fabs(b);
    good_ += bestAdd > 0.5f * lo && lo > 0 ? 1 : 0;
    total_ += 1;
    if (total_ > 20000) { total_ *= 0.5; good_ *= 0.5; }
    for (size_t i = 0; i < 64; i++) m_[i] = n_[i] - best;
    dec_.push_back(d);
}

float Viterbi::metricPerStep() const {
    return total_ > 0 ? (float)(good_ / total_) : 0;
}

void Viterbi::trace(size_t endStep, size_t keep, int state, std::vector<uint8_t>* out) {
    // from the end state walk back; the bit that entered at a step is the top bit (bit 5) of the state after it
    std::vector<uint8_t> tmp(endStep - emitted_, 0);
    int s = state;
    for (size_t step = endStep; step > emitted_; step--) {
        tmp[step - 1 - emitted_] = (uint8_t)((s >> 5) & 1);
        const int bit0 = (int)((dec_[step - 1] >> s) & 1);
        s = ((s << 1) & 63) | bit0;
    }
    if (out) out->insert(out->end(), tmp.begin(), tmp.begin() + (long)keep);
    emitted_ += keep;
}

void Viterbi::take(std::vector<uint8_t>& out) {
    const size_t depth = 96;
    while (dec_.size() - emitted_ > depth + 48) {
        int s = (int)(std::max_element(m_.begin(), m_.end()) - m_.begin());
        trace(dec_.size(), 48, s, &out);
        if (emitted_ > 4096) { dec_.erase(dec_.begin(), dec_.begin() + (long)emitted_); emitted_ = 0; }
    }
}

void Viterbi::flush(std::vector<uint8_t>& out) {
    if (dec_.size() <= emitted_) return;
    int s = (int)(std::max_element(m_.begin(), m_.end()) - m_.begin());
    const size_t n = dec_.size() - emitted_;
    trace(dec_.size(), n, s, &out);
}

// ---- STANAG 5066 D_PDU ----
const char* d5066TypeName(int t) {
    switch (t) {
    case 0: return "DATA_ONLY"; case 1: return "ACK_ONLY"; case 2: return "DATA_ACK"; case 3: return "RESET_WIN_RESYNC"; case 4: return "EXP_DATA_ONLY";
    case 5: return "EXP_ACK_ONLY"; case 6: return "MANAGEMENT"; case 7: return "NON_ARQ_DATA"; case 8: return "EXP_NON_ARQ_DATA"; case 15: return "WARNING";
    default: return "reserved";
    }
}
uint16_t crc5066Header(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int b = 0; b < 8; b++) c = (uint16_t)((c & 1) ? (c >> 1) ^ 0x9299 : c >> 1); }   // 0x9949 bit-reversed
    return c;
}
uint32_t crc5066Data(const uint8_t* p, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0xF3A4E550u : c >> 1; }   // 0x0AA725CF bit-reversed
    return c;
}

void Parser5066::tryParse(const std::function<void(const D5066&)>& cb) {
    // buf_: 90 EB type/eow eow eot addr|hdr ...
    if (buf_.size() < 6) return;
    const int hs = buf_[5] & 0x1F, as = buf_[5] >> 5;
    const size_t hdrEnd = (size_t)(hs + as + 2);
    if (hs < 4 || hdrEnd > buf_.size()) { if (hs < 4) { want_ = -1; } return; }
    const uint16_t c = crc5066Header(buf_.data() + 2, (size_t)(hs + as - 2));
    const uint16_t got = (uint16_t)(buf_[(size_t)(hs + as)] | (buf_[(size_t)(hs + as + 1)] << 8));
    if (c != got) { want_ = -1; buf_.clear(); return; }
    D5066 d;
    d.type = buf_[2] >> 4;
    d.typeName = d5066TypeName(d.type);
    d.eot = buf_[4];
    d.headerBytes = (int)hdrEnd;
    d.headerCrcOk = true;
    {
        std::string a;
        char t[4];
        for (int i = 0; i < as; i++) { std::snprintf(t, sizeof t, "%02X", buf_[(size_t)(6 + i)]); a += t; }
        d.dst = a.substr(0, a.size() / 2);
        d.src = a.substr(a.size() / 2);
    }
    int seg = 0;
    if (d.type == 0 || d.type == 2 || d.type == 4 || d.type == 7 || d.type == 8) {
        if ((size_t)(6 + as + 1) >= buf_.size()) return;
        seg = ((buf_[(size_t)(6 + as)] & 3) << 8) | buf_[(size_t)(7 + as)];
    }
    const size_t total = hdrEnd + (seg ? (size_t)seg + 4 : 0);
    if (buf_.size() < total) return;
    d.payload = seg;
    if (seg) {
        const uint32_t got32 = (uint32_t)buf_[hdrEnd + (size_t)seg] | ((uint32_t)buf_[hdrEnd + (size_t)seg + 1] << 8) | ((uint32_t)buf_[hdrEnd + (size_t)seg + 2] << 16) | ((uint32_t)buf_[hdrEnd + (size_t)seg + 3] << 24);
        d.dataCrcOk = crc5066Data(buf_.data() + hdrEnd, (size_t)seg) == got32;
        char t[4];
        for (int i = 0; i < std::min(seg, 32); i++) { std::snprintf(t, sizeof t, "%02X", buf_[hdrEnd + (size_t)i]); d.hex += t; }
    }
    found_++;
    if (cb) cb(d);
    want_ = -1;
    buf_.clear();
}

void Parser5066::pushBit(uint8_t b, const std::function<void(const D5066&)>& cb) {
    window_ = (window_ >> 1) | ((uint32_t)(b & 1) << 15);
    if (want_ < 0) {
        if ((window_ & 0xFFFF) == 0xEB90) { want_ = 8; buf_.clear(); buf_.push_back(0x90); buf_.push_back(0xEB); }
        return;
    }
    if (--want_ == 0) {
        buf_.push_back((uint8_t)((window_ >> 8) & 0xFF));
        want_ = 8;
        if (buf_.size() > 1200) { buf_.clear(); want_ = -1; return; }
        tryParse(cb);
    }
}

std::vector<uint8_t> make5066NonArq(const std::vector<uint8_t>& payload, uint32_t src, uint32_t dst) {
    std::vector<uint8_t> o;
    const int as = 4;
    o.push_back(0x90); o.push_back(0xEB);
    o.push_back(7 << 4);                     // type NON_ARQ_DATA, no EOW
    o.push_back(0);
    o.push_back(60);                         // EOT
    const int hs = 12;
    o.push_back((uint8_t)((as << 5) | hs));
    o.push_back((uint8_t)(dst >> 8)); o.push_back((uint8_t)dst); o.push_back((uint8_t)(src >> 8)); o.push_back((uint8_t)src);
    const int seg = (int)payload.size();
    o.push_back((uint8_t)((seg >> 8) & 3)); o.push_back((uint8_t)seg);
    for (int i = 0; i < hs + as - (int)o.size(); ) o.push_back(0);
    while ((int)o.size() < hs + as) o.push_back(0);
    const uint16_t c = crc5066Header(o.data() + 2, (size_t)(hs + as - 2));
    o.push_back((uint8_t)c); o.push_back((uint8_t)(c >> 8));
    o.insert(o.end(), payload.begin(), payload.end());
    const uint32_t c32 = crc5066Data(payload.data(), payload.size());
    for (int i = 0; i < 4; i++) o.push_back((uint8_t)(c32 >> (8 * i)));
    return o;
}

// ---- Generator ----
namespace {
void bitsToSym110(const std::vector<uint8_t>& ch, int bps, const Mil110Mode& m, std::vector<int>& out) {
    (void)bps;
    size_t i = 0;
    while (i + (size_t)m.bitsPerSym <= ch.size()) {
        int v = 0;
        if (m.bitsPerSym == 3) { const int b = ch[i] * 4 + ch[i + 1] * 2 + ch[i + 2]; static const int g[8] = {0, 1, 3, 2, 7, 6, 4, 5}; v = g[b]; }
        else if (m.bitsPerSym == 2) { const int b = ch[i] * 2 + ch[i + 1]; static const int g[4] = {0, 1, 3, 2}; v = g[b] * 2; }
        else v = ch[i] * 4;
        i += (size_t)m.bitsPerSym;
        out.push_back(v);
    }
}

// 75 bit/s: channel symbol (after modified Gray) to 32 tribits, normal or exceptional set
void sym75(int val, bool excep, int* out32) {
    static const int n[4][4] = {{0, 0, 0, 0}, {0, 4, 0, 4}, {0, 0, 4, 4}, {0, 4, 4, 0}};
    static const int e[4][8] = {{0, 0, 0, 0, 4, 4, 4, 4}, {0, 4, 0, 4, 4, 0, 4, 0}, {0, 0, 4, 4, 4, 4, 0, 0}, {0, 4, 4, 0, 4, 0, 0, 4}};
    for (int i = 0; i < 32; i++) out32[i] = excep ? e[val][i & 7] : n[val][i & 3];
}

struct Enc { uint32_t r = 0; void bit(int b, uint8_t& t1, uint8_t& t2) { r = (r >> 1) | ((uint32_t)b << 6); t1 = (uint8_t)par(r & 0b1011011u); t2 = (uint8_t)par(r & 0b1111001u); } };

void modulate(const std::vector<int>& syms, std::vector<cf32>& out) {
    static const std::vector<float> h = rrc();
    const int sps = 10;
    const size_t half = h.size() / 2;
    const size_t base = out.size();
    out.resize(base + syms.size() * sps + 2 * half, cf32(0, 0));
    for (size_t k = 0; k < syms.size(); k++) {
        const cf s = tribit(syms[k]);
        const size_t c = base + k * sps;
        for (size_t i = 0; i < h.size(); i++) out[c + i] += s * h[i];
    }
}
}  // namespace

std::vector<cf32> genSerial(const SerialGenCfg& c, const std::vector<uint8_t>& dataBits) {
    std::vector<int> syms;
    if (c.std == SerialStd::Mil110) {
        const Mil110Mode* m = nullptr;
        for (const auto& x : kMil) if (x.bps == c.bps) m = &x;
        if (!m) return {};
        const bool useIntl = !c.zeroIntl && c.bps != 4800;
        int rows = 0, cols = 0;
        if (c.bps != 4800) intlDims(c.bps, c.longIntl, rows, cols);
        // FEC
        std::vector<uint8_t> coded;
        std::vector<uint8_t> bits = dataBits;
        for (int i = 31; i >= 0; i--) bits.push_back((kEOM >> i) & 1);
        for (int i = 0; i < 144; i++) bits.push_back(0);
        if (c.bps == 4800) coded = bits;
        else {
            Enc e;
            for (auto b : bits) { uint8_t t1, t2; e.bit(b, t1, t2); for (int r = 0; r < m->rep; r++) { coded.push_back(t1); coded.push_back(t2); } }
        }
        const size_t block = useIntl ? (size_t)rows * cols : 0;
        if (block) while (coded.size() % block) coded.push_back(0);
        std::vector<uint8_t> ch(coded.size());
        if (block) {
            const int rowStep = c.bps == 75 ? 7 : 9, colDec = c.bps == 75 ? 7 : 17;
            for (size_t b = 0; b * block < coded.size(); b++) {
                std::vector<uint8_t> mat(block);
                { int row = 0, col = 0, cnt = 0; for (size_t i = 0; i < block; i++) { mat[(size_t)row * cols + col] = coded[b * block + i]; row = (row + rowStep) % rows; if (++cnt == rows) { cnt = 0; row = 0; col++; } } }
                { int row = 0, col = 0, colStart = 0; for (size_t i = 0; i < block; i++) { ch[b * block + i] = mat[(size_t)row * cols + col]; row++; col = (col + cols - (colDec % cols)) % cols; if (row == rows) { row = 0; colStart++; col = colStart % cols; } } }
            }
        } else ch = coded;
        // preamble
        const int d1 = c.longIntl ? m->d1l : m->d1s, d2 = c.longIntl ? m->d2l : m->d2s;
        const int segs = c.longIntl ? 24 : 3;
        for (int s = segs - 1; s >= 0; s--) {
            const int c1 = countSym(s >> 4), c2 = countSym(s >> 2), c3 = countSym(s);
            const int seq[15] = {0, 1, 3, 0, 1, 3, 1, 2, 0, d1, d2, c1, c2, c3, 0};
            int idx = 0;
            for (int q = 0; q < 15; q++) for (int k = 0; k < 32; k++, idx++) syms.push_back((preTribit(seq[q], k) + kSyncScr[idx % 32]) % 8);
        }
        // data phase
        Scr110 scr;
        std::vector<int> chSyms;
        bitsToSym110(ch, c.bps, *m, chSyms);
        if (c.bps == 75) {
            size_t nset = chSyms.size();
            const int excepEvery = c.longIntl ? 360 : 45;
            for (size_t i = 0; i < nset; i++) {
                int t[32];
                sym75(chSyms[i] / 2, (i + 1) % excepEvery == 0, t);
                for (int k = 0; k < 32; k++) syms.push_back((t[k] + scr.next()) % 8);
            }
        } else {
            const int cyc = m->U + m->K;
            const int blockSyms = c.longIntl ? 11520 : 1440;
            const int cycPerBlock = blockSyms / cyc;
            size_t pos = 0;
            int cycIdx = 0;
            while (pos < chSyms.size()) {
                for (int u = 0; u < m->U; u++) { const int v = pos < chSyms.size() ? chSyms[pos] : 0; pos++; syms.push_back((v + scr.next()) % 8); }
                const int kInBlock = cycIdx % cycPerBlock;
                const int fromEnd = cycPerBlock - 1 - kInBlock;
                int pat = -1;
                if (fromEnd == 1) pat = d1; else if (fromEnd == 0) pat = d2;
                for (int k = 0; k < m->K; k++) {
                    int v = 0;
                    if (pat >= 0 && k < 16) v = preTribit(pat, k);
                    syms.push_back((v + scr.next()) % 8);
                }
                cycIdx++;
            }
        }
    } else {
        // STANAG 4285
        const auto sy = sync4285();
        const auto scr = scr4285();
        const Prof4285* p = nullptr;
        for (int i = 0; i < kNP4285; i++) {
            const auto& q = kP4285[i];
            if (q.bps == c.bps && q.coded == c.coded && (!c.coded || (q.j >= 12) == c.longIntl)) p = &q;
        }
        if (!p) return {};
        std::vector<uint8_t> tx;
        std::vector<uint8_t> msg;
        if (c.coded) { for (int i = 31; i >= 0; i--) msg.push_back((kSOM >> i) & 1); }
        msg.insert(msg.end(), dataBits.begin(), dataBits.end());
        if (c.coded) {
            for (int i = 31; i >= 0; i--) msg.push_back((kEOM >> i) & 1);
            for (int i = 0; i < 32 * 32 * p->j + 200; i++) msg.push_back(0);
            Enc e;
            std::vector<uint8_t> coded;
            for (auto b : msg) { uint8_t t1, t2; e.bit(b, t1, t2); for (int r = 0; r < p->rep; r++) { coded.push_back(t1); coded.push_back(t2); } }
            while (coded.size() % 32) coded.push_back(0);
            // interleave: each input bit n goes to row 9n mod 32, delayed j * row cycles
            std::vector<std::deque<uint8_t>> rows(32);
            for (int r = 0; r < 32; r++) rows[(size_t)r].assign((size_t)(p->j * r), 0);
            for (size_t n = 0; n + 32 <= coded.size(); n += 32) {
                uint8_t cyc[32];
                for (int i = 0; i < 32; i++) {
                    const int r = (i * 9) % 32;
                    auto& q = rows[(size_t)r];
                    q.push_back(coded[n + (size_t)i]);
                    cyc[r] = q.front();
                    q.pop_front();
                }
                for (int r = 0; r < 32; r++) { if (p->phyBits == 3 && r % 4 == 3) continue; tx.push_back(cyc[r]); }
            }
        } else tx = msg;
        // frames
        size_t pos = 0;
        const int bitsPer = p->phyBits;
        while (pos < tx.size()) {
            for (int k = 0; k < 80; k++) syms.push_back(sy[(size_t)k] * 4);
            for (int k = 0; k < 176; k++) {
                int v = 0;
                if (isData4285(k)) {
                    int b0 = pos < tx.size() ? tx[pos] : 0; pos++;
                    if (bitsPer == 1) v = b0 * 4;
                    else if (bitsPer == 2) { int b1 = pos < tx.size() ? tx[pos] : 0; pos++; static const int g[4] = {0, 2, 6, 4}; v = g[b0 * 2 + b1]; }
                    else { int b1 = pos < tx.size() ? tx[pos] : 0; pos++; int b2 = pos < tx.size() ? tx[pos] : 0; pos++; v = k3bit4285[b0 * 4 + b1 * 2 + b2]; }
                }
                syms.push_back((v + scr[(size_t)k]) % 8);
            }
        }
    }
    std::vector<cf32> bb;
    bb.assign((size_t)(c.leadSec * kBaseRate), cf32(0, 0));
    modulate(syms, bb);
    bb.resize(bb.size() + (size_t)(c.tailSec * kBaseRate), cf32(0, 0));
    double ph = 0;
    const double step = 2 * kPi * kCarrier / kBaseRate;
    for (auto& v : bb) { v *= cf((float)std::cos(ph), (float)std::sin(ph)); ph += step; if (ph > 2 * kPi) ph -= 2 * kPi; }
    return bb;
}

// ---- Receiver ----
struct SerialRx::Impl {
    SerialStd std;
    int force = -1;
    std::vector<float> h = rrc();
    std::vector<cf> xfir;           // recent samples for the matched filter, baseband mixed
    size_t xpos = 0;
    int dcount = 0;
    double ph = 0;
    std::vector<cf> x;              // 4800 Hz stream
    size_t base = 0;                // sample index of x[0] in the stream
    size_t cursor = 0;              // next index to search, relative to x
    double fsErr = 0;
    double tSec = 0;
    std::function<void(uint8_t)> bitSink;
    std::function<void(const SerialEvent&)> evSink;
    std::function<void(const D5066&)> dSink;
    Parser5066 p5066;
    SerialStatus st;
    uint64_t totalIn = 0;

    // equalizer
    static constexpr int NF = 33, NB = 12;
    std::vector<cf> w, b;
    cf hist[NB];
    double eqSigma2 = 0.1;
    double phEq = 0, nu = 0;

    // common decode state
    int mode = 0;                     // 0 search, 1 110 preamble located, 2 110 data, 3 4285 tracking
    size_t pos = 0;                   // index in x of the next symbol centre
    double cfoRad = 0;                // per sample at 4800 Hz
    double cfoPhase = 0;

    // 110
    const Mil110Mode* m = nullptr;
    bool longIntl = false, zeroIntl = false, tried0 = false;
    Scr110 scr;
    size_t symCount = 0;
    int cycIdx = 0;
    std::vector<float> llrBuf;
    Viterbi vit;
    int d1 = 0, d2 = 0;
    std::vector<float> softCh;
    uint32_t eomReg = 0;
    uint64_t dataBits = 0;
    int badBlocks = 0, setCount = 0;
    cf eq75sum;

    // 4285
    std::vector<int> sy = sync4285(), scr4 = scr4285();
    struct Hyp {
        const Prof4285* p = nullptr;
        Delay4285 d;
        Viterbi v;
        std::vector<float> partial;     // bits waiting for a cycle of 32
        int phaseBits = 0;
        uint32_t reg = 0;
        uint64_t bitsSeen = 0;
        bool somSeen = false;
        std::vector<float> rep;
        uint64_t dataAfterSom = 0;
        uint64_t since = 0;
        float score = 0;
    };
    std::vector<Hyp> hyps;
    int locked = -1;
    int frameMissed = 0;
    int modOrderVotes[4] = {0, 0, 0, 0};
    uint64_t fr4285 = 0;

    explicit Impl(SerialStd s) : std(s) { clear(); }

    void clear() {
        xfir.assign(h.size(), cf(0, 0)); xpos = 0; dcount = 0; ph = 0;
        x.clear(); base = cursor = 0;
        mode = 0;
        st = SerialStatus();
        locked = -1; frameMissed = 0; hyps.clear(); fr4285 = 0;
    }

    void event(const std::string& s) {
        if (evSink) { SerialEvent e; e.text = s; e.timeSec = tSec; evSink(e); }
    }

    void emitBit(uint8_t bit) {
        st.bitsOut++;
        if (bitSink) bitSink(bit);
        p5066.pushBit(bit, dSink ? dSink : std::function<void(const D5066&)>{});
    }

    void eqReset() {
        w.assign(NF, cf(0, 0));
        w[NF / 2] = cf(1, 0);
        b.assign(NB, cf(0, 0));
        for (auto& v : hist) v = cf(0, 0);
        phEq = nu = 0;
    }

    // apply the equalizer around x[p] (with the current frequency correction)
    cf eqOut(size_t p, cf* window) {
        cf out(0, 0);
        double tp = cfoPhase + cfoRad * ((double)p - (NF / 2));
        const cf step((float)std::cos(cfoRad), (float)std::sin(cfoRad));
        cf rot((float)std::cos(tp), (float)std::sin(tp));
        for (int i = 0; i < NF; i++) {
            const size_t idx = p + (size_t)(NF / 2) - (size_t)i;
            window[i] = x[idx] * rot;
            rot *= std::conj(step);
            out += w[(size_t)i] * window[i];
        }
        return out;
    }

    cf eqStep(size_t p, const cf* known, cf* winOut, bool update) {
        cf win[NF];
        cf y = eqOut(p, win);
        for (int i = 0; i < NB; i++) y -= b[(size_t)i] * hist[(size_t)i];
        const cf rot((float)std::cos(phEq), (float)-std::sin(phEq));
        cf yr = y * rot;
        if (winOut) std::copy(win, win + NF, winOut);
        cf dec = known ? *known : yr;
        if (update && known) {
            cf e = (dec - yr) * std::conj(rot);
            double pw = 1e-6;
            for (int i = 0; i < NF; i++) pw += std::norm(win[i]);
            for (int i = 0; i < NB; i++) pw += std::norm(hist[i]);
            const float mu = 0.25f / (float)pw;
            for (int i = 0; i < NF; i++) w[(size_t)i] += mu * e * std::conj(win[i]);
            for (int i = 0; i < NB; i++) b[(size_t)i] -= mu * e * std::conj(hist[i]);
            const double pe = std::arg(yr * std::conj(dec));
            nu = std::clamp(nu + 0.0008 * pe, -0.02, 0.02);
            phEq += nu + 0.05 * pe;
            eqSigma2 = 0.995 * eqSigma2 + 0.005 * std::norm(dec - yr);
        }
        if (!(update && known)) phEq += nu;      // the loop keeps turning over the data symbols
        for (int i = NB - 1; i > 0; i--) hist[i] = hist[i - 1];
        hist[0] = dec * std::conj(rot);
        return yr;
    }

    // ---- 110: search ----
    std::vector<cf> known288;
    void buildKnown() {
        if (known288.empty()) {
            int idx = 0;
            for (int q = 0; q < 9; q++) for (int k = 0; k < 32; k++, idx++) known288.push_back(tribit(preTribit(kFixed[q], k) + kSyncScr[idx % 32]));
        }
    }

    // correlation of 8 symbol blocks over the fixed part; returns normalised peak metric and fills the block phases
    double corr110(size_t p, double* cfo) {
        const int NBLK = 36;
        cf bl[NBLK];
        double acc = 0, en = 0;
        for (int blk = 0; blk < NBLK; blk++) {
            cf s(0, 0);
            for (int k = 0; k < 8; k++) {
                const cf v = x[p + (size_t)(2 * (blk * 8 + k))];
                s += v * std::conj(known288[(size_t)(blk * 8 + k)]);
                en += std::norm(v);
            }
            bl[blk] = s;
            acc += std::abs(s);
        }
        if (cfo) {
            cf d(0, 0);
            for (int i = 0; i + 1 < NBLK; i++) d += bl[i + 1] * std::conj(bl[i]);
            *cfo = std::arg(d) / 16.0;   // radians per 4800 Hz sample
        }
        return acc / std::sqrt(en * 288.0 + 1e-18);
    }

    bool search110() {
        buildKnown();
        const size_t span = 576;
        while (cursor + span + 2 * 40 < x.size()) {
            double m0 = corr110(cursor, nullptr);
            if (m0 > 0.55) {
                size_t bestP = cursor;
                double bestM = m0;
                for (size_t i = 1; i < 30 && cursor + i + span + 80 < x.size(); i++) {
                    const double mi = corr110(cursor + i, nullptr);
                    if (mi > bestM) { bestM = mi; bestP = cursor + i; }
                }
                if (bestP + 960 + 2 * 40 > x.size()) return false;   // need the rest of a segment
                double cfo = 0;
                corr110(bestP, &cfo);
                if (startPreamble(bestP, cfo, bestM)) return true;
                cursor = bestP + 960;
                continue;
            }
            cursor += 1;
        }
        return false;
    }

    bool startPreamble(size_t p, double cfo, double metric) {
        (void)metric;
        cfoRad = -cfo;
        cfoPhase = 0;
        eqReset();
        // train on the fixed 288 symbols, three passes
        for (int pass = 0; pass < 4; pass++) {
            for (auto& v : hist) v = cf(0, 0);
            phEq = nu = 0;
            for (int k = 0; k < 288; k++) {
                const size_t c = p + (size_t)(2 * k);
                if (c < (size_t)NF / 2 + 1 || c + (size_t)NF / 2 + 2 >= x.size()) return false;
                eqStep(c, &known288[(size_t)k], nullptr, true);
            }
        }
        // the variable part: D1, D2, C1..C3
        const size_t q0 = p + 2 * 288;
        int dec[6];
        for (int sym = 0; sym < 6; sym++) {
            cf y[32];
            for (int k = 0; k < 32; k++) { const size_t c = q0 + (size_t)(2 * (32 * sym + k)); if (c + (size_t)NF / 2 + 2 >= x.size()) return false; y[k] = eqStep(c, nullptr, nullptr, false); }
            double best = -1;
            int bi = 0;
            for (int ch = 0; ch < 8; ch++) {
                cf a(0, 0);
                for (int k = 0; k < 32; k++) a += y[k] * std::conj(tribit(preTribit(ch, k) + kSyncScr[(288 + 32 * sym + k) % 32]));
                const double v = a.real();
                if (v > best) { best = v; bi = ch; }
            }
            dec[sym] = bi;
            for (int k = 0; k < 32; k++) { const cf ref = tribit(preTribit(bi, k) + kSyncScr[(288 + 32 * sym + k) % 32]); (void)ref; }
        }
        // count and mode
        int count = 0;
        for (int i = 2; i < 5; i++) count = count * 4 + (dec[i] & 3);
        bool lng = false;
        const Mil110Mode* md = findMode(dec[0], dec[1], lng);
        if (!md) return false;
        if (dec[0] < 4 || dec[2] < 4 || dec[3] < 4 || dec[4] < 4) return false;
        if (count > 23 || (!lng && count > 2)) return false;
        m = md; longIntl = lng; d1 = dec[0]; d2 = dec[1];
        size_t after = p + (size_t)(2 * 480) * ((size_t)count + 1);
        // refine the channel on what is left of the preamble (every symbol is now known)
        pos = p + 2 * 288;
        {
            int dsym = 0, rem = count;
            // this segment's tail
            for (int qq = 9; qq < 15; qq++) { int ch; if (qq == 9) ch = d1; else if (qq == 10) ch = d2; else if (qq == 14) ch = 0; else ch = dec[qq - 9]; for (int k = 0; k < 32; k++) { const cf r = tribit(preTribit(ch, k) + kSyncScr[(qq * 32 + k) % 32]); if (pos + (size_t)NF / 2 + 2 >= x.size()) goto waitMore; eqStep(pos, &r, nullptr, true); pos += 2; dsym++; } }
            while (rem > 0) {
                const int s = rem - 1;
                const int cc[3] = {countSym(s >> 4), countSym(s >> 2), countSym(s)};
                const int seq[15] = {0, 1, 3, 0, 1, 3, 1, 2, 0, d1, d2, cc[0], cc[1], cc[2], 0};
                for (int qq = 0; qq < 15; qq++) for (int k = 0; k < 32; k++) { const cf r = tribit(preTribit(seq[qq], k) + kSyncScr[(qq * 32 + k) % 32]); if (pos + (size_t)NF / 2 + 2 >= x.size()) goto waitMore; eqStep(pos, &r, nullptr, true); pos += 2; }
                rem--;
            }
            goto begun;
        waitMore:
            pendTrain = true;
            pendCount = count;
            mode = 1;
            pendPos = pos;
            pendAfter = after;
            cursor = after;   // resume training on arrival of more samples
            st.state = 1;
            return true;
        begun:;
        }
        pos = after;
        beginData();
        return true;
    }

    bool pendTrain = false;
    int pendCount = 0;
    size_t pendPos = 0, pendAfter = 0;

    void beginData() {
        pendTrain = false;
        char msg[120];
        std::snprintf(msg, sizeof msg, "%d bit/s, %s interleaver", m->bps, m->bps == 4800 ? "no" : longIntl ? "long (4.8 s)" : "short (0.6 s)");
        event(std::string("110 sync: ") + msg);
        st.mode = msg;
        st.state = 2;
        st.frames++;
        scr = Scr110();
        symCount = 0; cycIdx = 0;
        softCh.clear(); vit.reset(); eomReg = 0; dataBits = 0; badBlocks = 0; setCount = 0;
        p5066 = Parser5066();
        zeroIntl = false; tried0 = false;
        mode = 2;
    }

    int setsDone = 0;
    size_t intlBlockBits() const { if (m->bps == 4800) return 0; int r, c; intlDims(m->bps, longIntl, r, c); return (size_t)r * c; }

    void deinterleaveAndDecode() {
        const size_t block = intlBlockBits();
        while (block && softCh.size() >= block) {
            int rows, cols;
            intlDims(m->bps, longIntl, rows, cols);
            std::vector<float> mat((size_t)rows * cols);
            const int rowStep = m->bps == 75 ? 7 : 9, colDec = m->bps == 75 ? 7 : 17;
            { int row = 0, col = 0, colStart = 0; for (size_t i = 0; i < block; i++) { mat[(size_t)row * cols + col] = softCh[i]; row++; col = (col + cols - (colDec % cols)) % cols; if (row == rows) { row = 0; colStart++; col = colStart % cols; } } }
            std::vector<float> coded(block);
            { int row = 0, col = 0, cnt = 0; for (size_t i = 0; i < block; i++) { coded[i] = mat[(size_t)row * cols + col]; row = (row + rowStep) % rows; if (++cnt == rows) { cnt = 0; row = 0; col++; } } }
            softCh.erase(softCh.begin(), softCh.begin() + (long)block);
            feedViterbi(coded);
            st.blocks++;
        }
        if (!block) { std::vector<float> c; c.swap(softCh); feedViterbi(c); }
    }

    void feedViterbi(const std::vector<float>& coded) {
        if (m->bps == 4800) { for (float v : coded) outputBit(v < 0); return; }
        const int rep = m->rep;
        const size_t n = coded.size() / (2 * (size_t)rep);
        for (size_t i = 0; i < n; i++) {
            float a = 0, b2 = 0;
            for (int r = 0; r < rep; r++) { a += coded[i * 2 * rep + (size_t)(2 * r)]; b2 += coded[i * 2 * rep + (size_t)(2 * r) + 1]; }
            vit.push(a, b2);
        }
        std::vector<uint8_t> o;
        vit.take(o);
        for (auto b1 : o) outputBit(b1);
    }

    bool eomHit = false;
    void outputBit(uint8_t b1) {
        emitBit(b1);
        eomReg = (eomReg << 1) | b1;
        if (eomReg == kEOM) eomHit = true;
    }

    void llr8(cf y, double s2, float* out) {
        // bits b1 b2 b3 via the modified Gray mapping (first bit oldest)
        static const int g[8] = {0, 1, 3, 2, 7, 6, 4, 5};
        double mx0[3] = {-1e30, -1e30, -1e30}, mx1[3] = {-1e30, -1e30, -1e30};
        for (int b = 0; b < 8; b++) {
            const cf t = tribit(g[b]);
            const double m1 = (y.real() * t.real() + y.imag() * t.imag()) / s2;
            for (int i = 0; i < 3; i++) { const int bit = (b >> (2 - i)) & 1; if (bit) mx1[i] = std::max(mx1[i], m1); else mx0[i] = std::max(mx0[i], m1); }
        }
        for (int i = 0; i < 3; i++) out[i] = (float)(mx0[i] - mx1[i]);
    }

    void pushSoftSym(cf y, double s2) {
        if (m->bitsPerSym == 3) { float l[3]; llr8(y, s2, l); softCh.push_back(l[0]); softCh.push_back(l[1]); softCh.push_back(l[2]); }
        else if (m->bitsPerSym == 2) {
            std::vector<float> ll;
            softFor(2, y, s2, ll);
            softCh.push_back(ll[0]);
            softCh.push_back(ll[1]);
        } else softCh.push_back((float)(y.real() / s2));
    }

    void data110() {
        if (m->bps == 75) { data75(); return; }
        const int cyc = m->U + m->K;
        const int blockSyms = longIntl ? 11520 : 1440;
        const int cycPerBlock = blockSyms / cyc;
        while (pos + (size_t)NF / 2 + 2 + 2 * (size_t)cyc < x.size()) {
            for (int u = 0; u < m->U; u++) {
                const int s = scr.next();
                cf y = eqStep(pos, nullptr, nullptr, false);
                y *= std::conj(tribit(s));
                // decision-directed update using the nearest allowed constellation point
                int nearest = 0; double bm = -1e9;
                const int stepConst = m->bitsPerSym == 1 ? 4 : m->bitsPerSym == 2 ? 2 : 1;
                for (int t = 0; t < 8; t += stepConst) { const double v = (y * std::conj(tribit(t))).real(); if (v > bm) { bm = v; nearest = t; } }
                const cf ref = tribit(nearest + s);
                dd(pos, ref);
                pushSoftSym(y, std::max(eqSigma2, 0.005));
                pos += 2;
            }
            const int kInBlock = cycIdx % cycPerBlock;
            const int fromEnd = cycPerBlock - 1 - kInBlock;
            int pat = fromEnd == 1 ? d1 : fromEnd == 0 ? d2 : -1;
            double err = 0;
            for (int k = 0; k < m->K; k++) {
                const int s = scr.next();
                int v = 0;
                if (pat >= 0 && k < 16) v = preTribit(pat, k);
                const cf r = tribit(v + s);
                cf y = eqStep(pos, &r, nullptr, true);
                err += std::norm(y - r);
                pos += 2;
            }
            cycIdx++;
            if (err / std::max(1, m->K) > 0.9) { if (++badBlocks > 25) { lost("signal lost (probes no longer fit)"); return; } } else badBlocks = 0;
            if (kInBlock == cycPerBlock - 1 || m->bps == 4800) deinterleaveAndDecode();
            if (eomHit) { eomHit = false; event("end of message"); finish110(); return; }
        }
    }

    void dd(size_t p, cf ref) {
        // adaptation: the eqStep without a known symbol kept the history at the raw estimate: replace with the decision
        const cf rot((float)std::cos(phEq), (float)std::sin(phEq));
        hist[0] = ref * rot;
        (void)p;
    }

    void data75() {
        const int excepEvery = longIntl ? 360 : 45;
        while (pos + (size_t)NF / 2 + 2 + 64 < x.size()) {
            cf y[32];
            int s[32];
            for (int k = 0; k < 32; k++) { s[k] = scr.next(); y[k] = eqStep(pos, nullptr, nullptr, false) * std::conj(tribit(s[k])); pos += 2; }
            setCount++;
            const bool ex = setCount % excepEvery == 0;
            double e[4];
            for (int v = 0; v < 4; v++) {
                int t[32];
                sym75(v, ex, t);
                cf a(0, 0);
                for (int k = 0; k < 32; k++) a += y[k] * std::conj(tribit(t[k]));
                e[v] = std::norm(a);
            }
            int b0 = (int)(std::max_element(e, e + 4) - e);
            // retrain on the decision (replay is enough to hold the phase)
            int t[32];
            sym75(b0, ex, t);
            size_t p0 = pos - 64;
            for (int k = 0; k < 32; k++) { const cf r = tribit(t[k] + s[k]); hist[0] = r; (void)p0; }
            // channel symbols: value v carries Gray bits; bit1 = v>>1 pattern: table IX inverse (value 00->00, 01->01, 11->10, 10->11)
            static const int bitsOf[4] = {0, 1, 3, 2};
            float la = 0, lb = 0;
            for (int bit = 0; bit < 2; bit++) {
                double m0 = -1e30, m1 = -1e30;
                for (int v = 0; v < 4; v++) { const int two = bitsOf[v]; const int bv = bit == 0 ? (two >> 1) & 1 : two & 1; if (bv) m1 = std::max(m1, e[v]); else m0 = std::max(m0, e[v]); }
                if (bit == 0) la = (float)((m0 - m1) * 1e-3); else lb = (float)((m0 - m1) * 1e-3);
            }
            softCh.push_back(la); softCh.push_back(lb);
            if (setCount % excepEvery == 0) deinterleaveAndDecode();
            if (eomHit) { eomHit = false; event("end of message"); finish110(); return; }
        }
    }

    void finish110() {
        std::vector<uint8_t> o;
        vit.flush(o);
        for (auto b1 : o) outputBit(b1);
        eomHit = false;
        event("transmission ended");
        mode = 0;
        cursor = pos;
        st.state = 0;
        trim();
    }

    void lost(const char* why) {
        event(why);
        if (std == SerialStd::Mil110) {
            std::vector<uint8_t> o;
            vit.flush(o);
            for (auto b1 : o) outputBit(b1);
        }
        mode = 0;
        cursor = pos;
        st.state = 0;
    }

    // ---- 4285 ----
    std::vector<cf> known80;
    void build4285() {
        if (known80.empty()) for (int k = 0; k < 80; k++) known80.push_back(tribit(sy[(size_t)k] * 4));
    }

    double corr4285(size_t p, double* cfo) {
        const int NBLK = 10;
        cf bl[NBLK];
        double acc = 0, en = 0;
        for (int blk = 0; blk < NBLK; blk++) {
            cf s(0, 0);
            for (int k = 0; k < 8; k++) {
                const cf v = x[p + (size_t)(2 * (blk * 8 + k))];
                s += v * std::conj(known80[(size_t)(blk * 8 + k)]);
                en += std::norm(v);
            }
            bl[blk] = s;
            acc += std::abs(s);
        }
        if (cfo) {
            cf d(0, 0);
            for (int i = 0; i + 1 < NBLK; i++) d += bl[i + 1] * std::conj(bl[i]);
            *cfo = std::arg(d) / 16.0;
        }
        return acc / std::sqrt(en * 8.0 + 1e-18) / std::sqrt((double)NBLK);
    }

    void hypStart() {
        hyps.clear();
        for (int i = 0; i < kNP4285; i++) {
            if (force >= 0) {
                int fIdx = force >= 10 ? force - 10 + 9 : force;
                if (i != fIdx) continue;
            }
            Hyp h;
            h.p = &kP4285[i];
            if (h.p->coded) h.d.init(h.p->j);
            hyps.push_back(std::move(h));
        }
        if (force >= 0 && !hyps.empty()) { locked = 0; st.mode = hyps[0].p->name; st.state = 2; }
    }

    void pushHyp(Hyp& h, const std::vector<float>& ll) {
        if (!h.p->coded) {
            for (float v : ll) {
                const uint8_t b = v < 0;
                h.reg = (h.reg << 1) | b;
                h.bitsSeen++;
                if ((int)(&h - hyps.data()) == locked) emitBit(b);
            }
            return;
        }
        const int perCycle = h.p->phyBits == 3 ? 24 : 32;
        for (float v : ll) {
            h.partial.push_back(v);
            if ((int)h.partial.size() < perCycle) continue;
            float in[32], out[32];
            int idx = 0;
            for (int r = 0; r < 32; r++) { if (h.p->phyBits == 3 && r % 4 == 3) in[r] = 0; else in[r] = h.partial[(size_t)idx++]; }
            h.partial.clear();
            h.d.cycle(in, out);
            // out in row order -> coded bit order: bit n read from row 9n mod 32
            for (int n = 0; n < 32; n++) { h.rep.push_back(out[(n * 9) % 32]); }
            while ((int)h.rep.size() >= 2 * h.p->rep) {
                float a = 0, b = 0;
                for (int r = 0; r < h.p->rep; r++) { a += h.rep[(size_t)(2 * r)]; b += h.rep[(size_t)(2 * r + 1)]; }
                h.rep.erase(h.rep.begin(), h.rep.begin() + 2 * h.p->rep);
                h.v.push(a, b);
            }
            std::vector<uint8_t> o;
            h.v.take(o);
            for (auto b2 : o) {
                h.reg = (h.reg << 1) | b2;
                h.bitsSeen++;
                if (h.reg == kSOM) {
                    if (!h.somSeen && locked < 0) {
                        locked = (int)(&h - hyps.data());
                        char msg[140];
                        std::snprintf(msg, sizeof msg, "4285: %s (start of message found)", h.p->name);
                        event(msg);
                        st.mode = h.p->name;
                        st.state = 2;
                    }
                    h.somSeen = true;
                    continue;
                }
                if (locked == (int)(&h - hyps.data())) {
                    if (h.reg == kEOM) {
                        event("4285: end of message");
                        h.somSeen = false;
                        locked = -1;
                        continue;
                    }
                    emitBit(b2);
                }
            }
        }
    }

    double frameErr = 0;

    void track4285() {
        const auto& s = scr4;
        while (pos + (size_t)NF / 2 + 2 + 2 * 256 < x.size()) {
            // refine the frame start with the sync (+/- 1 sample)
            double best = -1;
            int bo = 0;
            for (int o = -1; o <= 1; o++) { const double c = corr4285(pos + (size_t)(o + 2) - 2, nullptr); if (c > best) { best = c; bo = o; } }
            if (best < 0.35) {
                if (++frameMissed > 8) { lost("4285: sync lost"); locked = -1; return; }
            } else frameMissed = 0;
            pos = (size_t)((long)pos + bo);
            for (auto& v : hist) v = cf(0, 0);
            for (int k = 0; k < 80; k++) { const cf r = known80[(size_t)k]; eqStep(pos, &r, nullptr, true); pos += 2; }
            std::vector<cf> y(176);
            int k2 = 0;
            for (int k = 0; k < 176; k++) {
                const cf ref = tribit(s[(size_t)k]);
                if (isData4285(k)) {
                    y[(size_t)k] = eqStep(pos, nullptr, nullptr, false) * std::conj(ref);
                } else {
                    y[(size_t)k] = eqStep(pos, &ref, nullptr, true) * std::conj(ref);
                }
                pos += 2;
                k2++;
            }
            (void)k2;
            fr4285++;
            st.frames++;
            processFrame(y);
        }
    }

    void softFor(int phyBits, cf y, double s2, std::vector<float>& ll) {
        if (phyBits == 1) ll.push_back((float)(y.real() / s2));
        else if (phyBits == 2) {
            // symbols 0, 2, 6, 4 for dibit 00, 01, 10, 11
            const float re = y.real() / (float)s2, im = y.imag() / (float)s2;
            float m[4] = {re, im, -re, -im};   // for symbol 0, 2, 4, 6
            const float l0 = std::max(m[0], m[1]) - std::max(m[3], m[2]);   // first bit: 0 for symbols 0,2; 1 for 6,4
            const float l1 = std::max(m[0], m[3]) - std::max(m[1], m[2]);   // second bit: 0 for 0,6; 1 for 2,4
            ll.push_back(l0); ll.push_back(l1);
        } else {
            double mx0[3] = {-1e30, -1e30, -1e30}, mx1[3] = {-1e30, -1e30, -1e30};
            for (int b = 0; b < 8; b++) {
                const cf t = tribit(k3bit4285[b]);
                const double m1 = (y.real() * t.real() + y.imag() * t.imag()) / s2;
                for (int i = 0; i < 3; i++) { if ((b >> (2 - i)) & 1) mx1[i] = std::max(mx1[i], m1); else mx0[i] = std::max(mx0[i], m1); }
            }
            for (int i = 0; i < 3; i++) ll.push_back((float)(mx0[i] - mx1[i]));
        }
    }

    void processFrame(const std::vector<cf>& y) {
        std::vector<float> ll1, ll2, ll3;
        const double s2 = std::max(eqSigma2, 0.01);
        for (int k = 0; k < 176; k++) {
            if (!isData4285(k)) continue;
            softFor(1, y[(size_t)k], s2, ll1);
            softFor(2, y[(size_t)k], s2, ll2);
            softFor(3, y[(size_t)k], s2, ll3);
        }
        for (size_t i = 0; i < hyps.size(); i++) {
            if (locked >= 0 && (int)i != locked) continue;
            Hyp& h = hyps[i];
            pushHyp(h, h.p->phyBits == 1 ? ll1 : h.p->phyBits == 2 ? ll2 : ll3);
        }
    }

    bool search4285() {
        build4285();
        const size_t span = 160;
        while (cursor + 2 * 256 + span + 200 < x.size()) {
            double m0 = corr4285(cursor, nullptr);
            if (m0 > 0.6) {
                // verify it repeats one frame later
                size_t bestP = cursor;
                double bestM = m0;
                for (size_t i = 1; i < 140 && cursor + i + span + 200 < x.size(); i++) {   // the sync sequence has period 31 symbols: take the true peak
                    const double mi = corr4285(cursor + i, nullptr);
                    if (mi > bestM) { bestM = mi; bestP = cursor + i; }
                }
                double c2 = 0;
                if (bestP + 512 + span + 200 < x.size()) c2 = corr4285(bestP + 512, nullptr);
                else return false;
                if (c2 > 0.5) {
                    double cfo = 0;
                    corr4285(bestP, &cfo);
                    cfoRad = -cfo;
                    cfoPhase = 0;
                    eqReset();
                    for (int pass = 0; pass < 6; pass++) {
                        for (auto& v : hist) v = cf(0, 0);
                        phEq = nu = 0;
                        for (int k = 0; k < 80; k++) { const size_t c = bestP + (size_t)(2 * k); if (c < (size_t)NF / 2 + 1 || c + (size_t)NF / 2 + 2 >= x.size()) return false; eqStep(c, &known80[(size_t)k], nullptr, true); }
                    }
                    pos = bestP;
                    mode = 3;
                    frameMissed = 0;
                    hypStart();
                    st.state = 1;
                    st.mode = "searching for the rate";
                    event("4285 frame sync");
                    return true;
                }
            }
            cursor += 1;
        }
        return false;
    }

    void trim() {
        const size_t keep = std::min({cursor, pos, x.size()});
        if (keep > 96000) {
            const size_t drop = keep - 48000;
            x.erase(x.begin(), x.begin() + (long)drop);
            base += drop;
            cursor -= drop;
            if (pos >= drop) pos -= drop; else pos = 0;
            if (pendPos >= drop) pendPos -= drop; else pendPos = 0;
            if (pendAfter >= drop) pendAfter -= drop; else pendAfter = 0;
        }
    }

    void pump() {
        for (;;) {
            if (mode == 0) {
                if (std == SerialStd::Mil110) { if (!search110()) { trim(); return; } }
                else { if (!search4285()) { trim(); return; } }
            }
            if (mode == 1) {
                // finish the preamble: all symbols known, count down the remaining segments
                if (pendPos + (size_t)NF / 2 + 2 >= x.size()) return;
                while (pendPos < pendAfter) {
                    if (pendPos + 10 + (size_t)NF / 2 + 2 >= x.size()) { trim(); return; }
                    const size_t rel = (pendPos - (pendAfter - 2 * 480 * ((size_t)pendCount + 1)));
                    const int segIdx = (int)(rel / 960);
                    const int symIdx = (int)((rel % 960) / 2);
                    const int s = (int)pendCount - 1 - (segIdx - 1 < 0 ? 0 : segIdx - 1);
                    int ch = 0;
                    const int qq = symIdx / 32, k = symIdx % 32;
                    if (qq < 9) ch = kFixed[qq];
                    else if (qq == 9) ch = d1;
                    else if (qq == 10) ch = d2;
                    else if (qq == 14) ch = 0;
                    else {
                        const int sC = segIdx == 0 ? pendCount : s;
                        const int c1 = countSym(sC >> 4), c2 = countSym(sC >> 2), c3 = countSym(sC);
                        ch = qq == 11 ? c1 : qq == 12 ? c2 : c3;
                    }
                    const cf r = tribit(preTribit(ch, k) + kSyncScr[(qq * 32 + k) % 32]);
                    eqStep(pendPos, &r, nullptr, true);
                    pendPos += 2;
                }
                pos = pendAfter;
                beginData();
            }
            if (mode == 2) { data110(); if (mode == 2) { trim(); return; } }
            if (mode == 3) { track4285(); if (mode == 3) { trim(); return; } }
        }
    }

    void push(const cf32* in, size_t n) {
        const double step = 2 * kPi * kCarrier / kBaseRate;
        for (size_t i = 0; i < n; i++) {
            const cf mix((float)std::cos(ph), (float)-std::sin(ph));
            ph += step;
            if (ph > 2 * kPi) ph -= 2 * kPi;
            xfir[xpos] = in[i] * mix;
            xpos = (xpos + 1) % xfir.size();
            if (++dcount == kDecim) {
                dcount = 0;
                cf acc(0, 0);
                size_t p = xpos;
                for (size_t k = 0; k < h.size(); k++) { acc += xfir[p] * h[k]; p = (p + 1) % xfir.size(); }
                x.push_back(acc);
            }
        }
        totalIn += n;
        tSec = (double)totalIn / kBaseRate;
        pump();
        st.cfoHz = -cfoRad * 4800.0 / (2 * kPi);
        st.snrDb = eqSigma2 > 0 ? 10 * std::log10(1.0 / std::max(eqSigma2, 1e-6)) : 0;
    }
};

SerialRx::SerialRx(SerialStd s) : p_(new Impl(s)) {}
SerialRx::~SerialRx() = default;
void SerialRx::forceProfile(int p) { p_->force = p; p_->clear(); }
void SerialRx::reset() { p_->clear(); }
void SerialRx::push(const cf32* x, size_t n) { p_->push(x, n); }
void SerialRx::setBitSink(std::function<void(uint8_t)> cb) { p_->bitSink = std::move(cb); }
void SerialRx::setEventSink(std::function<void(const SerialEvent&)> cb) { p_->evSink = std::move(cb); }
void SerialRx::setD5066Sink(std::function<void(const D5066&)> cb) { p_->dSink = std::move(cb); }
SerialStatus SerialRx::status() const { return p_->st; }

}  // namespace dect2::hfu
