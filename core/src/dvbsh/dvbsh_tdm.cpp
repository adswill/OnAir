// DVB-SH TDM (SH-B): the frame, the signalling field, the constellations, the generator and the receiver. See dvbsh_tdm.h.
#include "dect2/dvbsh/dvbsh_tdm.h"
#include "dect2/dvbs/dvbs_s2.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/pskdsp/pskdsp.h"
#include "../dvbs/dvbs_front.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

namespace dect2 {
namespace dvbsh {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kLlrScale = 4.f;   // int8 LLRs in the time de-interleaver, as SH-A

// table 5.12, rows GI 1/4, 1/8, 1/16, 1/32; per row the roll-offs 0.15, 0.25, 0.35; per roll-off OFDM QPSK: TDM QPSK, 8PSK, 16APSK, then
// OFDM 16-QAM: TDM QPSK, 8PSK, 16APSK
const int kFrameCu[4][3][6] = {
    {{952, 1428, 1904, 476, 714, 952}, {896, 1344, 1792, 448, 672, 896}, {812, 1218, 1624, 406, 609, 812}},
    {{868, 1302, 1736, 434, 651, 868}, {784, 1176, 1568, 392, 588, 784}, {728, 1092, 1456, 364, 546, 728}},
    {{812, 1218, 1624, 406, 609, 812}, {756, 1134, 1512, 378, 567, 756}, {700, 1050, 1400, 350, 525, 700}},
    {{784, 1176, 1568, 392, 588, 784}, {728, 1092, 1456, 364, 546, 728}, {672, 1008, 1344, 336, 504, 672}},
};
// table 5.13 in MHz as fractions: [bandwidth 8, 7, 6, 5, 1.7][GI 1/4, 1/8, 1/16, 1/32][roll-off 0.15, 0.25, 0.35]
struct Frac { int n, d; };
const Frac kRate[5][4][3] = {
    {{{34, 5}, {32, 5}, {29, 5}}, {{62, 9}, {56, 9}, {52, 9}}, {{116, 17}, {108, 17}, {100, 17}}, {{224, 33}, {208, 33}, {64, 11}}},
    {{{119, 20}, {28, 5}, {203, 40}}, {{217, 36}, {49, 9}, {91, 18}}, {{203, 34}, {189, 34}, {175, 34}}, {{196, 33}, {182, 33}, {56, 11}}},
    {{{51, 10}, {24, 5}, {87, 20}}, {{31, 6}, {14, 3}, {13, 3}}, {{87, 17}, {81, 17}, {75, 17}}, {{56, 11}, {52, 11}, {48, 11}}},
    {{{17, 4}, {4, 1}, {29, 8}}, {{155, 36}, {35, 9}, {65, 18}}, {{145, 34}, {135, 34}, {125, 34}}, {{140, 33}, {130, 33}, {40, 11}}},
    {{{34, 25}, {32, 25}, {29, 25}}, {{62, 45}, {56, 45}, {52, 45}}, {{116, 85}, {108, 85}, {20, 17}}, {{224, 165}, {208, 165}, {64, 55}}},
};
int bwIndex(double bw) {
    const double w[5] = {8, 7, 6, 5, 1.7};
    for (int i = 0; i < 5; i++) if (std::fabs(bw - w[i]) < 0.05) return i;
    return -1;
}

void putBits(uint8_t* b, int at, unsigned v, int n) { for (int i = 0; i < n; i++) b[at + i] = (uint8_t)((v >> (n - 1 - i)) & 1); }
unsigned getBits(const uint8_t* b, int at, int n) { unsigned v = 0; for (int i = 0; i < n; i++) v = v << 1 | (b[at + i] & 1); return v; }

// the bit-wise interleaver of table 5.6: b_w = a_H(w), H(w) = 73 w mod 5760
inline int sfAddr(int w) { return (int)((73LL * w) % kSfBits); }
} // namespace

const char* tdmModName(int mod) { return mod == kTdm8psk ? "8PSK" : mod == kTdm16apsk ? "16APSK" : "QPSK"; }
double tdmRollOff(int r) { return r == 1 ? 0.25 : r == 2 ? 0.35 : 0.15; }
int tdmFrameCu(int mod, int guard, int roll, bool q16) {
    if (mod < 0 || mod > 2 || guard < 0 || guard > 3 || roll < 0 || roll > 2) return 0;
    return kFrameCu[3 - guard][roll][(q16 ? 3 : 0) + mod];
}
double tdmSymbolRate(double bw, int guard, int roll) {
    const int b = bwIndex(bw);
    if (b < 0 || guard < 0 || guard > 3 || roll < 0 || roll > 2) return 0;
    const Frac f = kRate[b][3 - guard][roll];
    return 1e6 * f.n / f.d;
}

// ============================================================================ signalling field
bool TdmSignalling::operator==(const TdmSignalling& o) const {
    return version == o.version && frameWidthCu == o.frameWidthCu && pat == o.pat && ti == o.ti && llPresent == o.llPresent && llSatActive == o.llSatActive &&
           nextConf == o.nextConf && llPat == o.llPat && bursts == o.bursts && burst == o.burst;
}

void packSignalling(const TdmSignalling& s, uint8_t* b) {
    std::memset(b, 0, kSigInput);
    putBits(b, 0, (unsigned)s.version, 8);
    putBits(b, 8, 0, 8);
    putBits(b, 16, (unsigned)s.frameWidthCu, 12);
    putBits(b, 28, (unsigned)s.pat, 4);
    putBits(b, 32, (unsigned)s.ti.commonMultiplier, 6);
    putBits(b, 38, (unsigned)s.ti.lateTaps, 6);
    putBits(b, 44, (unsigned)s.ti.slices, 6);
    putBits(b, 50, (unsigned)s.ti.sliceDistance, 8);
    putBits(b, 58, (unsigned)s.ti.nonLateIncrement, 6);
    putBits(b, 96, crc16Bits(b, 96), 16);
    b[112] = s.llPresent; b[114] = s.llSatActive; b[115] = s.nextConf;
    putBits(b, 116, (unsigned)s.llPat, 4);
    putBits(b, 124, (unsigned)s.bursts, 4);
    for (int i = 0; i < 16; i++) putBits(b, 128 + 16 * i, s.burst[(size_t)i], 16);
    putBits(b, 384, crc16Bits(b + 112, 272), 16);
}

bool unpackSignalling(const uint8_t* b, TdmSignalling& s) {
    if (getBits(b, 96, 16) != crc16Bits(b, 96)) return false;
    if (getBits(b, 8, 8) != 0) return false;   // RFU not 0: the receiver ignores the field (table 5.9)
    s = TdmSignalling();
    s.version = (int)getBits(b, 0, 8);
    s.frameWidthCu = (int)getBits(b, 16, 12);
    s.pat = (int)getBits(b, 28, 4);
    s.ti.commonMultiplier = (int)getBits(b, 32, 6);
    s.ti.lateTaps = (int)getBits(b, 38, 6);
    s.ti.slices = (int)getBits(b, 44, 6);
    s.ti.sliceDistance = (int)getBits(b, 50, 8);
    s.ti.nonLateIncrement = (int)getBits(b, 58, 6);
    if (getBits(b, 384, 16) == crc16Bits(b + 112, 272)) {
        s.llPresent = b[112]; s.llSatActive = b[114]; s.nextConf = b[115];
        s.llPat = (int)getBits(b, 116, 4);
        s.bursts = (int)getBits(b, 124, 4);
        for (int i = 0; i < 16; i++) s.burst[(size_t)i] = (uint16_t)getBits(b, 128 + 16 * i, 16);
    }
    return true;
}

void encodeSignalling(const TdmSignalling& s, std::vector<uint8_t>& out) {
    uint8_t info[kSigInput];
    packSignalling(s, info);
    std::vector<uint8_t> coded;
    turboEncode(info, kSigInput, 0, coded);
    out.assign(kSfBits, 0);
    for (int w = 0; w < kSfBits && w < (int)coded.size(); w++) out[(size_t)w] = coded[(size_t)sfAddr(w)];
}

bool decodeSignalling(const float* llr, TdmSignalling& s, int* iters) {
    std::vector<float> a(kSfBits);
    for (int w = 0; w < kSfBits; w++) a[(size_t)sfAddr(w)] = llr[w];
    std::vector<uint8_t> info;
    auto stop = [](const uint8_t* b) { return getBits(b, 96, 16) == crc16Bits(b, 96); };
    const int it = turboDecode(a.data(), kSigInput, 0, 8, info, stop);
    if (iters) *iters = it;
    return (int)info.size() >= kSigInput && unpackSignalling(info.data(), s);
}

const std::array<uint8_t, kSofBits>& sofBits() {
    static const std::array<uint8_t, kSofBits> b = [] {
        static const uint32_t w[9] = {0xF3484536u, 0xB855DF1Bu, 0x6FD32468u, 0xF368BC5Au, 0x6CD02627u, 0x074CB0A4u, 0x11979705u, 0x08F31EDDu, 0xACCF9E4Fu};
        std::array<uint8_t, kSofBits> r{};
        for (int i = 0; i < kSofBits; i++) r[(size_t)i] = (uint8_t)((w[i / 32] >> (31 - i % 32)) & 1);
        return r;
    }();
    return b;
}

// ============================================================================ constellations
const cf32* tdmConstellation(int mod) {
    static const std::vector<cf32> apsk = [] {
        // figure 5.19: the DVB-S2 16APSK labels (EN 302 307 figure 12) on rings of R1 = 1/sqrt(7) and R2 = 3/sqrt(7) (gamma = 3)
        struct P { int label, ring; double deg; };
        static const P pts[16] = {{0xC, 0, 45}, {0xE, 0, 135}, {0xF, 0, 225}, {0xD, 0, 315}, {0x4, 1, 15}, {0x0, 1, 45}, {0x8, 1, 75}, {0xA, 1, 105},
                                  {0x2, 1, 135}, {0x6, 1, 165}, {0x7, 1, 195}, {0x3, 1, 225}, {0xB, 1, 255}, {0x9, 1, 285}, {0x1, 1, 315}, {0x5, 1, 345}};
        std::vector<cf32> v(16);
        const double r[2] = {1.0 / std::sqrt(7.0), 3.0 / std::sqrt(7.0)};
        for (const P& p : pts) v[(size_t)p.label] = cf32((float)(r[p.ring] * std::cos(p.deg * kPi / 180)), (float)(r[p.ring] * std::sin(p.deg * kPi / 180)));
        return v;
    }();
    if (mod == kTdm16apsk) return apsk.data();
    return dvbs::s2Constellation(mod == kTdm8psk ? dvbs::k8psk : dvbs::kQpsk, 4);   // QPSK and 8PSK: figures 5.17 and 5.18 = DVB-S2 figures 9 and 10
}

void tdmDemap(const cf32* y, int n, int mod, float sigma2, float* llr) {
    const int m = tdmBitsPerSymbol(mod), P = 1 << m;
    const cf32* c = tdmConstellation(mod);
    const float w = 1.f / (2.f * std::max(sigma2, 1e-6f));
    float d[16];
    for (int i = 0; i < n; i++) {
        for (int p = 0; p < P; p++) d[p] = std::norm(y[i] - c[p]);
        for (int b = 0; b < m; b++) {
            float m0 = 1e30f, m1 = 1e30f;
            const int sh = m - 1 - b;
            for (int p = 0; p < P; p++) { if ((p >> sh) & 1) m1 = std::min(m1, d[p]); else m0 = std::min(m0, d[p]); }
            llr[(size_t)i * m + b] = w * (m1 - m0);
        }
    }
}

const std::vector<cf32>& tdmScrambling(int n) {
    static std::mutex mu;
    static std::vector<std::pair<int, std::vector<cf32>>> cache;
    std::lock_guard<std::mutex> lk(mu);
    for (auto& e : cache) if (e.first == n) return e.second;
    const auto& R = dvbs::s2ScramblingRn(n);
    std::vector<cf32> v(kSlotSymbols);
    static const cf32 rot[4] = {cf32(1, 0), cf32(0, 1), cf32(-1, 0), cf32(0, -1)};
    for (int i = 0; i < kSlotSymbols; i++) v[(size_t)i] = rot[R[(size_t)i] & 3];
    cache.emplace_back(n, std::move(v));
    return cache.back().second;
}

// ============================================================================ generator
TdmGenerator::TdmGenerator(const TdmParams& p, PacketSource src) : p_(p), src_(src ? std::move(src) : dvbt::testTsSource(1)) {
    std::vector<int> L;
    const int fw = p_.frameCuOrTable();
    if (!tiDelays(p_.ti, std::max(3, tdmDataCu(fw, p_.pat)), L)) { p_.ti = TiConfig{}; tiDelays(p_.ti, tdmDataCu(fw, p_.pat), L); }
    ti_.configure(L, false);
}
TdmGenerator::~TdmGenerator() = default;

void TdmGenerator::buildFrame() {
    const int fw = p_.frameCuOrTable();
    frameBits_.assign((size_t)fw * kCuBits, 0);
    const auto& sof = sofBits();
    std::copy(sof.begin(), sof.end(), frameBits_.begin());
    TdmSignalling s;
    s.frameWidthCu = fw; s.pat = p_.pat; s.ti = p_.ti;
    std::vector<uint8_t> sf;
    encodeSignalling(s, sf);
    std::copy(sf.begin(), sf.end(), frameBits_.begin() + kSofBits);
    // DATA: whole code words through the time interleaver (only the data part is interleaved, clause 5.5.2.1)
    const int ncw = tdmCodewords(fw, p_.pat);
    size_t at = (size_t)kHeaderCu * kCuBits;
    uint8_t pk[188 * kPackets];
    std::vector<uint8_t> ef(kTcInput), cw;
    std::vector<uint8_t> data;
    data.reserve((size_t)ncw * nbil(p_.pat));
    for (int c = 0; c < ncw; c++) {
        for (int k = 0; k < kPackets; k++) src_(pk + k * 188);
        buildEframe(pk, c, ef.data());
        encodeCodeword(ef.data(), p_.pat, cw);
        data.insert(data.end(), cw.begin(), cw.end());
        codewords_++;
    }
    for (size_t i = 0; i + kIuBits <= data.size(); i += kIuBits) ti_.process(data.data() + i, frameBits_.data() + at + i);
    at += data.size();
    // PADDING: the PRBS loaded again for every SH frame
    if (at < frameBits_.size()) prbsBits(frameBits_.size() - at, frameBits_.data() + at);
    pos_ = 0;
    frames_++;
}

void TdmGenerator::nextSlot(std::vector<cf32>& out) {
    const int m = tdmBitsPerSymbol(p_.mod);
    if (pos_ >= frameBits_.size()) buildFrame();
    const cf32* c = tdmConstellation(p_.mod);
    const auto& C = tdmScrambling(p_.goldN);
    out.resize(kSlotSymbols);
    const cf32 pilot((float)std::sqrt(0.5), (float)std::sqrt(0.5));
    for (int i = 0; i < kSlotSymbols; i++) {
        cf32 v;
        if (tdmIsPilot(i)) v = pilot;
        else {
            unsigned l = 0;
            for (int b = 0; b < m; b++) l = l << 1 | frameBits_[pos_++];
            v = c[l];
        }
        out[(size_t)i] = v * C[(size_t)i];
    }
}

struct TdmSignal::Impl {
    TdmSignalConfig cfg;
    TdmGenerator gen;
    std::vector<float> rrc;
    std::vector<cf32> outBuf;
    size_t outPos = 0;
    ExactResampler rs;
    double ph = 0, dph = 0;
    float sigma = 0, gain = 1;
    genutil::NoiseSource noise;
    Impl(const TdmSignalConfig& c, TdmGenerator::PacketSource src) : cfg(c), gen(c.p, std::move(src)), noise(c.seed) {
        rrc = pskd::rrcTaps(4.0, tdmRollOff(c.p.rollIdx), 12);
        const double rsym = c.p.symbolRate() * (1.0 + c.clockPpm * 1e-6);
        rs.configure(4.0 * rsym, c.sampleRate);
        dph = 2 * kPi * c.cfoHz / c.sampleRate;
        // unit-energy taps at 4 samples per symbol and unit-power symbols: 1/4 per sample; Es/N0 = P (fs / Rs) / sigma^2
        const double P = 0.25, sps = c.sampleRate / rsym;
        gain = (float)(c.level / std::sqrt(P));
        sigma = c.snrDb >= 150 ? 0.f : (float)(gain * std::sqrt(P * sps / std::pow(10.0, c.snrDb / 10) / 2.0));
    }
    std::deque<cf32> symHist;   // the last symbols, newest first
    void more() {
        std::vector<cf32> sl;
        gen.nextSlot(sl);
        // upsampling by 4 with the RRC filter, polyphase: y[4n + p] = sum_k s[n - k] h[4k + p]
        const int L = (int)rrc.size(), K = (L + 3) / 4;
        std::vector<cf32> x;
        x.reserve(sl.size() * 4);
        for (const cf32& v : sl) {
            symHist.push_front(v);
            if ((int)symHist.size() > K) symHist.pop_back();
            for (int p = 0; p < 4; p++) {
                cf32 acc(0, 0);
                for (int k = 0; k < (int)symHist.size(); k++) { const int t = 4 * k + p; if (t < L) acc += symHist[(size_t)k] * rrc[(size_t)t]; }
                x.push_back(acc);
            }
        }
        std::vector<cf32> o;
        rs.process(x.data(), x.size(), o);
        for (auto& v : o) {
            v *= gain * cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += dph;
            if (ph > kPi) ph -= 2 * kPi;
        }
        if (sigma > 0) noise.add(o.data(), o.size(), sigma);
        outBuf.erase(outBuf.begin(), outBuf.begin() + (long)outPos);
        outPos = 0;
        outBuf.insert(outBuf.end(), o.begin(), o.end());
    }
};

TdmSignal::TdmSignal(const TdmSignalConfig& c, TdmGenerator::PacketSource src) : p_(std::make_unique<Impl>(c, std::move(src))) {}
TdmSignal::~TdmSignal() = default;
const TdmGenerator& TdmSignal::generator() const { return p_->gen; }
void TdmSignal::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    size_t done = 0;
    while (done < n) {
        if (m.outPos >= m.outBuf.size()) m.more();
        const size_t k = std::min(n - done, m.outBuf.size() - m.outPos);
        std::copy(m.outBuf.begin() + (long)m.outPos, m.outBuf.begin() + (long)(m.outPos + k), out + done);
        m.outPos += k; done += k;
    }
}

