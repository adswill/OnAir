// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRAPOL frame coding (PAS 0001-2 clause 6), data link (blocks, HDLC, TPDU, TSDU) and the receiver on the control-channel test signal:
// both polarities (automatic and set by hand), both band codings, frequency and clock offsets, noise.
#include "dect2/tetrapol/tetrapol_codec.h"
#include "dect2/tetrapol/tetrapol_gen.h"
#include "dect2/tetrapol/tetrapol_link.h"
#include "dect2/tetrapol/tetrapol_rx.h"
#include "dect2/tetrapol/tetrapol_tel.h"
#include "dect2/engine/modes.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
namespace tp = dect2::tetrapol;
#include "../common/test_util.h"

static void testTables() {
    for (int band : {tp::kVhf, tp::kUhf})
        for (int type : {tp::kVoice, tp::kData}) {
            bool seen[tp::kDataBits] = {};
            bool perm = true;
            for (int j = 0; j < tp::kDataBits; j++) {
                const int k = tp::interleaveIndex(j, band, type);
                if (k < 0 || k >= tp::kDataBits || seen[k]) perm = false; else seen[k] = true;
            }
            CHECK(perm, "interleave band %d type %d is not a permutation", band, type);
        }
    // the first 52 positions must agree between voice and data (the type bit is read before the type is known)
    for (int band : {tp::kVhf, tp::kUhf})
        for (int j = 0; j < 52; j++)
            CHECK(tp::interleaveIndex(j, band, tp::kVoice) == tp::interleaveIndex(j, band, tp::kData), "first part differs at %d", j);
    // scrambling sequence: period 127, seven ones first
    int ones = 0;
    for (int k = 0; k < 127; k++) ones += tp::scrambleBit(k, 127);
    CHECK(ones == 64, "m-sequence weight %d", ones);
    CHECK(tp::scrambleBit(0, 127) == 1 && tp::scrambleBit(7, 127) == 0, "sequence start");
}

static void testFrames() {
    std::mt19937 rng(7);
    int bad = 0, fixedOk = 0, tries = 0;
    for (int band : {tp::kVhf, tp::kUhf})
        for (int scr = 0; scr <= 127; scr++) {
            tp::DataFrame d;
            d.scramble = scr;
            for (auto& b : d.payload) b = (uint8_t)(rng() & 1);
            d.asb[0] = (uint8_t)(rng() & 1);
            uint8_t f160[tp::kFrameBits];
            tp::encodeDataFrame(d, band, f160);
            uint8_t h = 0;
            for (int i = 0; i < 8; i++) h |= (uint8_t)(f160[i] << i);
            if (h != tp::kFrameHdr) bad++;
            tp::Frame out;
            if (!tp::decodeFrame(f160 + 8, band, scr, out) || out.type != tp::kData || out.metric != 0 ||
                memcmp(out.data.payload, d.payload, sizeof d.payload) || out.data.asb[0] != d.asb[0]) bad++;
            // a few coded bits wrong: the circular code puts them right
            uint8_t e[tp::kDataBits];
            memcpy(e, f160 + 8, sizeof e);
            for (int k = 0; k < 2; k++) e[rng() % tp::kDataBits] ^= 1;
            tries++;
            if (tp::decodeFrame(e, band, scr, out) && !memcmp(out.data.payload, d.payload, sizeof d.payload)) fixedOk++;
            // the wrong constant must not decode as this frame
            tp::Frame w;
            if (scr > 0 && tp::decodeFrame(f160 + 8, band, (scr + 1) % 128, w) && w.metric == 0) bad++;

            tp::VoiceFrame v;
            v.scramble = scr;
            for (auto& b : v.codec) b = (uint8_t)(rng() & 1);
            tp::encodeVoiceFrame(v, band, f160);
            if (!tp::decodeFrame(f160 + 8, band, scr, out) || out.type != tp::kVoice || memcmp(out.voice.codec, v.codec, sizeof v.codec)) bad++;
        }
    CHECK(bad == 0, "%d frame round trips failed", bad);
    CHECK(fixedOk >= tries * 9 / 10, "two coded bit errors corrected in %d of %d", fixedOk, tries);
}

