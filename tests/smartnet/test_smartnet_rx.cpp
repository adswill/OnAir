// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Motorola Type II: the OSW codec, the band plans, the message layer, and the whole receiver against the generator (one wide capture: control channel and analog voice channels), with
// a frequency offset, a clock offset, an inverted data polarity, corrupted words and a noise sweep. Generated signals only: no real SmartNet / SmartZone system was heard.
#include "dect2/smartnet/smartnet_gen.h"
#include "dect2/smartnet/smartnet_proto.h"
#include "dect2/smartnet/smartnet_rx.h"
#include "dect2/smartnet/smartnet_tel.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::smartnet;
#include "../common/test_util.h"

static double toneShare(const std::vector<float>& a, double f, double fs) {
    std::vector<float> nz;
    for (float v : a) if (v != 0) nz.push_back(v);
    if (nz.size() < 4800) return 0;
    double c = 2 * std::cos(2 * M_PI * f / fs), s1 = 0, s2 = 0, tot = 0;
    for (float v : nz) { const double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; tot += (double)v * v; }
    return tot > 0 ? 2 * (s1 * s1 + s2 * s2 - c * s1 * s2) / ((double)nz.size() * tot) : 0;
}

static void testCodec() {
    // the CRC of a fixed word (the OP25 algorithm run by hand on these bits: a regression guard)
    uint8_t zeros[27] = {};
    CHECK(crc(zeros) == 0x0393, "crc of all-zero data is %03x", crc(zeros));
    std::mt19937 rng(11);
    for (int i = 0; i < 2000; i++) {
        Osw o; o.addr = (int)(rng() & 0xFFFF); o.group = rng() & 1; o.cmd = (int)(rng() & 0x3FF);
        uint8_t p[kPayloadBits];
        encodePayload(o, p);
        Osw d; int fixed = -1;
        CHECK(decodePayload(p, d, &fixed), "round trip %d", i);
        CHECK(d == o && fixed == 0, "round trip value %d", i);
    }
    // the parity repairs one wrong information bit per word
    int repaired = 0;
    for (int i = 0; i < 400; i++) {
        Osw o; o.addr = (int)(rng() & 0xFFFF); o.group = rng() & 1; o.cmd = (int)(rng() & 0x3FF);
        uint8_t p[kPayloadBits];
        encodePayload(o, p);
        // a position in the sent order; only information bits (even raw index) are repairable
        int r = 2 * (int)(rng() % 36);
        int sent = -1;
        for (int k = 0; k < 19 && sent < 0; k++) for (int l = 0; l < 4; l++) if (k * 4 + l == r) sent = k + l * 19;
        p[sent] ^= 1;
        Osw d; int fixed = 0;
        if (decodePayload(p, d, &fixed) && d == o && fixed == 1) repaired++;
    }
    CHECK(repaired >= 390, "repaired %d of 400", repaired);
    // two bit flips that the parity cannot repair must not pass the CRC as a different word
    int passedWrong = 0;
    for (int i = 0; i < 3000; i++) {
        Osw o; o.addr = (int)(rng() & 0xFFFF); o.group = rng() & 1; o.cmd = (int)(rng() & 0x3FF);
        uint8_t p[kPayloadBits];
        encodePayload(o, p);
        for (int k = 0; k < 6; k++) p[rng() % kPayloadBits] ^= 1;
        Osw d;
        if (decodePayload(p, d) && !(d == o)) passedWrong++;
    }
    CHECK(passedWrong < 12, "%d wrong words passed the CRC", passedWrong);
}