// ============================================================================ receiver
struct TdmReceiver::Impl {
    double fs = 0, bw = 5, fixedRate = 0;
    int goldN = 0;
    size_t maxTiBytes = (size_t)768 << 20;
    std::function<void(const uint8_t*, size_t)> cb;
    TdmStatus st;
    mutable std::mutex stMu;
    // candidates of table 5.13 for the width (rows GI x roll-off), and where the search is
    std::vector<std::pair<double, double>> cands;   // (symbol rate, roll-off)
    int cand = 0;
    double candSymbols = 0;
    dvbs::DvbsFront front;
    std::vector<cf32> sy;          // symbols from the front end
    long long syBase = 0;          // absolute index of sy[0]
    long long symbolsIn = 0, samplesIn = 0, lockSamples = 0, lockSymbols = 0;
    // slot tracking
    std::vector<cf32> pref;        // the scrambled pilot reference of a slot (zero on data positions)
    bool slotLock = false;
    long long slotStart = 0;       // absolute symbol index of the next slot to process
    double w = 0;                  // carrier frequency, radians per symbol (removed by the NCO)
    double fieldPhase[2] = {0, 0}; // unwrapped phase of the previous two pilot fields
    double prevFieldPhase = 0;
    bool havePrevField = false;
    long badFields = 0;
    double lastPhase = 0, goodRate = 0;
    double amp = 1, sigma2 = 0.1;
    // data symbols of the current slot (descrambled, phase corrected), waiting for the second sub-slot
    std::vector<cf32> slotData;
    long long slotIdx = 0;         // slots since the slot lock
    std::vector<cf32> pendingSub2; // raw (NCO-corrected) second sub-slot of the previous slot, until the next pilot field
    long long pendingSub2Start = -1;
    double pendingPhase2 = 0;
    std::vector<cf32> sub1;        // first sub-slot of the slot being assembled
    // frame
    int mod = -1;
    bool frameLock = false;
    long long frameSlot = 0;       // slotIdx of the current frame's first slot
    int slotsPerFrame = 0, frameCu = 0;
    long missedSof = 0;
    TdmSignalling sf;
    bool sfOk = false;
    std::vector<float> sfLlr;
    // data path
    TimeInterleaver<int8_t> ti;
    std::vector<int> L;
    bool dataOn = false;
    int pat = 4, ncw = 0, cwIu = 0;
    long long gIn = 0;
    std::vector<int8_t> iuAcc;
    std::vector<float> cwLlr, coded;
    std::vector<uint8_t> info;
    int cwIdx = -1, cwFill = 0;
    bool cwValid = false;
    long iterSum = 0;

