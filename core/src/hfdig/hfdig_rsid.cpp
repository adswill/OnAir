// RSID detector (hfdig_rsid.h).
#include "dect2/hfdig/hfdig_rsid.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/dsp/fftutil.h"
#include <cmath>
#include <deque>

namespace dect2 {

namespace {

constexpr double kFs = 11025.0;
constexpr int kSym = 1024, kFft = 2048, kHop = 512;
constexpr int kBinLow = 6;              // about 32 Hz
constexpr int kMaxDist = 2;

int gfMul(int a, int b) {               // the field of fldigi's table: x^4 + x^3 + 1 (0x19)
    int r = 0;
    for (int i = 0; i < 4; i++) if (b & (1 << i)) r ^= a << i;
    for (int i = 7; i >= 4; i--) if (r & (1 << i)) r ^= 0x19 << (i - 4);
    return r & 15;
}

struct Compiled { const RsidMode* m; uint8_t sym[15]; };

const std::vector<Compiled>& compiled() {
    static const std::vector<Compiled> v = [] {
        std::vector<Compiled> o;
        for (const auto& m : rsidModes()) { Compiled c; c.m = &m; rsidEncode(m.code, c.sym); o.push_back(c); }
        return o;
    }();
    return v;
}

} // namespace

void rsidEncode(int code, uint8_t out[15]) {
    static const int kIdx[12] = {2, 4, 8, 9, 11, 15, 7, 14, 5, 10, 13, 3};
    uint8_t r[15] = {};
    r[0] = (uint8_t)((code >> 8) & 15); r[1] = (uint8_t)((code >> 4) & 15); r[2] = (uint8_t)(code & 15);
    for (int i = 0; i < 12; i++) {
        for (int j = 14; j > 0; j--) r[j] = (uint8_t)(r[j - 1] ^ gfMul(r[j], kIdx[i]));
        r[0] = (uint8_t)gfMul(r[0], kIdx[i]);
    }
    for (int i = 0; i < 15; i++) out[i] = r[i];
}

const std::vector<RsidMode>& rsidModes() {
    static const std::vector<RsidMode> v = {
        {1, "BPSK31", -1, nullptr, 0, 0}, {2, "BPSK63", -1, nullptr, 0, 0}, {4, "BPSK125", -1, nullptr, 0, 0}, {110, "QPSK31", -1, nullptr, 0, 0}, {3, "QPSK63", -1, nullptr, 0, 0},
        {37, "RTTY 7-bit", -1, nullptr, 0, 0}, {38, "RTTY 8-bit", -1, nullptr, 0, 0}, {39, "RTTY 45", -1, nullptr, 0, 0}, {40, "RTTY 50", -1, nullptr, 0, 0}, {41, "RTTY 75", -1, nullptr, 0, 0},
        {57, "MFSK16", KbdMfsk, "MFSK16", 0, 0}, {147, "MFSK32", KbdMfsk, "MFSK32", 0, 0},
        {203, "Olivia 4/125", KbdOlivia, nullptr, 4, 125}, {75, "Olivia 4/250", KbdOlivia, nullptr, 4, 250}, {74, "Olivia 4/500", KbdOlivia, nullptr, 4, 500}, {229, "Olivia 4/1000", KbdOlivia, nullptr, 4, 1000}, {238, "Olivia 4/2000", KbdOlivia, nullptr, 4, 2000},
        {163, "Olivia 8/125", KbdOlivia, nullptr, 8, 125}, {69, "Olivia 8/250", KbdOlivia, nullptr, 8, 250}, {72, "Olivia 8/500", KbdOlivia, nullptr, 8, 500}, {116, "Olivia 8/1000", KbdOlivia, nullptr, 8, 1000}, {214, "Olivia 8/2000", KbdOlivia, nullptr, 8, 2000},
        {70, "Olivia 16/500", KbdOlivia, nullptr, 16, 500}, {73, "Olivia 16/1000", KbdOlivia, nullptr, 16, 1000}, {234, "Olivia 16/2000", KbdOlivia, nullptr, 16, 2000},
        {71, "Olivia 32/1000", KbdOlivia, nullptr, 32, 1000}, {221, "Olivia 32/2000", KbdOlivia, nullptr, 32, 2000}, {211, "Olivia 64/2000", KbdOlivia, nullptr, 64, 2000},
        {204, "Contestia 4/125", KbdContestia, nullptr, 4, 125}, {55, "Contestia 4/250", KbdContestia, nullptr, 4, 250}, {54, "Contestia 4/500", KbdContestia, nullptr, 4, 500}, {255, "Contestia 4/1000", KbdContestia, nullptr, 4, 1000}, {254, "Contestia 4/2000", KbdContestia, nullptr, 4, 2000},
        {169, "Contestia 8/125", KbdContestia, nullptr, 8, 125}, {49, "Contestia 8/250", KbdContestia, nullptr, 8, 250}, {52, "Contestia 8/500", KbdContestia, nullptr, 8, 500}, {117, "Contestia 8/1000", KbdContestia, nullptr, 8, 1000}, {247, "Contestia 8/2000", KbdContestia, nullptr, 8, 2000},
        {275, "Contestia 16/250", KbdContestia, nullptr, 16, 250}, {50, "Contestia 16/500", KbdContestia, nullptr, 16, 500}, {53, "Contestia 16/1000", KbdContestia, nullptr, 16, 1000}, {259, "Contestia 16/2000", KbdContestia, nullptr, 16, 2000},
        {51, "Contestia 32/1000", KbdContestia, nullptr, 32, 1000}, {201, "Contestia 32/2000", KbdContestia, nullptr, 32, 2000},
        {136, "THOR 4", KbdThor, "THOR 4", 0, 0}, {137, "THOR 8", KbdThor, "THOR 8", 0, 0}, {138, "THOR 16", KbdThor, "THOR 16", 0, 0}, {139, "THOR 5", KbdThor, "THOR 5", 0, 0}, {143, "THOR 11", KbdThor, "THOR 11", 0, 0}, {145, "THOR 22", KbdThor, "THOR 22", 0, 0},
        {84, "DominoEX 4", KbdDominoex, "DominoEX 4", 0, 0}, {85, "DominoEX 5", KbdDominoex, "DominoEX 5", 0, 0}, {86, "DominoEX 8", KbdDominoex, "DominoEX 8", 0, 0}, {87, "DominoEX 11", KbdDominoex, "DominoEX 11", 0, 0}, {88, "DominoEX 16", KbdDominoex, "DominoEX 16", 0, 0}, {90, "DominoEX 22", KbdDominoex, "DominoEX 22", 0, 0},
        {9, "MT63-500 long", -1, nullptr, 0, 0}, {10, "MT63-500 short", -1, nullptr, 0, 0}, {12, "MT63-1000 long", -1, nullptr, 0, 0}, {13, "MT63-1000 short", -1, nullptr, 0, 0}, {15, "MT63-2000 long", -1, nullptr, 0, 0}, {17, "MT63-2000 short", -1, nullptr, 0, 0},
        {104, "Feld Hell", KbdHell, "Feld Hell", 0, 0}, {106, "Hell 80", KbdHell, "Hell 80", 0, 0},
    };
    return v;
}

struct RsidDetector::Impl {
    double ph = 0;
    float prev = 0;
    std::vector<float> in;           // at 11025
    std::vector<cf32> fbuf;
    Fft fft{kFft};
    std::vector<float> power, hann;
    struct Frame { std::vector<int8_t> offs; std::vector<float> pow; };   // for each base bin, the strongest of the 16 tone positions; the bin powers
    std::deque<Frame> hist;
    std::deque<RsidHit> hits;
    int cool = 0;
    double lastCode = -1;
    int lastBin = -1;