static void testStream() {
    // the test cycle as it goes on air: header found after the differential, both polarities, 152 bits back
    for (int inv = 0; inv < 2; inv++) {
        std::vector<uint8_t> x = tetrapolTestBits(0, kTetrapolTestScramble, tp::kUhf);
        std::vector<uint8_t> e = x;
        tp::diffEncode(e.data(), (int)e.size(), 0);
        if (inv) for (auto& b : e) b ^= 1;
        int okFrames = 0, n = (int)(e.size() / tp::kFrameBits);
        for (int f = 1; f < n; f++) {   // frame 0's first bit depends on the bit before the stream
            const uint8_t* r = &e[(size_t)f * tp::kFrameBits];
            if (tp::syncErrors(r, inv == 1) != 0 || tp::syncErrors(r, inv == 0) == 0) continue;
            uint8_t b[tp::kDataBits];
            tp::frameBits152(r, inv == 1, b);
            if (!memcmp(b, &x[(size_t)f * tp::kFrameBits + 8], tp::kDataBits)) okFrames++;
        }
        CHECK(okFrames == n - 1, "polarity %d: %d of %d frames", inv, okFrames, n - 1);
    }
}

static void testLink() {
    for (int nb : {8, 16, 24, 40}) {
        std::vector<uint8_t> bytes((size_t)nb);
        for (int i = 0; i < nb; i++) bytes[(size_t)i] = (uint8_t)(i * 37 + 11);
        std::vector<tp::DataFrame> fr = tp::blockFrames(bytes, 9);
        tp::BlockAssembler as;
        std::vector<uint8_t> got;
        int done = 0;
        for (size_t i = 0; i < fr.size(); i++) done += as.push(fr[i], true, got) ? 1 : 0;
        CHECK(done == 1 && got == bytes, "block of %d bytes: %d blocks, %zu bytes back", nb, done, got.size());
        if (nb >= 24) {   // one frame lost: the parity frame repairs it
            tp::BlockAssembler a2;
            done = 0;
            for (size_t i = 0; i < fr.size(); i++) {
                tp::DataFrame f = fr[i];
                const bool lost = i == 1;
                if (lost) memset(f.payload, 0, sizeof f.payload);
                done += a2.push(f, !lost, got) ? 1 : 0;
            }
            CHECK(done == 1 && got == bytes && a2.repaired() == 1, "parity repair of a %d byte block", nb);
        }
    }
    // HDLC with the FCS, TPDU UI, TSDU
    tp::SysInfo si;
    si.country = 33; si.systemId = 7; si.locArea = 101; si.bnId = 2; si.bsId = 18; si.rswId = 3; si.uScramble = 5;
    const std::vector<uint8_t> t = tp::buildSysInfo(si);
    const int nb = tp::uiBlockBytes(t.size());
    std::vector<uint8_t> h = tp::buildHdlc(0, 7, 0xfff, tp::kCmdUi, tp::uiInfo(t, nb), nb);
    tp::Hdlc hd;
    CHECK(tp::parseHdlc(h, hd) && hd.y == 7 && hd.x == 0xfff, "hdlc");
    std::vector<uint8_t> back;
    CHECK(tp::tsduOf(hd, back) && back == t, "tsdu");
    tp::SysInfo so;
    CHECK(tp::parseSysInfo(back.data(), (int)back.size(), so) && so.country == 33 && so.systemId == 7 && so.locArea == 101 && so.bsId == 18 &&
          so.rswId == 3 && so.bnId == 2, "sysinfo fields");
    for (int bit = 0; bit < nb * 8; bit += 7) {
        std::vector<uint8_t> c = h;
        c[(size_t)(bit / 8)] ^= (uint8_t)(1u << (bit % 8));
        if (tp::fcsOk(c.data(), nb)) { CHECK(false, "FCS missed a bit error at %d", bit); break; }
    }
    // a single-frame UI (2 TSDU bytes, no length byte)
    const std::vector<uint8_t> end = tp::buildCallEnd();
    h = tp::buildHdlc(0, 7, 0, tp::kCmdUi, tp::uiInfo(end, 8), 8);
    tp::CallSignal cs;
    CHECK(tp::parseHdlc(h, hd) && tp::tsduOf(hd, back) && tp::parseCallSignal(back.data(), (int)back.size(), cs) && cs.kind == tp::CallSignal::CallEnd, "call end");
    const std::vector<uint8_t> ga = tp::buildGroupActivation(1234, 40, 5, 0x13);
    CHECK(tp::parseCallSignal(ga.data(), (int)ga.size(), cs) && cs.kind == tp::CallSignal::GroupActivation && cs.groupId == 1234 &&
          cs.channelId == 40 && cs.keyRef == 0x13 && !tp::keyRefClear(cs.keyRef), "group activation");
    const std::vector<uint8_t> st = tp::buildCallStart(0x00);
    CHECK(tp::parseCallSignal(st.data(), (int)st.size(), cs) && cs.kind == tp::CallSignal::CallStart && tp::keyRefClear(cs.keyRef), "call start clear");
    const std::vector<uint8_t> st2 = tp::buildCallStart(-1);
    // PAS 0001-3-2 5.3.41: only key type 0 (RNK) or 15 (ESC) with key index 0 is a clear call; type 5 index 0 is the default OAK
    CHECK(tp::keyRefClear(0x00) && tp::keyRefClear(0xf0) && !tp::keyRefClear(0x50) && !tp::keyRefClear(0x10) && !tp::keyRefClear(0x4e) &&
          !tp::keyRefClear(0xfe), "key reference table");
    CHECK(tp::parseCallSignal(st2.data(), (int)st2.size(), cs) && cs.keyRef == -1 && !tp::keyRefClear(cs.keyRef), "call start without key reference");
}

