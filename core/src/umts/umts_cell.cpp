// UMTS cell search: see umts_cell.h. Code tables: 3GPP TS 25.213 clauses 5.1.2 (scrambling), 5.2.3 (synchronisation codes) and table 4.
#include "dect2/umts/umts_cell.h"
#include "dect2/umts/umts_bch.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <random>
#include <vector>

namespace dect2 {
namespace {
using namespace umts;
constexpr double kPi = 3.14159265358979323846;
const int kSeq[64][15] = {
#include "umts_ssc.inc"
};
// the Golay pieces: psc = (1+j) a..., ssc built from z and a Hadamard row
const int a[16] = {1, 1, 1, 1, 1, 1, -1, -1, 1, -1, 1, -1, 1, -1, -1, 1};
const int zSeq[16] = {1, 1, 1, -1, 1, 1, -1, -1, 1, -1, 1, -1, -1, -1, -1, -1};
const int pscSeq[16] = {1, 1, 1, -1, -1, 1, -1, -1, 1, 1, 1, -1, 1, -1, 1, 1};

const std::vector<int>& psc() { static const std::vector<int> v = [] { std::vector<int> o(256); for (int i = 0; i < 16; i++) for (int k = 0; k < 16; k++) o[(size_t)(i * 16 + k)] = pscSeq[i] * a[k]; return o; }(); return v; }
int bitParity(int v) { int p = 0; while (v) { p ^= v & 1; v >>= 1; } return p; }
const std::vector<std::vector<int>>& ssc() {
    static const std::vector<std::vector<int>> all = [] {
        int b[16]; for (int i = 0; i < 16; i++) b[i] = i < 8 ? a[i] : -a[i];
        std::vector<std::vector<int>> v(16, std::vector<int>(256));
        for (int k = 0; k < 16; k++) {
            const int m = 16 * k;
            for (int i = 0; i < 256; i++) {
                const int h = bitParity(i & m) ? -1 : 1;
                v[(size_t)k][(size_t)i] = h * zSeq[i >> 4] * b[i & 15];
            }
        }
        return v;
    }();
    return all;
}
// the downlink scrambling code n, 38400 chips (Gold, x18+x7+1 and x18+x10+x7+x5+1). I = z_n(i), Q = z_n(i + 131072 mod 262143)
struct Gold {
    std::vector<uint8_t> x, y;
    Gold() {
        const int L = 262143 + 131072 + 38400 + 1;
        x.resize((size_t)L); y.resize((size_t)L);
        x[0] = 1; for (int i = 1; i < 18; i++) x[(size_t)i] = 0;
        for (int i = 0; i < 18; i++) y[(size_t)i] = 1;
        for (int i = 0; i + 18 < L; i++) { x[(size_t)i + 18] = x[(size_t)i + 7] ^ x[(size_t)i]; y[(size_t)i + 18] = y[(size_t)i + 10] ^ y[(size_t)i + 7] ^ y[(size_t)i + 5] ^ y[(size_t)i]; }
    }
};
const Gold& gold() { static const Gold g; return g; }
void scramble(int n, std::vector<cf32>& out) {
    const Gold& g = gold();
    const int N = 262143;
    out.resize(38400);
    for (int i = 0; i < 38400; i++) {
        const int zi = g.x[(size_t)((i + n) % N)] ^ g.y[(size_t)i];
        const int ii = (i + 131072) % N;
        const int zq = g.x[(size_t)((ii + n) % N)] ^ g.y[(size_t)ii];
        out[(size_t)i] = cf32(zi ? -1.f : 1.f, zq ? -1.f : 1.f);
    }
}
}  // namespace

std::vector<UmtsCell> umtsCellSearch(const cf32* x2, size_t n, double maxCfoHz, int maxCells, double* scanMs) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<UmtsCell> found;
    // chip-rate samples (every second sample) and their power
    const size_t N = n / 2;
    if (N < (size_t)kChipsPerFrame * 2) return found;
    std::vector<cf32> c(N);
    double ptot = 0;
    for (size_t i = 0; i < N; i++) { c[i] = x2[2 * i]; ptot += std::norm(c[i]); }
    ptot /= (double)N;
    if (ptot <= 0) return found;
    const std::vector<int>& p = psc();
    const int bins = std::max(1, (int)std::ceil(maxCfoHz / 2500.0));
    std::vector<double> bestByPos;
    struct Pk { int pos; double m; double cfo; };
    std::vector<Pk> peaks;
    std::vector<double> slot((size_t)kChipsPerSlot * 4);
    // a correlation of 256 chips is split into 16 sub-sums of 16 chips, so each frequency bin only re-sums 16 values; per position the best bin is kept
    for (int phase = 0; phase < 2; phase++) {          // (half-chip timing: this pass uses the other sample of each pair)
        std::vector<cf32> ch(N);
        for (size_t i = 0; i < N; i++) ch[i] = x2[2 * i + (size_t)phase * 1];
        std::vector<cf32> sub(16);
        std::vector<double> pwr((size_t)kChipsPerSlot, 0.0);
        std::vector<int> cnt((size_t)kChipsPerSlot, 0);
        std::vector<double> frq((size_t)kChipsPerSlot, 0.0);
        const size_t nb = (size_t)(2 * bins + 1);
        std::vector<double> binP((size_t)kChipsPerSlot * nb, 0.0);
        const size_t lim = N - 256;
        for (size_t s = 0; s + 256 <= lim; s += 1) {
            // only every 16th position needs a fresh partial sum; here simple form with a fixed budget of blocks
            if (s % kChipsPerSlot == 0) {}
            for (int b = 0; b < 16; b++) { cf32 acc(0, 0); for (int k = 0; k < 16; k++) acc += ch[s + (size_t)b * 16 + (size_t)k] * (float)p[(size_t)b * 16 + (size_t)k]; sub[(size_t)b] = acc; }
            for (int d = -bins; d <= bins; d++) {
                const double hz = d * 2500.0;
                const double w = 2 * kPi * hz * 16 / kChipRate;
                cf32 acc(0, 0);
                for (int b = 0; b < 16; b++) { const cf32 ph((float)std::cos(w * b), -(float)std::sin(w * b)); acc += sub[(size_t)b] * ph; }
                binP[(size_t)(s % kChipsPerSlot) * nb + (size_t)(d + bins)] += std::norm(acc);
            }
            cnt[s % kChipsPerSlot]++;
        }
        for (int i = 0; i < kChipsPerSlot; i++) { double bm = -1; int bd = 0; for (int d = 0; d < (int)nb; d++) if (binP[(size_t)i * nb + (size_t)d] > bm) { bm = binP[(size_t)i * nb + (size_t)d]; bd = d; } pwr[(size_t)i] = bm; frq[(size_t)i] = (bd - bins) * 2500.0; }
        double floor = 0; for (int i = 0; i < kChipsPerSlot; i++) { if (cnt[(size_t)i]) pwr[(size_t)i] /= cnt[(size_t)i]; floor += pwr[(size_t)i]; }
        floor /= kChipsPerSlot;
        std::vector<int> order(kChipsPerSlot); std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a2, int b2) { return pwr[(size_t)a2] > pwr[(size_t)b2]; });
        for (int k = 0; k < std::min(kChipsPerSlot, 40); k++) {
            const int o = order[(size_t)k];
            if (pwr[(size_t)o] < floor * 2.5) break;
            bool near = false;
            for (auto& q : peaks) { const int d = std::abs(q.pos - o); if (std::min(d, kChipsPerSlot - d) < 4) near = true; }
            if (near) continue;
            peaks.push_back({o, pwr[(size_t)o], frq[(size_t)o]});
        }
        if (phase == 0 && !peaks.empty()) break;       // the first pass found something: its timing is good enough
    }
    std::sort(peaks.begin(), peaks.end(), [](const Pk& a2, const Pk& b2) { return a2.m > b2.m; });
    const std::vector<std::vector<int>>& S = ssc();
    for (const Pk& pk : peaks) {
        if ((int)found.size() >= maxCells) break;
        // the secondary code of each slot of the block
        const int slots = std::min((int)((N - (size_t)pk.pos) / kChipsPerSlot) - 1, 15 * 6);
        std::vector<std::vector<double>> ms((size_t)slots, std::vector<double>(16, 0.0));
        const cf32 cosc = 1.f;
        (void)cosc;
        for (int sl = 0; sl < slots; sl++) {
            const size_t base = (size_t)pk.pos + (size_t)sl * kChipsPerSlot;
            const double phs = 2 * kPi * pk.cfo * (double)base / kChipRate;
            for (int k = 0; k < 16; k++) {
                cf32 acc(0, 0);
                for (int i = 0; i < 256; i++) { const double w = phs + 2 * kPi * pk.cfo * i / kChipRate; acc += c[base + (size_t)i] * cf32((float)std::cos(w), -(float)std::sin(w)) * (float)S[(size_t)k][(size_t)i]; }
                ms[(size_t)sl][(size_t)k] = std::norm(acc);
            }
        }
        // table 4: the group and the frame position
        double best = -1; int bg = -1, bo = 0;
        for (int g = 0; g < 64; g++) for (int o = 0; o < 15; o++) {
            double m = 0;
            for (int sl = 0; sl < slots; sl++) m += ms[(size_t)sl][(size_t)kSeq[g][(sl + o) % 15]];
            if (m > best) { best = m; bg = g; bo = o; }
        }
        if (bg < 0) continue;
        // the frame starts where slot number 0 is: the position of the first peak's slot with number (bo)
        const int startSlot = (15 - bo) % 15;
        const size_t frame0 = (size_t)pk.pos + (size_t)startSlot * kChipsPerSlot;
        if (frame0 + 38400 > N) continue;
        // the CPICH on the eight codes of the group: symbol (1+j), spreading all ones, over one frame
        double bestC = -1, sec = 0; int bcode = -1; double bCfo = pk.cfo;
        double perCode[8] = {};
        static thread_local std::vector<cf32> code;
        for (int k = 0; k < 8; k++) {
            scramble(16 * (bg * 8 + k), code);
            for (int fb = -3; fb <= 3; fb++) {
                const double cfo = pk.cfo + fb * 400.0;
                double m = 0;
                for (int sl = 0; sl < 15; sl++) {       // coherent over a slot, added in power over the frame: a few hundred Hz of frequency error are harmless
                    cf32 acc(0, 0);
                    for (int i = 0; i < 2560; i++) { const size_t ci = (size_t)sl * 2560 + (size_t)i; const double w = 2 * kPi * cfo * ((double)frame0 + (double)ci) / kChipRate; acc += c[frame0 + ci] * std::conj(code[ci]) * cf32((float)std::cos(w), -(float)std::sin(w)); }
                    m += std::norm(acc);
                }
                m /= 2560.0 * 2560.0 * 15;
                perCode[k] = std::max(perCode[k], m);
                if (m > bestC) { bestC = m; bcode = k; bCfo = cfo; }
            }
        }
        for (int k = 0; k < 8; k++) if (k != bcode) sec = std::max(sec, perCode[k]);
        if (bcode < 0 || bestC < 2.5 * sec) continue;
        // the CPICH symbol is (1+j)/sqrt-less: amplitude in c is about that of a unit-power chip, so |acc/38400|^2 is its power times two (1+j squared magnitude 2 at the code's unit components)
        const double cpichPow = bestC / 2.0;
        UmtsCell cell;
        cell.scramblingCode = bg * 8 + bcode;
        cell.group = bg;
        cell.frameOffsetChips = (int)(frame0 % 38400);
        cell.cfoHz = bCfo;
        cell.ecIoDb = 10 * std::log10(std::max(1e-12, cpichPow) / ptot);
        cell.rscpDbfs = 10 * std::log10(std::max(1e-12, cpichPow));
        cell.slotPeak = pk.pos;
        // the broadcast channel, where the block holds whole 20 ms transport blocks: P-CCPCH on code 1 of SF 256, off for the first 256 chips of a slot
        {
            static thread_local std::vector<cf32> code2;
            scramble(16 * (bg * 8 + bcode), code2);
            umts::BchParser parser;
            for (int parity = 0; parity < 2; parity++) {
                int good = 0;
                for (size_t tti = 0;; tti++) {
                    const size_t start = frame0 + (size_t)parity * 38400 + tti * 76800;
                    if (start + 76800 > N) break;
                    std::vector<float> soft(540);
                    for (int fr = 0; fr < 2; fr++) for (int sl = 0; sl < 15; sl++) {
                        const size_t sb = start + (size_t)fr * 38400 + (size_t)sl * 2560;
                        cf32 cp(0, 0);
                        for (int i = 0; i < 2560; i++) { const double w = 2 * kPi * bCfo * (double)(sb + (size_t)i) / kChipRate; cp += c[sb + (size_t)i] * std::conj(code2[(size_t)(sl * 2560 + i)]) * cf32((float)std::cos(w), -(float)std::sin(w)); }
                        const cf32 h = cp / cf32(1, 1);                 // the channel, from the pilot (1 + j)
                        for (int sy = 0; sy < 9; sy++) {
                            cf32 r(0, 0);
                            for (int i = 0; i < 256; i++) { const size_t ci = (size_t)sl * 2560 + 256 + (size_t)sy * 256 + (size_t)i; const double w = 2 * kPi * bCfo * (double)(start + (size_t)fr * 38400 + ci) / kChipRate; r += c[start + (size_t)fr * 38400 + ci] * std::conj(code2[ci]) * cf32(i < 128 ? 1.f : -1.f, 0) * cf32((float)std::cos(w), -(float)std::sin(w)); }
                            const cf32 z = r * std::conj(h);
                            const size_t idx = (size_t)fr * 270 + (size_t)sl * 18 + (size_t)sy * 2;
                            soft[idx] = z.real(); soft[idx + 1] = z.imag();
                        }
                    }
                    uint8_t tb[umts::kBchInfo];
                    if (umts::bchDecode(soft.data(), tb)) { parser.feed(tb); good++; }
                }
                cell.bchBlocks += good;
                if (good) break;
            }
            const umts::BchInfo& bi = parser.info();
            if (bi.haveMib) { cell.mcc = bi.mcc; cell.mnc = bi.mnc; cell.mncDigits = bi.mncDigits; }
            if (bi.haveSib3) cell.cellIdentity = bi.cellIdentity;
        }
        bool dup = false;
        for (auto& f : found) if (f.scramblingCode == cell.scramblingCode) dup = true;
        if (!dup) found.push_back(cell);
    }
    if (scanMs) *scanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return found;
}

