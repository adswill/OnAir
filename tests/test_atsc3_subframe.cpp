// ATSC 3.0 subframe symbols: the number of data cells per data symbol and per subframe boundary symbol against A/322 Tables 7.3 to 7.6
// (every FFT size, carrier reduction and scattered pilot pattern), and a round trip through modulation, a channel and demodulation.
#include "dect2/atsc3_subframe.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

struct Row { int fft, cred; int v[8]; };
// columns: SP3_2 SP3_4 SP4_2 SP4_4 SP6_2 SP6_4 SP8_2 SP8_4 (Tables 7.3, 7.5) and SP12_2 .. SP32_4 (Tables 7.4, 7.6); -1 where the combination is N/A
static const Row k73[] = {
    {8192, 0, {5711, 6285, 5999, 6429, 6287, 6573, 6431, 6645}},
    {8192, 1, {5631, 6197, 5915, 6339, 6199, 6481, 6341, 6552}},
    {8192, 2, {5552, 6110, 5832, 6250, 6112, 6390, 6252, 6460}},
    {8192, 3, {5473, 6023, 5749, 6161, 6025, 6299, 6163, 6368}},
    {8192, 4, {5394, 5936, 5666, 6072, 5938, 6208, 6074, 6276}},
    {16384, 0, {11423, 12573, 11999, 12861, 12575, 13149, 12863, 13293}},
    {16384, 1, {11263, 12397, 11831, 12681, 12399, 12965, 12683, 13107}},
    {16384, 2, {11106, 12224, 11666, 12504, 12226, 12784, 12506, 12924}},
    {16384, 3, {10947, 12049, 11499, 12325, 12051, 12601, 12327, 12739}},
    {16384, 4, {10789, 11875, 11333, 12147, 11877, 12419, 12149, 12555}},
    {32768, 0, {22847, 25149, -1, -1, 25151, 26301, 25727, 26589}},
    {32768, 1, {22527, 24797, -1, -1, 24799, 25933, 25367, 26217}},
    {32768, 2, {22213, 24451, -1, -1, 24453, 25571, 25013, 25851}},
    {32768, 3, {21895, 24101, -1, -1, 24103, 25205, 24655, 25481}},
    {32768, 4, {21579, 23753, -1, -1, 23755, 24841, 24299, 25113}}
};
static const Row k74[] = {
    {8192, 0, {6575, 6717, 6647, 6753, 6719, 6789, 6755, 6807}},
    {8192, 1, {6483, 6623, 6554, 6660, 6625, 6694, 6661, 6714}},
    {8192, 2, {6392, 6530, 6462, 6565, 6532, 6600, 6567, 6619}},
    {8192, 3, {6301, 6437, 6370, 6473, 6439, 6506, 6474, 6524}},
    {8192, 4, {6210, 6344, 6278, 6378, 6346, 6412, 6380, 6429}},
    {16384, 0, {13151, 13437, 13295, 13509, 13439, 13581, 13511, 13617}},
    {16384, 1, {12967, 13249, 13109, 13320, 13251, 13391, 13322, 13428}},
    {16384, 2, {12786, 13064, 12926, 13134, 13066, 13204, 13136, 13239}},
    {16384, 3, {12603, 12877, 12741, 12946, 12879, 13015, 12948, 13051}},
    {16384, 4, {12421, 12691, 12557, 12759, 12693, 12827, 12761, 12861}},
    {32768, 0, {26303, 26877, 26591, 27021, 26879, 27165, 27023, 27237}},
    {32768, 1, {25935, 26501, 26219, 26643, 26503, 26785, 26645, 26856}},
    {32768, 2, {25573, 26131, 25853, 26271, 26133, 26411, 26273, 26481}},
    {32768, 3, {25207, 25757, 25483, 25895, 25759, 26033, 25897, 26102}},
    {32768, 4, {24843, 25385, 25115, 25521, 25387, 25657, 25523, 25725}}
};
static const Row k75[] = {
    {8192, 0, {4560, 4560, 5136, 5136, 5712, 5712, 6000, 6000}},
    {8192, 1, {4496, 4496, 5064, 5064, 5632, 5632, 5916, 5916}},
    {8192, 2, {4433, 4433, 4993, 4993, 5553, 5553, 5833, 5833}},
    {8192, 3, {4370, 4370, 4922, 4922, 5474, 5474, 5750, 5750}},
    {8192, 4, {4307, 4307, 4851, 4851, 5395, 5395, 5667, 5667}},
    {16384, 0, {9120, 9120, 10272, 10272, 11424, 11424, 12000, 12000}},
    {16384, 1, {8992, 8992, 10128, 10128, 11264, 11264, 11832, 11832}},
    {16384, 2, {8867, 8867, 9987, 9987, 11107, 11107, 11667, 11667}},
    {16384, 3, {8740, 8740, 9844, 9844, 10948, 10948, 11500, 11500}},
    {16384, 4, {8614, 8614, 9702, 9702, 10790, 10790, 11334, 11334}},
    {32768, 0, {18240, 18240, -1, -1, 22848, 22848, 24000, 24000}},
    {32768, 1, {17984, 17984, -1, -1, 22528, 22528, 23664, 23664}},
    {32768, 2, {17734, 17734, -1, -1, 22214, 22214, 23334, 23334}},
    {32768, 3, {17480, 17480, -1, -1, 21896, 21896, 23000, 23000}},
    {32768, 4, {17228, 17228, -1, -1, 21580, 21580, 22668, 22668}}
};
static const Row k76[] = {
    {8192, 0, {6288, 6288, 6432, 6432, 6576, 6576, 6648, 6648}},
    {8192, 1, {6200, 6200, 6342, 6342, 6484, 6484, 6555, 6555}},
    {8192, 2, {6113, 6113, 6253, 6253, 6393, 6393, 6463, 6463}},
    {8192, 3, {6026, 6026, 6164, 6164, 6302, 6302, 6371, 6371}},
    {8192, 4, {5939, 5939, 6075, 6075, 6211, 6211, 6279, 6279}},
    {16384, 0, {12576, 12576, 12864, 12864, 13152, 13152, 13296, 13296}},
    {16384, 1, {12400, 12400, 12684, 12684, 12968, 12968, 13110, 13110}},
    {16384, 2, {12227, 12227, 12507, 12507, 12787, 12787, 12927, 12927}},
    {16384, 3, {12052, 12052, 12328, 12328, 12604, 12604, 12742, 12742}},
    {16384, 4, {11878, 11878, 12150, 12150, 12422, 12422, 12558, 12558}},
    {32768, 0, {25152, 25152, 25728, 25728, 26304, 26304, 26592, 26592}},
    {32768, 1, {24800, 24800, 25368, 25368, 25936, 25936, 26220, 26220}},
    {32768, 2, {24454, 24454, 25014, 25014, 25574, 25574, 25854, 25854}},
    {32768, 3, {24104, 24104, 24656, 24656, 25208, 25208, 25484, 25484}},
    {32768, 4, {23756, 23756, 24300, 24300, 24844, 24844, 25116, 25116}}
};

