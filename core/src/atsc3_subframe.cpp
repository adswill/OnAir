#include "dect2/atsc3_subframe.h"
#include "atsc3_tables.h"
#include "dect2/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace atsc3 {

namespace {

const int kDx[16] = {3, 3, 4, 4, 6, 6, 8, 8, 12, 12, 16, 16, 24, 24, 32, 32};
const int kDy[16] = {2, 4, 2, 4, 2, 4, 2, 4, 2, 4, 2, 4, 2, 4, 2, 4};

// Table 9.14: pilot power in dB for boost 0..4, per pattern
const double kBoostDb[16][5] = {
    {0, 0, 1.4, 2.2, 2.9}, {0, 1.4, 2.9, 3.8, 4.4}, {0, 0.6, 2.1, 3.0, 3.6}, {0, 2.1, 3.6, 4.4, 5.1},
    {0, 1.6, 3.1, 4.0, 4.6}, {0, 3.0, 4.5, 5.4, 6.0}, {0, 2.2, 3.8, 4.6, 5.3}, {0, 3.6, 5.1, 6.0, 6.6},
    {0, 3.2, 4.7, 5.6, 6.2}, {0, 4.5, 6.0, 6.9, 7.5}, {0, 3.8, 5.3, 6.2, 6.8}, {0, 5.2, 6.7, 7.6, 8.2},
    {0, 4.7, 6.2, 7.1, 7.7}, {0, 6.1, 7.6, 8.5, 9.1}, {0, 5.4, 6.9, 7.7, 8.4}, {0, 6.7, 8.2, 9.1, 9.7}};

// Table D.1.4: additional continual pilots that may be scattered pilot positions; `paren` entries are not used for odd Cred_coeff
struct Acp { int idx; bool paren; };
struct AcpSet { std::vector<Acp> k8, k16, k32; };

const AcpSet& acpFor(int pattern) {
    static const AcpSet sets[16] = {
        /* SP3_2  */ {{{1731, false}}, {{3471, false}}, {{6939, false}}},
        /* SP3_4  */ {{{1731, false}, {2886, false}, {5733, false}}, {{3471, false}, {5778, false}, {11469, false}}, {}},
        /* SP4_2  */ {{{1732, false}}, {{3460, false}}, {}},
        /* SP4_4  */ {{{1732, false}, {2888, false}, {5724, false}}, {{3460, false}, {5768, false}, {11452, false}}, {}},
        /* SP6_2  */ {{{1734, false}}, {{3462, false}}, {{6942, false}}},
        /* SP6_4  */ {{{1734, false}, {2892, false}, {5730, false}}, {{3462, false}, {5772, false}, {11466, false}}, {}},
        /* SP8_2  */ {{{1736, false}}, {{3464, false}}, {{6920, false}}},
        /* SP8_4  */ {{{1736, false}, {2896, false}, {5720, false}}, {{3464, false}, {5776, false}, {11448, false}}, {}},
        /* SP12_2 */ {{{1740, false}}, {{3468, false}}, {{6924, false}}},
        /* SP12_4 */ {{{1740, false}, {2904, false}, {5748, false}}, {{3468, false}, {5784, false}, {11460, false}}, {}},
        /* SP16_2 */ {{{1744, false}}, {{3472, false}}, {{6928, false}}},
        /* SP16_4 */ {{{1744, false}, {2912, true}, {5744, true}}, {{3472, false}, {5792, false}, {11440, false}}, {}},
        /* SP24_2 */ {{}, {{3480, false}}, {{6936, false}}},
        /* SP24_4 */ {{}, {{3480, false}, {5808, false}, {11496, false}}, {}},
        /* SP32_2 */ {{{1696, true}}, {{3488, false}}, {{6944, false}}},
        /* SP32_4 */ {{}, {{3488, false}, {5824, true}, {11488, true}}, {}},
    };
    return sets[pattern];
}

std::vector<int> additionalCps(const SubframeParams& s) {
    std::vector<int> v;
    const AcpSet& a = acpFor(s.spPattern);
    const auto& list = s.fftSize == 8192 ? a.k8 : s.fftSize == 16384 ? a.k16 : a.k32;
    for (auto& e : list) if (!(e.paren && (s.cred & 1))) v.push_back(e.idx);
    if (s.fftSize == 8192 && s.spPattern == 15) {   // Table D.1.5, 8K with SP32_4
        static const int c0[] = {1696, 2880, 5728}, c2[] = {1696, 2880}, c3[] = {1696, 2880, 5728}, c4[] = {1696, 2880, 5728};
        if (s.cred == 0) v.assign(c0, c0 + 3);
        else if (s.cred == 1) v.clear();
        else if (s.cred == 2) v.assign(c2, c2 + 2);
        else if (s.cred == 3) v.assign(c3, c3 + 3);
        else v.assign(c4, c4 + 3);
    }
    return v;
}

int cunitOf(int fft) { return fft == 8192 ? 96 : fft == 16384 ? 192 : 384; }

} // namespace