    void setupFront() {
        const double rate = fixedRate > 0 ? fixedRate : cands.empty() ? 4e6 : cands[(size_t)cand].first;
        const double ro = fixedRate > 0 ? 0.25 : cands.empty() ? 0.25 : cands[(size_t)cand].second;
        dvbs::DvbsFront::Config c;
        c.fs = fs; c.symbolRate = rate; c.centerHz = 0; c.rollOff = ro;
        front.configure(c);
        front.timer().setLoop(0.002, 0.707);
        sy.clear(); syBase = 0; candSymbols = 0;
        slotLock = false; frameLock = false; mod = -1; sfOk = false; dataOn = false;
        std::lock_guard<std::mutex> lk(stMu);
        st.level = 0; st.symbolRate = rate; st.rateCandidate = fixedRate > 0 ? -1 : cand; st.mod = -1;
    }
    void configure(double f, double b) {
        fs = f; bw = b;
        cands.clear();
        for (int r = 0; r < 3; r++) for (int g = 3; g >= 0; g--) { const double s = tdmSymbolRate(bw, g, r); if (s > 0 && s * 1.4 < fs) cands.push_back({s, tdmRollOff(r)}); }
        cand = 0;
        pref.assign(kSlotSymbols, cf32(0, 0));
        const auto& C = tdmScrambling(goldN);
        const cf32 pilot((float)std::sqrt(0.5), (float)std::sqrt(0.5));
        for (int i = 0; i < kSlotSymbols; i++) if (tdmIsPilot(i)) pref[(size_t)i] = pilot * C[(size_t)i];
        { std::lock_guard<std::mutex> lk(stMu); st = TdmStatus(); }
        setupFront();
    }

