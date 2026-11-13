// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// PI4 (PA0NHC, 4-FSK, a JT4 / WSPR relative for 10 GHz and microwave beacons): 8 characters of "0-9 A-Z space /" packed as a base 38 number into 42 bits, 31
// zero tail bits, the WSPR rate 1/2 K = 32 convolutional code (146 bits), the 8 bit bit-reversal interleaver, and 146 symbols of 2048 samples at 11025 Hz
// (185.76 ms). The tone number of a symbol is the sync bit (a fixed 146 bit vector) plus 2 times the coded bit; tones are 5.383 Hz apart.
// Layout read from MSHV (LZ2HV, GPL-3.0). The receiver finds the frequency and the start by correlating with the sync vector, then a Fano decoder reads the bits.
// There is no checksum in PI4: a decode is accepted only when re-encoding it agrees with at least 70 % of the received symbols.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_int.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {
namespace hamsci {
using namespace detail;

namespace {
const int kSync[146] = {
    0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1,
    1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0,
    0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0,
    1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1,
    0, 1, 1, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1};
const char kSet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ /";   // 38
constexpr uint32_t kP1 = 0xF2D05351u, kP2 = 0xE4613C47u;
constexpr double kDf = 11025.0 / 2048.0;

int rev8(int v) { int r = 0; for (int i = 0; i < 8; i++) if (v & (1 << i)) r |= 1 << (7 - i); return r; }
const std::vector<int>& j0() {
    static std::vector<int> t = [] { std::vector<int> v; for (int i = 0; i < 256; i++) { const int n = rev8(i); if (n <= 145) v.push_back(n); } return v; }();
    return t;
}

void encode(const uint8_t bits42[6], uint8_t coded[146]) {
    uint32_t reg = 0;
    for (int i = 0; i < 73; i++) {
        int bit = 0;
        if (i < 42) {   // MSB first: byte 0 holds bits 41..34, ..., byte 4 holds bits 9..2, byte 5 the top two bits are bits 1..0
            const int pos = 41 - i;   // value bit number
            const int byte = pos >= 2 ? (41 - pos) / 8 : 5;
            const int sh = pos >= 2 ? 7 - ((41 - pos) % 8) : (pos == 1 ? 7 : 6);
            bit = (bits42[byte] >> sh) & 1;
        }
        reg = (reg << 1) | (uint32_t)bit;
        coded[2 * i] = (uint8_t)parity32(reg & kP1);
        coded[2 * i + 1] = (uint8_t)parity32(reg & kP2);
    }
}
void symbolsFor(const uint8_t bits42[6], int tone[146]) {
    uint8_t coded[146], tx[146];
    encode(bits42, coded);
    const auto& J = j0();
    for (int i = 0; i < 146; i++) tx[J[(size_t)i]] = coded[i];
    for (int j = 0; j < 146; j++) tone[j] = kSync[j] + 2 * tx[j];
}
}  // namespace

bool pi4Pack(const std::string& msg, uint8_t b[6]) {
    std::string m;
    for (char c : msg) m += (char)std::toupper((unsigned char)c);
    if (m.size() > 8) return false;
    while (m.size() < 8) m += ' ';
    uint64_t v = 0;
    for (char c : m) {
        int k = -1;
        for (int i = 0; i < 38; i++) if (kSet[i] == c) k = i;
        if (k < 0) return false;
        v = v * 38 + (uint64_t)k;
    }
    // data_val = b0 << 34 | b1 << 26 | b2 << 18 | b3 << 10 | b4 << 2 | b5 >> 6
    b[0] = (uint8_t)(v >> 34); b[1] = (uint8_t)(v >> 26); b[2] = (uint8_t)(v >> 18); b[3] = (uint8_t)(v >> 10); b[4] = (uint8_t)(v >> 2); b[5] = (uint8_t)((v & 3) << 6);
    return true;
}
std::string pi4Unpack(const uint8_t b[6]) {
    uint64_t v = ((uint64_t)b[0] << 34) | ((uint64_t)b[1] << 26) | ((uint64_t)b[2] << 18) | ((uint64_t)b[3] << 10) | ((uint64_t)b[4] << 2) | ((uint64_t)b[5] >> 6);
    std::string s(8, ' ');
    for (int i = 7; i >= 0; i--) { s[(size_t)i] = kSet[v % 38]; v /= 38; }
    return s;
}

std::vector<float> pi4Generate(const std::string& msg8, double fs, double f0Hz, double amp, double startSec, double totalSec) {
    uint8_t b[6];
    if (!pi4Pack(msg8, b)) return {};
    int tone[146];
    symbolsFor(b, tone);
    const size_t n = (size_t)(totalSec * fs);
    std::vector<float> y(n, 0.f);
    const double nsps = kPi4SymbolSec * fs;
    double ph = 0;
    const size_t first = (size_t)(startSec * fs);
    for (int j = 0; j < 146; j++) {
        const size_t a = first + (size_t)std::llround(j * nsps), e = std::min(n, first + (size_t)std::llround((j + 1) * nsps));
        const double w = 2 * kPi * (f0Hz + tone[j] * kDf) / fs;
        for (size_t i = a; i < e; i++) { ph += w; y[i] = (float)(amp * std::sin(ph)); }
    }
    return y;
}

bool pi4Decode(const float* x, size_t n, double fs, Pi4Result& out, double fLoHz, double fHiHz) {
    const double nsps = kPi4SymbolSec * fs;
    const double hop = nsps / 4.0;
    const size_t wlen = (size_t)std::llround(nsps);
    if ((double)n < 146.0 * nsps + wlen) return false;
    const size_t N = nextPow2(wlen) * 2;
    const double binHz = fs / (double)N;
    const size_t nh = (size_t)((double)(n - wlen) / hop) + 1;
    int toneOff8[8];   // column offset of the tones -2 .. 5 from the column of tone -2
    for (int m = 0; m < 8; m++) toneOff8[m] = (int)std::lround(m * kDf / binHz);
    const int b0lo = std::max(toneOff8[2] + 1, (int)std::floor(fLoHz / binHz)), b0hi = (int)std::ceil((fHiHz) / binHz);
    const int colStart = b0lo - toneOff8[2];
    const int nb = b0hi - b0lo + 1 + toneOff8[7] + 1;
    std::vector<float> E(nh * (size_t)nb);
    std::vector<cd> a(N);
    std::vector<double> win(wlen);
    for (size_t i = 0; i < wlen; i++) win[i] = 1.0;   // rectangular: the tones are orthogonal over one symbol (spacing = 1 / symbol time)
    for (size_t h = 0; h < nh; h++) {
        const size_t st = (size_t)std::floor((double)h * hop);
        for (size_t i = 0; i < N; i++) a[i] = i < wlen ? cd((double)x[st + i] * win[i], 0) : cd(0, 0);
        fft(a);
        for (int b = 0; b < nb; b++) E[h * (size_t)nb + (size_t)b] = (float)std::norm(a[(size_t)(colStart + b)]);
    }
    int toneOff[4];
    for (int k = 0; k < 4; k++) toneOff[k] = toneOff8[k + 2];
    const size_t hMax = nh - (size_t)(4 * 146) - 1;
    if (nh < (size_t)(4 * 146 + 2)) return false;
    double best = -1;
    int bestB = 0;
    size_t bestH = 0;
    for (int b = 0; b <= b0hi - b0lo; b++)
        for (size_t h = 0; h <= hMax; h++) {
            double A = 0, B = 0, T = 0;
            for (int j = 0; j < 146; j++) {
                const float* r = &E[(h + (size_t)(4 * j)) * (size_t)nb + (size_t)b];
                const int s = kSync[j];
                A += r[toneOff[s]] + r[toneOff[s + 2]];
                B += r[toneOff[1 - s]] + r[toneOff[3 - s]];
                T += r[toneOff8[0]] + r[toneOff8[1]] + r[toneOff8[6]] + r[toneOff8[7]];   // the two tones on each side of the four
            }
            // tones outside the four count against the lock: a lock two tones too high or low must not look as good as the right one
            const double c = (A - B) / (A + B + T + 1e-30);
            if (c > best) { best = c; bestB = b; bestH = h; }
        }
    // noise level of the band for an S/N figure: the median energy of the spectrogram bins near the signal
    std::vector<float> med;
    for (size_t h = 0; h < nh; h += 4) for (int b = 0; b < nb; b += 3) med.push_back(E[h * (size_t)nb + (size_t)b]);
    std::nth_element(med.begin(), med.begin() + (long)med.size() / 2, med.end());
    const double noiseE = med[med.size() / 2] / 0.69;   // median of an exponential distribution = 0.69 mean
    if (best < 0.2) return false;
    // soft symbols at the best lock (and the two neighbouring time steps: take the one whose hard decisions re-encode best)
    bool done = false;
    double bestMatch = 0;
    std::string text;
    for (int dh = -1; dh <= 1 && !done; dh++) {
        const long hh = (long)bestH + dh;
        if (hh < 0 || (size_t)hh > hMax) continue;
        double ss = 0;
        float d[146];
        for (int j = 0; j < 146; j++) {
            const float* r = &E[((size_t)hh + (size_t)(4 * j)) * (size_t)nb + (size_t)bestB];
            const int s = kSync[j];
            const double lo = std::sqrt((double)r[toneOff[s]]), hi = std::sqrt((double)r[toneOff[s + 2]]);
            d[j] = (float)(hi - lo);
            ss += d[j] * d[j];
        }
        const double rms = std::sqrt(ss / 146.0) + 1e-12;
        const auto& J = j0();
        for (double scale : {3.0, 6.0, 1.5}) {
            float coded[146];
            for (int i = 0; i < 146; i++) coded[i] = (float)(scale * d[J[(size_t)i]] / rms);
            uint8_t bits[42];
            if (!fanoDecode(coded, 73, 42, bits, 400000)) continue;
            // pack the bits and check by re-encoding
            uint8_t pk[6] = {};
            for (int i = 0; i < 42; i++) {
                const int pos = 41 - i;
                const int byte = pos >= 2 ? (41 - pos) / 8 : 5;
                const int sh = pos >= 2 ? 7 - ((41 - pos) % 8) : (pos == 1 ? 7 : 6);
                if (bits[i]) pk[byte] |= (uint8_t)(1 << sh);
            }
            int tone[146];
            symbolsFor(pk, tone);
            int match = 0;
            for (int j = 0; j < 146; j++) {
                const int txBit = tone[j] - kSync[j];
                const int rxBit = d[j] > 0 ? 1 : 0;
                if (txBit / 2 == rxBit) match++;
            }
            const double frac = match / 146.0;
            if (frac >= 0.70 && frac > bestMatch) {
                bestMatch = frac;
                text = pi4Unpack(pk);
                done = true;
            }
            if (done) break;
        }
    }
    if (!done) return false;
    // trim trailing spaces
    while (!text.empty() && text.back() == ' ') text.pop_back();
    out.text = text;
    out.freqHz = (b0lo + bestB) * binHz;   // tone 0
    out.startSec = (double)bestH * hop / fs;
    out.sync = best;
    // signal energy per symbol against the noise: A / 146 is the energy of the chosen tone pair... report the lock quality as a rough SNR
    double sig = 0;
    for (int j = 0; j < 146; j++) {
        const float* r = &E[(bestH + (size_t)(4 * j)) * (size_t)nb + (size_t)bestB];
        sig += std::max(r[toneOff[kSync[j]]], r[toneOff[kSync[j] + 2]]);
    }
    sig /= 146.0;
    // noise in the 2500 Hz reference bandwidth: energy per bin of width binHz -> per Hz, times 2500
    const double snr = (sig - noiseE) / std::max(1e-30, noiseE) * (binHz / 2500.0) * 1.0;
    out.snrDb = 10 * std::log10(std::max(1e-6, snr));
    return true;
}

// ------------------------------------------------------------------------------------------------------------------ streaming
Pi4Decoder::Pi4Decoder(double fs) : fs_(fs) { reset(); }
void Pi4Decoder::reset() { buf_.clear(); total_ = 0; level_ = -120; last_.clear(); out_.clear(); }
std::vector<Pi4Result> Pi4Decoder::take() { std::vector<Pi4Result> r; r.swap(out_); return r; }

void Pi4Decoder::feed(const float* x, size_t n) {
    buf_.insert(buf_.end(), x, x + n);
    const uint64_t before = total_;
    total_ += n;
    double e = 0;
    for (size_t i = 0; i < n; i++) e += (double)x[i] * x[i];
    level_ = 10 * std::log10(std::max(1e-12, e / (double)std::max<size_t>(1, n)));
    const size_t maxN = (size_t)(36 * fs_);
    if (buf_.size() > maxN) buf_.erase(buf_.begin(), buf_.begin() + (long)(buf_.size() - maxN));
    const uint64_t step = (uint64_t)(6 * fs_);
    if (before / step == total_ / step || buf_.size() < (size_t)(30 * fs_)) return;
    Pi4Result r;
    if (pi4Decode(buf_.data(), buf_.size(), fs_, r)) {
        r.startSec += (double)(total_ - buf_.size()) / fs_;
        if (r.text != last_) { out_.push_back(r); last_ = r.text; }
    }
}

}  // namespace hamsci
}  // namespace dect2
