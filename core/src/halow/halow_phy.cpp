// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah, S1G) PHY tables: tone layouts of the 1 - 16 MHz channels, preamble training fields, MCS table (halow_phy.h).
// IEEE 802.11-2020 clause 23 (S1G PHY) follows the 802.11ac VHT PHY at one tenth of the clock: 1 MHz = new 32-point layout, 2 / 4 / 8 / 16 MHz = the
// 11ac 20 / 40 / 80 / 160 MHz layouts.
#include "dect2/halow/halow_phy.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace dect2::halow {

namespace {

// 11a / 11ac 20 MHz long training sequence, k = -26 .. 26 (the 2 MHz LTS is this with the tones +-27, +-28 added)
const int kLtfNeg[26] = {1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1, 1, 1, -1, -1, 1, 1, -1, 1, -1, 1, 1, 1, 1};
const int kLtfPos[26] = {1, -1, -1, 1, 1, -1, 1, -1, 1, -1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1, 1, -1, 1, 1, 1, 1};

// 2 MHz LTS: the 20 MHz sequence plus +-27, +-28 (clause 23.3.9.? VHT-LTF 20 MHz layout)
int ltf2(int k) {
    if (k == 0 || k < -28 || k > 28) return 0;
    if (k == -28 || k == -27) return 1;
    if (k == 27 || k == 28) return -1;
    return k < 0 ? kLtfNeg[k + 26] : kLtfPos[k - 1];
}

// 4 MHz LTS: the 11ac 40 MHz VHT-LTF as the author remembers it: {left, 1, right, -1 -1 -1 1, 0 0 0, -1 1 1 -1, left, 1, right} on k = -58 .. 58
int ltf4(int k) {
    if (k < -58 || k > 58 || (k >= -1 && k <= 1)) return 0;
    if (k <= -33) return kLtfNeg[k + 58];
    if (k == -32) return 1;
    if (k <= -6) return kLtfPos[k + 31];
    if (k <= -2) { static const int v[4] = {-1, -1, -1, 1}; return v[k + 5]; }
    if (k <= 5) { static const int v[4] = {-1, 1, 1, -1}; return v[k - 2]; }
    if (k <= 31) return kLtfNeg[k - 6];
    if (k == 32) return 1;
    return kLtfPos[k - 33];
}

// 8 MHz LTS: unverified. The 4 MHz sequence inside +-58, the legacy 26-tone halves repeated beyond (the true 11ac 80 MHz table was not available)
int ltf8(int k) {
    if (std::abs(k) < 2 || std::abs(k) > 122) return 0;
    if (std::abs(k) <= 58) return ltf4(k);
    const int s = (std::abs(k) - 59) % 26;
    return k > 0 ? kLtfPos[s] : kLtfNeg[25 - s];
}

// 16 MHz LTS: unverified. Two 8 MHz halves around +-128 (tones -250 .. -6 and 6 .. 250, the 8 MHz gap tones empty)
int ltf16(int k) {
    const int c = k < 0 ? -128 : 128;
    return ltf8(k - c);
}

// 11a short training sequence on the tones that are multiples of 4 (k = -24 .. 24); the unit is (1 + j) up to the scale
int stf2(int k) {
    switch (k) {
        case -24: return 1;  case -20: return -1; case -16: return 1;  case -12: return -1; case -8: return -1; case -4: return 1;
        case 4: return -1;   case 8: return -1;   case 12: return 1;   case 16: return 1;   case 20: return 1;  case 24: return 1;
        default: return 0;
    }
}

std::vector<int> range(int lo, int hi) { std::vector<int> v; for (int k = lo; k <= hi; k++) v.push_back(k); return v; }

// tone present in the width's used set (data or pilot)
bool usedTone(const WidthInfo& wi, int k) {
    if (k == 0 || std::abs(k) > wi.usedHi) return false;
    if (wi.w == W16) { const int t = std::abs(std::abs(k) - 128); return t >= 2 && t <= 122; }
    if (wi.w == W1) return std::abs(k) >= 1 && std::abs(k) <= 13;
    if (wi.w == W2) return std::abs(k) >= 1 && std::abs(k) <= 28;
    return std::abs(k) >= 2;
}

// Data layout of a width: tones usedLo .. hi minus the pilots, sorted by tone index
void buildLayout(int hi, int lo, const std::vector<int>& pil, Layout& lay, int shift = 0) {
    for (int p : pil) lay.pilotK.push_back(p + shift);
    for (int k = -hi; k <= hi; k++) {
        if (std::abs(k) < lo) continue;
        if (std::find(pil.begin(), pil.end(), k) != pil.end()) continue;
        lay.dataK.push_back(k + shift);
    }
}

// Builds the tone layout, interleaver and preamble sizes of one width (clause 23.3.8, 23.3.9).
WidthInfo makeInfo(int w) {
    WidthInfo wi;
    wi.w = w;
    wi.N = nfft(w);
    const bool wide = w >= W8;
    if (w == W1) {
        wi.usedHi = 13;
        buildLayout(13, 1, {-7, 7}, wi.data);
        wi.ncol = 6; wi.nrow = 4;
        wi.sigSyms = 6; wi.sigInfoBits = 36; wi.stfSamples = 640; wi.ltfSyms = 4;
    } else if (w == W2) {
        wi.usedHi = 28;
        buildLayout(28, 1, {-21, -7, 7, 21}, wi.data);
        wi.ncol = 13; wi.nrow = 4;
        wi.sigSyms = 2; wi.sigInfoBits = 52; wi.stfSamples = 320; wi.ltfSyms = 2;
    } else if (w == W4) {
        wi.usedHi = 58;
        buildLayout(58, 2, {-53, -25, -11, 11, 25, 53}, wi.data);
        wi.ncol = 18; wi.nrow = 6;
        wi.sigSyms = 2; wi.sigInfoBits = 52; wi.stfSamples = 320; wi.ltfSyms = 2;
    } else if (w == W8) {
        wi.usedHi = 122;
        buildLayout(122, 2, {-103, -75, -39, -11, 11, 39, 75, 103}, wi.data);
        wi.ncol = 26; wi.nrow = 9;
        wi.sigSyms = 2; wi.sigInfoBits = 52; wi.stfSamples = 640; wi.ltfSyms = 2;
    } else {   // 16 MHz: two 8 MHz layouts around -128 and +128
        wi.usedHi = 250;
        Layout lo8, hi8;
        buildLayout(122, 2, {-103, -75, -39, -11, 11, 39, 75, 103}, lo8, -128);
        buildLayout(122, 2, {-103, -75, -39, -11, 11, 39, 75, 103}, hi8, 128);
        wi.data.dataK = lo8.dataK; wi.data.dataK.insert(wi.data.dataK.end(), hi8.dataK.begin(), hi8.dataK.end());
        wi.data.pilotK = lo8.pilotK; wi.data.pilotK.insert(wi.data.pilotK.end(), hi8.pilotK.begin(), hi8.pilotK.end());
        wi.ncol = 26; wi.nrow = 18;
        wi.sigSyms = 2; wi.sigInfoBits = 52; wi.stfSamples = 1280; wi.ltfSyms = 2;
    }
    std::sort(wi.data.pilotK.begin(), wi.data.pilotK.end());
    wi.nsd = (int)wi.data.dataK.size();
    wi.ncbps = wi.nsd;
    wi.nUsed = wi.nsd + (int)wi.data.pilotK.size();
    if (w == W1) {
        wi.sig.push_back(wi.data);
        wi.sigNcbps = 24; wi.sigNcol = 6; wi.sigNrow = 4;
    } else {
        // the 2 MHz SIG field is repeated in every 2 MHz of the channel; tones that would fall outside the used set are dropped
        Layout base;
        buildLayout(28, 1, {-21, -7, 7, 21}, base);
        std::vector<int> shifts;
        if (w == W2) shifts = {0};
        else if (w == W4) shifts = {-32, 32};
        else if (w == W8) shifts = {-96, -32, 32, 96};
        else shifts = {-224, -160, -96, -32, 32, 96, 160, 224};
        for (int shift : shifts) {
            Layout l;
            for (int k : base.dataK) l.dataK.push_back(usedTone(wi, k + shift) ? k + shift : 0);
            for (int k : base.pilotK) if (usedTone(wi, k + shift)) l.pilotK.push_back(k + shift);
            wi.sig.push_back(l);
        }
        wi.sigNcbps = 52; wi.sigNcol = 13; wi.sigNrow = 4;
    }
    (void)wide;
    return wi;
}

// A cached FFT of size n for this thread.
Fft& fftN(int n) {
    static thread_local std::map<int, std::unique_ptr<Fft>> m;
    auto& p = m[n];
    if (!p) p = std::make_unique<Fft>(n);
    return *p;
}

} // namespace

