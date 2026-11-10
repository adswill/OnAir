// NavIC L5 SPS receiver (see navic.h).
#include "dect2/navic/navic.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/gnss/gnss_msg.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <random>

namespace dect2 {

namespace {
// Table 7: the initial state of the G2 register for each PRN (L5, S), written as in the ICD: bit 1 first. The first ten chips of each are the ICD's check column.
const char* const kG2L5[14] = {"1110100111", "0000100110", "1000110100", "0101110010", "1110110000", "0001101011", "0000010100", "0100110000", "0010011000", "1101100100", "0001001100", "1101111100", "1011010010", "0111101010"};
const char* const kG2S[14] = {"0011101111", "0101111101", "1000110001", "0010101011", "1010010001", "0100101100", "0010001110", "0100100110", "1100001110", "1010111110", "1110010001", "1101101001", "0101000101", "0100001101"};

void gold(const char* g2init, uint8_t* out) {
    int g1[11], g2[11];
    for (int i = 1; i <= 10; i++) { g1[i] = 1; g2[i] = g2init[10 - i] == '1'; }   // the table's bit 1 is the output stage: the check column of Table 7 shows it
    for (int c = 0; c < 1023; c++) {
        out[c] = (uint8_t)(g1[10] ^ g2[10]);
        const int f1 = g1[3] ^ g1[10];
        const int f2 = g2[2] ^ g2[3] ^ g2[6] ^ g2[8] ^ g2[9] ^ g2[10];
        for (int i = 10; i > 1; i--) { g1[i] = g1[i - 1]; g2[i] = g2[i - 1]; }
        g1[1] = f1; g2[1] = f2;
    }
}

constexpr uint8_t kSync[16] = {1, 1, 1, 0, 1, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0};   // EB90

// Rate 1/2, K = 7, G1 = 171, G2 = 133 (octal), the 292 bits end in six tail zeros
constexpr uint8_t kPar171 = 0171, kPar133 = 0133;
inline int parity(unsigned v) { return __builtin_parity(v); }

void viterbi(const float* soft, int nBits, uint8_t* out) {
    const int S = 64;
    std::vector<float> cost(S, 1e9f), next(S);
    std::vector<uint8_t> tb((size_t)nBits * S);
    cost[0] = 0;
    for (int n = 0; n < nBits; n++) {
        std::fill(next.begin(), next.end(), 1e30f);
        for (int s = 0; s < S; s++) {
            if (cost[(size_t)s] > 1e29f) continue;
            for (int b = 0; b < 2; b++) {
                const unsigned reg = (unsigned)((b << 6) | s);
                const int c1 = parity(reg & kPar171), c2 = parity(reg & kPar133);
                const float d = (c1 ? soft[2 * n] : -soft[2 * n]) + (c2 ? soft[2 * n + 1] : -soft[2 * n + 1]);
                const int ns = (reg >> 1) & 63;
                const float v = cost[(size_t)s] + d;
                if (v < next[(size_t)ns]) { next[(size_t)ns] = v; tb[(size_t)(n * S + ns)] = (uint8_t)((s << 1) | b); }
            }
        }
        cost.swap(next);
    }
    int s = 0;
    for (int n = nBits - 1; n >= 0; n--) {
        const int e = tb[(size_t)(n * S + s)];
        out[n] = (uint8_t)(e & 1);
        s = e >> 1;
    }
}
} // namespace

bool navicChips(int prn, uint8_t* chips, bool s) {
    if (prn < 1 || prn > kNavicMaxPrn) return false;
    gold((s ? kG2S : kG2L5)[prn - 1], chips);
    return true;
}

unsigned navicFirst10Octal(int prn, bool s) {
    uint8_t c[1023];
    if (!navicChips(prn, c, s)) return 0;
    unsigned v = 0;
    for (int i = 0; i < 10; i++) v = (v << 1) | c[i];
    return v;
}

void navicSetCrcTail(uint8_t* b) {
    const uint32_t c = crc24q(b, 262);
    for (int i = 0; i < 24; i++) b[262 + i] = (uint8_t)((c >> (23 - i)) & 1);
    for (int i = 286; i < 292; i++) b[i] = 0;
}

bool navicCrcOk(const uint8_t* b) {
    const uint32_t c = crc24q(b, 262);
    uint32_t got = 0;
    for (int i = 0; i < 24; i++) got = (got << 1) | b[262 + i];
    return c == got;
}

void navicEncodeSubframe(const uint8_t* bits, uint8_t* sym) {
    std::memcpy(sym, kSync, 16);
    uint8_t coded[kNavicCoded];
    unsigned st = 0;
    for (int i = 0; i < kNavicBits; i++) {
        const unsigned reg = ((unsigned)bits[i] << 6) | st;
        coded[2 * i] = (uint8_t)parity(reg & kPar171);
        coded[2 * i + 1] = (uint8_t)parity(reg & kPar133);
        st = (reg >> 1) & 63;
    }
    // Table 9: written in columns (73 x 8), read in rows
    for (int r = 0; r < 8; r++) for (int c = 0; c < 73; c++) sym[16 + r * 73 + c] = coded[c * 8 + r];
}

bool navicDecodeSymbols(const float* soft, uint8_t* bits) {
    float de[kNavicCoded];
    for (int r = 0; r < 8; r++) for (int c = 0; c < 73; c++) de[c * 8 + r] = soft[r * 73 + c];
    viterbi(de, kNavicBits, bits);
    return navicCrcOk(bits);
}

static uint32_t field(const uint8_t* b, int first1, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | b[first1 - 1 + i]; return v; }

void navicParse(const uint8_t* b, NavicFields& f) {
    f.towc = field(b, 9, 17);
    f.alert = b[25]; f.autonav = b[26];
    f.subframeId = (int)field(b, 28, 2) + 1;   // Table 10
    if (f.subframeId == 1) f.wn = (int)field(b, 31, 10);
    if (f.subframeId >= 3) { f.msgId = (int)field(b, 31, 6); f.prnId = (int)field(b, 257, 6); }
}

namespace {
struct Channel {
    int prn = 0;
    const uint8_t* chips = nullptr;
    double codePhase = 0, codeRate = 1.023e6, phase = 0, doppler = 0;
    double pllI = 0, dllI = 0;
    double ie = 0, ip = 0, il = 0, qe = 0, qp = 0, ql = 0;
    int samples = 0;
    double prevI = 0, prevQ = 0;
    bool havePrev = false, prevSign = false;
    int ms = 0;
    // bit sync
    std::array<int, 20> hist{};
    int histN = 0, bitEdge = -1, msInBit = 0;
    double symI = 0;
    std::vector<float> soft;          // since the last check
    uint64_t symbols = 0, sinceSync = 0;
    bool haveSync = false;
    uint32_t frames = 0, bad = 0;
    NavicSat rep;
    float cn0 = 0;
    double nsSum = 0, nbSum = 0; int cn0N = 0;
    double lostAcc = 0; int lostN = 0;
    bool lost = false;
};
} // namespace

struct NavicReceiver::Impl {
    std::mutex mu;
    double rate = 0;
    std::atomic<bool> resetReq{false};
    uint64_t nIn = 0, nextPub = 0, seq = 0, okN = 0, badN = 0;
    NavicTelemetry pub;
    double resampT = 0;
    cf32 prev{0, 0};
    std::function<void(const std::string&)> logCb;

