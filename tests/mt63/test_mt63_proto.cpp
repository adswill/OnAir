// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 coding layer: Walsh codewords, interleaver delays, encoder -> deinterleaver round trip for both interleaves, the pulse shape.
#include "dect2/mt63/mt63_proto.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // Walsh: every character is its own codeword, inverted for 64..127, and the decoder finds it with a clean margin
    for (int code = 0; code < 128; code++) {
        int8_t w[mt63::kCarriers];
        mt63::walshEncode(code, w);
        float s[mt63::kCarriers];
        for (int i = 0; i < mt63::kCarriers; i++) s[i] = w[i];
        float conf = 0;
        const int got = mt63::walshDecode(s, conf);
        CHECK(got == code && conf > 50, "walsh %d -> %d (conf %.1f)", code, got, conf);
    }
    {   // 15 % of the soft values flipped: still decoded
        srand(5);
        int bad = 0;
        for (int k = 0; k < 400; k++) {
            const int code = rand() & 127;
            int8_t w[mt63::kCarriers];
            mt63::walshEncode(code, w);
            float s[mt63::kCarriers];
            for (int i = 0; i < mt63::kCarriers; i++) s[i] = (rand() % 100 < 15) ? -w[i] : w[i];
            float c;
            if (mt63::walshDecode(s, c) != code) bad++;
        }
        CHECK(bad == 0, "%d of 400 characters lost with 15%% wrong carriers", bad);
    }
    // delays
    for (bool L : {false, true}) {
        const int len = mt63::intlLen(L);
        int mn = len, mx = 0;
        for (int i = 0; i < mt63::kCarriers; i++) { const int d = mt63::intlDelay(L, i); CHECK(d >= 0 && d < len, "delay %d", d); mn = std::min(mn, d); mx = std::max(mx, d); }
        CHECK(mt63::intlDelay(L, 0) == 0, "first carrier has no delay");
        printf("  %s interleave: %d symbols, delays %d..%d\n", L ? "long" : "short", len, mn, mx);
        CHECK(mx >= len / 2, "delays spread over the pipe");
    }
    // encoder -> (differential BPSK, error free) -> deinterleaver -> characters
    for (bool L : {false, true}) {
        mt63::Encoder enc(L, 7);
        mt63::Deinterleaver dei(L);
        const char* msg = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789\r";
        std::string sent, got;
        int phase[mt63::kCarriers];
        for (int i = 0; i < mt63::kCarriers; i++) phase[i] = 1;
        int prev[mt63::kCarriers];
        for (int i = 0; i < mt63::kCarriers; i++) prev[i] = 1;
        const int n = (int)strlen(msg) + 140;
        for (int k = 0; k < n; k++) {
            const int code = k < (int)strlen(msg) ? msg[k] : 0;
            if (k < (int)strlen(msg)) sent += msg[k];
            uint8_t bits[mt63::kCarriers];
            enc.process(code, bits);
            float u[mt63::kCarriers];
            for (int i = 0; i < mt63::kCarriers; i++) {
                phase[i] *= bits[i] ? 1 : -1;               // bit 1 keeps the phase, 0 reverses it
                u[i] = (phase[i] * prev[i] < 0) ? 1.f : -1.f;   // what the receiver sees: reversed -> positive
                prev[i] = phase[i];
            }
            int c; float conf;
            if (dei.push(u, c, conf)) { if (conf > 20 && c) got += (char)c; }
        }
        // the first intlLen-1 symbols are the pipe filling (random bits): the text follows after them
        CHECK(got.find(sent) != std::string::npos || got.size() >= sent.size() - 2, "%s: sent \"%s\" got \"%s\"", L ? "long" : "short", sent.c_str(), got.c_str());
        CHECK(got.find("QUICK BROWN FOX") != std::string::npos, "%s: text lost: \"%s\"", L ? "long" : "short", got.c_str());
    }
    {   // the wrong interleave sees noise
        mt63::Encoder enc(true, 3);
        mt63::Deinterleaver wrong(false);
        int phase[mt63::kCarriers], prev[mt63::kCarriers];
        for (int i = 0; i < mt63::kCarriers; i++) phase[i] = prev[i] = 1;
        double sum = 0; int cnt = 0;
        for (int k = 0; k < 300; k++) {
            uint8_t bits[mt63::kCarriers];
            enc.process('A' + (k % 26), bits);
            float u[mt63::kCarriers];
            for (int i = 0; i < mt63::kCarriers; i++) { phase[i] *= bits[i] ? 1 : -1; u[i] = (phase[i] * prev[i] < 0) ? 1.f : -1.f; prev[i] = phase[i]; }
            int c; float conf;
            if (wrong.push(u, c, conf)) { sum += conf; cnt++; }
        }
        printf("  wrong interleave: mean quality %.2f\n", sum / cnt);
        CHECK(sum / cnt < 4.5, "wrong interleave decoded as clean (%.2f)", sum / cnt);
    }
    // the pulse
    CHECK(std::fabs(mt63::pulse(256) - 1.0) < 1e-3, "pulse peak %.4f", mt63::pulse(256));
    CHECK(std::fabs(mt63::pulse(256 - 77.3) - mt63::pulse(256 + 77.3)) < 1e-3, "pulse symmetric");
    CHECK(std::fabs(mt63::pulse(56)) < 0.02 && std::fabs(mt63::pulse(456)) < 0.02, "pulse nearly zero one symbol away");
    CHECK(mt63::pulse(-1) == 0 && mt63::pulse(513) == 0, "pulse outside");
    CHECK(mt63::symbolSec(1000) > 0.0999 && mt63::symbolSec(1000) < 0.1001 && mt63::symbolSec(500) > 0.1999 && mt63::symbolSec(2000) > 0.0499 && mt63::symbolSec(2000) < 0.0501, "symbol times");
    return testSummary("mt63 proto");
}