const char* preambleName(int p) { return p == PreShort ? "short" : p == PreLong ? "long" : "1M"; }

// The tables of a width (clamped to 1 - 16 MHz).
const WidthInfo& info(int w) {
    static const WidthInfo t[kNumWidths] = {makeInfo(W1), makeInfo(W2), makeInfo(W4), makeInfo(W8), makeInfo(W16)};
    return t[w < 0 ? 0 : w > 4 ? 4 : w];
}

// ---------------------------------------------------------------- preamble

// Short training field spectrum: the 2 MHz sequence in every 2 MHz of the channel.
void stfSpectrum(int w, cf32* x) {
    const int N = nfft(w);
    for (int i = 0; i < N; i++) x[i] = cf32(0, 0);
    auto put = [&](int k, int v) { if (v) x[bin(k, N)] = cf32((float)v, (float)v); };
    if (w == W1) {
        for (int k : {-12, -8, -4, 4, 8, 12}) put(k, stf2(k));
    } else if (w == W2) {
        for (int k = -24; k <= 24; k += 4) put(k, stf2(k));
    } else {   // 4 MHz and up: the 2 MHz STF in every 2 MHz of the channel (centres +-32, +-96, +-160, +-224); the 11ac phase rotations are not applied (PAPR only)
        std::vector<int> centres = w == W4 ? std::vector<int>{-32, 32} : w == W8 ? std::vector<int>{-96, -32, 32, 96} : std::vector<int>{-224, -160, -96, -32, 32, 96, 160, 224};
        for (int c : centres) for (int p = -24; p <= 24; p += 4) if (p) put(c + p, stf2(p));
    }
}