    uint8_t chips[kNavicMaxPrn][1023];
    std::vector<std::vector<cf32>> codeFft;
    std::vector<cf32> acqBuf, recent;
    size_t recentPos = 0;
    uint64_t sampleCount = 0;
    size_t acqFill = 0;
    bool acqReady = false;
    int acqPrn = 1, acqDop = 0;
    std::vector<float> acqSum;
    float bestPk = 0, bestFloor = 1; int bestPos = 0; double bestDop = 0;
    std::vector<Channel> chans;
    static constexpr int N = 4096;

    Impl() {
        codeFft.resize(kNavicMaxPrn);
        for (int p = 1; p <= kNavicMaxPrn; p++) {
            navicChips(p, chips[p - 1]);
            std::vector<cf32> c((size_t)N);
            for (int i = 0; i < N; i++) { const int k = (int)((double)i * 1023.0 / N); c[(size_t)i] = cf32(chips[p - 1][k] ? -1.f : 1.f, 0.f); }
            Fft f(N); f.forward(c.data());
            for (auto& v : c) v = std::conj(v);
            codeFft[(size_t)p - 1] = std::move(c);
        }
        init();
    }

    void init() {
        recent.assign((size_t)N * 10, cf32(0, 0)); recentPos = 0; sampleCount = 0;
        acqBuf.assign((size_t)N * 10, cf32(0, 0)); acqFill = 0; acqReady = false; acqPrn = 1; acqDop = 0;
        acqSum.assign((size_t)N, 0.f); bestPk = 0; bestFloor = 1; bestPos = 0; bestDop = 0;
        chans.clear(); nIn = 0; nextPub = 0; okN = 0; badN = 0; resampT = 0; prev = cf32(0, 0);
        pub = NavicTelemetry{};
    }