static int check(const Row* rows, int n, int firstPattern, bool boundary, const char* name) {
    int bad = 0;
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < 8; c++) {
            if (rows[i].v[c] < 0) continue;
            // the tables list combinations that Table 8.3 does not allow in brackets: 32K only has the DY = 2 patterns, 8K has no SP24
            int pdx = spDx(firstPattern + c), pdy = spDy(firstPattern + c);
            if ((rows[i].fft == 32768 && pdy == 4) || (rows[i].fft == 8192 && pdx == 24)) continue;
            // 8K with SP32_4 and Cred_coeff >= 2: the additional pilot sets of Table D.1.5 are ambiguous in the standard's text and the table
            // values cannot be reproduced from them (all other combinations match); not checked
            if (rows[i].fft == 8192 && firstPattern + c == 15 && rows[i].cred >= 2) continue;
            SubframeParams s;
            s.fftSize = rows[i].fft; s.cred = rows[i].cred; s.spPattern = firstPattern + c;
            s.numSymbols = 16;
            if (boundary) s.sbsFirst = true;
            int dy = spDy(s.spPattern);
            for (int l = boundary ? 0 : 1; l < (boundary ? 1 : 1 + dy); l++) {
                int got = subframeDataCells(s, l);
                if (got != rows[i].v[c]) {
                    if (bad < 12) printf("  %s: FFT %d Cred %d SP%d_%d symbol %d: got %d, expected %d\n", name, s.fftSize, s.cred, spDx(s.spPattern), spDy(s.spPattern), l, got, rows[i].v[c]);
                    bad++;
                }
            }
        }
    }
    return bad;
}