int spDx(int p) { return kDx[p & 15]; }
int spDy(int p) { return kDy[p & 15]; }

double spAmplitude(const SubframeParams& s) { return std::pow(10.0, kBoostDb[s.spPattern & 15][std::min(std::max(s.spBoost, 0), 4)] / 20.0); }

int guardFromCode(int c) {
    static const int g[16] = {0, 192, 384, 512, 768, 1024, 1536, 2048, 2432, 3072, 3648, 4096, 4864, 0, 0, 0};
    return g[c & 15];
}

int fftFromCode(int c) { return c == 0 ? 8192 : c == 1 ? 16384 : c == 2 ? 32768 : 0; }

bool isBoundarySymbol(const SubframeParams& s, int l) { return (l == 0 && s.sbsFirst) || (l == s.numSymbols - 1 && s.sbsLast); }

std::vector<uint8_t> subframeCarrierKinds(const SubframeParams& s, int l) {
    const int noc = numCarriers(s.fftSize, s.cred);
    std::vector<uint8_t> kind(noc, 0);
    const int dx = spDx(s.spPattern), dy = spDy(s.spPattern);
    if (isBoundarySymbol(s, l)) {
        for (int k = 0; k < noc; k += dx) kind[k] = 1;
    } else {
        const int ph = dx * (l % dy), per = dx * dy;
        for (int k = 0; k < noc; k++) if (k % per == ph) kind[k] = 1;
    }
    for (int k : continualPilots(s.fftSize, s.cred)) kind[k] = 2;
    if (!isBoundarySymbol(s, l))
        for (int k : additionalCps(s)) if (k < noc && kind[k] == 0) kind[k] = 3;
    kind[0] = 4;
    kind[noc - 1] = 4;
    return kind;
}

int subframeDataCells(const SubframeParams& s, int l) {
    auto k = subframeCarrierKinds(s, l);
    int n = 0;
    for (auto v : k) n += v == 0;
    return n;
}

int subframeActiveCells(const SubframeParams& s, int l) {
    int n = subframeDataCells(s, l);
    return isBoundarySymbol(s, l) ? n - s.sbsNullCells : n;
}

