#include "dect2/atsc3/atsc3_ofdm.h"
#include "atsc3_tables.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

namespace dect2 {
namespace atsc3 {

namespace {

struct GiDx { int gi, dx; };

// Table H.1.1: five L1-Basic modes for every row.
const GiDx k8k[] = {{192, 16}, {384, 8}, {512, 6}, {768, 4}, {1024, 3}, {1536, 4}, {2048, 3}};
const GiDx k16k[] = {{192, 32}, {384, 16}, {512, 12}, {768, 8}, {1024, 6}, {1536, 4}, {2048, 3}, {2432, 3}, {3072, 4}, {3648, 4}, {4096, 3}};
const GiDx k32k[] = {{192, 32}, {384, 32}, {512, 24}, {768, 16}, {1024, 12}, {1536, 8}, {2048, 6}, {2432, 6}, {3072, 8}, {3072, 3}, {3648, 8}, {3648, 3}, {4096, 3}, {4864, 3}};

int cunit(int fftSize) { return fftSize == 8192 ? 96 : fftSize == 16384 ? 192 : 384; }

} // namespace

bool preambleParams(int ps, PreambleParams& o) {
    if (ps < 0 || ps > 159) return false;
    int mode = ps % 5 + 1, row = ps / 5;
    if (row < 7) { o.fftSize = 8192; o.guard = k8k[row].gi; o.dx = k8k[row].dx; }
    else if (row < 18) { o.fftSize = 16384; o.guard = k16k[row - 7].gi; o.dx = k16k[row - 7].dx; }
    else { o.fftSize = 32768; o.guard = k32k[row - 18].gi; o.dx = k32k[row - 18].dx; }
    o.l1BasicMode = mode;
    return true;
}

int maxCarriers(int fftSize) { return fftSize == 8192 ? 6913 : fftSize == 16384 ? 13825 : 27649; }

int numCarriers(int fftSize, int cred) { return maxCarriers(fftSize) - cred * cunit(fftSize); }

std::vector<int> continualPilots(int fftSize, int cred) {
    std::vector<int> v;
    const int* t = fftSize == 8192 ? kCp8 : fftSize == 16384 ? kCp16 : kCp32;
    int n = fftSize == 8192 ? 48 : fftSize == 16384 ? 96 : 192;
    int shift = cred * cunit(fftSize) / 2, noc = numCarriers(fftSize, cred);
    for (int i = 0; i < n; i++) {
        int rel = t[i] - shift;
        if (rel >= 0 && rel < noc) v.push_back(rel);
    }
    return v;
}

static const std::vector<uint8_t>& referenceCached(int count) {
    static std::mutex mu;
    static std::map<int, std::vector<uint8_t>> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(count);
    if (it == cache.end()) {
        std::vector<uint8_t> r(count);
        int x[13] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1};
        for (int i = 0; i < count; i++) {
            r[i] = x[12];
            int fb = x[8] ^ x[9] ^ x[11] ^ x[12];
            for (int j = 12; j > 0; j--) x[j] = x[j - 1];
            x[0] = fb;
        }
        it = cache.emplace(count, std::move(r)).first;
    }
    return it->second;
}

std::vector<uint8_t> referenceSequence(int count) { return referenceCached(count); }

std::vector<uint8_t> referenceSequenceSlow(int count) {
    std::vector<uint8_t> r(count);
    int x[13] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1};   // x1 .. x13
    for (int i = 0; i < count; i++) {
        r[i] = x[12];
        int fb = x[8] ^ x[9] ^ x[11] ^ x[12];   // 1 + X^9 + X^10 + X^12 + X^13
        for (int j = 12; j > 0; j--) x[j] = x[j - 1];
        x[0] = fb;
    }
    return r;
}