static TetrapolTelemetry run(double snr, double cfo, double ppm, double rate, int polarity, int band, double secs, int rxPol = 0, int rxBand = 0) {
    SynthConfig sc;
    sc.mode = 81; sc.snrDb = snr; sc.cfoHz = cfo; sc.sroPpm = ppm;
    sc.modeOpt[1] = 0;
    sc.modeOpt[2] = polarity;
    sc.modeOpt[3] = band;
    auto syn = makeTetrapolSynth(sc, rate);
    TetrapolReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(kTetrapolTestCentreHz);
    rx.setPolarity(rxPol);
    rx.setBand(rxBand);
    rx.reset();
    const size_t chunk = (size_t)std::max(1024.0, rate * 0.05);
    std::vector<cf32> buf(chunk);
    TetrapolTelemetry t;
    for (double done = 0; done < secs; done += (double)chunk / rate) {
        syn->generate(buf.data(), chunk);
        rx.feed(buf.data(), chunk);
    }
    rx.telemetry(t, 0);
    return t;
}

static bool cellOk(const TetrapolTelemetry& t) {
    for (const auto& c : t.cells)
        if (c.hasSysinfo && c.country == kTetrapolTestCountry && c.systemId == kTetrapolTestSystem && c.cellId == kTetrapolTestCell &&
            c.rswId == kTetrapolTestRsw && c.locArea == kTetrapolTestLa && c.bnId == kTetrapolTestBn && c.scramble == kTetrapolTestScramble)
            return true;
    return false;
}

static bool hasMsg(const TetrapolTelemetry& t, const char* channel, const char* text) {
    for (const auto& m : t.msgs)
        if (m.channel == channel && m.text.find(text) != std::string::npos) return true;
    return false;
}

// Everything the control channel test signal sends must come out, each on the logical channel of its superframe position.
static bool controlChannelOk(const TetrapolTelemetry& t) {
    char paged[32], ack[32];
    snprintf(paged, sizeof paged, "paged %s", tp::rtiText(0, 0, kTetrapolTestGroup).c_str());
    snprintf(ack, sizeof ack, "ACK %s", tp::rtiText(0, 1, kTetrapolTestRchX).c_str());
    bool nb = false;
    for (const auto& n : t.neighbours) nb |= n.channelId == kTetrapolTestNeighbourCh && n.bnNb == 2 && n.bsId == 19 && n.rswId == 3;
    const bool ok = t.channelKind == 1 && t.superframePos >= 0 && hasMsg(t, "BCH", "D_SYSTEM_INFO") && hasMsg(t, "PCH", paged) &&
                    hasMsg(t, "RCH", ack) && hasMsg(t, "SDCH", "D_NEIGHBOURING_CELL: 2 adjacent cells") && t.neighbours.size() == 2 && nb &&
                    hasMsg(t, "SDCH", "group 1234 coverage") && t.stuffing > 0;
    if (!ok) {
        printf("    kind %d sfn %d stuffing %llu neighbours %zu\n", t.channelKind, t.superframePos, (unsigned long long)t.stuffing, t.neighbours.size());
        for (const auto& m : t.msgs) printf("    %s x%llu %s\n", m.channel.c_str(), (unsigned long long)m.count, m.text.c_str());
    }
    return ok;
}