// Long training field: +-1 on every used tone of the width.
void ltfSpectrum(int w, float* l) {
    const int N = nfft(w);
    for (int i = 0; i < N; i++) l[i] = 0;
    const int hi = info(w).usedHi;
    for (int k = -hi; k <= hi; k++) {
        int v = 0;
        if (w == W1) v = std::abs(k) <= 13 ? ltf2(k) : 0;   // unverified: the 2 MHz sequence on the 26 tones
        else if (w == W2) v = ltf2(k);
        else if (w == W4) v = ltf4(k);
        else if (w == W8) v = ltf8(k);
        else v = ltf16(k);
        if (v) l[bin(k, N)] = (float)v;
    }
}

// Scale that gives the STF unit power per sample.
float stfScale(int w) {
    std::vector<cf32> x((size_t)nfft(w));
    stfSpectrum(w, x.data());
    double e = 0;
    for (const cf32& v : x) e += std::norm(v);
    return (float)(1.0 / std::sqrt(e));
}

float symScale(int w) { return (float)(1.0 / std::sqrt((double)info(w).nUsed)); }

// One long training symbol in the time domain (N samples).
void ltfTime(int w, cf32* x) {
    const int N = nfft(w);
    std::vector<float> l((size_t)N);
    ltfSpectrum(w, l.data());
    for (int i = 0; i < N; i++) x[i] = cf32(l[(size_t)i], 0);
    fftN(N).inverse(x);
    const float g = symScale(w);
    for (int i = 0; i < N; i++) x[i] *= g;
}