static void testBands() {
    CustomPlan cp; cp.baseHz = 460e6; cp.baseSpacingHz = 12500;
    CHECK(std::fabs(channelHz(Band::Domestic800, cp, 0x014) - 851.5125e6) < 1, "domestic");
    CHECK(std::fabs(channelHz(Band::Domestic800, cp, 0x2d0) - 866.0e6) < 1, "upper");
    CHECK(channelHz(Band::Domestic800, cp, 0x300) == 0, "not a channel");
    CHECK(std::fabs(channelHz(Band::Rebanded800, cp, 0x1b8) - 851.025e6) < 1, "rebanded");
    CHECK(std::fabs(channelHz(Band::Splinter800, cp, 0x258) - 866.0125e6) < 1, "splinter");
    CHECK(std::fabs(channelHz(Band::International800, cp, 0x020) - 851.8125e6) < 1, "international");
    CHECK(std::fabs(channelHz(Band::InternationalSplinter800, cp, 0x020) - 851.8e6) < 1, "international splinter");
    CHECK(std::fabs(channelHz(Band::Us900, cp, 2) - 935.0375e6) < 1, "900");
    CHECK(std::fabs(channelHz(Band::Custom, cp, 400) - (460e6 + 20 * 12500)) < 1, "custom");
    CHECK(channelHz(Band::Custom, cp, 10) == 0 && isCustomTx(Band::Custom, cp, 10), "custom tx channels");
    CHECK(bandFromControl(0x014, 851.5125e6) == Band::Domestic800, "auto domestic");
    CHECK(bandFromControl(0x020, 851.8e6) == Band::Splinter800 || bandFromControl(0x020, 851.8e6) == Band::InternationalSplinter800, "auto splinter");
    CHECK(bandFromControl(5, 935.075e6) == Band::Us900, "auto 900");
    CHECK(bandFromControl(-1, 460e6) == Band::Auto, "auto nothing");
}

static std::vector<Msg> parseAll(std::vector<Osw> w) {
    Parser p;
    for (const auto& o : w) p.feed(o);
    for (int i = 0; i < 8; i++) { Osw idle; idle.cmd = 0x2F8; p.feed(idle); }
    std::vector<Msg> out;
    Msg m;
    while (p.next(m)) if (m.kind != Kind::Idle) out.push_back(m);
    return out;
}
static Osw W(int a, bool g, int c) { Osw o; o.addr = a; o.group = g; o.cmd = c; return o; }

static void testMessages() {
    auto m = parseAll({W(12345, true, 0x308), W(0x1232, true, 0x014)});   // analog grant, option 2 = emergency
    CHECK(m.size() == 1 && m[0].kind == Kind::GroupGrant && m[0].tg == 0x1230 && m[0].src == 12345 && !m[0].digital && m[0].emergency && std::fabs(m[0].freqHz - 851.5125e6) < 1, "analog group grant");
    m = parseAll({W(555, true, 0x321), W(0x4568, true, 0x020)});          // digital, encrypted (bit 3)
    CHECK(m.size() == 1 && m[0].kind == Kind::GroupGrant && m[0].digital && m[0].encrypted && m[0].tg == 0x4560, "digital grant");
    m = parseAll({W(0x4560, true, 0x020)});
    CHECK(m.size() == 1 && m[0].kind == Kind::GroupUpdate && m[0].tg == 0x4560, "update");
    m = parseAll({W(111, false, 0x308), W(222, false, 0x030)});
    CHECK(m.size() == 1 && m[0].kind == Kind::PrivateGrant && m[0].src == 222 && m[0].dst == 111 && !m[0].group, "private");
    m = parseAll({W(0, false, 0x308), W(222, false, 0x030)});
    CHECK(m.size() == 1 && m[0].kind == Kind::InterconnectGrant && m[0].src == 222, "interconnect");
    m = parseAll({W(0x4A2C, true, 0x308), W(0x1F12, false, 0x014)});
    CHECK(m.size() == 1 && m[0].kind == Kind::ControlChannel && m[0].system == 0x4A2C && m[0].chan == 0x014, "control channel");
    const int word1 = (2 << 10) | (3 << 7) | 0x1F;
    m = parseAll({W(0x4A2C, true, 0x308), W(word1, true, 0x320), W(0x6014, false, 0x30B)});
    CHECK(m.size() == 1 && m[0].kind == Kind::AltSite && m[0].system == 0x4A2C && m[0].site == 3 && m[0].band == 3 && m[0].features == 0x1F && isWideArea(m[0].features), "site info");
    m = parseAll({W(0x4A2C, true, 0x308), W(word1, true, 0x320), W(0x2F8 * 0, false, 0x2F8), W(0x6014, false, 0x30B)});
    CHECK(m.size() == 1 && m[0].kind == Kind::AltSite, "a lone idle inside a three-word message");
    m = parseAll({W(0x1233, true, 0x308), W(0x0456, true, 0x340)});
    CHECK(m.size() == 1 && m[0].kind == Kind::Patch && m[0].tg == 0x1230 && m[0].dst == 0x4560, "patch");
    m = parseAll({W((26 << 9) | (3 << 5) | 14, true, 0x308), W((1 << 13) | (17 << 8) | 45, true, 0x322)});
    CHECK(m.size() == 1 && m[0].kind == Kind::DateTime && m[0].year == 2026 && m[0].month == 3 && m[0].day == 14 && m[0].hour == 17 && m[0].minute == 45, "date");
    m = parseAll({W((1 << 13) | 0x1000 | 5, false, 0x3C0)});
    CHECK(m.size() == 1 && m[0].kind == Kind::SystemStatus && m[0].opcode == 1 && m[0].typeII, "status");
    // OBT: a transmit channel number first, then the receive channel
    Parser p;
    CustomPlan cp; cp.baseHz = 460e6;
    p.setBand(Band::Custom, cp);
    for (const auto& o : {W(777, true, 100), W(0x1230, true, 400)}) p.feed(o);
    for (int i = 0; i < 8; i++) { Osw idle; idle.cmd = 0x2F8; p.feed(idle); }
    Msg g; bool found = false;
    while (p.next(g)) if (g.kind == Kind::GroupGrant) found = g.tg == 0x1230 && g.src == 777 && !g.digital && g.chan == 400;
    CHECK(found, "OBT grant");
    // a lost word: the parts on either side are not joined into a false message
    Parser pb;
    pb.feed(W(12345, true, 0x308)); pb.bad(); pb.feed(W(0x1230, true, 0x014));
    for (int i = 0; i < 8; i++) { Osw idle; idle.cmd = 0x2F8; pb.feed(idle); }
    Msg x; bool grantFound = false;
    while (pb.next(x)) grantFound |= x.kind == Kind::GroupGrant;
    CHECK(!grantFound, "no grant across a lost word");
}