double preamblePilotAmplitude(const PreambleParams& p) {
    double db = 0;
    if (p.fftSize == 8192) db = p.dx == 16 ? 5.3 : p.dx == 8 ? 3.6 : p.dx == 6 ? 2.9 : p.dx == 4 ? 1.8 : 0.9;
    else if (p.fftSize == 16384) db = p.dx == 32 ? 6.8 : p.dx == 16 ? 5.3 : p.dx == 12 ? 4.6 : p.dx == 8 ? 3.6 : p.dx == 6 ? 2.9 : p.dx == 4 ? 2.1 : 1.3;
    else db = p.dx == 32 ? 6.8 : p.dx == 24 ? 6.2 : p.dx == 16 ? 5.3 : p.dx == 12 ? 4.6 : p.dx == 8 ? 4.0 : p.dx == 6 ? 3.2 : 1.3;
    return std::pow(10.0, db / 20.0);
}

double continualPilotAmplitude() { return std::pow(10.0, 8.52 / 20.0); }

namespace {

// Per carrier of a Preamble symbol: 0 = data, 1 = Preamble pilot, 2 = continual pilot
std::vector<uint8_t> carrierKinds(const PreambleParams& p, int cred) {
    int noc = numCarriers(p.fftSize, cred);
    std::vector<uint8_t> kind(noc, 0);
    for (int k = 0; k < noc; k += p.dx) kind[k] = 1;
    for (int k : continualPilots(p.fftSize, cred)) kind[k] = 2;
    return kind;
}

} // namespace

std::vector<int> preambleDataCarriers(const PreambleParams& p, int cred) {
    auto kind = carrierKinds(p, cred);
    std::vector<int> v;
    for (int k = 0; k < (int)kind.size(); k++) if (kind[k] == 0) v.push_back(k);
    return v;
}

int preamblePilotCount(const PreambleParams& p, int cred) {
    auto kind = carrierKinds(p, cred);
    int n = 0;
    for (auto k : kind) n += k != 0;
    return n;
}

static std::vector<int> interleaverUncached(int fftSize, int nData, int l);

std::vector<int> frequencyInterleaverSequence(int fftSize, int nData, int l) {
    // the sequence depends on the symbol only through its parity (wire permutation) and l / 2 (offset): cached, because it is slow to make
    static std::mutex mu;
    static std::map<std::tuple<int, int, int>, std::vector<int>> cache;
    const auto key = std::make_tuple(fftSize, nData, l);
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    std::vector<int> h = interleaverUncached(fftSize, nData, l);
    std::lock_guard<std::mutex> lk(mu);
    if (cache.size() > 600) cache.clear();
    cache[key] = h;
    return h;
}

static std::vector<int> interleaverUncached(int fftSize, int nData, int l) {
    const int nr = fftSize == 8192 ? 13 : fftSize == 16384 ? 14 : 15;   // log2 of Mmax
    const int mmax = 1 << nr;
    // wire permutations (Tables 7.12 to 7.14): the bit R'[pos] becomes bit perm[...] of R, written for R' bit positions from the top
    static const int w8e[12] = {5, 11, 3, 0, 10, 8, 6, 9, 2, 4, 1, 7}, w8o[12] = {8, 10, 7, 6, 0, 5, 2, 1, 3, 9, 4, 11};
    static const int w16e[13] = {8, 4, 3, 2, 0, 11, 1, 5, 12, 10, 6, 7, 9}, w16o[13] = {7, 9, 5, 3, 11, 1, 4, 0, 2, 12, 10, 8, 6};
    static const int w32[14] = {6, 5, 0, 10, 8, 1, 11, 12, 2, 9, 4, 3, 13, 7};
    const int* perm = fftSize == 8192 ? ((l & 1) ? w8o : w8e) : fftSize == 16384 ? ((l & 1) ? w16o : w16e) : w32;
    const int rbits = nr - 1;
    // symbol offset G_k: constant for two symbols
    unsigned g = (1u << nr) - 1;
    for (int k = 0; k < l / 2; k++) {
        unsigned fb;
        if (fftSize == 8192) fb = ((g >> 0) ^ (g >> 1) ^ (g >> 4) ^ (g >> 5) ^ (g >> 9) ^ (g >> 11)) & 1;
        else if (fftSize == 16384) fb = ((g >> 0) ^ (g >> 1) ^ (g >> 2) ^ (g >> 12)) & 1;
        else fb = ((g >> 0) ^ (g >> 1)) & 1;
        g = (g >> 1) | (fb << (nr - 1));
    }
    std::vector<int> h;
    h.reserve(nData);
    unsigned rp = 0;   // R', rbits wide
    for (int i = 0; i < mmax; i++) {
        if (i == 2) rp = 1;
        else if (i > 2) {
            unsigned fb;
            if (fftSize == 8192) fb = ((rp >> 0) ^ (rp >> 1) ^ (rp >> 4) ^ (rp >> 6)) & 1;
            else if (fftSize == 16384) fb = ((rp >> 0) ^ (rp >> 1) ^ (rp >> 4) ^ (rp >> 5) ^ (rp >> 9) ^ (rp >> 11)) & 1;
            else fb = ((rp >> 0) ^ (rp >> 1) ^ (rp >> 2) ^ (rp >> 12)) & 1;
            rp = (rp >> 1) | (fb << (rbits - 1));
        }
        unsigned r = 0;
        for (int j = 0; j < rbits; j++) {   // table column j lists the target bit of R' bit (rbits - 1 - j)
            if ((rp >> (rbits - 1 - j)) & 1) r |= 1u << perm[j];
        }
        unsigned hv = (((unsigned)(i & 1) << (nr - 1)) | r) ^ g;
        if ((int)hv < nData) h.push_back((int)hv);
    }
    return h;
}