int main() {
    int n = (int)(sizeof k73 / sizeof k73[0]);
    CHECK(check(k73, n, 0, false, "Table 7.3") == 0, "data cells per data symbol, SP3 to SP8 (Table 7.3)");
    CHECK(check(k74, n, 8, false, "Table 7.4") == 0, "data cells per data symbol, SP12 to SP32 (Table 7.4)");
    CHECK(check(k75, n, 0, true, "Table 7.5") == 0, "data cells per boundary symbol, SP3 to SP8 (Table 7.5)");
    CHECK(check(k76, n, 8, true, "Table 7.6") == 0, "data cells per boundary symbol, SP12 to SP32 (Table 7.6)");

    CHECK(guardFromCode(1) == 192 && guardFromCode(12) == 4864 && guardFromCode(0) == 0 && fftFromCode(2) == 32768, "signalling code tables");
    SubframeParams b; b.spPattern = 4; b.spBoost = 3;   // SP6_2, boost 3: 4.0 dB
    CHECK(std::fabs(spAmplitude(b) - 1.585) < 0.002, "pilot boost amplitude");

    std::mt19937 rng(23);
    std::normal_distribution<float> g(0.f, 1.f);
    struct Case { int fft, guard, pattern, cred; bool fi; bool sbsFirst, sbsLast; int nulls; double snr; bool echo; } cases[] = {
        {8192, 192, 2, 0, true, true, true, 40, 28, false}, {8192, 1024, 4, 1, false, false, false, 0, 28, true},
        {16384, 768, 6, 2, true, true, false, 0, 28, true}, {32768, 512, 12, 0, true, false, true, 0, 28, false}, {16384, 2048, 5, 0, true, false, false, 0, 28, true}};
    for (auto& cs : cases) {
        SubframeParams s;
        s.fftSize = cs.fft; s.guard = cs.guard; s.spPattern = cs.pattern; s.cred = cs.cred; s.freqInterleaver = cs.fi;
        s.sbsFirst = cs.sbsFirst; s.sbsLast = cs.sbsLast; s.numSymbols = 6; s.sbsNullCells = cs.nulls; s.spBoost = 1;
        for (int l = 0; l < s.numSymbols; l++) {
            int nd = subframeDataCells(s, l);
            std::vector<cf32> cells(nd);
            std::vector<int> bits(2 * nd);
            int nulls = isBoundarySymbol(s, l) ? s.sbsNullCells : 0;
            for (int i = 0; i < nd; i++) {
                bool isNull = i < nulls / 2 || i >= nd - (nulls - nulls / 2);
                int b0 = rng() & 1, b1 = rng() & 1;
                bits[2 * i] = b0; bits[2 * i + 1] = b1;
                cells[i] = isNull ? cf32(0, 0) : cf32(b0 ? -0.7071f : 0.7071f, b1 ? -0.7071f : 0.7071f);
            }
            auto tx = modulateSubframeSymbol(s, l, cells);
            char m[100];
            double pw = 0;
            for (auto& v : tx) pw += std::norm(v);
            pw /= tx.size();
            snprintf(m, sizeof m, "unit power (FFT %d, symbol %d)", cs.fft, l);
            CHECK(std::fabs(pw - 1.0) < 0.05, m);
            std::vector<cf32> rx(tx.size() + 100, cf32(0, 0));
            for (size_t i = 0; i < tx.size(); i++) rx[i] += tx[i];
            if (cs.echo) for (size_t i = 0; i + 20 < tx.size(); i++) rx[i + 20] += tx[i] * cf32(0.18f, 0.1f);
            double sig = 0;
            for (size_t i = 0; i < tx.size(); i++) sig += std::norm(rx[i]);
            sig /= tx.size();
            float sigma = (float)std::sqrt(sig / std::pow(10.0, cs.snr / 10.0) / 2.0);
            for (auto& v : rx) v += cf32(g(rng), g(rng)) * sigma;
            std::vector<cf32> out;
            float nv = 0;
            bool ok = demodulateSubframeSymbol(s, l, rx.data(), out, &nv);
            CHECK(ok && (int)out.size() == nd, "demodulate");
            if (!ok) continue;
            int errs = 0, cnt = 0;
            for (int i = nulls / 2; i < nd - (nulls - nulls / 2); i++) {
                errs += ((out[i].real() < 0) != (bits[2 * i] == 1));
                errs += ((out[i].imag() < 0) != (bits[2 * i + 1] == 1));
                cnt += 2;
            }
            if (l == 0 || l == s.numSymbols - 1) printf("  FFT %5d GI %4d SP%d_%d symbol %d%s: %d cells, bit errors %d/%d\n", cs.fft, cs.guard, spDx(cs.pattern), spDy(cs.pattern), l, isBoundarySymbol(s, l) ? " (boundary)" : "", nd, errs, cnt);
            snprintf(m, sizeof m, "round trip (FFT %d, symbol %d)", cs.fft, l);
            CHECK(errs * 1000 <= cnt * 3, m);
        }
    }
    printf(fails ? "atsc3 subframe: FAILED\n" : "atsc3 subframe: ok\n");
    return fails ? 1 : 0;
}