    Impl() : power(kFft / 2), hann(kSym) {
        for (int i = 0; i < kSym; i++) hann[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * i / kSym);
        fbuf.resize(kFft);
    }

    void frame() {
        for (int i = 0; i < kFft; i++) fbuf[(size_t)i] = i < kSym ? cf32(in[(size_t)i] * hann[(size_t)i], 0.f) : cf32(0, 0);
        fft.forward(fbuf.data());
        for (int i = 0; i < kFft / 2; i++) power[(size_t)i] = std::norm(fbuf[(size_t)i]);
        const int top = kFft / 2 - 32;
        Frame f;
        f.offs.resize((size_t)top);
        for (int b = kBinLow; b < top; b++) {
            int best = 0; float bp = power[(size_t)b];
            for (int j = 1; j < 16; j++) { const float p = power[(size_t)(b + 2 * j)]; if (p > bp) { bp = p; best = j; } }
            f.offs[(size_t)b] = (int8_t)best;
        }
        f.pow.assign(power.begin(), power.end());
        hist.push_back(std::move(f));
        if (hist.size() > 29) hist.pop_front();
        if (cool > 0) cool--;
        if (hist.size() == 29 && cool == 0) search();
    }

    void search() {
        const int top = (int)hist[0].offs.size();
        int bestBin = -1, bestDist = 99; const Compiled* bestC = nullptr;
        float bestE = 0;
        for (const auto& c : compiled()) {
            for (int b = kBinLow; b < top; b++) {
                int d = 0;
                for (int s = 0; s < 15 && d <= kMaxDist; s++) if (hist[(size_t)s * 2].offs[(size_t)b] != c.sym[s]) d++;
                if (d > kMaxDist) continue;
                float e = 0;
                for (int s = 0; s < 15; s++) e += hist[(size_t)s * 2].pow[(size_t)(b + 2 * c.sym[s])];
                // resampling images repeat the tones far weaker: the strongest match is the signal
                if (d < bestDist || (d == bestDist && e > bestE)) { bestDist = d; bestBin = b; bestC = &c; bestE = e; }
            }
        }
        if (!bestC) return;
        RsidHit h;
        h.code = bestC->m->code; h.name = bestC->m->name; h.distance = bestDist;
        h.audioHz = (bestBin / 2.0 + 7.0) * kFs / 1024.0;   // the tone of symbol 7 is the modem's frequency
        hits.push_back(h);
        cool = 30;                                          // the sequence is 15 symbols long
    }
};

RsidDetector::RsidDetector() : p_(std::make_unique<Impl>()) { reset(); }
RsidDetector::~RsidDetector() = default;
void RsidDetector::reset() { p_->ph = 0; p_->prev = 0; p_->in.clear(); p_->hist.clear(); p_->hits.clear(); p_->cool = 0; }

void RsidDetector::feedAudio(const float* x, size_t n) {
    Impl& m = *p_;
    const double step = 8000.0 / kFs;
    for (size_t i = 0; i < n; i++) {
        while (m.ph <= 1.0) {
            m.in.push_back(m.prev + (float)m.ph * (x[i] - m.prev));
            m.ph += step;
            if (m.in.size() == (size_t)kSym + 1024) {
                // keep the window in the first kSym samples
                while (m.in.size() >= (size_t)kSym) { m.frame(); m.in.erase(m.in.begin(), m.in.begin() + kHop); }
            }
        }
        m.ph -= 1.0;
        m.prev = x[i];
    }
}

bool RsidDetector::poll(RsidHit& out) {
    if (p_->hits.empty()) return false;
    out = p_->hits.front();
    p_->hits.pop_front();
    return true;
}

} // namespace dect2