    static double dopOfBin(int k) { return -10000 + k * 250.0; }

    // One step of the search: one PRN at one Doppler.
    void acqStep() {
        if (!acqReady) return;
        const double dop = dopOfBin(acqDop);
        const int prn = acqPrn;
        const bool tracked = std::any_of(chans.begin(), chans.end(), [&](const Channel& c) { return c.prn == prn; });
        if (!tracked) {
            std::fill(acqSum.begin(), acqSum.end(), 0.f);
            Fft f(N);
            std::vector<cf32> b((size_t)N), r((size_t)N);
            for (int m = 0; m < 10; m++) {
                double ph = -2 * M_PI * dop * m * N / kNavicFs;
                for (int i = 0; i < N; i++, ph -= 2 * M_PI * dop / kNavicFs) b[(size_t)i] = acqBuf[(size_t)(m * N + i)] * cf32((float)std::cos(ph), (float)std::sin(ph));
                f.forward(b.data());
                for (int i = 0; i < N; i++) r[(size_t)i] = b[(size_t)i] * codeFft[(size_t)prn - 1][(size_t)i];
                f.inverse(r.data());
                for (int i = 0; i < N; i++) acqSum[(size_t)i] += std::norm(r[(size_t)i]);
            }
            int bp = 0; float pk = 0;
            double mean = 0;
            for (int i = 0; i < N; i++) { mean += acqSum[(size_t)i]; if (acqSum[(size_t)i] > pk) { pk = acqSum[(size_t)i]; bp = i; } }
            mean /= N;
            if (pk > bestPk) { bestPk = pk; bestFloor = (float)mean; bestPos = bp; bestDop = dop; }
        }
        if (++acqDop >= 81) {
            acqDop = 0;
            if (bestPk > 4.0f * bestFloor) confirm(prn, bestDop);
            bestPk = 0; bestFloor = 1;
            acqPrn = acqPrn % kNavicMaxPrn + 1;
            if (acqPrn == 1) { acqFill = 0; acqReady = false; }
        }
    }

    // Search again around the found Doppler in the newest samples (the search buffer is old by now) and start a channel there
    void confirm(int prn, double dop0) {
        if (chans.size() >= 8) return;
        std::vector<cf32> buf((size_t)N * 10);
        for (size_t i = 0; i < buf.size(); i++) buf[i] = recent[(recentPos + i) % recent.size()];
        const uint64_t start = sampleCount - buf.size();
        Fft f(N);
        std::vector<cf32> b((size_t)N), r((size_t)N);
        std::vector<float> sum((size_t)N);
        float best = 0, mean = 1; int pos = 0; double bd = dop0;
        for (double dop = dop0 - 250; dop <= dop0 + 250; dop += 50) {
            std::fill(sum.begin(), sum.end(), 0.f);
            for (int m = 0; m < 10; m++) {
                double ph = -2 * M_PI * dop * m * N / kNavicFs;
                for (int i = 0; i < N; i++, ph -= 2 * M_PI * dop / kNavicFs) b[(size_t)i] = buf[(size_t)(m * N + i)] * cf32((float)std::cos(ph), (float)std::sin(ph));
                f.forward(b.data());
                for (int i = 0; i < N; i++) r[(size_t)i] = b[(size_t)i] * codeFft[(size_t)prn - 1][(size_t)i];
                f.inverse(r.data());
                for (int i = 0; i < N; i++) sum[(size_t)i] += std::norm(r[(size_t)i]);
            }
            double m2 = 0; float pk = 0; int p = 0;
            for (int i = 0; i < N; i++) { m2 += sum[(size_t)i]; if (sum[(size_t)i] > pk) { pk = sum[(size_t)i]; p = i; } }
            if (pk > best) { best = pk; mean = (float)(m2 / N); pos = p; bd = dop; }
        }
        if (best < 3.5f * mean) return;
        Channel c;
        c.prn = prn; c.chips = chips[prn - 1]; c.doppler = bd; c.pllI = bd;
        c.codeRate = 1.023e6 * (1 + bd / kNavicL5Hz);
        // the code epoch is pos samples into the newest buffer; carry it to the current position
        const double elapsed = (double)(sampleCount - start - (uint64_t)pos);
        c.codePhase = std::fmod(elapsed * c.codeRate / kNavicFs, 1023.0);
        chans.push_back(c);
    }

