// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ / IFKP coding layer: the varicode against the entries of fldigi's tables, the tone steps, the CRC of the callsign, the line parser.
#include "dect2/fsq/kbd33.h"
#include "dect2/fsq/kbd33_gen.h"
#include "dect2/fsq/kbd33_station.h"
#include <cmath>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static void expectCode(bool ifkp, int ch, int a, int b) {
    int s1 = -9, s2 = -9;
    const bool ok = kbd33::encode(ifkp, ch, s1, s2);
    CHECK(ok && s1 == a && s2 == b, "%s %d: got (%d,%d), wanted (%d,%d)", ifkp ? "IFKP" : "FSQ", ch, s1, s2, a, b);
}

// the receiver's rule: the step to the previous tone, minus one, mod 33
static std::string decodeTones(bool ifkp, const std::vector<int>& tones) {
    kbd33::CharDecoder d(ifkp);
    std::string out;
    for (size_t i = 1; i < tones.size(); i++) {
        const int nib = (((tones[i] - tones[i - 1] - 1) % 33) + 33) % 33;
        const int c = d.push(nib);
        if (c > 0) out += (char)c;
    }
    const int c = d.flush();
    if (c > 0) out += (char)c;
    return out;
}

int main() {
    // the parameters
    for (int s = 0; s < kbd33::kFsqSpeeds; s++) {
        const kbd33::Spec sp = kbd33::fsqSpec(s);
        CHECK(std::fabs(sp.spacingHz - 8.7890625) < 1e-9, "FSQ spacing %f", sp.spacingHz);
        static const double baud[5] = {1.46, 1.95, 2.93, 3.91, 5.86};   // the programs call them 1.5, 2, 3, 4.5 and 6
        CHECK(std::fabs(sp.baud() - 12000.0 / (s == 0 ? 8192 : s == 1 ? 6144 : s == 2 ? 4096 : s == 3 ? 3072 : 2048)) < 1e-9, "FSQ baud");
        CHECK(std::fabs(sp.baud() - baud[s]) < 0.01, "FSQ speed %d: %f baud", s, sp.baud());
    }
    for (int s = 0; s < kbd33::kIfkpSpeeds; s++) {
        const kbd33::Spec sp = kbd33::ifkpSpec(s);
        CHECK(std::fabs(sp.spacingHz - 11.71875) < 1e-9, "IFKP spacing %f", sp.spacingHz);
        static const double baud[3] = {1.953125, 3.90625, 7.8125};
        CHECK(std::fabs(sp.baud() - baud[s]) < 1e-9, "IFKP speed %d: %f baud", s, sp.baud());
    }
    CHECK(std::fabs(kbd33::fsqSpec(2).bandHz() - 290.0) < 1.0 && std::fabs(kbd33::ifkpSpec(1).bandHz() - 386.7) < 1.0, "bands");

    // entries of the published tables (fldigi: fsq_varicode / ifkp_varicode)
    expectCode(false, 'a', 1, -1); expectCode(false, 'z', 26, -1); expectCode(false, ' ', 0, -1); expectCode(false, '\n', 28, -1); expectCode(false, '.', 27, -1);
    expectCode(false, 'A', 1, 29); expectCode(false, '@', 0, 29); expectCode(false, ',', 27, 29); expectCode(false, '?', 28, 29);
    expectCode(false, '0', 10, 30); expectCode(false, '1', 1, 30); expectCode(false, '!', 11, 30); expectCode(false, '~', 0, 30); expectCode(false, '>', 27, 30);
    expectCode(false, '=', 0, 31); expectCode(false, '[', 1, 31); expectCode(false, 8, 27, 31); expectCode(false, 127, 28, 31); expectCode(false, 163, 14, 31);
    expectCode(true, ' ', 28, -1); expectCode(true, '\n', 28, 30); expectCode(true, 'a', 1, -1); expectCode(true, '.', 27, -1);
    expectCode(true, 'A', 1, 29); expectCode(true, '9', 9, 30); expectCode(true, ',', 27, 29);
    int a, b;
    CHECK(!kbd33::encode(false, 0, a, b) && !kbd33::encode(false, 200, a, b), "NUL and a character without a code are refused");

    // every character with a code comes back, alone and in a text
    for (bool ifkp : {false, true}) {
        int n = 0;
        for (int ch = 1; ch < 256; ch++) {
            int s1, s2;
            if (!kbd33::encode(ifkp, ch, s1, s2)) continue;
            n++;
            kbd33::CharDecoder d(ifkp);
            int got = d.push(s1);
            if (s2 >= 0) got = d.push(s2); else got = d.push(0) ;   // a following space-ish symbol completes a one-symbol character
            if (s2 < 0 && ifkp) {   // IFKP: the symbol that follows is symbol 0 = idle
                CHECK(got == ch, "IFKP %d decoded as %d", ch, got);
            } else if (s2 < 0) {
                CHECK(got == ch, "FSQ %d decoded as %d", ch, got);
            } else {
                CHECK(got == ch, "%s %d (%d,%d) decoded as %d", ifkp ? "IFKP" : "FSQ", ch, s1, s2, got);
            }
        }
        CHECK(n >= 100, "%d characters have codes", n);
    }
    {
        const std::string text = "cq cq cq de ly2abc/p ly2abc/p kn54 Test 73, 599 ?!@#$%&*()-+=[]{}<>:;'\"/\\|~`^_ \n";
        for (bool ifkp : {false, true}) {
            const std::string got = decodeTones(ifkp, kbd33::toneSequence(ifkp, text, 7, 2));
            CHECK(got == text, "%s text came back as \"%s\"", ifkp ? "IFKP" : "FSQ", got.c_str());
        }
        // the first tone is a reference and any start gives the same text
        for (int st = 0; st < 33; st += 4) CHECK(decodeTones(false, kbd33::toneSequence(false, "hello", st, 2)) == "hello", "start tone %d", st);
        // a symbol is never the same tone twice
        const auto t = kbd33::toneSequence(false, "aaaa eeee    ....", 3, 2);
        for (size_t i = 1; i < t.size(); i++) CHECK(t[i] != t[i - 1], "repeated tone at %zu", i);
        for (int v : t) CHECK(v >= 0 && v < 33, "tone %d", v);
    }
    CHECK(kbd33::crc8hex("123456789") == "f4", "CRC-8 check value: %s", kbd33::crc8hex("123456789").c_str());
    CHECK(kbd33::crc8hex("").size() == 2 && kbd33::crc8hex("LY2ABC") != kbd33::crc8hex("LY2ABD"), "CRC");

    // lines
    {
        const std::string crc = kbd33::crc8hex("LY2ABC");
        Kbd33Msg m = fsqParseLine(" LY2ABC:" + crc + "LY2XYZ? snr please", "ly2xyz");
        CHECK(m.from == "LY2ABC" && m.crcOk && m.to == "LY2XYZ" && m.trigger == '?' && m.text == "snr please" && m.toMe, "directed: from %s crc %d to %s trig %c text \"%s\" toMe %d", m.from.c_str(), m.crcOk, m.to.c_str(), m.trigger, m.text.c_str(), m.toMe);
        m = fsqParseLine("LY2ABC:" + crc + "allcall$", "");
        CHECK(m.to == "allcall" && m.trigger == '$' && m.text.empty() && !m.toMe, "allcall");
        m = fsqParseLine("LY2ABC:00LY2XYZ hello", "LY2XYZ");
        CHECK(m.from == "LY2ABC" && !m.crcOk && m.to == "LY2XYZ" && m.trigger == ' ' && m.text == "hello", "bad CRC: from %s crc %d to %s text \"%s\"", m.from.c_str(), m.crcOk, m.to.c_str(), m.text.c_str());
        m = fsqParseLine("cq cq cq de ly2abc", "LY2XYZ");
        CHECK(m.from.empty() && m.to.empty() && m.text == "cq cq cq de ly2abc", "plain text");
        m = fsqParseLine("LY2ABC:" + crc + "cqcqcq cq contest", "LY2XYZ");
        CHECK(m.to == "cqcqcq" && m.toMe, "cqcqcq goes to everybody");
        CHECK(std::string(fsqTriggerMeaning('?')).size() > 3 && std::string(fsqTriggerMeaning('Z')).empty(), "trigger meanings");
        const std::string f = fsqFrame("LY2ABC", "LY2XYZ? hi", true);
        CHECK(f == " \nLY2ABC:" + crc + "LY2XYZ? hi  \b  ", "frame \"%s\"", f.c_str());
    }
    return testSummary("fsq codec");
}