std::vector<cf32> umtsGenerate(const std::vector<UmtsTxCell>& cells, size_t samples, double snrDb, unsigned seed) {
    std::vector<cf32> out(samples, cf32(0, 0));
    std::mt19937 rng(seed);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    const std::vector<int>& p = psc();
    const auto& S = ssc();
    for (const UmtsTxCell& c : cells) {
        const int g = c.code / 8, k = c.code % 8;
        std::vector<cf32> code; scramble(16 * (g * 8 + k), code);
        // the broadcast channel: MIB and SIB 3 in alternating transport blocks
        std::vector<std::vector<uint8_t>> tb;
        if (c.mcc >= 0) { auto m = umts::bchEncode(umts::bchMakeMib(c.mcc, c.mnc, c.mncDigits).data()); auto s3 = umts::bchEncode(umts::bchMakeSib3(c.cellIdentity).data()); tb.push_back(m); tb.push_back(s3); }
        const int frames = (int)(samples / 76800) + 3;
        std::vector<std::vector<cf32>> allFrames;
        std::vector<cf32> chips((size_t)38400);
        for (int i = 0; i < 38400; i++) chips[(size_t)i] = code[(size_t)i] * cf32(1, 1) * (float)(0.3);    // CPICH
        if (c.dataPower > 0) { for (int i = 0; i < 38400; i++) { const int b = (int)(rng() & 3); chips[(size_t)i] += code[(size_t)i] * cf32(b & 1 ? 1.f : -1.f, b & 2 ? 1.f : -1.f) * (float)std::sqrt(c.dataPower); } }
        for (int sl = 0; sl < 15; sl++) {
            const int cc = kSeq[g][sl];
            for (int i = 0; i < 256; i++) chips[(size_t)(sl * 2560 + i)] += cf32(1, 1) * 0.7f * (float)p[(size_t)i] * 0.5f + cf32(1, 1) * 0.7f * (float)S[(size_t)cc][(size_t)i] * 0.5f;
        }
        for (int f = 0; f < frames; f++) {
            std::vector<cf32> fr = chips;
            if (!tb.empty()) {
                const auto& t = tb[(size_t)((f / 2) % 2)];
                for (int sl = 0; sl < 15; sl++) for (int sy = 0; sy < 9; sy++) {
                    const int bitI = t[(size_t)(f & 1) * 270 + (size_t)sl * 18 + (size_t)sy * 2], bitQ = t[(size_t)(f & 1) * 270 + (size_t)sl * 18 + (size_t)sy * 2 + 1];
                    const cf32 sv(bitI ? -1.f : 1.f, bitQ ? -1.f : 1.f);
                    for (int i = 0; i < 256; i++) { const int ci = sl * 2560 + 256 + sy * 256 + i; fr[(size_t)ci] += code[(size_t)ci] * sv * (i < 128 ? 0.3f : -0.3f); }
                }
            }
            allFrames.push_back(std::move(fr));
        }
        for (size_t i = 0; i < samples; i++) {
            const size_t ch0 = i / 2 + (size_t)c.chipOffset;
            const size_t chip = ch0 % 38400;
            const auto& chips = allFrames[std::min<size_t>(ch0 / 38400, allFrames.size() - 1)];
            const double w = 2 * kPi * c.cfoHz * (double)i / (2 * kChipRate);
            out[i] += chips[chip] * (float)c.amp * cf32((float)std::cos(w), (float)std::sin(w));
        }
    }
    double pw = 0; for (auto& v : out) pw += std::norm(v);
    pw /= (double)samples;
    const float sg = snrDb < 150 ? (float)std::sqrt(pw * std::pow(10.0, -snrDb / 10) / 2) : 0.f;
    if (sg > 0) for (auto& v : out) v += cf32(nd(rng), nd(rng)) * sg;
    return out;
}

}  // namespace dect2