    void trackEpoch(Channel& c) {
        const double I = c.ip, Q = c.qp;
        const double e = std::hypot(c.ie, c.qe), l = std::hypot(c.il, c.ql);
        // DLL
        if (e + l > 0) {
            const double err = (e - l) / (e + l);
            c.dllI += err * 0.04;
            c.codeRate = 1.023e6 * (1 + c.doppler / kNavicL5Hz) + c.dllI * 30 + err * 80;
        }
        // PLL (Costas) with FLL aid for the first half second
        const double dt = 1e-3;
        const double err = I >= 0 ? std::atan2(Q, std::max(I, 1e-12)) : std::atan2(-Q, std::max(-I, 1e-12));
        double fErr = 0;
        if (c.havePrev && c.ms < 500) {
            const double dot = I * c.prevI + Q * c.prevQ, cross = c.prevI * Q - c.prevQ * I;
            fErr = std::atan2(cross, std::fabs(dot) > 1e-12 ? std::fabs(dot) : 1e-12) * (dot >= 0 ? 1 : -1);
            fErr /= 2 * M_PI * dt;
            c.pllI += 0.08 * fErr;
        }
        const double bw = c.ms < 500 ? 40 : 22, w0 = bw * 1.89;
        c.pllI += w0 * w0 * dt * err / (2 * M_PI);
        c.doppler = c.pllI + 1.414 * w0 * err / (2 * M_PI);
        c.prevI = I; c.prevQ = Q; c.havePrev = true;
        c.ms++;
        // C/N0 over 20 ms blocks
        c.nbSum += I; c.nsSum += I * I + Q * Q; c.cn0N++;
        c.lostAcc += std::hypot(I, Q); c.lostN++;
        if (c.cn0N == 20) {
            const double np = (c.nbSum * c.nbSum) / std::max(c.nsSum, 1e-20);
            const double cn = 1000.0 * (np - 1) / std::max(20.0 - np, 1e-6);
            if (cn > 0) c.cn0 = c.cn0 == 0 ? (float)(10 * std::log10(cn)) : 0.9f * c.cn0 + 0.1f * (float)(10 * std::log10(cn));
            c.nbSum = 0; c.nsSum = 0; c.cn0N = 0;
            if (c.ms > 2000 && c.cn0 < 22) c.lost = true;
        }
        // bit sync: sign changes per 1 ms position modulo 20
        const bool tr = c.prevSign != (I >= 0);
        c.prevSign = I >= 0;
        if (c.ms > 500 && c.bitEdge < 0) {
            if (tr) c.hist[(size_t)(c.ms % 20)]++;
            if (++c.histN >= 1200) {
                int best = 0, sum = 0;
                for (int i = 0; i < 20; i++) { sum += c.hist[(size_t)i]; if (c.hist[(size_t)i] > c.hist[(size_t)best]) best = i; }
                if (sum > 10 && c.hist[(size_t)best] * 2 > sum) { c.bitEdge = best; c.msInBit = 0; c.symI = 0; }
                else { c.hist.fill(0); c.histN = 0; }
            }
        } else if (c.bitEdge >= 0) {
            if (c.ms % 20 == c.bitEdge && c.msInBit > 0) { pushSymbol(c); c.symI = 0; c.msInBit = 0; }
            c.symI += I; c.msInBit++;
        }
        c.ie = c.ip = c.il = c.qe = c.qp = c.ql = 0;
        c.samples = 0;
    }