namespace {

struct Plan {
    PreambleParams p;
    int cred, noc, nData;
    std::vector<uint8_t> kind;
    std::vector<float> pilotVal;   // reference value for pilot carriers (signed amplitude), 0 for data
};

void makePlan(const PreambleParams& p, int cred, Plan& pl) {
    pl.p = p; pl.cred = cred;
    pl.noc = numCarriers(p.fftSize, cred);
    pl.kind = carrierKinds(p, cred);
    auto r = referenceSequence(pl.noc);
    double ap = preamblePilotAmplitude(p), ac = continualPilotAmplitude();
    pl.pilotVal.assign(pl.noc, 0.f);
    pl.nData = 0;
    for (int k = 0; k < pl.noc; k++) {
        if (pl.kind[k] == 0) { pl.nData++; continue; }
        double a = pl.kind[k] == 1 ? ap : ac;
        pl.pilotVal[k] = (float)(2 * a * (0.5 - r[k]));
    }
}

} // namespace

std::vector<cf32> modulatePreambleSymbol(const PreambleParams& p, int cred, int l, const std::vector<cf32>& cells) {
    Plan pl;
    makePlan(p, cred, pl);
    std::vector<cf32> c(pl.noc, cf32(0, 0));
    auto h = frequencyInterleaverSequence(p.fftSize, pl.nData, l);
    std::vector<int> dk;
    for (int k = 0; k < pl.noc; k++) if (pl.kind[k] == 0) dk.push_back(k);
    std::vector<cf32> inter(pl.nData, cf32(0, 0));
    auto cell = [&](int i) { return i < (int)cells.size() ? cells[i] : cf32(0, 0); };
    if (p.fftSize == 32768 && (l % 2) == 0) { for (int q = 0; q < pl.nData; q++) inter[h[q]] = cell(q); }   // 32K, even symbol: A[H(q)] = X[q]
    else { for (int q = 0; q < pl.nData; q++) inter[q] = cell(h[q]); }                                      // otherwise: A[q] = X[H(q)]
    for (int q = 0; q < pl.nData; q++) c[dk[q]] = inter[q];
    for (int k = 0; k < pl.noc; k++) if (pl.kind[k] != 0) c[k] = cf32(pl.pilotVal[k], 0);
    double power = 0;
    for (auto& v : c) power += std::norm(v);
    float scale = (float)(1.0 / std::sqrt(power));
    const int N = p.fftSize, G = p.guard;
    std::vector<cf32> bins(N, cf32(0, 0));
    int mid = (pl.noc - 1) / 2;
    for (int k = 0; k < pl.noc; k++) bins[((k - mid) % N + N) % N] = c[k] * scale;
    Fft fft(N);
    fft.inverse(bins.data());
    std::vector<cf32> out(G + N);
    for (int i = 0; i < G; i++) out[i] = bins[N - G + i];
    for (int i = 0; i < N; i++) out[G + i] = bins[i];
    return out;
}

