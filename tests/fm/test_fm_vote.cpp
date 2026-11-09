// Soft combining of repeated RDS blocks: a noisy, fading bit stream through the block decoder with the plain vote and with the extended one; count the
// groups that come out and the wrong ones (the safeguard against a chance hit of the checkword must stay).
#include "dect2/fm/fm_rds.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint32_t block(uint16_t d, uint16_t off) {
    uint32_t r = (uint32_t)d << 10;
    for (int i = 25; i >= 10; i--) if (r >> i & 1) r ^= 0x5B9u << (i - 10);
    return ((uint32_t)d << 10) | ((r & 0x3FF) ^ off);
}

struct Res { uint64_t groups = 0, wrong = 0, voted = 0; int tDone = -1; std::string ps; };

static Res run(double ebn0, double fadeRate, int depth, unsigned seed) {
    std::vector<std::array<uint16_t, 4>> grp;
    const char* ps = "OLYMPIA ", *rt = "Soft combining of repeated groups test....      ";
    const uint16_t pi = 0x4C21;
    for (int s = 0; s < 4; s++) grp.push_back({pi, (uint16_t)(0x0000 | 0x0400 | s), 0xE0E0, (uint16_t)((ps[s * 2] << 8) | ps[s * 2 + 1])});
    for (int s = 0; s < 12; s++) grp.push_back({pi, (uint16_t)(0x2000 | s), (uint16_t)((rt[s * 4] << 8) | rt[s * 4 + 1]), (uint16_t)((rt[s * 4 + 2] << 8) | rt[s * 4 + 3])});
    RdsDecoder d;
    d.setVoteDepth(depth);
    Res r;
    std::mt19937 rng(seed); std::normal_distribution<float> n(0, 1);
    const float amp = (float)std::sqrt(2 * std::pow(10.0, ebn0 / 10));
    int prevTx = 0, prevRx = 0;
    double fade = 1, phase = 0;
    static const uint16_t off[4] = {0x0FC, 0x198, 0x168, 0x1B4};
    const int total = 26 * 4 * 16 * 80;     // 80 rounds of the 16 groups
    for (int bitNo = 0; bitNo < total; bitNo++) {
        const auto& g = grp[(size_t)(bitNo / 104) % grp.size()];
        const int blk = (bitNo % 104) / 26, pos = bitNo % 26;
        const uint32_t w = block(g[(size_t)blk], off[blk]);
        const int bit = (w >> (25 - pos)) & 1;
        prevTx ^= bit;
        phase += 2 * M_PI * fadeRate / 1187.5;
        fade = fadeRate > 0 ? 0.55 + 0.45 * std::cos(phase) + 0.15 : 1;
        const float y = (prevTx ? 1.f : -1.f) * amp * (float)fade + n(rng);
        const int sym = y >= 0;
        d.pushBit(sym ^ prevRx, std::fabs(y));
        prevRx = sym;
        if (r.tDone < 0 && d.ps == ps && d.rt.rfind("Soft combining of repeated groups test....", 0) == 0) r.tDone = bitNo;
        if (d.ps.size() == 8 && d.ps != ps && !d.ps.empty() && bitNo % 104 == 0) r.wrong++;   // a wrong name on the screen, sampled each group
    }
    r.groups = d.groups; r.voted = d.info.blocksVoted; r.ps = d.ps;
    if (r.tDone < 0) r.tDone = total;
    return r;
}

int main() {
    double a = 0, b = 0, ta = 0, tb = 0, fadeA = 0, fadeB = 0;
    for (double fade : {0.0, 3.0, 11.0}) for (double ebn0 : {2.0, 3.0, 4.0, 5.0, 6.0, 8.0}) {
        Res cl, ex; double t1 = 0, t2 = 0;
        for (unsigned s = 1; s <= 4; s++) {
            Res p = run(ebn0, fade, 1, s), q = run(ebn0, fade, 2, s);
            cl.groups += p.groups; cl.wrong += p.wrong; cl.voted += p.voted; ex.groups += q.groups; ex.wrong += q.wrong; ex.voted += q.voted;
            t1 += p.tDone / 1187.5; t2 += q.tDone / 1187.5;
        }
        printf("fade %4.1f Hz Eb/N0 %.0f dB: groups %5llu -> %5llu, voted blocks %5llu -> %5llu, wrong %llu -> %llu, time to the full name and text %6.1f -> %6.1f s\n", fade, ebn0, (unsigned long long)cl.groups, (unsigned long long)ex.groups,
               (unsigned long long)cl.voted, (unsigned long long)ex.voted, (unsigned long long)cl.wrong, (unsigned long long)ex.wrong, t1 / 4, t2 / 4);
        a += (double)cl.groups; b += (double)ex.groups; ta += t1; tb += t2;
        if (fade == 11.0 && ebn0 >= 5) { fadeA += (double)cl.groups; fadeB += (double)ex.groups; }
        CHECK(ex.groups + 2 >= cl.groups, "fade %.0f Eb/N0 %.0f: fewer groups", fade, ebn0);
        CHECK(ex.wrong <= cl.wrong + 3, "fade %.0f Eb/N0 %.0f: wrong names %llu (plain vote %llu)", fade, ebn0, (unsigned long long)ex.wrong, (unsigned long long)cl.wrong);
    }
    printf("groups over the sweep: %.0f -> %.0f (+%.1f %%), mean time to full name and text %.1f -> %.1f s\n", a, b, 100 * (b / a - 1), ta / 72, tb / 72);
    printf("fast fading, Eb/N0 5 dB and up: groups %.0f -> %.0f (+%.1f %%)\n", fadeA, fadeB, 100 * (fadeB / fadeA - 1));
    CHECK(b > a * 1.003, "no gain in groups");
    CHECK(fadeB > fadeA * 1.03, "no gain under fading");
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
