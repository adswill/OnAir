// Spreading codes of the satellite navigation signals (see gnss_codes.h).
#include "dect2/gnss_codes.h"

namespace dect2 {

// G2 output stages (1 based) per PRN, IS-GPS-200 Table 3-I "code phase selection"
static const uint8_t kCaTaps[33][2] = {
    {0, 0}, {2, 6}, {3, 7}, {4, 8}, {5, 9}, {1, 9}, {2, 10}, {1, 8}, {2, 9}, {3, 10}, {2, 3}, {3, 4}, {5, 6}, {6, 7}, {7, 8}, {8, 9}, {9, 10},
    {1, 4}, {2, 5}, {3, 6}, {4, 7}, {5, 8}, {6, 9}, {1, 3}, {4, 6}, {5, 7}, {6, 8}, {7, 9}, {8, 10}, {1, 6}, {2, 7}, {3, 8}, {4, 9}};

bool gpsCaChips(int prn, uint8_t* chips) {
    if (prn < 1 || prn > 32) return false;
    // G1 = 1 + x^3 + x^10 (feedback from stages 3 and 10), G2 = 1 + x^2 + x^3 + x^6 + x^8 + x^9 + x^10 (stages 2, 3, 6, 8, 9, 10); both start all ones;
    // the output is G1 stage 10 xor the two chosen G2 stages
    int g1[11], g2[11];
    for (int i = 1; i <= 10; i++) g1[i] = g2[i] = 1;
    const int a = kCaTaps[prn][0], b = kCaTaps[prn][1];
    for (int n = 0; n < kGpsCaLen; n++) {
        chips[n] = (uint8_t)(g1[10] ^ g2[a] ^ g2[b]);
        const int f1 = g1[3] ^ g1[10];
        const int f2 = g2[2] ^ g2[3] ^ g2[6] ^ g2[8] ^ g2[9] ^ g2[10];
        for (int i = 10; i > 1; i--) { g1[i] = g1[i - 1]; g2[i] = g2[i - 1]; }
        g1[1] = f1; g2[1] = f2;
    }
    return true;
}

unsigned gpsCaFirst10(int prn) {
    uint8_t c[kGpsCaLen];
    if (!gpsCaChips(prn, c)) return 0;
    unsigned v = 0;
    for (int i = 0; i < 10; i++) v = (v << 1) | c[i];
    return v;
}

} // namespace dect2