// The short training field in the time domain (N samples, four periods of 8 us).
void stfTime(int w, cf32* x) {
    const int N = nfft(w);
    stfSpectrum(w, x);
    fftN(N).inverse(x);
    const float g = stfScale(w);
    for (int i = 0; i < N; i++) x[i] *= g;
}

// ---------------------------------------------------------------- MCS

// Modulation and rate of an MCS (clause 23.5, table of S1G MCSs).
const Mcs& mcsInfo(int mcs) {
    static const Mcs t[11] = {{1, 1, 2, false, "BPSK 1/2"},    {2, 1, 2, false, "QPSK 1/2"},   {2, 3, 4, false, "QPSK 3/4"},   {4, 1, 2, false, "16-QAM 1/2"},
                              {4, 3, 4, false, "16-QAM 3/4"},  {6, 2, 3, false, "64-QAM 2/3"},  {6, 3, 4, false, "64-QAM 3/4"}, {6, 5, 6, false, "64-QAM 5/6"},
                              {8, 3, 4, false, "256-QAM 3/4"}, {8, 5, 6, false, "256-QAM 5/6"}, {1, 1, 2, true, "BPSK 1/2 x2"}};
    return t[mcs < 0 ? 0 : mcs > 10 ? 10 : mcs];
}

// Valid when the data bits per symbol come out whole (the 11ac rule), minus the table exceptions the 11ac PHY has (80 MHz MCS 6 with 3 streams,
// 160 MHz MCS 9 with 3 streams); 4 spatial streams at most. Unverified against the S1G tables.
bool mcsValid(int w, int mcs, int nss) {
    if (mcs < 0 || mcs > 10 || nss < 1 || nss > 4 || w < 0 || w >= kNumWidths) return false;
    if (mcs == 10) return w == W1;
    const Mcs& m = mcsInfo(mcs);
    const int n = info(w).nsd * m.nbpscs * nss;
    if ((n * m.num) % m.den) return false;
    if (w == W8 && mcs == 6 && nss == 3) return false;
    if (w == W16 && mcs == 9 && nss == 3) return false;
    return true;
}

// Data bits per OFDM symbol (N_DBPS) of a valid combination, 0 otherwise.
int dataBitsPerSym(int w, int mcs, int nss) {
    if (!mcsValid(w, mcs, nss)) return 0;
    const Mcs& m = mcsInfo(mcs);
    int n = info(w).nsd * m.nbpscs * nss;
    if (m.rep) n /= 2;
    return n * m.num / m.den;
}

// BCC encoders: one per 2400 data bits per symbol (the 11ac rule that keeps each encoder under 600 Mbit/s at a 4 us symbol); unverified for S1G
int encoders(int w, int mcs, int nss) {
    const int nd = dataBitsPerSym(w, mcs, nss);
    return nd <= 0 ? 1 : (nd + 2399) / 2400;
}

// Data symbols of a PSDU: ceil((8 + 8 len + 6 N_ES) / N_DBPS), rounded up to even with STBC.
int dataSymbols(int w, int mcs, int len, int nss, bool stbc) {
    const int nd = dataBitsPerSym(w, mcs, nss);
    if (nd <= 0) return 0;
    const int nes = encoders(w, mcs, nss);
    const int bits = 8 + 8 * len + 6 * nes;
    int n = (bits + nd - 1) / nd;
    if (stbc && (n & 1)) n++;
    return n;
}

// PHY rate in Mbit/s (a 40 us symbol, 36 us with the short guard interval).
double dataRateMbps(int w, int mcs, int nss, bool sgi) {
    const int nd = dataBitsPerSym(w, mcs, nss);
    return nd <= 0 ? 0.0 : nd / (sgi ? 36.0 : 40.0);
}

int symLen(int w, bool sgi) { const int N = nfft(w); return sgi ? N + N / 8 : N + N / 4; }

} // namespace dect2::halow