std::vector<cf32> modulateSubframeSymbol(const SubframeParams& s, int l, const std::vector<cf32>& cells) {
    const int noc = numCarriers(s.fftSize, s.cred);
    auto kind = subframeCarrierKinds(s, l);
    auto r = referenceSequence(noc);
    const double asp = spAmplitude(s), acp = continualPilotAmplitude();
    std::vector<int> dk;
    for (int k = 0; k < noc; k++) if (kind[k] == 0) dk.push_back(k);
    const int nd = (int)dk.size();
    // frequency interleaver on all data cells of the symbol
    std::vector<cf32> inter(nd, cf32(0, 0));
    auto cell = [&](int i) { return i < (int)cells.size() ? cells[i] : cf32(0, 0); };
    if (s.freqInterleaver) {
        const int fl = l + s.fiOffset;
        auto h = frequencyInterleaverSequence(s.fftSize, nd, fl);
        if (s.fftSize == 32768 && (fl % 2) == 0) { for (int q = 0; q < nd; q++) inter[h[q]] = cell(q); }
        else { for (int q = 0; q < nd; q++) inter[q] = cell(h[q]); }
    } else {
        for (int q = 0; q < nd; q++) inter[q] = cell(q);
    }
    std::vector<cf32> c(noc, cf32(0, 0));
    for (int q = 0; q < nd; q++) c[dk[q]] = inter[q];
    for (int k = 0; k < noc; k++) {
        if (kind[k] == 0) continue;
        double a = kind[k] == 2 ? acp : asp;
        c[k] = cf32((float)(2 * a * (0.5 - r[k])), 0);
    }
    double power = 0;
    for (auto& v : c) power += std::norm(v);
    const float scale = (float)(1.0 / std::sqrt(power));
    const int N = s.fftSize, G = s.guard;
    std::vector<cf32> bins(N, cf32(0, 0));
    const int mid = (noc - 1) / 2;
    for (int k = 0; k < noc; k++) bins[((k - mid) % N + N) % N] = c[k] * scale;
    Fft fft(N);
    fft.inverse(bins.data());
    std::vector<cf32> out(G + N);
    for (int i = 0; i < G; i++) out[i] = bins[N - G + i];
    for (int i = 0; i < N; i++) out[G + i] = bins[i];
    return out;
}

bool demodulateSubframeSymbol(const SubframeParams& s, int l, const cf32* x, std::vector<cf32>& cells, float* noiseVar) {
    const int noc = numCarriers(s.fftSize, s.cred);
    auto kind = subframeCarrierKinds(s, l);
    auto r = referenceSequence(noc);
    const double asp = spAmplitude(s), acp = continualPilotAmplitude();
    const int N = s.fftSize, G = s.guard;
    std::vector<cf32> bins(N);
    for (int i = 0; i < N; i++) bins[i] = x[G + i];
    Fft fft(N);
    fft.forward(bins.data());
    const int mid = (noc - 1) / 2;
    std::vector<cf32> y(noc);
    for (int k = 0; k < noc; k++) y[k] = bins[((k - mid) % N + N) % N];
    std::vector<int> pk;
    std::vector<cf32> ph;
    for (int k = 0; k < noc; k++) {
        if (kind[k] == 0) continue;
        double a = kind[k] == 2 ? acp : asp;
        pk.push_back(k);
        ph.push_back(y[k] / (float)(2 * a * (0.5 - r[k])));
    }
    if (pk.size() < 2) return false;
    std::vector<cf32> hch(noc);
    size_t seg = 0;
    for (int k = 0; k < noc; k++) {
        while (seg + 2 < pk.size() && pk[seg + 1] < k) seg++;
        float t = (float)(k - pk[seg]) / (float)(pk[seg + 1] - pk[seg]);
        hch[k] = ph[seg] * (1 - t) + ph[seg + 1] * t;
    }
    std::vector<cf32> a;
    for (int k = 0; k < noc; k++) if (kind[k] == 0) a.push_back(y[k] / hch[k]);
    const int nd = (int)a.size();
    cells.assign(nd, cf32(0, 0));
    if (s.freqInterleaver) {
        const int fl = l + s.fiOffset;
        auto h = frequencyInterleaverSequence(s.fftSize, nd, fl);
        if (s.fftSize == 32768 && (fl % 2) == 0) { for (int q = 0; q < nd; q++) cells[q] = a[h[q]]; }
        else { for (int q = 0; q < nd; q++) cells[h[q]] = a[q]; }
    } else cells = a;
    if (noiseVar) {
        double nv = 0; int nn = 0;
        for (size_t i = 0; i + 1 < pk.size(); i++) if (pk[i + 1] - pk[i] <= 2 * spDx(s.spPattern)) { nv += std::norm(ph[i + 1] - ph[i]) / 2.0; nn++; }
        double hm = 0;
        for (auto& c : hch) hm += std::norm(c);
        hm /= noc;
        *noiseVar = (nn && hm > 0) ? (float)(nv / nn * asp * asp / hm) : 0.f;
    }
    return true;
}

} // namespace atsc3
} // namespace dect2