    void pushSymbol(Channel& c) {
        const float s = (float)(c.symI);
        c.soft.push_back(s);   // logic 0 is +1 on the carrier; a Costas loop may be inverted, the sync word tells
        c.symbols++;
        // Keep the last 16+584 symbols; look for the sync word
        if (c.soft.size() >= (size_t)kNavicSymbolsPerSubframe) {
            size_t n = c.soft.size();
            const float* w = &c.soft[n - kNavicSymbolsPerSubframe];
            int m1 = 0, m2 = 0;
            for (int i = 0; i < 16; i++) { const bool b = w[i] < 0; if (b == (kSync[i] != 0)) m1++; if (b != (kSync[i] != 0)) m2++; }
            if (m1 >= 15 || m2 >= 15) {
                float sc[kNavicCoded];
                const float sign = m2 > m1 ? -1.f : 1.f;   // positive favours 0 after this
                float rms = 0;
                for (int i = 0; i < kNavicCoded; i++) { sc[i] = sign * w[16 + i]; rms += sc[i] * sc[i]; }
                rms = std::sqrt(rms / kNavicCoded) + 1e-9f;
                for (int i = 0; i < kNavicCoded; i++) sc[i] /= rms;
                uint8_t bits[kNavicBits];
                if (navicDecodeSymbols(sc, bits)) {
                    NavicFields f; navicParse(bits, f);
                    std::lock_guard<std::mutex> lk(mu);
                    c.frames++; okN++;
                    c.rep.towc = f.towc; c.rep.lastSubframe = f.subframeId; c.rep.alert = f.alert;
                    if (f.wn >= 0) c.rep.wn = f.wn;
                    c.soft.clear();
                } else { c.bad++; badN++; }
            }
            if (c.soft.size() > (size_t)kNavicSymbolsPerSubframe) c.soft.erase(c.soft.begin());
        }
    }

    void sample(const cf32& x) {
        recent[recentPos] = x;
        if (++recentPos == recent.size()) recentPos = 0;
        sampleCount++;
        // the search buffer
        if (!acqReady) {
            if (acqFill < acqBuf.size()) acqBuf[acqFill++] = x;
            if (acqFill == acqBuf.size()) acqReady = true;
        }
        for (auto& c : chans) {
            c.phase += 2 * M_PI * c.doppler / kNavicFs;
            if (c.phase > M_PI) c.phase -= 2 * M_PI; else if (c.phase < -M_PI) c.phase += 2 * M_PI;
            const cf32 m = x * cf32((float)std::cos(-c.phase), (float)std::sin(-c.phase));
            const double chip = c.codePhase;
            const auto code = [&](double d) { double p = chip + d; if (p < 0) p += 1023; else if (p >= 1023) p -= 1023; return c.chips[(int)p] ? -1.0 : 1.0; };
            const double ce = code(0.5), cp = code(0), cl = code(-0.5);
            c.ip += m.real() * cp; c.qp += m.imag() * cp;
            c.ie += m.real() * ce; c.qe += m.imag() * ce;
            c.il += m.real() * cl; c.ql += m.imag() * cl;
            c.samples++;
            c.codePhase += c.codeRate / kNavicFs;
            if (c.codePhase >= 1023) { c.codePhase -= 1023; trackEpoch(c); }
        }
        chans.erase(std::remove_if(chans.begin(), chans.end(), [](const Channel& c) { return c.lost; }), chans.end());
    }

    void publish() {
        pub.seq = ++seq;
        pub.timeSec = (double)nIn / rate; pub.inputRate = rate; pub.searchPrn = acqPrn;
        pub.sats.clear();
        float best = 0; double dop = 0;
        for (const auto& c : chans) {
            NavicSat s = c.rep;
            s.prn = c.prn; s.cn0 = c.cn0; s.dopplerHz = c.doppler; s.frames = c.frames; s.bad = c.bad;
            s.state = c.frames ? 3 : c.bitEdge >= 0 ? 2 : 1;
            if (s.cn0 > best) { best = s.cn0; dop = s.dopplerHz; }
            pub.sats.push_back(s);
        }
        pub.state = okN ? 2 : chans.empty() ? 0 : 1;
        pub.dataValid = okN > 0; pub.blocksOk = okN; pub.blocksBad = badN; pub.snrDb = best; pub.cfoHz = dop;
    }
};

NavicReceiver::NavicReceiver() : p_(std::make_unique<Impl>()) {}
NavicReceiver::~NavicReceiver() = default;
void NavicReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->init(); }
bool NavicReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate > 0; }
void NavicReceiver::reset() { p_->resetReq = true; }
void NavicReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }

void NavicReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { std::lock_guard<std::mutex> lk(m.mu); m.init(); }
    if (m.rate <= 0 || n == 0) return;
    const double step = m.rate / kNavicFs;
    for (size_t i = 0; i < n; i++) {
        const cf32 cur = std::isfinite(x[i].real()) && std::isfinite(x[i].imag()) ? x[i] : cf32(0, 0);
        while (m.resampT <= 1.0) {
            const float a = (float)m.resampT;
            m.sample(m.prev * (1 - a) + cur * a);
            m.resampT += step;
        }
        m.resampT -= 1.0;
        m.prev = cur;
    }
    m.nIn += n;
    for (int k = 0; k < 3; k++) m.acqStep();
    if (m.nIn >= m.nextPub) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.nextPub = m.nIn + (uint64_t)(0.25 * m.rate);
        m.publish();
    }
}

