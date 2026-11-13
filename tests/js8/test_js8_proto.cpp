// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// JS8 protocol: the CRC-12 frame, the (174,87) code (every parity check holds, decoding through noise and bit errors), the tones and
// Costas arrays of the three submodes, callsign / grid / 50-bit call packing, heartbeat, CQ, directed, data frames, a whole message.
#include "dect2/js8/js8_proto.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static std::string show(const uint8_t* m, int flags) { return js8::unpackFrame(m, flags).display(); }

int main() {
    std::mt19937 rng(7);
    uint8_t msg[72], b87[87], cw[174];
    for (int i = 0; i < 72; i++) msg[i] = (uint8_t)(rng() & 1);
    js8::frameBits(msg, 3, b87);
    CHECK(js8::checkCrc(b87), "crc");
    b87[10] ^= 1;
    CHECK(!js8::checkCrc(b87), "crc catches a flipped bit");
    b87[10] ^= 1;
    js8::encode174(b87, cw);
    int bad = 0;
    for (int i = 0; i < js8::kM; i++) { int s = 0; for (int j = 0; j < js8::kNmCount[i]; j++) s += cw[js8::kNm[i][j]]; bad += s & 1; }
    CHECK(bad == 0, "%d parity checks fail", bad);
    // through noise: BPSK-like soft values, SNR low enough to flip some bits
    for (int trial = 0; trial < 20; trial++) {
        dect2::normal_distribution<float> nd(0.f, 0.65f);
        float llr[174];
        int flipped = 0;
        for (int i = 0; i < 174; i++) { const float v = (cw[i] ? 1.f : -1.f) + nd(rng); if ((v > 0) != (cw[i] != 0)) flipped++; llr[i] = 2 * v / (0.65f * 0.65f); }
        uint8_t out[174];
        const int r = js8::ldpcDecode(llr, 30, out);
        CHECK(r == 0 && std::memcmp(out, cw, 174) == 0, "trial %d: %d bits flipped, %d checks left", trial, flipped, r);
    }
    // tones
    const auto t = js8::tones(js8::kNormal, b87);
    CHECK(t.size() == 79, "79 tones");
    for (int k = 0; k < 7; k++) CHECK(t[(size_t)k] == js8::costas(0, 0)[k] && t[(size_t)(36 + k)] == js8::costas(0, 1)[k] && t[(size_t)(72 + k)] == js8::costas(0, 2)[k], "costas");
    const auto tf = js8::tones(js8::kTurbo, b87);
    CHECK(tf[0] == 0 && tf[1] == 6 && tf[36] == 1 && tf[72] == 2, "turbo costas");
    for (int v : t) CHECK(v >= 0 && v < 8, "tone %d", v);
    CHECK(std::fabs(js8::spec(0).baud() - 6.25) < 1e-9 && std::fabs(js8::spec(1).baud() - 10) < 1e-9 && std::fabs(js8::spec(2).baud() - 20) < 1e-9, "baud");
    // packing
    for (const char* c : {"LY2ABC", "K1ABC", "G4ABC", "W1AW", "OK1XX", "3DA0XYZ", "LY2ABC/P", "@ALLCALL", "@HB"}) {
        bool p = false;
        const uint32_t v = js8::packCallsign(c, &p);
        CHECK(v != 0 && js8::unpackCallsign(v, p) == c, "call %s -> %u -> %s", c, v, js8::unpackCallsign(v, p).c_str());
    }
    for (const char* c : {"LY2ABC", "PJ4/K1ABC", "VE3/LY2ABC", "G4ABC"}) CHECK(js8::unpackAlphaNumeric50(js8::packAlphaNumeric50(c)) == c, "alnum50 %s -> %s", c, js8::unpackAlphaNumeric50(js8::packAlphaNumeric50(c)).c_str());
    for (const char* g : {"KO24", "JO62", "FN42", "IO91", "PM95", "AA00", "RR99", "JN11"}) CHECK(js8::unpackGrid(js8::packGrid(g)) == g, "grid %s -> %s", g, js8::unpackGrid(js8::packGrid(g)).c_str());
    double la, lo;
    CHECK(js8::gridToLatLon("KO24", la, lo) && std::fabs(la - 54.5) < 0.6 && std::fabs(lo - 25) < 1.1, "KO24 at %.2f %.2f", la, lo);
    // frames
    uint8_t m[72];
    CHECK(js8::packHeartbeat("LY2ABC", "KO24", m) && show(m, 3) == "LY2ABC: @HB HEARTBEAT KO24", "heartbeat: %s", show(m, 3).c_str());
    CHECK(js8::packCq("DL1XYZ", "JO62", 0, m) && show(m, 3) == "DL1XYZ: @ALLCALL CQ CQ CQ JO62", "cq: %s", show(m, 3).c_str());
    CHECK(js8::packDirected("OK1XX", "LY2ABC", 25, -12, m) && show(m, 3) == "OK1XX: LY2ABC SNR -12", "snr: %s", show(m, 3).c_str());
    CHECK(js8::packDirected("OK1XX", "LY2ABC", 25, 7, m) && show(m, 3) == "OK1XX: LY2ABC SNR +07", "snr+: %s", show(m, 3).c_str());
    CHECK(js8::packDirected("G4ABC", "@ALLCALL", 4, 0, m) && show(m, 3) == "G4ABC: @ALLCALL GRID?", "grid?: %s", show(m, 3).c_str());
    CHECK(js8::packDirected("EA3XYZ/P", "LY2ABC", 23, 0, m) && show(m, 3) == "EA3XYZ/P: LY2ABC QSL", "portable: %s", show(m, 3).c_str());
    const int n = js8::packData("HELLO WORLD", m);
    CHECK(n == 11 && show(m, 0) == "HELLO WORLD", "data %d: %s", n, show(m, 0).c_str());
    CHECK(js8::charsToBits(js8::bitsToChars(m), msg) && std::memcmp(m, msg, 72) == 0, "chars round trip");
    CHECK(js8::unpackFrame(m, 4).compressed, "fast data flag");
    // a whole message
    const auto fr = js8::buildMessage("G4ABC", "DL1XYZ", 31, 0, "HELLO FROM LONDON. HOW COPY? 73");
    CHECK(fr.size() >= 3 && (fr.front().flags & js8::kFirst) && (fr.back().flags & js8::kLast), "%zu frames", fr.size());
    std::string all;
    for (const auto& f : fr) all += show(f.bits, f.flags);
    CHECK(all == "G4ABC: DL1XYZ HELLO FROM LONDON. HOW COPY? 73", "message: \"%s\"", all.c_str());
    const auto rl = js8::buildMessage("EA3XYZ", "OK1XX", 5, 0, "SP5AB PLEASE RELAY");
    std::string rall;
    for (const auto& f : rl) rall += show(f.bits, f.flags);
    CHECK(rall == "EA3XYZ: OK1XX>SP5AB PLEASE RELAY", "relay: \"%s\"", rall.c_str());
    printf(fails ? "js8 proto: %d FAILED\n" : "js8 proto: all passed\n", fails);
    return fails ? 1 : 0;
}