// A retune while running: the signal moves (the tuned frequency changed inside the capture: only the offset) or the radio's centre
// changed (another carrier). The receiver must find the new carrier again by itself within a few seconds.
static double relockSecs(double rate, double cfo1, double cfo2, double newCentre, int pol2, int scr2) {
    TetrapolReceiver rx;
    rx.configure(rate);
    rx.setCenterHz(kTetrapolTestCentreHz);
    rx.setSignalOffset(cfo1);
    rx.reset();
    const size_t chunk = (size_t)std::max(1024.0, rate * 0.05);
    std::vector<cf32> buf(chunk);
    uint64_t okAtRetune = 0;
    auto feedFor = [&](ModeSynth& syn, double secs, double& t0, double centre, bool watch) {
        double found = -1;
        for (double done = 0; done < secs; done += (double)chunk / rate) {
            syn.generate(buf.data(), chunk);
            rx.feed(buf.data(), chunk);
            if (watch && found < 0) {
                TetrapolTelemetry t;
                rx.telemetry(t, 0);
                // 30 good blocks after the retune, and the cell of the new centre heard with its system info
                if (t.blocksOk >= okAtRetune + 30)
                    for (const auto& c : t.cells)
                        if (c.hasSysinfo && c.freqHz == centre && c.scramble == scr2) found = done + (double)chunk / rate;
            }
        }
        t0 += secs;
        return found;
    };
    SynthConfig a;
    a.mode = 81; a.snrDb = 25; a.cfoHz = cfo1; a.sroPpm = 20;
    auto s1 = makeTetrapolSynth(a, rate);
    double t = 0;
    feedFor(*s1, 3.0, t, kTetrapolTestCentreHz, false);
    // the retune: a short gap of noise (the radio settling), then the new carrier
    SynthConfig g;
    g.mode = 81; g.snrDb = -10; g.cfoHz = 0;
    auto gap = makeTetrapolSynth(g, rate);
    feedFor(*gap, 0.3, t, 0, false);
    { TetrapolTelemetry t0; rx.telemetry(t0, 0); okAtRetune = t0.blocksOk; }
    rx.setSignalOffset(cfo2);
    rx.setCenterHz(newCentre);
    SynthConfig b;
    b.mode = 81; b.snrDb = 25; b.cfoHz = cfo2; b.sroPpm = -30;
    b.modeOpt[0] = scr2; b.modeOpt[2] = pol2;
    auto s2 = makeTetrapolSynth(b, rate);
    return feedFor(*s2, 12.0, t, newCentre, true);
}

static void testRetune() {
    struct Case { double rate, cfo1, cfo2, centre; int pol2, scr2; const char* what; };
    const Case cases[] = {
        {250000, 0, 3000, kTetrapolTestCentreHz, 0, 5, "tuned frequency +3 kHz, same centre"},
        {250000, 800, -4200, kTetrapolTestCentreHz, 0, 5, "tuned frequency -5 kHz, offset impairment"},
        {250000, 0, 0, kTetrapolTestCentreHz + 1.5e6, 0, 5, "centre changed"},
        {250000, 600, -900, kTetrapolTestCentreHz + 2.5e6, 1, 9, "centre changed, other cell, inverted, carrier offset"},
        {2400000, 0, 12500, kTetrapolTestCentreHz, 0, 5, "2.4 Msps, next channel"},
    };
    for (const Case& c : cases) {
        const double s = relockSecs(c.rate, c.cfo1, c.cfo2, c.centre, c.pol2, c.scr2);
        printf("  retune (%s): locked again after %.2f s\n", c.what, s);
        CHECK(s > 0 && s < 8.0, "no relock after a retune: %s (%.2f)", c.what, s);
    }
}