    // ---- slot search: the pilot fields of four slots, correlated in blocks of 20 symbols (frequency offsets up to 1/40 cycle per symbol)
    bool searchSlot() {
        const size_t need = (size_t)kSlotSymbols * 5;
        if (sy.size() < need) return false;
        std::vector<float> m(kSlotSymbols, 0.f);
        double e = 0;
        for (size_t i = 0; i < (size_t)kSlotSymbols * 4; i++) e += std::norm(sy[i]);
        const float ms = (float)std::sqrt(e / (kSlotSymbols * 4.0));
        for (int t = 0; t < kSlotSymbols; t++) {
            float acc = 0;
            for (int s = 0; s < 4; s++)
                for (int f = 0; f < 2; f++)
                    for (int b = 0; b < 4; b++) {
                        const int o = f * (kPilotField + kSubSlot) + b * 20;
                        cf32 z(0, 0);
                        const cf32* y = &sy[(size_t)(t + s * kSlotSymbols + o)];
                        const cf32* r = &pref[(size_t)o];
                        for (int i = 0; i < 20; i++) z += y[i] * std::conj(r[i]);
                        acc += std::abs(z);
                    }
            m[(size_t)t] = acc / (ms * 640.f);
        }
        int best = 0;
        for (int t = 1; t < kSlotSymbols; t++) if (m[(size_t)t] > m[(size_t)best]) best = t;
        std::vector<float> srt(m);
        std::nth_element(srt.begin(), srt.begin() + kSlotSymbols / 2, srt.end());
        const float med = srt[kSlotSymbols / 2];
        if (!(m[(size_t)best] > 0.45f && m[(size_t)best] > 2.5f * med)) {
            // nothing: drop most of what was searched
            const size_t drop = (size_t)kSlotSymbols * 3;
            sy.erase(sy.begin(), sy.begin() + (long)drop); syBase += (long long)drop;
            return false;
        }
        // frequency: phase steps between the 20-symbol blocks of the fields (coarse), then between the two fields of each slot
        std::complex<double> c1(0, 0), c2(0, 0);
        for (int s = 0; s < 4; s++) {
            cf32 zf[2] = {cf32(0, 0), cf32(0, 0)};
            for (int f = 0; f < 2; f++) {
                cf32 zb[4];
                for (int b = 0; b < 4; b++) {
                    const int o = f * (kPilotField + kSubSlot) + b * 20;
                    cf32 z(0, 0);
                    for (int i = 0; i < 20; i++) z += sy[(size_t)(best + s * kSlotSymbols + o + i)] * std::conj(pref[(size_t)(o + i)]);
                    zb[b] = z;
                }
                for (int b = 0; b + 1 < 4; b++) c1 += std::complex<double>(zb[b + 1] * std::conj(zb[b]));
                zf[f] = zb[0] + zb[1] + zb[2] + zb[3];
            }
            c2 += std::complex<double>(zf[1] * std::conj(zf[0]));
        }
        const double wc = std::arg(c1) / 20.0;
        // refine with the 1088-symbol spacing: remove the coarse turn, take the residual
        const double res = std::remainder(std::arg(c2) - wc * (kPilotField + kSubSlot), 2 * kPi);
        w = wc + res / (kPilotField + kSubSlot);
        slotLock = true;
        goodRate = 0;
        slotStart = syBase + best;
        lockSamples = samplesIn; lockSymbols = symbolsIn;
        havePrevField = false; badFields = 0; slotIdx = 0; pendingSub2Start = -1;
        frameLock = false; mod = -1;
        std::lock_guard<std::mutex> lk(stMu);
        st.level = std::max(st.level, 1);
        return true;
    }

