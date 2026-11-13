// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Galileo SAR return link messages: part ordering by the start bit, spare pages, parity. Field layout from the Galileo OS SIS ICD issue 2.1 tables 60-63, 103-104. Simulated data only.
#include "dect2/gnss/gnss_msg.h"
#include <cstdio>
#include <string>
#include <cstring>
using namespace dect2;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void parts(uint8_t all[][22], const uint8_t* bits, bool lng) {
    const int n = lng ? 8 : 4;
    for (int p = 0; p < n; p++) {
        all[p][0] = p == 0;
        all[p][1] = lng;
        for (int i = 0; i < 20; i++) all[p][2 + i] = bits[p * 20 + i];
    }
}
int main() {
    uint8_t bits[160] = {};
    const char* hex = "ADCD0228C50D801";
    for (int i = 0; i < 15; i++) { int v = hex[i] <= '9' ? hex[i] - '0' : hex[i] - 'A' + 10; for (int b = 0; b < 4; b++) bits[i * 4 + b] = (uint8_t)((v >> (3 - b)) & 1); }
    bits[60] = 0; bits[61] = 0; bits[62] = 0; bits[63] = 1;   // acknowledgement
    bits[64] = 1;
    int ones = 0; for (int i = 0; i < 79; i++) ones += bits[i];
    bits[79] = ones & 1;
    uint8_t p[8][22];
    parts(p, bits, false);
    GalRlmAssembler a; GalRlm r;
    // a spare start page first: start = 1, next start = 1 again
    uint8_t spare[22] = {}; spare[0] = 1;
    CHECK(!a.push(spare, r));
    CHECK(!a.push(p[0], r));
    CHECK(!a.push(p[1], r));
    CHECK(!a.push(p[2], r));
    CHECK(a.push(p[3], r));
    CHECK(std::string(r.hex) == hex && r.messageCode == 1 && r.ack && r.parityOk && !r.longRlm);
    // long RLM, 160 bits
    { uint8_t b2[160] = {}; std::memcpy(b2, bits, 64); int o2 = 0; for (int i = 0; i < 159; i++) o2 += b2[i]; b2[159] = o2 & 1; uint8_t q[8][22]; parts(q, b2, true); GalRlmAssembler s; GalRlm r2; bool got = false; for (int i = 0; i < 8; i++) got = s.push(q[i], r2); CHECK(got && r2.longRlm && r2.parityOk); }
    // a corrupted bit fails the parity
    bits[10] ^= 1;
    parts(p, bits, false);
    GalRlmAssembler s; GalRlm r3; bool got = false; for (int i = 0; i < 4; i++) got = s.push(p[i], r3);
    CHECK(got && !r3.parityOk);
    // a part without a start cannot start a message
    GalRlmAssembler s2; GalRlm r4; p[1][0] = 0; CHECK(!s2.push(p[1], r4));
    CHECK(std::string(osnmaStatusName(2)) == "operational" && std::string(osnmaChainStatusName(7)) == "alert message");
    return testSummary("gnss_galrlm");
}