// Frames heard on a real TETRAPOL network (issue #408 recording, UHF, control channel and a traffic channel; infrastructure messages only):
// FCS, message parsing (PAS 0001-3-2 layouts) and the text shown.
static std::vector<uint8_t> hexBytes(const char* h) {
    std::vector<uint8_t> v;
    for (const char* p = h; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        unsigned x = 0;
        sscanf(p, "%2x", &x);
        v.push_back((uint8_t)x);
        p += 2;
    }
    return v;
}
static std::string blockText(const std::vector<uint8_t>& b, std::string* talker = nullptr) {
    tp::Hdlc h;
    if (!tp::parseHdlc(b, h)) return "(fcs)";
    if (h.cmd == tp::kCmdUiVch) return tp::describeVch(h.info.data(), (int)h.info.size());
    if (h.cmd == tp::kCmdUiCd) return tp::describeCd(h.info.data(), (int)h.info.size(), talker);
    std::vector<uint8_t> t;
    if (!tp::tsduOf(h, t)) return "(no tsdu)";
    return tp::describeTsdu(t.data(), (int)t.size());
}
static void testRealFrames() {
    struct Case { const char* hex; const char* want; };
    const Case cases[] = {
        {"7f ff 03 00 11 90 10 62 03 36 00 1d 2b 02 90 41 00 63 fc 00 11 3f 04 67",
         "D_SYSTEM_INFO: country 3 system 54 LA 0 BN 29 cell 43/0 cell BN 0x290, mux type 0, U scramble 65"},
        {"7f ff 03 35 09 55 11 37 c8 0a f1 4b 6a 4e 2c 9e",
         "D_GROUP_ACTIVATION: open channel (automatic with tone) group 311 coverage 0xc8 channel 2801 U scramble 75 D scramble 106 key ref 0x4e (encrypted"},
        {"7f ff 03 00 2f 94 08 00 0a b9 83 0a fb 83 0a fd 83 0b 0b 83 0b 02 83 0b 2a 83 0b 07 83 0a fc 83 02 10 16 00 17 00 00 00 1c 00 29 00 02 00 2d 00 2c 00 04 00 00 00 98 e4",
         "D_NEIGHBOURING_CELL: 8 adjacent cells: ch 2745 BN 0 cell 22/0; ch 2811 BN 0 cell 23/0;"},
        {"7f ff 03 00 31 92 46 80 4b 4c 00 10 ff ca 00 10 ff 76 00 10 05 6f 00 10 04 64 00 00 ff f1 00 10 ff c8 00 00 ff 68 00 10 04 81 00 10 ff fc 00 10 ff 7b 00 10 04 00 27 f2",
         "D_GROUP_LIST: revision 2, talk group coverage ids 0x4c 0xca"},
        {"7f ff 03 05 58 12 89 c8", "D_GROUP_IDLE: cause 18"},
        {"7f ff c3 00 00 00 49 e4", "D_CHANNEL_FREE"},
        {"7f ff c3 01 00 00 95 be", "D_FORCED_LISTENING"},
        {"7f ff 03 00 05 97 1a ef 31 32 00 00 00 00 b0 f7", "D_DDCH_DESCRIPTION: 1a ef 31 32"},
    };
    for (const Case& c : cases) {
        const std::string got = blockText(hexBytes(c.hex));
        CHECK(got.find(c.want) == 0, "real frame: got '%s', want '%s'", got.c_str(), c.want);
    }
    // the BCH D_SYSTEM_INFO says where it sits in the superframe (cell_state bch: frames 100..103)
    {
        const auto b = hexBytes(cases[0].hex);
        tp::Hdlc h; std::vector<uint8_t> t; tp::SysInfo si;
        CHECK(tp::parseHdlc(b, h) && tp::tsduOf(h, t) && tp::parseSysInfo(t.data(), (int)t.size(), si) && si.bch == 1 && si.superframe == 0x13f,
              "cell state / superframe count");
    }
    std::string text;
    CHECK(tp::describeRch(hexBytes("c0 00 c0 00 c0 00 8f 01"), text) && text == "RCH: NACK (noise), NACK (noise), NACK (noise)", "RCH: %s", text.c_str());
    CHECK(tp::isStuffing(hexBytes("70 00 03 ec 7c d2 09 0a")) && !tp::isStuffing(hexBytes("7f ff c3 00 00 00 49 e4")), "fill frames");
    CHECK(tp::describePch(hexBytes("28 00 00 00 00 00 00 00 01 37 70 00 70 00 70 00")) == "PCH: activation bitmap 28 00 00 00 00 00 00 00, paged 0.0.0x137", "PCH");
    // a TP_ADDRESS of the talking party (made up digits, the layout of the recording)
    std::string who;
    const std::string cd = blockText(tp::buildHdlc(0, 7, 0xfff, tp::kCmdUiCd, hexBytes("01 a0 11 23 45 67 89 ff ff ff ff"), 16), &who);
    CHECK(who == "123-4-56-789" && cd.find("TP_ADDRESS: talking party 123-4-56-789") == 0, "talking party: %s", cd.c_str());
}

