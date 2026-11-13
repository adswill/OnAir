// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ / IFKP signal layer (see kbd33.h): the varicode, the CRC, the tone sequence and the receiver.
//
// Receiver: the 8 kHz audio goes into a 4096-point spectrum (averaged over a couple of seconds); a box as wide as the 33 tones that holds
// more power than the bands on both sides marks the band. The tone grid (the tones are a fixed spacing apart) is found from where the
// spectral peaks of single frames fall modulo that spacing. 73 grid positions (the band with 20 on each side, because the first guess of
// the band can be that far off) are then mixed down and integrated in blocks of 16 samples, as running sums; any symbol window is then a
// difference of two sums. The symbol timing and the speed come from the same sums (the right grid puts one tone in each window and never the same
// tone twice in a row). From then on every symbol is the strongest of the positions; the step to the previous tone is the data.
// The window is nudged early/late by the energy of the winning tone, the grid by the phase turn of the winning tone between blocks.
#include "dect2/fsq/kbd33.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace kbd33 {

using dect2::math::kPi;

// ------------------------------------------------------------------ parameters and the varicode

Spec fsqSpec(int speed) {
    static const int len[kFsqSpeeds] = {8192, 6144, 4096, 3072, 2048};
    speed = std::max(0, std::min(kFsqSpeeds - 1, speed));
    Spec s;
    s.ifkp = false; s.speed = speed;
    s.spacingHz = 3.0 * kFsqRate / 4096.0;
    s.symSec = len[speed] / kFsqRate;
    return s;
}

Spec ifkpSpec(int speed) {
    static const int len[kIfkpSpeeds] = {8192, 4096, 2048};
    speed = std::max(0, std::min(kIfkpSpeeds - 1, speed));
    Spec s;
    s.ifkp = true; s.speed = speed;
    s.spacingHz = 3.0 * kIfkpRate / 4096.0;
    s.symSec = len[speed] / kIfkpRate;
    return s;
}

const char* speedName(bool ifkp, int speed) {
    static const char* f[kFsqSpeeds] = {"1.5", "2", "3", "4.5", "6"};
    static const char* i[kIfkpSpeeds] = {"0.5", "1.0", "2.0"};
    if (ifkp) return i[std::max(0, std::min(kIfkpSpeeds - 1, speed))];
    return f[std::max(0, std::min(kFsqSpeeds - 1, speed))];
}

namespace {

struct Codes {
    int single[29];          // symbol -> character (0 none)
    int pair[29][3];         // first symbol, marker 29 / 30 / 31 -> character (-1 none, 0 idle)
    int8_t enc[256][2];
    bool has[256];
};

const Codes& codes(bool ifkp) {
    static Codes tab[2];
    static bool built[2] = {false, false};
    Codes& c = tab[ifkp ? 1 : 0];
    if (built[ifkp ? 1 : 0]) return c;
    for (int i = 0; i < 29; i++) { c.single[i] = 0; for (int j = 0; j < 3; j++) c.pair[i][j] = -1; }
    for (int i = 1; i <= 26; i++) c.single[i] = 'a' + i - 1;
    c.single[27] = '.';
    c.single[28] = ifkp ? ' ' : '\n';
    c.single[0] = ifkp ? 0 : ' ';
    const char* m29 = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ,?";
    const char* m30 = "~1234567890!\"#$%&'()*+-/:;<>";
    for (int i = 0; i < 29; i++) c.pair[i][0] = (unsigned char)m29[i];
    for (int i = 0; i < 28; i++) c.pair[i][1] = (unsigned char)m30[i];
    c.pair[28][1] = ifkp ? '\n' : 0;
    static const int m31[15] = {'=', '[', '\\', ']', '^', '_', '{', '|', '}', '`', 177, 247, 176, 215, 163};
    for (int i = 0; i < 15; i++) c.pair[i][2] = m31[i];
    c.pair[27][2] = 8;
    c.pair[28][2] = 127;
    std::memset(c.enc, 0, sizeof c.enc);
    std::memset(c.has, 0, sizeof c.has);
    for (int i = 0; i < 29; i++)
        if (c.single[i] > 0 && !c.has[c.single[i]]) { c.has[c.single[i]] = true; c.enc[c.single[i]][0] = (int8_t)i; c.enc[c.single[i]][1] = -1; }
    for (int i = 0; i < 29; i++)
        for (int j = 0; j < 3; j++) {
            const int ch = c.pair[i][j];
            if (ch > 0 && ch < 256 && !c.has[ch]) { c.has[ch] = true; c.enc[ch][0] = (int8_t)i; c.enc[ch][1] = (int8_t)(29 + j); }
        }
    built[ifkp ? 1 : 0] = true;
    return c;
}

} // namespace