struct Run { SmartnetTelemetry t; std::map<int, std::vector<float>> audio; std::map<int, int> rate; };

static Run run(const SmartnetGenConfig& c, Band b = Band::Auto, bool knownCentre = true) {
    Run r;
    SmartnetReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    rx.setCenterHz(knownCentre ? c.centreHz : 0);
    rx.setSignalOffset(c.controlHz - c.centreHz + c.cfoHz);
    rx.setBand(b, c.custom);
    rx.setAudioTap([&](int tg, const float* x, size_t n, int rate) { auto& v = r.audio[tg]; v.insert(v.end(), x, x + n); r.rate[tg] = rate; });
    const auto sig = smartnetSystemSignal(c);
    const size_t step = 4096;
    for (size_t i = 0; i < sig.size(); i += step) rx.feed(sig.data() + i, std::min(step, sig.size() - i));
    // a little silence so that calls end
    std::vector<cf32> tail((size_t)(c.rate * 0.5), cf32(0, 0));
    std::mt19937 rng(c.seed + 9);
    std::normal_distribution<float> g(0.f, (float)c.noise);
    for (auto& v : tail) v = cf32(g(rng), g(rng));
    for (size_t i = 0; i < tail.size(); i += step) rx.feed(tail.data() + i, std::min(step, tail.size() - i));
    rx.telemetry(r.t, 0);
    return r;
}

static SmartnetGenConfig base() {
    SmartnetGenConfig c;
    SmartnetGenCall a; a.chan = 0x018; a.tg = 0x1230; a.src = 12345; a.startSec = 1.0; a.endSec = 3.0; a.toneHz = 1000;
    SmartnetGenCall b; b.chan = 0x01A; b.tg = 0x4560; b.src = 54321; b.startSec = 3.5; b.endSec = 5.0; b.toneHz = 1500;
    SmartnetGenCall e; e.chan = 0x022; e.tg = 0x7770; e.src = 777; e.options = 8; e.startSec = 2.0; e.endSec = 3.0; e.voiceCarrier = false;    // encrypted: logged, never followed
    c.calls = {a, b, e};
    return c;
}