static TetrapolTelemetry runScene(TetrapolReceiver& rx, int scene, int scr, double secs, double snr = 30, int pol = 0, int band = 0) {
    SynthConfig sc;
    sc.mode = 81; sc.snrDb = snr; sc.cfoHz = 0;
    sc.modeOpt[0] = scr; sc.modeOpt[1] = scene; sc.modeOpt[2] = pol; sc.modeOpt[3] = band;
    auto syn = makeTetrapolSynth(sc, 250000);
    std::vector<cf32> buf(12500);
    for (double done = 0; done < secs; done += 0.05) {
        syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
    TetrapolTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

// The traffic channel: the talking party (UI_CD), the voice channel state (UI_VCH) and the call, on both polarities and band codings.
static void testTraffic() {
    for (int pol = 0; pol < 2; pol++)
        for (int band = 0; band < 2; band++) {
            TetrapolReceiver rx;
            rx.configure(250000);
            rx.reset();
            const TetrapolTelemetry t = runScene(rx, 1, 9, 5.0, 25, pol, band);
            bool talker = false;   // the first call may start before the frame sync; a later one must carry its talking party
            for (const auto& c : t.calls) talker |= c.talker == kTetrapolTestTalker;
            const bool ok = t.channelKind == 2 && talker && hasMsg(t, "VCH", "D_START_SPEECH") && hasMsg(t, "VCH", "D_CHANNEL_FREE") &&
                            hasMsg(t, "SCH", "D_CALL_START key ref 0x00 (clear)") && hasMsg(t, "SCH", "D_CALL_END") && t.calls.front().verdict == 1 &&
                            t.stuffing > 0;
            if (!ok) for (const auto& m : t.msgs) printf("    %s x%llu %s\n", m.channel.c_str(), (unsigned long long)m.count, m.text.c_str());
            CHECK(ok, "traffic channel pol %d band %d: kind %d talker %d calls %zu", pol, band, t.channelKind, talker, t.calls.size());
        }
}

// A traffic channel whose own signalling has no key reference (as on the issue #408 network): the encryption state and the group come from
// the D_GROUP_ACTIVATION the control channel sent for that downlink scrambling constant.
static void testControlThenTraffic() {
    for (int enc = 0; enc < 2; enc++) {
        TetrapolReceiver rx;
        rx.configure(250000);
        rx.reset();
        runScene(rx, 0, kTetrapolTestScramble, 3.0);
        rx.reset();   // the retune to the traffic channel
        const TetrapolTelemetry t = runScene(rx, 3, kTetrapolTestScramble + enc, 5.0);
        const int wantGroup = kTetrapolTestGroup + enc, wantVerdict = enc ? 2 : 1;
        const bool ok = !t.calls.empty() && t.calls.back().groupId == wantGroup && t.calls.back().verdict == wantVerdict;
        CHECK(ok, "traffic channel after the control channel (%s): calls %zu group %d verdict %d", enc ? "encrypted" : "clear", t.calls.size(),
              t.calls.empty() ? -1 : t.calls.back().groupId, t.calls.empty() ? -1 : t.calls.back().verdict);
        if (enc) CHECK(t.voicePassed == 0, "encrypted call handed %llu frames to the codec", (unsigned long long)t.voicePassed);
    }
    // without the control channel the state stays unknown and nothing goes to the codec
    TetrapolReceiver rx;
    rx.configure(250000);
    rx.reset();
    const TetrapolTelemetry t = runScene(rx, 3, kTetrapolTestScramble, 5.0);
    CHECK(!t.calls.empty() && t.calls.back().verdict == 0 && t.voicePassed == 0, "unknown state alone");
}

// Noise sweep (SNR in the 12.5 kHz channel), a carrier drifting through +-2.5 kHz, and an adjacent channel 12.5 kHz away at the same level.
static void testImpairments() {
    double prev = -1;
    for (double snr : {6.0, 8.0, 10.0, 12.0, 15.0, 20.0}) {
        const TetrapolTelemetry t = run(snr, 0, 0, 250000, 1, 0, 6.0);
        const double tot = (double)(t.blocksOk + t.blocksBad);
        const double rate = tot > 0 ? (double)t.blocksOk / tot : 0;
        printf("  noise sweep: snr %4.1f dB, frames ok %5.1f %% (%llu), cell %s, superframe %s\n", snr, 100 * rate, (unsigned long long)t.blocksOk,
               cellOk(t) ? "yes" : "no", t.superframePos >= 0 ? "yes" : "no");
        if (snr >= 10) CHECK(cellOk(t) && t.superframePos >= 0, "no cell at %.0f dB", snr);
        if (snr >= 12) CHECK(rate >= 0.97, "frames ok %.3f at %.0f dB", rate, snr);
        if (snr >= 8) CHECK(rate >= prev - 0.03, "frame rate falls with more SNR (%.3f after %.3f)", rate, prev);
        prev = rate;
    }
    // drift: the carrier moves linearly from -2.5 kHz to +2.5 kHz over 8 s (a radio warming up, far faster than a real one)
    {
        SynthConfig sc;
        sc.mode = 81; sc.snrDb = 20;
        auto syn = makeTetrapolSynth(sc, 250000);
        TetrapolReceiver rx;
        rx.configure(250000);
        rx.reset();
        std::vector<cf32> buf(12500);
        double ph = 0;
        size_t n = 0;
        for (int k = 0; k < 160; k++) {
            syn->generate(buf.data(), buf.size());
            for (auto& v : buf) {
                const double f = -2500.0 + 5000.0 * (double)n++ / (8.0 * 250000);
                ph += 2 * 3.14159265358979323846 * f / 250000;
                v *= cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            rx.feed(buf.data(), buf.size());
        }
        TetrapolTelemetry t;
        rx.telemetry(t, 0);
        const double rate = (double)t.blocksOk / std::max<double>(1, (double)(t.blocksOk + t.blocksBad));
        printf("  drift -2.5 .. +2.5 kHz in 8 s at 20 dB: frames ok %.1f %%\n", 100 * rate);
        CHECK(cellOk(t) && controlChannelOk(t) && rate >= 0.95, "drift: frames ok %.3f", rate);
    }
    // an adjacent channel (another TETRAPOL carrier 12.5 kHz up, same level, other scrambling constant)
    {
        SynthConfig a, b;
        a.mode = b.mode = 81; a.snrDb = 25; b.snrDb = 200; b.cfoHz = 12500; b.modeOpt[0] = 40;
        auto sa = makeTetrapolSynth(a, 250000), sb = makeTetrapolSynth(b, 250000);
        TetrapolReceiver rx;
        rx.configure(250000);
        rx.reset();
        std::vector<cf32> ba(12500), bb(12500);
        float pa = 0, pb = 0;
        for (int k = 0; k < 100; k++) {
            sa->generate(ba.data(), ba.size());
            sb->generate(bb.data(), bb.size());
            for (size_t i = 0; i < ba.size(); i++) { pa += std::norm(ba[i]); pb += std::norm(bb[i]); }
            const float g = k == 0 ? 1.f : std::sqrt(pa / std::max(pb, 1e-9f));   // the neighbour at the wanted carrier's power
            for (size_t i = 0; i < ba.size(); i++) ba[i] += bb[i] * g;
            rx.feed(ba.data(), ba.size());
        }
        TetrapolTelemetry t;
        rx.telemetry(t, 0);
        const double rate = (double)t.blocksOk / std::max<double>(1, (double)(t.blocksOk + t.blocksBad));
        printf("  adjacent channel at the same level: frames ok %.1f %%, scramble %d\n", 100 * rate, t.scramble);
        CHECK(cellOk(t) && rate >= 0.95 && t.scramble == kTetrapolTestScramble, "adjacent channel: frames ok %.3f scramble %d", rate, t.scramble);
    }
}

int main() {
    // traffic channel frequency from a grant: French 10 kHz raster, Swiss 12.5 kHz, unknown control channel number
    CHECK(std::fabs(tetrapolChannelHz(400.0e6, 10, 14, 10000) - 400.04e6) < 1, "10 kHz raster, +4 channels");
    CHECK(std::fabs(tetrapolChannelHz(400.0e6, 10, 6, 12500) - 399.95e6) < 1, "12.5 kHz raster, -4 channels");
    CHECK(tetrapolChannelHz(400.0e6, -1, 6, 10000) < 0, "control channel number unknown");
    testTables();
    testFrames();
    testStream();
    testLink();
    testRealFrames();
    testTraffic();
    testControlThenTraffic();
    testImpairments();
    testRetune();
    const ModeTuning mt = tetrapolTuning();
    CHECK(mt.stdMode == 81 && modeTuningById("tetrapol") && modeTuning(81), "tuning table");

    struct Case { double snr, cfo, ppm, rate; int pol, band; };
    const Case cases[] = {
        {30, 0, 0, 250000, 0, 0}, {30, 0, 0, 250000, 1, 0}, {30, 0, 0, 250000, 0, 1}, {30, 0, 0, 250000, 1, 1},
        {25, 600, 0, 250000, 1, 0}, {25, -900, 40, 250000, 0, 0}, {25, 300, -60, 96000, 1, 1}, {18, 0, 0, 250000, 1, 0},
        {25, 0, 0, 64000, 0, 0},
    };
    for (const Case& c : cases) {
        const TetrapolTelemetry t = run(c.snr, c.cfo, c.ppm, c.rate, c.pol, c.band, 5.0);   // two BCH and a PCH of the 4 s superframe
        const bool ok = cellOk(t);
        int clear = 0, enc = 0, numbersOnEnc = 0;
        for (const auto& a : t.activations) {
            if (a.verdict == 1 && a.groupId == kTetrapolTestGroup && a.channelId == kTetrapolTestChannel && a.dScramble == kTetrapolTestScramble) clear++;
            if (a.verdict == 2) { enc++; if (a.groupId == kTetrapolTestGroup + 1 && a.channelId == kTetrapolTestChannel + 1) numbersOnEnc++; }
        }
        printf("  snr %.0f cfo %+.0f ppm %+.0f rate %.0f pol %d band %d: %s; ok %llu bad %llu pol %d band %d scr %d, calls clear %d enc %d\n",
               c.snr, c.cfo, c.ppm, c.rate, c.pol, c.band, tetrapolSummary(t).c_str(), (unsigned long long)t.blocksOk,
               (unsigned long long)t.blocksBad, t.polarity, t.band, t.scramble, clear, enc);
        CHECK(ok, "no cell (snr %.0f cfo %.0f ppm %.0f pol %d band %d)", c.snr, c.cfo, c.ppm, c.pol, c.band);
        CHECK(t.polarity == (c.pol ? 2 : 1), "polarity found %d, sent %d", t.polarity, c.pol);
        CHECK(t.band == (c.band ? tp::kVhf : tp::kUhf), "band found %d", t.band);
        // signalling is in the clear for both calls: the encrypted call's group and channel are shown too (its voice is never decoded)
        CHECK(clear == 1 && enc == 1 && numbersOnEnc == 1, "activations clear %d enc %d (numbers of the encrypted call: %d)", clear, enc, numbersOnEnc);
        CHECK(controlChannelOk(t), "control channel signalling (snr %.0f cfo %.0f pol %d band %d)", c.snr, c.cfo, c.pol, c.band);
    }
    // a 20 ppm radio at 400 MHz is 8 kHz out (the sweep uses a quarter of the channel, 3.1 kHz): near the threshold the frames decoded must stay close to
    // those of a signal on the centre (before the frequency control 3.1 kHz at 12 dB gave half of them)
    for (double cfo : {3125.0, -3125.0}) {
        const TetrapolTelemetry on = run(12, 0, 0, 250000, 0, 0, 3.0), off = run(12, cfo, 0, 250000, 0, 0, 3.0);
        printf("  snr 12: centre ok %llu, cfo %+.0f ok %llu\n", (unsigned long long)on.blocksOk, cfo, (unsigned long long)off.blocksOk);
        CHECK(off.blocksOk * 10 >= on.blocksOk * 8, "cfo %+.0f at 12 dB: %llu frames against %llu on the centre", cfo, (unsigned long long)off.blocksOk, (unsigned long long)on.blocksOk);
    }
    // polarity set by hand: the right one works, the wrong one never syncs
    {
        const TetrapolTelemetry r = run(30, 0, 0, 250000, 1, 0, 3.0, 2);
        CHECK(cellOk(r) && !r.polarityAuto, "inverted set by hand");
        const TetrapolTelemetry w = run(30, 0, 0, 250000, 1, 0, 2.0, 1);
        CHECK(!cellOk(w) && w.blocksOk == 0, "wrong polarity by hand decoded %llu frames", (unsigned long long)w.blocksOk);
        const TetrapolTelemetry b = run(30, 0, 0, 250000, 0, 1, 2.0, 0, 2);
        CHECK(!cellOk(b), "UHF coding forced on a VHF signal");
    }
    // noise only: no cell, no frames
    {
        const TetrapolTelemetry n = run(-200, 0, 0, 250000, 0, 0, 0.0);
        (void)n;
        SynthConfig sc;
        TetrapolReceiver rx;
        rx.configure(250000);
        rx.reset();
        std::mt19937 g(3);
        std::normal_distribution<float> nd(0.f, 0.1f);
        std::vector<cf32> buf(12500);
        for (int k = 0; k < 60; k++) {
            for (auto& v : buf) v = cf32(nd(g), nd(g));
            rx.feed(buf.data(), buf.size());
        }
        TetrapolTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.cells.empty() || !t.cells.front().hasSysinfo, "a cell out of noise");
        CHECK(t.voiceFrames == 0 && t.tsdus == 0, "frames out of noise: voice %llu tsdu %llu", (unsigned long long)t.voiceFrames, (unsigned long long)t.tsdus);
    }
    return testSummary("tetrapol_rx");
}