    // NCO-corrected symbol at absolute index n (phase of the carrier at slotStart taken as reference: ph(n) = w (n - ref))
    long long ncoRef = 0;
    double ncoPh = 0;
    cf32 at(long long n) const {
        const double p = ncoPh + w * (double)(n - ncoRef);
        return sy[(size_t)(n - syBase)] * cf32((float)std::cos(-p), (float)std::sin(-p));
    }
    // phase and amplitude of the pilot field starting at absolute index s (field f of its slot)
    bool fieldAt(long long s, int f, double& ph, double& a, double& var) {
        const int o = f * (kPilotField + kSubSlot);
        std::complex<double> z(0, 0);
        double p = 0;
        for (int i = 0; i < kPilotField; i++) { const cf32 v = at(s + i) * std::conj(pref[(size_t)(o + i)]); z += std::complex<double>(v); p += std::norm(v); }
        z /= kPilotField; p /= kPilotField;
        ph = std::arg(z);
        a = std::abs(z) / std::sqrt(0.5 * 2);   // the pilot has unit magnitude after the reference (|pref| = 1)
        var = std::max(1e-9, p - std::norm(z));
        return std::abs(z) * std::abs(z) > 0.25 * p;
    }

    void emitData(const cf32* y, int n) {
        // n data symbols of slot slotIdx (in order), to the frame / bit stream
        slotData.insert(slotData.end(), y, y + n);
    }