static void testSystem(const char* name, SmartnetGenConfig c, Band band = Band::Auto) {
    Run r = run(c, band);
    CHECK(r.t.oswOk > 180, "%s: only %llu good words", name, (unsigned long long)r.t.oswOk);
    CHECK(r.t.systemId == c.system, "%s: system %d", name, r.t.systemId);
    CHECK(r.t.siteId == c.site && r.t.smartZone, "%s: site %d smartzone %d", name, r.t.siteId, (int)r.t.smartZone);
    CHECK(r.t.typeIIKnown && r.t.typeII, "%s: type", name);
    CHECK(r.t.talkgroups.size() >= 3, "%s: talkgroups %zu", name, r.t.talkgroups.size());
    int followed = 0;
    bool encLogged = false;
    for (const auto& x : r.t.calls) {
        if (x.tg == 0x1230 || x.tg == 0x4560) { followed += x.followed; CHECK(x.durationSec > 1.0, "%s: duration %f", name, x.durationSec); }
        if (x.tg == 0x7770) { encLogged = x.encrypted && !x.followed; }
    }
    CHECK(followed == 2, "%s: followed %d", name, followed);
    CHECK(encLogged, "%s: the encrypted call must be logged and not followed", name);
    CHECK(!r.audio.count(0x7770), "%s: no sound from the encrypted call", name);
    double s1 = toneShare(r.audio[0x1230], 1000, 48000), s2 = toneShare(r.audio[0x4560], 1500, 48000);
    CHECK(s1 > 0.5, "%s: tone of the first call %.2f", name, s1);
    CHECK(s2 > 0.5, "%s: tone of the second call %.2f", name, s2);
    printf("%-22s: %4llu words, %llu bad, snr %.1f dB, followed %d, tones %.2f %.2f, band %s\n", name, (unsigned long long)r.t.oswOk, (unsigned long long)r.t.oswBad, (double)r.t.snrDb, followed, s1, s2, r.t.band.c_str());
}

int main() {
    testCodec();
    testBands();
    testMessages();
    testSystem("clean", base());
    { auto c = base(); c.cfoHz = 900; testSystem("offset +900 Hz", c); }
    { auto c = base(); c.cfoHz = -1500; testSystem("offset -1500 Hz", c); }
    { auto c = base(); c.invert = true; testSystem("inverted", c); }
    { auto c = base(); c.clockPpm = 150; testSystem("clock +150 ppm", c); }
    { auto c = base(); c.clockPpm = -300; testSystem("clock -300 ppm", c); }
    { auto c = base(); c.deviationHz = 700; testSystem("deviation 700 Hz", c); }
    { auto c = base(); c.oswErrorRate = 0.05; testSystem("5% bad words", c); }
    { auto c = base(); c.idleInsideMessages = true; testSystem("idle inside messages", c); }
    { auto c = base(); c.rate = 1920000; testSystem("1.92 Msps", c); }
    { auto c = base(); c.rate = 600000; testSystem("600 ksps", c); }
    { auto c = base(); c.features = 0x0F; c.wideArea = false; Run r = run(c); CHECK(!r.t.smartZone, "a site trunk system is not SmartZone"); }
    for (double noise : {0.02, 0.05, 0.1, 0.16}) {
        auto c = base(); c.noise = noise;
        Run r = run(c);
        printf("noise %.3f: %llu good, %llu bad, snr %.1f dB, %zu calls\n", noise, (unsigned long long)r.t.oswOk, (unsigned long long)r.t.oswBad, (double)r.t.snrDb, r.t.calls.size());
        if (noise <= 0.05) CHECK(r.t.oswOk > 150, "noise %.2f: %llu good", noise, (unsigned long long)r.t.oswOk);
    }
    { // pure noise: no false lock
        SmartnetGenConfig c; c.durSec = 6; c.controlAmp = 0; c.noise = 0.1;
        Run r = run(c);
        CHECK(r.t.oswOk == 0, "pure noise produced %llu good words", (unsigned long long)r.t.oswOk);
        CHECK(!r.t.locked, "pure noise locked");
    }
    { // a call outside the capture is listed and not followed
        auto c = base();
        c.calls[0].chan = 0x200;   // 863.8 MHz, out of the 960 kHz capture
        Run r = run(c);
        bool ok = false;
        for (const auto& x : r.t.calls) if (x.tg == 0x1230) ok = x.missed && !x.followed && !x.note.empty();
        CHECK(ok, "out of capture");
    }
    return testSummary("smartnet_rx");
}