bool NavicReceiver::telemetry(NavicTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == 0 || p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning navicTuning() {
    ModeTuning t;
    t.stdMode = 563; t.id = "navic"; t.name = "NavIC";
    t.minMhz = 1100; t.maxMhz = 2600; t.defMhz = 1176.45;
    t.sampleRate = kNavicFs; t.minSampleRate = 2.2e6; t.bandwidthMhz = 3.5;
    return t;
}

// ---- Test signal
struct NavicGenerator::Impl {
    struct Sat { int prn; uint8_t chips[1023]; double code, codeRate, ph, dop; uint8_t sym[kNavicSymbolsPerSubframe]; int symPos, sub, ms; uint32_t towc; };
    NavicGenConfig c;
    std::vector<Sat> s;
    std::mt19937 g;
    std::normal_distribution<float> n{0.f, 1.f};
    float sigma = 0.1f; double amp = 0;
    void makeSub(Sat& v) {
        uint8_t b[kNavicBits] = {};
        for (int i = 0; i < 17; i++) b[8 + i] = (uint8_t)((v.towc >> (16 - i)) & 1);
        b[27] = (uint8_t)((v.sub >> 1) & 1); b[28] = (uint8_t)(v.sub & 1);
        for (int i = 30; i < 262; i++) b[i] = (uint8_t)(g() & 1);
        if (v.sub == 0) for (int i = 0; i < 10; i++) b[30 + i] = (uint8_t)((1234 >> (9 - i)) & 1);
        navicSetCrcTail(b);
        navicEncodeSubframe(b, v.sym);
        v.symPos = 0; v.sub = (v.sub + 1) & 3; v.towc++;
    }
};

NavicGenerator::NavicGenerator(const NavicGenConfig& c) : p_(std::make_unique<Impl>()) {
    p_->c = c; p_->g.seed(c.seed);
    p_->amp = p_->sigma * std::sqrt(2.0 * std::pow(10.0, c.cn0Db / 10) / c.rate);
    const int prns[] = {2, 5, 9, 3, 11, 7, 13};
    for (int i = 0; i < std::min(c.nSats, 7); i++) {
        Impl::Sat v{};
        v.prn = prns[i]; navicChips(v.prn, v.chips);
        v.dop = c.dopplerHz * (i == 0 ? 1 : i == 1 ? -0.5 : 0.25 * i);
        v.code = 37.5 * (i + 1); v.codeRate = 1.023e6 * (1 + v.dop / kNavicL5Hz); v.towc = 5000 + (uint32_t)i;
        p_->makeSub(v);
        p_->s.push_back(v);
    }
}
NavicGenerator::~NavicGenerator() = default;

void NavicGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    for (size_t i = 0; i < n; i++) {
        cf32 x(0, 0);
        for (auto& v : m.s) {
            const int chip = (int)v.code;
            double a = v.chips[chip] ? -m.amp : m.amp;
            if (v.sym[v.symPos]) a = -a;
            x += cf32((float)(a * std::cos(v.ph)), (float)(a * std::sin(v.ph)));
            v.ph += 2 * M_PI * v.dop / m.c.rate;
            if (v.ph > M_PI) v.ph -= 2 * M_PI; else if (v.ph < -M_PI) v.ph += 2 * M_PI;
            v.code += v.codeRate / m.c.rate;
            if (v.code >= 1023) {
                v.code -= 1023;
                if (++v.ms == 20) { v.ms = 0; if (++v.symPos == kNavicSymbolsPerSubframe) m.makeSub(v); }
            }
        }
        out[i] = x + cf32(m.n(m.g) * m.sigma, m.n(m.g) * m.sigma);
    }
}

} // namespace dect2

namespace dect2 {
namespace {
class NavicSynth : public ModeSynth {
public:
    NavicSynth(const SynthConfig& s, double rate) : rate_(rate), gen_(cfg(s, rate)) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_.generate(out, n); }
private:
    static NavicGenConfig cfg(const SynthConfig& s, double rate) { NavicGenConfig c; c.rate = rate; c.nSats = 4; c.cn0Db = std::min(s.snrDb + 20, 55.0); c.dopplerHz = 2500; return c; }
    double rate_;
    NavicGenerator gen_;
};
}
std::unique_ptr<ModeSynth> makeNavicSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<NavicSynth>(cfg, sampleRate); }
}