    // one slot: called when the slot's two fields and the next slot's first field are in the buffer
    bool processSlot() {
        const long long s = slotStart;
        const long long need = s + kSlotSymbols + kPilotField + 4;
        if (need > syBase + (long long)sy.size()) return false;
        if (slotIdx == 0 && !havePrevField) { ncoRef = s; ncoPh = 0; }
        // slot position check: the first field at -2 .. 2 (a symbol slip of the timing loop moves the grid); only on a field that is there
        double p1, a1, v1, p2, a2, v2, p3, a3, v3;
        const bool ok1 = fieldAt(s, 0, p1, a1, v1);
        if (ok1 || badFields > 0) {
            double bestA = -1, a0 = 0; int bestD = 0;
            const int span = badFields > 0 ? 32 : 2;   // after a blockage the grid may have moved further
            for (int d = -span; d <= span; d++) {
                if (s + d - syBase < 0) continue;
                double ph, a, v;
                const bool there = fieldAt(s + d, 0, ph, a, v);
                if (d == 0) a0 = a;
                if (there && a > bestA) { bestA = a; bestD = d; }   // only a field that is really there (not noise during a blockage)
            }
            if (bestD != 0 && bestA > 1.5 * a0) { slotStart += bestD; return true; }   // clearly better there: try again at the moved position
        }
        const bool ok2 = fieldAt(s + kPilotField + kSubSlot, 1, p2, a2, v2);
        const bool ok3 = fieldAt(s + kSlotSymbols, 0, p3, a3, v3);
        // A blockage (a bridge, a tunnel: what the long time interleaver is for): the slot grid, the frequency and the levels are held
        // (flywheel) for up to about two seconds, so that the de-interleaver keeps its alignment and the code words come back after it
        const bool good = ok1 && ok2 && ok3;
        if (!ok1 && !ok2) {
            if (++badFields > std::max(50.0, 2.0 * front.symbolRateNow() / kSlotSymbols)) { slotLock = false; frameLock = false; dataOn = false; std::lock_guard<std::mutex> lk(stMu); st.level = 0; return true; }
        } else badFields = 0;
        if (!ok1) p1 = lastPhase;
        if (!ok2) p2 = ok1 ? p1 : lastPhase;
        if (!ok3) p3 = ok2 ? p2 : p1;
        lastPhase = ok3 ? p3 : ok2 ? p2 : p1;
        // unwrap the three field phases, and steer the NCO frequency with the drift between the fields
        const double u2 = p1 + std::remainder(p2 - p1, 2 * kPi), u3 = u2 + std::remainder(p3 - p2, 2 * kPi);
        const double cen1 = (double)s + 39.5, cen2 = (double)(s + kPilotField + kSubSlot) + 39.5, cen3 = (double)(s + kSlotSymbols) + 39.5;
        if (good) {
            goodRate = goodRate > 0 ? 0.95 * goodRate + 0.05 * front.symbolRateNow() : front.symbolRateNow();
            amp = 0.9 * amp + 0.1 * std::max(1e-3, (a1 + a2) / 2);
            sigma2 = 0.9 * sigma2 + 0.1 * ((v1 + v2) / 2 / std::max(1e-6, amp * amp)) / 2;   // per real dimension, relative to unit symbols
        }
        const auto& C = tdmScrambling(goldN);
        std::vector<cf32> d((size_t)kSlotData);
        int k = 0;
        for (int i = 0; i < kSlotSymbols; i++) {
            if (tdmIsPilot(i)) continue;
            const double pos = (double)(s + i);
            const double ph = i < kPilotField + kSubSlot ? p1 + (u2 - p1) * (pos - cen1) / (cen2 - cen1) : u2 + (u3 - u2) * (pos - cen2) / (cen3 - cen2);
            const cf32 rot((float)std::cos(-ph), (float)std::sin(-ph));
            d[(size_t)k++] = at(s + i) * std::conj(C[(size_t)i]) * rot / (float)amp;
        }
        // frequency loop: the phase drifts by (u3 - p1) over one slot
        const double drift = good ? (u3 - p1) / (cen3 - cen1) : 0.0;
        w += 0.5 * drift;
        // keep the NCO continuous: re-reference at the next slot
        ncoPh = ncoPh + (w - 0.5 * drift) * (double)(s + kSlotSymbols - ncoRef);
        ncoPh = std::remainder(ncoPh, 2 * kPi);
        ncoRef = s + kSlotSymbols;
        slotStart = s + kSlotSymbols;
        slotsDone(d, !ok1 && !ok2);
        slotIdx++;
        // drop what is no longer needed
        const long long keep = slotStart - 8;
        if (keep > syBase) { sy.erase(sy.begin(), sy.begin() + (long)(keep - syBase)); syBase = keep; }
        return true;
    }