bool encode(bool ifkp, int ch, int& s1, int& s2) {
    if (ch < 0 || ch > 255) return false;
    const Codes& c = codes(ifkp);
    if (!c.has[ch]) return false;
    s1 = c.enc[ch][0]; s2 = c.enc[ch][1];
    return true;
}

int idleSymbol(bool ifkp, int& s2) {
    if (ifkp) { s2 = -1; return 0; }
    s2 = 30;
    return 28;
}

int CharDecoder::single(int s) const { return codes(ifkp_).single[s]; }
int CharDecoder::pair(int a, int b) const { return codes(ifkp_).pair[a][b - 29]; }

int CharDecoder::push(int sym) {
    if (sym < 0 || sym > 31) { have_ = false; return -1; }
    if (!have_) {
        if (sym < 29) { prev_ = sym; have_ = true; return 0; }
        return -1;
    }
    if (sym < 29) { const int c = single(prev_); prev_ = sym; return c; }
    const int c = pair(prev_, sym);
    have_ = false;
    return c;
}

int CharDecoder::flush() {
    if (!have_) return 0;
    have_ = false;
    return single(prev_);
}

std::string crc8hex(const std::string& s) {
    uint8_t crc = 0;
    for (unsigned char b : s) {
        crc ^= b;
        for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    char b[4];
    snprintf(b, sizeof b, "%02x", crc);
    return b;
}

std::vector<int> toneSequence(bool ifkp, const std::string& text, int startTone, int idleTail) {
    std::vector<int> t;
    int tone = ((startTone % kTones) + kTones) % kTones;
    t.push_back(tone);   // the reference: the first symbol of a receiver has no tone to step from
    auto sym = [&](int s) { tone = (tone + s + 1) % kTones; t.push_back(tone); };
    for (unsigned char ch : text) {
        int a, b;
        if (!encode(ifkp, ch, a, b)) continue;
        sym(a);
        if (b >= 0) sym(b);
    }
    for (int i = 0; i < idleTail; i++) {
        int b;
        const int a = idleSymbol(ifkp, b);
        sym(a);
        if (b >= 0) sym(b);
    }
    return t;
}

// ------------------------------------------------------------------ receiver

namespace {
using cd = std::complex<double>;
constexpr int kBlk = 16;                 // samples per block
constexpr int kRingB = 4096;             // blocks of running sums kept (8.2 s)
constexpr int kHistB = 3750;             // blocks looked back at a synchronisation (7.5 s)
constexpr int kMargin = 20;              // grid positions before the first guess of the band
constexpr int kN = 4096, kHop = 2048;    // the search spectrum
constexpr int kLatN = 2048, kLatHop = 1024;
constexpr double kGate = 0.16;           // share of the energy a symbol needs in its strongest tone to be used
constexpr double kDropPf = 0.12;         // smoothed share under which the lock is given up
constexpr double kAcceptPf = 0.20;       // mean share a new lock needs
constexpr int kRawBits = 17;
}

struct Rx::Impl {
    bool ifkp;
    std::atomic<int> speedReq{-1};
    // samples and the search spectrum
    std::vector<cf32> raw;
    int64_t n = 0;
    Fft fft{kN}, fftLat{kLatN};
    std::vector<float> win, winLat, avgP, acc;
    int accN = 0, frames = 0;
    float floorLin = 0;
    double cooldown = 0;
    int loBin = 0, hiBin = 0;
    // lock
    bool locked = false;
    int speed = -1, nSyncs = 0;
    Spec spec;
    double nbd = 0;                      // blocks per symbol
    double grid0 = 0, afc = 0;
    std::array<double, Rx::kPos> omega{};
    std::array<bool, Rx::kPos> valid{};
    std::vector<double> Wre, Wim;
    std::array<cd, Rx::kPos> B, stp;
    std::vector<cd> pre;                 // running sums of the blocks, kRingB x kPos
    int64_t gbDone = 0, gbFirst = 0;     // blocks summed; the first block of the sums
    double symPos = 0;
    int prevTone = -1;
    CharDecoder dec;
    double emaPf = 0, snr = 0;
    uint64_t nSym = 0, nDrop = 0;
    std::vector<float> use;
    std::array<uint32_t, Rx::kPos> cnt{};
    uint32_t cntTotal = 0;
    int blockCount = 0;

    explicit Impl(bool i) : ifkp(i), dec(i) {
        raw.assign((size_t)1 << kRawBits, cf32(0.f, 0.f));
        win.resize(kN); winLat.resize(kLatN);
        for (int k = 0; k < kN; k++) win[(size_t)k] = (float)(0.5 - 0.5 * std::cos(2 * kPi * k / kN));
        for (int k = 0; k < kLatN; k++) winLat[(size_t)k] = (float)(0.5 - 0.5 * std::cos(2 * kPi * k / kLatN));
        loBin = (int)std::ceil(Rx::kLoHz / (Rx::kFs / kN)); hiBin = (int)std::floor(Rx::kHiHz / (Rx::kFs / kN));
        spec = specOf(i, 0);
        reset();
    }

    std::function<void(int, double)>* ch = nullptr;

    void reset() {
        std::fill(raw.begin(), raw.end(), cf32(0.f, 0.f));
        n = 0; frames = 0; accN = 0; floorLin = 0; cooldown = 0;
        avgP.assign(kN / 2, 0.f);
        acc.assign((size_t)(hiBin - loBin + 1), 0.f);
        unlock(false);
        nSyncs = 0; nSym = nDrop = 0;
        use.assign(Rx::kPos, 0.f); cnt.fill(0); cntTotal = 0;
        emaPf = 0; snr = 0;
    }

    void unlock(bool flush) {
        if (locked && flush) {
            const int c = dec.flush();
            if (c > 0 && ch && *ch) (*ch)(c, (double)n / Rx::kFs);
        }
        locked = false; speed = -1; prevTone = -1;
        dec.reset();
        std::vector<cd>().swap(pre);
    }

    // ---------------------------------------------------------------- search
    void spectrumFrame() {
        std::vector<cf32> b(kN);
        for (int i = 0; i < kN; i++) b[(size_t)i] = raw[(size_t)((n - kN + i) & (((int64_t)1 << kRawBits) - 1))] * win[(size_t)i];
        fft.forward(b.data());
        for (int k = 0; k < kN / 2; k++) {
            const float p = std::norm(b[(size_t)k]) * (4.f / ((float)kN * kN));
            avgP[(size_t)k] += 0.12f * (p - avgP[(size_t)k]);
            if (k >= loBin && k <= hiBin) acc[(size_t)(k - loBin)] += p;
        }
        accN++;
        std::vector<float> srt(avgP.begin() + loBin, avgP.begin() + hiBin);
        std::nth_element(srt.begin(), srt.begin() + srt.size() / 2, srt.end());
        floorLin = srt[srt.size() / 2];
        frames++;
    }

    // The start (Hz) of a box as wide as the band that holds more power than the bands beside it; false when there is none
    bool findBand(double& fcStart) {
        const double binHz = Rx::kFs / kN;
        const int Wb = (int)std::lround(spec.bandHz() / binHz);
        std::vector<double> ps((size_t)kN / 2 + 1, 0.0);
        for (int k = 0; k < kN / 2; k++) ps[(size_t)k + 1] = ps[(size_t)k] + avgP[(size_t)k];
        const double med = std::max(1e-20f, floorLin);
        auto mean = [&](int a, int b) { return (ps[(size_t)b + 1] - ps[(size_t)a]) / (b - a + 1); };
        const int guard = 6, fl = Wb / 2;
        struct P { int a; double c; };
        std::vector<P> cand;
        double best = 0;
        for (int a = loBin; a + Wb <= hiBin; a += 2) {
            const double sin = mean(a, a + Wb);
            if (sin < 1.8 * med) continue;
            double sout = 1e30;
            if (a - guard - fl >= loBin) sout = std::min(sout, mean(a - guard - fl, a - guard));
            if (a + Wb + guard + fl <= hiBin) sout = std::min(sout, mean(a + Wb + guard, a + Wb + guard + fl));
            if (sout > 1e29) continue;
            const double c = sin / std::max(sout, 0.5 * med);
            cand.push_back({a, c});
            best = std::max(best, c);
        }
        if (best < 1.8) return false;
        int amin = 1 << 30, amax = -1;
        for (const P& p : cand) if (p.c >= 0.9 * best) { amin = std::min(amin, p.a); amax = std::max(amax, p.a); }
        fcStart = 0.5 * (amin + amax) * binHz;
        return true;
    }

    // The offset of the tone grid: where the spectral peaks of single frames fall modulo the spacing
    bool findLattice(double fcStart, double& base) {
        const double bh = Rx::kFs / kLatN, sp = spec.spacingHz;
        const double flo = std::max(Rx::kLoHz, fcStart - 17 * sp), fhi = std::min(Rx::kHiHz, fcStart + 50 * sp);
        const int b0 = (int)std::ceil(flo / bh), b1 = (int)std::floor(fhi / bh);
        if (b1 - b0 < 8) return false;
        const int64_t histN = std::min<int64_t>(n, (int64_t)10 * 8000);
        cd s(0, 0);
        int cntF = 0;
        std::vector<cf32> b(kLatN);
        std::vector<float> p((size_t)(b1 - b0 + 1)), srt;
        const int64_t mask = ((int64_t)1 << kRawBits) - 1;
        for (int64_t end = n; end - kLatN >= n - histN; end -= kLatHop) {
            for (int i = 0; i < kLatN; i++) b[(size_t)i] = raw[(size_t)((end - kLatN + i) & mask)] * winLat[(size_t)i];
            fftLat.forward(b.data());
            int im = 0;
            for (int k = b0; k <= b1; k++) {
                p[(size_t)(k - b0)] = std::norm(b[(size_t)k]);
                if (p[(size_t)(k - b0)] > p[(size_t)im]) im = k - b0;
            }
            srt = p;
            std::nth_element(srt.begin(), srt.begin() + srt.size() / 2, srt.end());
            const float md = srt[srt.size() / 2];
            if (im <= 0 || im >= (int)p.size() - 1 || p[(size_t)im] < 8.f * md) continue;
            const double l = std::log((double)p[(size_t)im - 1] + 1e-30), c = std::log((double)p[(size_t)im]), r = std::log((double)p[(size_t)im + 1] + 1e-30);
            const double den = l - 2 * c + r;
            const double d = den < -1e-12 ? 0.5 * (l - r) / den : 0.0;
            const double f = (b0 + im + std::max(-0.5, std::min(0.5, d))) * bh;
            s += std::polar(1.0, 2 * kPi * (f - fcStart) / sp);
            cntF++;
        }
        if (cntF < 10 || std::abs(s) / cntF < 0.45) return false;
        base = fcStart + std::arg(s) / (2 * kPi) * sp;
        return true;
    }

    // ---------------------------------------------------------------- the running sums
    void setTones() {
        Wre.resize((size_t)Rx::kPos * kBlk); Wim.resize((size_t)Rx::kPos * kBlk);
        for (int k = 0; k < Rx::kPos; k++) {
            const double f = grid0 + afc + k * spec.spacingHz;
            valid[(size_t)k] = f >= 60 && f <= 3940;
            omega[(size_t)k] = 2 * kPi * f / Rx::kFs;
            for (int m = 0; m < kBlk; m++) { Wre[(size_t)(k * kBlk + m)] = std::cos(omega[(size_t)k] * m); Wim[(size_t)(k * kBlk + m)] = -std::sin(omega[(size_t)k] * m); }
            stp[(size_t)k] = std::polar(1.0, -omega[(size_t)k] * kBlk);
        }
    }
    void startPhasors(int64_t gb) {
        for (int k = 0; k < Rx::kPos; k++) B[(size_t)k] = std::polar(1.0, -std::fmod(omega[(size_t)k] * kBlk * (double)gb, 2 * kPi));
    }
    cd* row(int64_t g) { return &pre[(size_t)(g % kRingB) * Rx::kPos]; }
    void computeBlock(int64_t gb) {
        const int64_t mask = ((int64_t)1 << kRawBits) - 1;
        float xr[kBlk], xi[kBlk];
        for (int m = 0; m < kBlk; m++) { const cf32 v = raw[(size_t)((gb * kBlk + m) & mask)]; xr[m] = v.real(); xi[m] = v.imag(); }
        const cd* p0 = row(gb);
        cd* p1 = row(gb + 1);
        for (int k = 0; k < Rx::kPos; k++) {
            cd S(0, 0);
            if (valid[(size_t)k]) {
                double ar = 0, ai = 0;
                const double* wr = &Wre[(size_t)k * kBlk];
                const double* wi = &Wim[(size_t)k * kBlk];
                for (int m = 0; m < kBlk; m++) { ar += xr[m] * wr[m] - xi[m] * wi[m]; ai += xr[m] * wi[m] + xi[m] * wr[m]; }
                S = B[(size_t)k] * cd(ar, ai);
            }
            p1[k] = p0[k] + S;
            B[(size_t)k] *= stp[(size_t)k];
        }
        if ((++blockCount & 255) == 0) for (int k = 0; k < Rx::kPos; k++) B[(size_t)k] /= std::abs(B[(size_t)k]);
    }
    // energy of every position in the blocks [a, b)
    void energies(int64_t a, int64_t b, double* E) {
        const cd* pa = row(a);
        const cd* pb = row(b);
        for (int k = 0; k < Rx::kPos; k++) E[k] = valid[(size_t)k] ? std::norm(pb[k] - pa[k]) : 0.0;
    }
    double amp(int64_t a, int64_t b, int k) { return std::abs(row(b)[k] - row(a)[k]); }

    // ---------------------------------------------------------------- synchronisation
    struct Fit { double score = 0, pf = 0, rep = 1; int speed = -1; int64_t start = 0; };

    Fit scoreGrid(int si, int64_t gbStart, int64_t gbEnd) {
        Fit f;
        const double nb = specOf(ifkp, si).symSec * Rx::kFs / kBlk;
        const int M = (int)std::min(24.0, std::floor((double)(gbEnd - gbStart) / nb) - 1);
        if (M < 6) return f;
        const int64_t region = gbEnd - (int64_t)std::ceil((M + 1) * nb);
        double E[Rx::kPos];
        for (int phi = 0; phi < (int)std::ceil(nb); phi++) {
            double pfSum = 0;
            int rep = 0, prev = -1;
            for (int j = 0; j < M; j++) {
                const int64_t a = region + phi + (int64_t)std::llround(j * nb), b = region + phi + (int64_t)std::llround((j + 1) * nb);
                energies(a, b, E);
                int w = 0; double sum = 0;
                for (int k = 0; k < Rx::kPos; k++) { sum += E[k]; if (E[k] > E[w]) w = k; }
                pfSum += sum > 0 ? E[w] / sum : 0;
                if (w == prev) rep++;
                prev = w;
            }
            const double pf = pfSum / M, rr = (double)rep / std::max(1, M - 1);
            const double sc = pf * (1 - rr) * (1 - rr);
            if (sc > f.score) { f.score = sc; f.pf = pf; f.rep = rr; f.speed = si; f.start = region + phi; }
        }
        return f;
    }

    bool trySync(double fcStart) {
        double base;
        spec = specOf(ifkp, 0);   // the spacing is the same for every speed
        if (!findLattice(fcStart, base)) return false;
        grid0 = base - kMargin * spec.spacingHz;
        afc = 0;
        setTones();
        const int64_t gbEnd = n / kBlk, gbStart = std::max<int64_t>(0, gbEnd - kHistB);
        pre.assign((size_t)kRingB * Rx::kPos, cd(0, 0));
        startPhasors(gbStart);
        blockCount = 0;
        for (int64_t gb = gbStart; gb < gbEnd; gb++) computeBlock(gb);
        gbDone = gbEnd; gbFirst = gbStart;
        Fit best;
        const int want = speedReq.load();
        for (int si = 0; si < speedCount(ifkp); si++) {
            if (want >= 0 && want != si) continue;
            const Fit f = scoreGrid(si, gbStart, gbEnd);
            if (f.score > best.score) best = f;
        }
        if (best.speed < 0 || best.pf < kAcceptPf || best.rep > 0.25) { std::vector<cd>().swap(pre); return false; }
        speed = best.speed;
        spec = specOf(ifkp, speed);
        nbd = spec.symSec * Rx::kFs / kBlk;
        // the first window of the grid that is still in the history
        const double k0 = std::floor((double)(best.start - gbStart) / nbd);
        symPos = (double)best.start - k0 * nbd;
        locked = true;
        nSyncs++;
        prevTone = -1;
        dec.reset();
        emaPf = best.pf; snr = 0;
        use.assign(Rx::kPos, 0.f); cnt.fill(0); cntTotal = 0;
        decodeAvailable();
        return true;
    }

    // ---------------------------------------------------------------- symbols
    void decodeAvailable() {
        const int dl = std::max(1, (int)std::lround(nbd / 12));
        double E[Rx::kPos];
        while (locked) {
            const int64_t a = (int64_t)std::llround(symPos), b = (int64_t)std::llround(symPos + nbd);
            if (b + dl > gbDone) break;
            if (a - dl < gbDone - kRingB + 2) { unlock(true); cooldown = 1.0; break; }
            if (a - dl < gbFirst) { symPos += nbd; continue; }   // before the sums began
            energies(a, b, E);
            int w = 0; double sum = 0;
            for (int k = 0; k < Rx::kPos; k++) { sum += E[k]; if (E[k] > E[w]) w = k; }
            const double pf = sum > 0 ? E[w] / sum : 0;
            emaPf += (pf - emaPf) * (1.0 / 16);
            nSym++;
            double shift = 0;
            if (pf >= kGate) {
                // the noise per tone: the median of the energies
                std::array<double, Rx::kPos> srt;
                int nv = 0;
                for (int k = 0; k < Rx::kPos; k++) if (valid[(size_t)k]) srt[(size_t)nv++] = E[k];
                std::nth_element(srt.begin(), srt.begin() + nv / 2, srt.begin() + nv);
                const double en = srt[(size_t)nv / 2] / 0.693 + 1e-30;
                const double r = std::max(1e-3, (E[w] - en) / en);
                const double bw = spec.bandHz(), tt = (double)(b - a) * kBlk / Rx::kFs;
                const double s = 10 * std::log10(r) - 10 * std::log10(tt * bw);
                snr += (s - snr) * (nSym < 4 ? 1.0 : 0.1);
                for (int k = 0; k < Rx::kPos; k++) use[(size_t)k] *= 0.985f;
                use[(size_t)w] += 0.015f;
                cnt[(size_t)w]++; cntTotal++;
                if (prevTone >= 0) {
                    const int nib = (((w - prevTone - 1) % kTones) + kTones) % kTones;
                    const int c = dec.push(nib);
                    if (c > 0 && ch && *ch) (*ch)(c, (double)b * kBlk / Rx::kFs);
                }
                prevTone = w;
                // grid: the phase turn of the winning tone between the two halves of the window
                const int64_t mid = a + (b - a) / 2;
                const cd s1 = row(mid)[w] - row(a)[w], s2 = row(b)[w] - row(mid)[w];
                if (pf >= 0.3 && std::abs(s1) > 0 && std::abs(s2) > 0) {
                    const double half = (double)(mid - a) * kBlk / Rx::kFs;   // seconds between the centres of the halves
                    const double lim = 0.45 / (2 * half);
                    const double d = std::max(-lim, std::min(lim, std::arg(s2 * std::conj(s1)) / (2 * kPi * half)));
                    afc += 0.3 * d;
                    afc = std::max(-60.0, std::min(60.0, afc));
                    setTones();
                }
                // timing: the winning tone in a window a little late against one a little early
                if (pf >= 0.25) {
                    const double al = amp(a + dl, b + dl, w), ae = amp(a - dl, b - dl, w);
                    const double err = (al - ae) / (al + ae + 1e-30);
                    shift = std::max(-nbd / 16, std::min(nbd / 16, 0.15 * err * nbd));
                }
            } else {
                nDrop++;
                dec.reset();
                prevTone = -1;   // the next tone is a reference again
            }
            symPos += nbd + shift;
            if (emaPf < kDropPf && nSym > 24) { unlock(true); cooldown = 1.0; break; }
        }
    }

    // ---------------------------------------------------------------- samples
    void process(const cf32* x, size_t len) {
        const int64_t mask = ((int64_t)1 << kRawBits) - 1;
        const int64_t n0 = n;
        for (size_t i = 0; i < len; i++) raw[(size_t)((n0 + (int64_t)i) & mask)] = x[i];
        const int64_t n1 = n0 + (int64_t)len;
        // spectrum frames at every hop crossed
        for (int64_t h = (n0 / kHop + 1) * kHop; h <= n1; h += kHop) {
            if (h < kN) continue;
            const int64_t save = n;
            n = h;
            spectrumFrame();
            n = save;
        }
        n = n1;
        if (locked) {
            while ((gbDone + 1) * kBlk <= n) computeBlock(gbDone++);
            decodeAvailable();
        } else {
            if (cooldown > 0) cooldown -= (double)len / Rx::kFs;
            else if (frames >= 4 && n > 6 * (int64_t)Rx::kFs) {
                frames = 0;
                double fc;
                if (findBand(fc) && !trySync(fc)) cooldown = 1.5;
            }
        }
    }
};

Rx::Rx(bool ifkp) : p_(std::make_unique<Impl>(ifkp)) { p_->ch = &onChar; }
Rx::~Rx() = default;
void Rx::reset() { p_->reset(); }
void Rx::setSpeed(int speed) { p_->speedReq = speed < 0 || speed >= speedCount(p_->ifkp) ? -1 : speed; }
void Rx::feed(const cf32* x, size_t n) {
    for (size_t i = 0; i < n; i += 512) p_->process(x + i, std::min<size_t>(512, n - i));
}
bool Rx::locked() const { return p_->locked; }
int Rx::speed() const { return p_->locked ? p_->speed : -1; }
double Rx::timeSec() const { return (double)p_->n / kFs; }
double Rx::snrDb() const { return p_->snr; }
double Rx::afcHz() const { return p_->afc; }
double Rx::quality() const { return p_->locked ? p_->emaPf : 0; }
double Rx::gridHz() const { return p_->grid0 + p_->afc; }
double Rx::spacingHz() const { return p_->spec.spacingHz; }
const std::vector<float>& Rx::toneUse() const { return p_->use; }
uint64_t Rx::symbols() const { return p_->nSym; }
uint64_t Rx::droppedSymbols() const { return p_->nDrop; }
int Rx::syncs() const { return p_->nSyncs; }
double Rx::spectrumLoHz() const { return p_->loBin * (kFs / kN); }

void Rx::occupied(double& loHz, double& hiHz) const {
    const Impl& m = *p_;
    loHz = hiHz = 0;
    if (!m.locked || m.cntTotal < 8) return;
    const uint32_t th = std::max<uint32_t>(2, m.cntTotal / 100);
    int lo = -1, hi = -1;
    for (int k = 0; k < kPos; k++) if (m.cnt[(size_t)k] >= th) { if (lo < 0) lo = k; hi = k; }
    if (lo < 0) return;
    loHz = m.grid0 + m.afc + lo * m.spec.spacingHz;
    hiHz = m.grid0 + m.afc + hi * m.spec.spacingHz;
}

bool Rx::takeSpectrum(std::vector<float>& out, float& floorDb) {
    Impl& m = *p_;
    if (m.accN <= 0) return false;
    out.resize(m.acc.size());
    for (size_t i = 0; i < m.acc.size(); i++) { out[i] = (float)(10 * std::log10(m.acc[i] / m.accN + 1e-20)); m.acc[i] = 0; }
    m.accN = 0;
    floorDb = (float)(10 * std::log10(m.floorLin + 1e-20));
    return true;
}

} // namespace kbd33
} // namespace dect2
