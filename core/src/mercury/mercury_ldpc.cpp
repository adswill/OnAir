// The LDPC codes of Mercury's OFDM data modes (see mercury_phy.h): codec2's IRA codes from their parity-check tables, the 16200 / 9720
// code from the DVB-S2 short frame rate 3/5 table (codec2's H_16200_9720 is that code: every one of its 6480 checks was compared), the
// accumulator encoder of codec2 (mpdecode_core.c encode) and OnAir's layered normalised min-sum decoder.
#include "dect2/mercury/mercury_phy.h"
#include "../dvbs/dvbs_ldpc_tables.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace dect2 {
namespace mercury {

namespace {
#include "mercury_ldpc_tables.inc"

void fromRows(LdpcCode& c, const uint16_t* t, size_t len) {
    c.rowStart.assign(1, 0);
    size_t i = 0;
    while (i < len) {
        const int n = t[i++];
        for (int j = 0; j < n; j++) c.rowCols.push_back(t[i++]);
        c.rowStart.push_back((int)c.rowCols.size());
    }
}

std::vector<LdpcCode> buildCodes() {
    std::vector<LdpcCode> v((size_t)kNumLdpcCodes);
    auto mk = [&](int id, const char* name, int n, int k, const uint16_t* t, size_t len) {
        LdpcCode& c = v[(size_t)id];
        c.name = name; c.n = n; c.k = k; c.m = n - k;
        fromRows(c, t, len);
    };
    mk(kH128x256, "H_128_256_5", 256, 128, kTabH128x256, sizeof kTabH128x256 / sizeof *kTabH128x256);
    mk(kH256x512, "H_256_512_4", 512, 256, kTabH256x512, sizeof kTabH256x512 / sizeof *kTabH256x512);
    mk(kH256x768, "H_256_768_22", 768, 256, kTabH256x768, sizeof kTabH256x768 / sizeof *kTabH256x768);
    mk(kH1024x2048, "H_1024_2048_4f", 2048, 1024, kTabH1024x2048, sizeof kTabH1024x2048 / sizeof *kTabH1024x2048);
    mk(kH4096x8192, "H_4096_8192_3d", 8192, 4096, kTabH4096x8192, sizeof kTabH4096x8192 / sizeof *kTabH4096x8192);
    mk(kHra56x56, "HRA_56_56", 112, 56, kTabHra56x56, sizeof kTabHra56x56 / sizeof *kTabHra56x56);
    mk(kMfsk1600, "MFSK 8/16", 1600, 800, kTabMfsk1600, sizeof kTabMfsk1600 / sizeof *kTabMfsk1600);
    {   // DVB-S2 short 3/5: information bit g * 360 + j joins parity check (x + j * q) mod m for every address x of group g
        LdpcCode& c = v[(size_t)kH16200x9720];
        c.name = "H_16200_9720"; c.n = 16200; c.k = 9720; c.m = 6480;
        const dvbs::S2LdpcTable& t = dvbs::kS2LdpcTables[1][4];
        const int q = c.m / 360;
        std::vector<std::vector<int>> rows((size_t)c.m);
        int at = 0;
        for (int g = 0; g < t.rows; g++) {
            const int cnt = t.data[at++];
            for (int a = 0; a < cnt; a++, at++)
                for (int j = 0; j < 360; j++) rows[(size_t)((t.data[at] + j * q) % c.m)].push_back(g * 360 + j);
        }
        c.rowStart.assign(1, 0);
        for (auto& r : rows) { std::sort(r.begin(), r.end()); c.rowCols.insert(c.rowCols.end(), r.begin(), r.end()); c.rowStart.push_back((int)c.rowCols.size()); }
    }
    return v;
}
} // namespace

const LdpcCode& ldpcCode(int id) {
    static std::once_flag once;
    static std::vector<LdpcCode> codes;
    std::call_once(once, [] { codes = buildCodes(); });
    return codes[(size_t)std::max(0, std::min(kNumLdpcCodes - 1, id))];
}

void ldpcEncode(const LdpcCode& c, const uint8_t* info, uint8_t* parity) {
    int prev = 0;
    for (int p = 0; p < c.m; p++) {
        int s = prev;
        for (int e = c.rowStart[(size_t)p]; e < c.rowStart[(size_t)p + 1]; e++) s ^= info[c.rowCols[(size_t)e]] & 1;
        parity[p] = (uint8_t)s;
        prev = s;
    }
}

LdpcDecoder::LdpcDecoder(int codeId) : c_(ldpcCode(codeId)) {
    chkStart_.assign(1, 0);
    for (int p = 0; p < c_.m; p++) {
        for (int e = c_.rowStart[(size_t)p]; e < c_.rowStart[(size_t)p + 1]; e++) chkVar_.push_back(c_.rowCols[(size_t)e]);
        if (p > 0) chkVar_.push_back(c_.k + p - 1);
        chkVar_.push_back(c_.k + p);
        chkStart_.push_back((int)chkVar_.size());
    }
    msg_.assign(chkVar_.size(), 0.f);
    post_.assign((size_t)c_.n, 0.f);
}

bool LdpcDecoder::decode(const float* llr, int maxIter, uint8_t* bits, int* iters, int* unsatisfied) {
    const int n = c_.n, m = c_.m;
    constexpr float kAlpha = 0.75f, kClamp = 60.f;
    for (int v = 0; v < n; v++) post_[(size_t)v] = std::max(-kClamp, std::min(kClamp, std::isfinite(llr[v]) ? llr[v] : 0.f));
    std::fill(msg_.begin(), msg_.end(), 0.f);
    auto unsat = [&]() {
        int bad = 0;
        for (int p = 0; p < m; p++) {
            int s = 0;
            for (int e = chkStart_[(size_t)p]; e < chkStart_[(size_t)p + 1]; e++) s ^= post_[(size_t)chkVar_[(size_t)e]] < 0;
            bad += s;
        }
        return bad;
    };
    int it = 0, bad = unsat();
    std::vector<float> q;
    while (bad && it < maxIter) {
        it++;
        for (int p = 0; p < m; p++) {
            const int a = chkStart_[(size_t)p], b = chkStart_[(size_t)p + 1];
            q.resize((size_t)(b - a));
            float min1 = 1e30f, min2 = 1e30f;
            int arg = -1, sgn = 0;
            for (int e = a; e < b; e++) {
                const float t = post_[(size_t)chkVar_[(size_t)e]] - msg_[(size_t)e];
                q[(size_t)(e - a)] = t;
                const float at = std::fabs(t);
                sgn ^= t < 0;
                if (at < min1) { min2 = min1; min1 = at; arg = e; }
                else if (at < min2) min2 = at;
            }
            for (int e = a; e < b; e++) {
                const float t = q[(size_t)(e - a)];
                const float mag = kAlpha * (e == arg ? min2 : min1);
                const float r = ((sgn ^ (t < 0)) ? -mag : mag);
                msg_[(size_t)e] = r;
                post_[(size_t)chkVar_[(size_t)e]] = std::max(-kClamp, std::min(kClamp, t + r));
            }
        }
        bad = unsat();
    }
    for (int v = 0; v < n; v++) bits[v] = post_[(size_t)v] < 0;
    if (iters) *iters = it;
    if (unsatisfied) *unsatisfied = bad;
    return bad == 0;
}

} // namespace mercury
} // namespace dect2