    // ---- frames
    std::vector<cf32> sofSym[3];
    void makeSof() {
        if (!sofSym[0].empty()) return;
        const auto& b = sofBits();
        for (int m = 0; m < 3; m++) {
            const int k = tdmBitsPerSymbol(m);
            const cf32* c = tdmConstellation(m);
            for (int i = 0; i + k <= kSofBits; i += k) { unsigned l = 0; for (int j = 0; j < k; j++) l = l << 1 | b[(size_t)(i + j)]; sofSym[m].push_back(c[l]); }
        }
    }
    float sofScore(const std::vector<cf32>& d, int m) {
        const auto& s = sofSym[m];
        std::complex<double> z(0, 0);
        double ey = 0, es = 0;
        for (size_t i = 0; i < s.size(); i++) { z += std::complex<double>(d[i] * std::conj(s[i])); ey += std::norm(d[i]); es += std::norm(s[i]); }
        return (float)(std::abs(z) / std::sqrt(std::max(1e-12, ey * es)));
    }
    long long frameBitPos = 0;     // bits of the current frame seen
    std::vector<float> llrBuf;
    void slotsDone(const std::vector<cf32>& d, bool erased) {
        makeSof();
        {
            std::lock_guard<std::mutex> lk(stMu);
            st.slots++;
            st.snrDb = (float)(10 * std::log10(1.0 / std::max(1e-9, 2 * sigma2)));
            st.cfoHz = w / (2 * kPi) * front.symbolRateNow();
            st.symbolRate = front.symbolRateNow();
            // the clock offset from the symbols the front end produced against the samples fed since the slot lock (the timing loop's own
            // rate estimate jitters by tens of ppm)
            // against the table rate nearest to what is measured (the timing loop can pull to a neighbouring row of table 5.13)
            double nominal = fixedRate > 0 ? fixedRate : cands.empty() ? 0 : cands[(size_t)cand].first;
            if (fixedRate <= 0) for (const auto& c : cands) if (std::fabs(c.first / st.symbolRate - 1) < std::fabs(nominal / st.symbolRate - 1)) nominal = c.first;
            const double secs = (double)(samplesIn - lockSamples) / fs;
            if (nominal > 0 && secs > 0.05) st.clockPpm = ((double)(symbolsIn - lockSymbols) / secs / nominal - 1) * 1e6;
        }
        const bool atFrameStart = frameLock && slotsPerFrame > 0 && (slotIdx - frameSlot) % slotsPerFrame == 0;
        if (erased && (!frameLock || atFrameStart)) {
            if (!frameLock) return;
            frameSlot = slotIdx; frameBitPos = 0; sfLlr.clear();   // a frame start inside a blockage: keep the grid, no SOF to check
        } else if (!frameLock || atFrameStart) {
            int bestM = -1; float bestS = 0;
            for (int m = 0; m < 3; m++) { if (frameLock && m != mod) continue; const float sc = sofScore(d, m); if (sc > bestS) { bestS = sc; bestM = m; } }
            if (bestS > 0.6f) {
                if (!frameLock || bestM != mod) { mod = bestM; frameLock = true; sfOk = false; slotsPerFrame = 0; dataOn = false; }
                frameSlot = slotIdx; frameBitPos = 0; missedSof = 0; sfLlr.clear();
                std::lock_guard<std::mutex> lk(stMu);
                st.frames++; st.mod = mod; st.level = std::max(st.level, 2);
            } else if (frameLock) {
                if (++missedSof >= 3) { frameLock = false; sfOk = false; dataOn = false; return; }
                frameSlot = slotIdx; frameBitPos = 0; sfLlr.clear();   // keep going on the expected grid
            } else return;
        }
        if (!frameLock) return;
        const int m = tdmBitsPerSymbol(mod);
        llrBuf.resize((size_t)kSlotData * m);
        tdmDemap(d.data(), kSlotData, mod, (float)sigma2, llrBuf.data());
        // a slot without its pilots (a blockage: the front end's gain has turned the noise up to the signal's level) carries no information:
        // erasures, not confident wrong values, go into the de-interleaver
        if (erased) std::fill(llrBuf.begin(), llrBuf.end(), 0.f);
        for (size_t i = 0; i < llrBuf.size(); i++) bitIn(llrBuf[i]);
    }

    void bitIn(float l) {
        const long long b = frameBitPos++;
        if (b < kSofBits) return;
        if (b < (long long)kHeaderCu * kCuBits) {
            sfLlr.push_back(l);
            if ((int)sfLlr.size() == kSfBits) signalling();
            return;
        }
        if (!sfOk || !dataOn) return;
        const long long db = b - (long long)kHeaderCu * kCuBits;
        if (db >= (long long)ncw * nbil(pat)) return;   // padding
        const float t = std::max(-127.f, std::min(127.f, std::round(l * kLlrScale)));
        iuAcc.push_back((int8_t)(t == 0 ? (l >= 0 ? 1 : -1) : t));
        if ((int)iuAcc.size() == kIuBits) { pushIu(); iuAcc.clear(); }
    }

    void signalling() {
        TdmSignalling s;
        int it = 0;
        const bool ok = decodeSignalling(sfLlr.data(), s, &it);
        {
            std::lock_guard<std::mutex> lk(stMu);
            if (ok) { st.sfOk++; st.sf = s; st.level = std::max(st.level, 3); } else st.sfBad++;
        }
        if (!ok) return;
        if (s.frameWidthCu <= kHeaderCu || s.frameWidthCu % tdmCuPerSlot(mod) != 0 || nbil(s.pat) <= 0) return;
        const bool changed = !sfOk || !(s == sf);
        sf = s; sfOk = true;
        frameCu = s.frameWidthCu;
        slotsPerFrame = frameCu / tdmCuPerSlot(mod);
        if (changed || !dataOn) startData();
    }

    void startData() {
        pat = sf.pat;
        ncw = tdmCodewords(frameCu, pat);
        cwIu = nbil(pat) / kIuBits;
        std::lock_guard<std::mutex> lk(stMu);
        st.tiTooLarge = false;
        if (ncw <= 0 || !tiDelays(sf.ti, tdmDataCu(frameCu, pat), L)) { dataOn = false; return; }
        int lmax = 0; size_t bytes = 0;
        for (int v : L) { lmax = std::max(lmax, v); bytes += (size_t)v * kIuBits; }
        st.tiLmax = lmax; st.tiBytes = bytes;
        if (bytes > maxTiBytes) { st.tiTooLarge = true; dataOn = false; return; }
        ti.configure(L, true);
        gIn = 0; iuAcc.clear(); cwIdx = -1; cwFill = 0; cwValid = false;
        cwLlr.assign((size_t)nbil(pat), 0.f);
        dataOn = true;
    }