// Channel estimate from the pilots of a regular comb (carrier k0 + D m): the impulse response is taken from the comb (white noise, no interpolation
// error), the taps under a noise threshold are dropped, and the response goes back to every carrier. Delays are taken from -N/(4D) to 3N/(4D) samples
// (one comb period, a quarter of it before the first path; the guard interval is 1/16 to 1/4 of N), so an echo up to 0.75 of a comb period is kept.
// The loss it removes: with linear interpolation between noisy pilots every cell carries the pilot noise of two pilots on top of its own (a constant
// 0.5 to 1.5 dB at any SNR for the usual pilot boost).
bool cirDenoiseChannel(int N, int noc, int D, int k0, const std::vector<cf32>& est, const std::vector<uint8_t>& ok, std::vector<cf32>& hch) {
    if (D < 1 || noc < 16 || (int)est.size() < noc) return false;
    const int mid = (noc - 1) / 2;
    std::vector<int> ks;
    std::vector<cf32> hv;
    std::vector<uint8_t> have;
    for (int k = k0; k < noc; k += D) { ks.push_back(k); hv.push_back(ok[k] ? est[k] : cf32(0, 0)); have.push_back(ok[k]); }
    const int M = (int)ks.size();
    if (M < 16) return false;
    int nHave = 0;
    for (auto v : have) nHave += v;
    if (nHave < M * 3 / 4) return false;
    for (int i = 0; i < M; i++) {   // holes (the middle carriers): between the neighbours that exist
        if (have[i]) continue;
        int a = i - 1, b = i + 1;
        while (a >= 0 && !have[a]) a--;
        while (b < M && !have[b]) b++;
        if (a >= 0 && b < M) { const float t = (float)(i - a) / (float)(b - a); hv[i] = hv[a] * (1 - t) + hv[b] * t; }
        else hv[i] = a >= 0 ? hv[a] : hv[b];
    }
    auto bin = [&](int k) { return ((k - mid) % N + N) % N; };
    std::vector<cf32> b(N, cf32(0, 0));
    for (int i = 0; i < M; i++) b[bin(ks[i])] = hv[i];
    Fft fft(N);
    fft.inverse(b.data());
    const int W = std::min(N, (int)std::lround((double)N / D)), pre = W / 4;
    std::vector<float> pw((size_t)W);
    float pmax = 0;
    for (int j = 0; j < W; j++) { pw[(size_t)j] = std::norm(b[((j - pre) % N + N) % N]); pmax = std::max(pmax, pw[(size_t)j]); }
    std::vector<float> srt(pw);
    std::nth_element(srt.begin(), srt.begin() + W / 4, srt.end());
    const float meanNoise = srt[(size_t)(W / 4)] / 0.288f;           // 25th percentile of an exponential distribution
    const float thr = std::max(4.f * meanNoise, 1e-4f * pmax);       // 6 dB over the noise, and never more than 40 dB under the strongest tap
    std::vector<cf32> c(N, cf32(0, 0));
    int kept = 0;
    for (int j = 0; j < W; j++) {
        if (pw[(size_t)j] < thr) continue;
        const int n = ((j - pre) % N + N) % N;
        c[n] = b[n]; kept++;
    }
    if (kept == 0 || kept > W * 3 / 4) return false;
    fft.forward(c.data());
    hch.resize(noc);
    const float g = 1.f / (float)M;
    for (int k = 0; k < noc; k++) hch[k] = c[bin(k)] * g;
    return true;
}