    void pushIu() {
        int8_t out[kIuBits];
        const int branch = (int)(gIn % kBranches);
        ti.setBranch(branch);
        ti.process(iuAcc.data(), out);
        const bool valid = gIn >= 48LL * L[(size_t)branch];
        const long long gOut = gIn - 48LL * ti.lmax();
        gIn++;
        const long long frameIu = (long long)ncw * cwIu;
        const long long posOut = ((gOut % frameIu) + frameIu) % frameIu;
        const int c = (int)(posOut / cwIu), off = (int)(posOut % cwIu);
        if (off == 0) { cwIdx = c; cwFill = 0; cwValid = true; }
        if (cwIdx != c || off != cwFill) { cwIdx = -1; return; }
        if (!valid) cwValid = false;
        for (int i = 0; i < kIuBits; i++) cwLlr[(size_t)off * kIuBits + i] = out[i] / kLlrScale;
        if (++cwFill == cwIu) { if (cwValid) decodeCodeword(); cwIdx = -1; }
    }

    void decodeCodeword() {
        coded.resize((size_t)ntcb(pat));
        bitDeinterleave(cwLlr.data(), pat, coded.data());
        const int it = turboDecode(coded.data(), kTcInput, pat, 8, info, [](const uint8_t* b) { return eframeChecks(b); });
        uint8_t pk[188 * kPackets];
        const EframeInfo e = parseEframe(info.data(), pk);
        iterSum += it;
        {
            std::lock_guard<std::mutex> lk(stMu);
            if (e.headerOk && e.packetsOk == kPackets) st.codewordsOk++; else st.codewordsBad++;
            st.packetsOk += e.packetsOk;
            st.packetsBad += kPackets - e.packetsOk;
            const long n = st.codewordsOk + st.codewordsBad;
            st.avgIterations = n ? (double)iterSum / n : 0;
            st.level = 4;
        }
        if (cb) cb(pk, kPackets);
    }

    std::vector<cf32> tmp;
    void feed(const cf32* x, size_t n) {
        if (fs <= 0) return;
        // in pieces of about half a slot, so that a blockage holds the timing loop before it has wandered on the noise
        const size_t piece = std::max<size_t>(1024, (size_t)(fs / std::max(1.0, front.config().symbolRate) * kSlotSymbols / 2));
        for (size_t o = 0; o < n; o += piece) feedPiece(x + o, std::min(piece, n - o));
    }
    // A blockage (the input power falls well below what it was while the slots were good): the timing loop is held at the rate of the
    // last good slots, since on noise alone it wanders by hundreds of ppm and would come back at another timing phase
    bool blocked = false;
    double goodPower = 0;
    void feedPiece(const cf32* x, size_t n) {
        double pw = 0;
        for (size_t i = 0; i < n; i++) pw += std::norm(x[i]);
        pw /= std::max<size_t>(1, n);
        const bool low = slotLock && goodPower > 0 && pw < 0.3 * goodPower;
        if (low && !blocked) {
            blocked = true;
            front.timer().setLoop(1e-7, 0.707);
            if (goodRate > 0) front.timer().shiftIntegrator(front.timer().rateOffset() - (goodRate / front.config().symbolRate - 1.0));
        } else if (!low && blocked && pw > 0.5 * goodPower) {
            blocked = false;
            front.timer().setLoop(0.002, 0.707);
        }
        if (slotLock && badFields == 0 && !blocked) goodPower = goodPower > 0 ? 0.98 * goodPower + 0.02 * pw : pw;
        if (!slotLock) goodPower = 0;
        tmp.clear();
        front.process(x, n, tmp);
        samplesIn += (long long)n;
        sy.insert(sy.end(), tmp.begin(), tmp.end());
        symbolsIn += (long long)tmp.size();
        candSymbols += (double)tmp.size();
        if (front.timer().symbols() % 4096 < tmp.size() && !blocked) front.recentre();
        for (;;) {
            if (!slotLock) {
                if (!searchSlot()) break;
                continue;
            }
            if (!processSlot()) break;
        }
        // automatic symbol rate: the next row of table 5.13 when no PL slot is found in 0.1 s, or no frame (SOF) in 0.7 s (a frame lasts
        // about 0.2 - 0.25 s)
        bool haveFrames;
        { std::lock_guard<std::mutex> lk(stMu); haveFrames = st.level >= 2; }
        const double rate = front.config().symbolRate;
        if (fixedRate <= 0 && !cands.empty() && !haveFrames && candSymbols > (slotLock ? 0.7 : 0.1) * rate) {
            cand = (cand + 1) % (int)cands.size();
            setupFront();
        }
        if (haveFrames) candSymbols = 0;
    }
};

TdmReceiver::TdmReceiver() : d_(std::make_unique<Impl>()) {}
TdmReceiver::~TdmReceiver() = default;
void TdmReceiver::configure(double fs, double bw) { d_->configure(fs, bw); }
void TdmReceiver::setSymbolRate(double r) { d_->fixedRate = r; if (d_->fs > 0) d_->setupFront(); }
void TdmReceiver::setGoldCode(int n) { d_->goldN = n; if (d_->fs > 0) d_->configure(d_->fs, d_->bw); }
void TdmReceiver::setMaxInterleaverBytes(size_t b) { d_->maxTiBytes = b; }
void TdmReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t)> cb) { d_->cb = std::move(cb); }
void TdmReceiver::reset() { if (d_->fs > 0) d_->configure(d_->fs, d_->bw); }
void TdmReceiver::feed(const cf32* x, size_t n) { d_->feed(x, n); }
TdmStatus TdmReceiver::status() const { std::lock_guard<std::mutex> lk(d_->stMu); return d_->st; }

} // namespace dvbsh
} // namespace dect2