bool demodulatePreambleSymbol(const PreambleParams& p, int cred, int l, const cf32* x, std::vector<cf32>& cells, float* noiseVar) {
    Plan pl;
    makePlan(p, cred, pl);
    const int N = p.fftSize, G = p.guard;
    std::vector<cf32> bins(N);
    for (int i = 0; i < N; i++) bins[i] = x[G + i];
    Fft fft(N);
    fft.forward(bins.data());
    int mid = (pl.noc - 1) / 2;
    std::vector<cf32> y(pl.noc);
    for (int k = 0; k < pl.noc; k++) y[k] = bins[((k - mid) % N + N) % N];
    // channel estimate at the pilots, linear interpolation between them
    std::vector<int> pk;
    std::vector<cf32> ph;
    for (int k = 0; k < pl.noc; k++)
        if (pl.kind[k] != 0) { pk.push_back(k); ph.push_back(y[k] / pl.pilotVal[k]); }
    if (pk.size() < 2) return false;
    std::vector<cf32> hch(pl.noc);
    size_t seg = 0;
    for (int k = 0; k < pl.noc; k++) {
        while (seg + 2 < pk.size() && pk[seg + 1] < k) seg++;
        float t = (float)(k - pk[seg]) / (float)(pk[seg + 1] - pk[seg]);
        hch[k] = ph[seg] * (1 - t) + ph[seg + 1] * t;
    }
    {   // below about 25 dB the pilot noise of the interpolation is the larger part of the loss: response from the pilot comb, noise taps dropped
        double nv0 = 0, hm0 = 0; int nn0 = 0;
        for (size_t i = 0; i + 1 < pk.size(); i++) { nv0 += std::norm(ph[i + 1] - ph[i]) / 2.0; nn0++; }
        for (auto& c : hch) hm0 += std::norm(c);
        hm0 /= pl.noc;
        const double ap0 = preamblePilotAmplitude(p);
        if (nn0 && hm0 > 0 && nv0 / nn0 * ap0 * ap0 / hm0 > 3e-3 && !getenv("ATSC3_NOCIR")) {
            std::vector<cf32> est((size_t)pl.noc);
            std::vector<uint8_t> okp((size_t)pl.noc, 0);
            for (int k = 0; k < pl.noc; k += p.dx) if (pl.kind[k] != 0) { est[(size_t)k] = y[k] / pl.pilotVal[k]; okp[(size_t)k] = 1; }
            std::vector<cf32> hc;
            if (cirDenoiseChannel(N, pl.noc, p.dx, 0, est, okp, hc)) hch = hc;
        }
    }
    double nv = 0; int nn = 0;
    std::vector<cf32> a;   // equalised data cells in carrier order
    for (int k = 0; k < pl.noc; k++)
        if (pl.kind[k] == 0) a.push_back(y[k] / hch[k]);
    // the cells are scaled like the pilots were: the transmitter normalised the whole symbol, so undo that with the pilot reference
    // (the channel estimate above already contains the normalisation, so `a` is on the scale of the transmitted constellation)
    for (size_t i = 0; i + 1 < pk.size(); i++) {   // noise from the difference of neighbouring pilot estimates
        nv += std::norm(ph[i + 1] - ph[i]) / 2.0; nn++;
    }
    int nd = pl.nData;
    auto h = frequencyInterleaverSequence(N, nd, l);
    cells.assign(nd, cf32(0, 0));
    if (N == 32768 && (l % 2) == 0) { for (int q = 0; q < nd; q++) cells[q] = a[h[q]]; }
    else { for (int q = 0; q < nd; q++) cells[h[q]] = a[q]; }
    if (noiseVar) {
        // pilot estimates have noise |N|^2 / |pilot|^2 each; the data cells are noisier by about the pilot-to-data power ratio
        double ap2 = std::pow(preamblePilotAmplitude(p), 2);
        float v = nn ? (float)(nv / nn) : 0.f;
        double hmean = 0;
        for (auto& c : hch) hmean += std::norm(c);
        hmean /= pl.noc;
        *noiseVar = hmean > 0 ? (float)(v * ap2 / hmean) : 0.f;
    }
    return true;
}

} // namespace atsc3
} // namespace dect2
