// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB+ slide show: the PAD in the data stream element, X-PAD (variable and short, with and without content indicators), data groups, MOT segments.
// The generator's signal (dab_gen_pad.cpp, written from the standards without the receiver's code) goes through the PAD decoder directly, through
// DabAudio (Reed-Solomon, access units, libavcodec) and through the whole receiver; hand made PAD covers the other X-PAD forms and bad data.
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_gen.h"
#include "dect2/dab/dab_pad.h"
#include "dect2/dab/dab_announce.h"
extern "C" {
#include <libavformat/avformat.h>
}
#include <cmath>
#include <cstdio>
#include <string>
#include <cstring>
#include <vector>
using namespace dect2;

#include "../common/test_util.h"

static void expectedImage(int& w, int& h, std::vector<uint8_t>& px) { dabgen::slideImage(w, h, px); }

static bool sameImage(const DabSlide& s) {
    int w, h; std::vector<uint8_t> px;
    expectedImage(w, h, px);
    return s.w == w && s.h == h && s.rgba == px;
}

static dabgen::TxConfig config() {
    dabgen::TxConfig tc;
    tc.services = dabgen::defaultServices();
    tc.services[2].slide = true;     // "OnAir Melody", 64 kbit/s
    tc.utcSeconds = 1700000000;
    return tc;
}

static void testDirect() {
    dabgen::Transmitter tx(config());
    const auto& aus = tx.accessUnits(2);
    CHECK(aus.size() >= 400, "access units: %zu", aus.size());
    DabPadDecoder pd;
    for (const auto& au : aus) pd.accessUnit(au.data(), (int)au.size());
    CHECK(pd.seq() == 1, "one picture expected, seq %llu", (unsigned long long)pd.seq());
    CHECK(sameImage(pd.slide()), "pixels differ");
    CHECK(pd.slide().name == "OnAir test slide", "name '%s'", pd.slide().name.c_str());
    CHECK(pd.label() == "OnAir Melody - test tune", "label '%s'", pd.label().c_str());
    const uint64_t s1 = pd.seq();
    for (const auto& au : aus) pd.accessUnit(au.data(), (int)au.size());   // the carousel: the same object again is not a new picture
    CHECK(pd.seq() == s1, "repeat counted as new");
    // the services without PAD
    DabPadDecoder q;
    for (const auto& au : tx.accessUnits(0)) q.accessUnit(au.data(), (int)au.size());
    CHECK(q.seq() == 0, "picture from a service without PAD");
}

// a MOT object as a data group, built here
static std::vector<uint8_t> group(int type, const std::vector<uint8_t>& seg, bool goodCrc = true) {
    std::vector<uint8_t> g = {(uint8_t)(0x40 | 0x20 | 0x10 | type), 0x00, 0x80, 0x00, 0x12, 0x00, 0x07, (uint8_t)(seg.size() >> 8), (uint8_t)seg.size()};
    g.insert(g.end(), seg.begin(), seg.end());
    const uint16_t c = dabgen::crc16(g.data(), (int)g.size());
    g.push_back((uint8_t)(c >> 8)); g.push_back((uint8_t)c ^ (goodCrc ? 0 : 1));
    return g;
}
static std::vector<uint8_t> motHeader(size_t bodySize) {
    const size_t hs = 7;
    return {(uint8_t)(bodySize >> 20), (uint8_t)(bodySize >> 12), (uint8_t)(bodySize >> 4), (uint8_t)((bodySize << 4) | (hs >> 9)), (uint8_t)(hs >> 1), (uint8_t)(((hs & 1) << 7) | 4), 3};
}
static void dgli(DabPadDecoder& pd, size_t len) {
    uint8_t l[4] = {(uint8_t)((len >> 8) & 0x3F), (uint8_t)len, 0, 0};
    const uint16_t c = dabgen::crc16(l, 2);
    l[2] = (uint8_t)(c >> 8); l[3] = (uint8_t)c;
    const uint8_t xp[6] = {0x01, 0x00, l[0], l[1], l[2], l[3]};   // CI: type 1, length index 0; end marker
    const uint8_t fp[2] = {0x20, 0x02};
    pd.pad(xp, 6, fp);
}
// the data group in pieces of `piece` bytes: `how` 0 = every piece with a content indicator, 1 = indicator on the first only (variable X-PAD),
// 2 = short X-PAD (3 bytes after an indicator, then 4 without)
static void send(DabPadDecoder& pd, const std::vector<uint8_t>& g, int how) {
    dgli(pd, g.size());
    size_t p = 0;
    bool first = true;
    while (p < g.size()) {
        if (how == 2) {
            const size_t n = first ? 3 : 4;
            std::vector<uint8_t> xp;
            if (first) xp.push_back(12);
            for (size_t i = 0; i < n; i++) xp.push_back(p + i < g.size() ? g[p + i] : 0);
            const uint8_t fp[2] = {0x10, (uint8_t)(first ? 0x02 : 0x00)};
            pd.pad(xp.data(), (int)xp.size(), fp);
            p += n;
        } else if (how == 1 && !first) {
            std::vector<uint8_t> xp(16, 0);
            for (size_t i = 0; i < 16 && p + i < g.size(); i++) xp[i] = g[p + i];
            const uint8_t fp[2] = {0x20, 0x00};
            pd.pad(xp.data(), 16, fp);
            p += 16;
        } else {
            std::vector<uint8_t> xp = {(uint8_t)((4 << 5) | (first ? 12 : 13)), 0x00};
            for (size_t i = 0; i < 16; i++) xp.push_back(p + i < g.size() ? g[p + i] : 0);
            const uint8_t fp[2] = {0x20, 0x02};
            pd.pad(xp.data(), 18, fp);
            p += 16;
        }
        first = false;
    }
}
static void testForms() {
    const std::vector<uint8_t> png = dabgen::slidePng();
    for (int how = 0; how < 3; how++) {
        DabPadDecoder pd;
        send(pd, group(3, motHeader(png.size())), how);
        send(pd, group(4, png), how);
        CHECK(pd.seq() == 1 && sameImage(pd.slide()), "X-PAD form %d: seq %llu", how, (unsigned long long)pd.seq());
    }
    {   // a bad CRC in the body: no picture; the good one after it: the picture
        DabPadDecoder pd;
        send(pd, group(3, motHeader(png.size())), 0);
        send(pd, group(4, png, false), 0);
        CHECK(pd.seq() == 0, "picture from a data group with a bad CRC");
        send(pd, group(4, png), 0);
        CHECK(pd.seq() == 1 && sameImage(pd.slide()), "recovery");
    }
    {   // a body of the wrong size, and not an image
        DabPadDecoder pd;
        send(pd, group(3, motHeader(png.size() + 1)), 0);
        send(pd, group(4, png), 0);
        CHECK(pd.seq() == 0, "body size not checked");
        pd.pad(nullptr, 0, std::vector<uint8_t>{0x20, 0x02}.data());
        const uint8_t junk[5] = {0x80, 0xFF, 0xFF, 0x00, 0x00};
        pd.accessUnit(junk, 5);
        const uint8_t junk2[4] = {0x80, 0x02, 0x20, 0x02};
        pd.accessUnit(junk2, 4);
    }
}

// a label by hand: three segments, UTF-8, a toggle change, bad CRC, X-PAD without indicators in between
static void label(DabPadDecoder& pd, int toggle, int segNo, bool last, const char* text, int charset = 15, bool goodCrc = true) {
    const size_t n = std::strlen(text);
    std::vector<uint8_t> s = {(uint8_t)((toggle << 7) | (segNo == 0 ? 0x40 : 0) | (last ? 0x20 : 0) | (n - 1)), (uint8_t)(segNo == 0 ? charset << 4 : segNo << 4)};
    s.insert(s.end(), text, text + n);
    const uint16_t c = dabgen::crc16(s.data(), (int)s.size());
    s.push_back((uint8_t)(c >> 8)); s.push_back((uint8_t)c ^ (goodCrc ? 0 : 1));
    // sent as a type 2 start with 8 bytes, the rest as continuation without indicators
    std::vector<uint8_t> xp = {(uint8_t)((2 << 5) | 2), 0x00};
    xp.insert(xp.end(), s.begin(), s.begin() + 8);
    uint8_t fp[2] = {0x20, 0x02};
    pd.pad(xp.data(), (int)xp.size(), fp);
    for (size_t p = 8; p < s.size(); p += 8) {
        std::vector<uint8_t> x(8, 0);
        for (size_t i = 0; i < 8 && p + i < s.size(); i++) x[i] = s[p + i];
        fp[1] = 0x00;
        pd.pad(x.data(), 8, fp);
    }
}
static void testLabel() {
    DabPadDecoder pd;
    label(pd, 0, 0, false, "Beyonc\xC3\xA9 - ");
    CHECK(pd.label().empty(), "label before the last segment");
    label(pd, 0, 1, true, "Halo");
    CHECK(pd.label() == "Beyonc\xC3\xA9 - Halo", "label '%s'", pd.label().c_str());
    label(pd, 1, 0, true, "Next song", 15, false);       // bad CRC: ignored
    CHECK(pd.label() == "Beyonc\xC3\xA9 - Halo", "bad CRC accepted: '%s'", pd.label().c_str());
    label(pd, 1, 0, true, "Next song");
    CHECK(pd.label() == "Next song", "toggle change: '%s'", pd.label().c_str());
    const uint8_t clr[4] = {0x10 | 0x40 | 0x20 | 1, 0x00, 0, 0};   // command 1: clear display
    std::vector<uint8_t> s(clr, clr + 2);
    const uint16_t c = dabgen::crc16(s.data(), 2);
    s.push_back((uint8_t)(c >> 8)); s.push_back((uint8_t)c);
    const uint8_t xp[6] = {(uint8_t)((0 << 5) | 2), 0x00, 0, 0, 0, 0};
    std::vector<uint8_t> x = {(uint8_t)((0 << 5) | 2), 0x00};
    x.insert(x.end(), s.begin(), s.end());
    const uint8_t fp[2] = {0x20, 0x02};
    (void)xp;
    pd.pad(x.data(), (int)x.size(), fp);
    CHECK(pd.label().empty(), "clear display: '%s'", pd.label().c_str());
}

// a dynamic label segment or command by hand (prefix and data, CRC added), sent as a type 2 start with 8 bytes and continuations without indicators
static void dlsRaw(DabPadDecoder& pd, std::vector<uint8_t> s) {
    const uint16_t c = dabgen::crc16(s.data(), (int)s.size());
    s.push_back((uint8_t)(c >> 8)); s.push_back((uint8_t)c);
    std::vector<uint8_t> xp = {(uint8_t)((2 << 5) | 2), 0x00};
    xp.insert(xp.end(), s.begin(), s.begin() + std::min<size_t>(8, s.size()));
    while (xp.size() < 10) xp.push_back(0);
    uint8_t fp[2] = {0x20, 0x02};
    pd.pad(xp.data(), (int)xp.size(), fp);
    for (size_t p = 8; p < s.size(); p += 8) {
        std::vector<uint8_t> x(8, 0);
        for (size_t i = 0; i < 8 && p + i < s.size(); i++) x[i] = s[p + i];
        fp[1] = 0x00;
        pd.pad(x.data(), 8, fp);
    }
}
// DL Plus command: item toggle/running bits, tags (type, start, length - 1)
static void dlPlus(DabPadDecoder& pd, int toggle, bool running, std::vector<std::vector<int>> tags) {
    std::vector<uint8_t> s = {(uint8_t)((toggle << 7) | 0x40 | 0x20 | 0x10 | 2), (uint8_t)((tags.size() * 3) << 4)};   // field 2 is not evaluated
    s.push_back((uint8_t)((running ? 4 : 0) | (tags.size() - 1)));
    for (const auto& t : tags) for (int v : t) s.push_back((uint8_t)v);
    dlsRaw(pd, s);
}
static void testDlPlus() {
    DabPadDecoder pd;
    label(pd, 0, 0, false, "Beyonc\xC3\xA9 - ");
    label(pd, 0, 1, true, "Halo");
    CHECK(pd.dlPlus().empty(), "tags before the command");
    dlPlus(pd, 0, true, {{4, 0, 6}, {1, 10, 3}});
    CHECK(pd.dlPlus().size() == 2 && pd.dlPlus()[0].type == 4 && pd.dlPlus()[0].text == "Beyonc\xC3\xA9" && pd.dlPlus()[1].type == 1 && pd.dlPlus()[1].text == "Halo",
          "tags: %zu '%s'", pd.dlPlus().size(), pd.dlPlus().empty() ? "" : pd.dlPlus()[0].text.c_str());
    CHECK(pd.dlPlusRunning(), "item running");
    CHECK(pd.label() == "Beyonc\xC3\xA9 - Halo", "label kept by the command: '%s'", pd.label().c_str());
    label(pd, 1, 0, true, "Next song");
    CHECK(pd.dlPlus().empty(), "tags of the old label");
    dlPlus(pd, 1, false, {{1, 0, 8}, {0, 0, 0}});
    CHECK(pd.dlPlus().size() == 1 && pd.dlPlus()[0].text == "Next song" && !pd.dlPlusRunning(), "tags after the new label (dummy tag dropped)");
    dlPlus(pd, 1, true, {{4, 5, 40}});   // beyond the end: dropped
    CHECK(pd.dlPlus().empty(), "tag past the end of the label");
    CHECK(std::string(dabDlpTypeName(1)) == "Title" && std::string(dabDlpTypeName(4)) == "Artist", "type names");
    // the clear command still works and the command with a bad CRC changes nothing
    DabPadDecoder pd2;
    label(pd2, 0, 0, true, "Hello");
    std::vector<uint8_t> bad = {0xF2, 0x30, 0x04, 4, 0, 4};
    bad.push_back(0); bad.push_back(0);
    uint8_t fp[2] = {0x20, 0x02};
    std::vector<uint8_t> x = {(uint8_t)((2 << 5) | 2), 0x00};
    x.insert(x.end(), bad.begin(), bad.end());
    pd2.pad(x.data(), (int)x.size(), fp);
    CHECK(pd2.dlPlus().empty(), "tags from a bad CRC");
}

// DAB (MP2) stations: the PAD at the end of the audio frame. A frame of 128 kbit/s at 48 kHz (384 bytes) with the X-PAD sent as in label(): the type 2
// indicator and the end marker, 8 bytes of the segment, then continuations without indicators
static std::vector<uint8_t> mp2WithPad(const std::vector<uint8_t>& xp, uint8_t fp0, uint8_t fp1) {
    std::vector<uint8_t> f(384, 0x55);
    f[0] = 0xFF; f[1] = 0xFD; f[2] = 0x84; f[3] = 0x04;
    f[382] = fp0; f[383] = fp1;
    for (size_t k = 0; k < xp.size(); k++) f[383 - 2 - k] = xp[k];
    return f;
}
static void testMp2Pad() {
    DabPadDecoder pd;
    std::vector<uint8_t> s = {0x40 | 0x20 | 7, 0x00};        // first and last segment of 8 characters, charset 0
    const char* t = "Mp2 Radio";
    s.insert(s.end(), t, t + 8);
    const uint16_t c = dabgen::crc16(s.data(), (int)s.size());
    s.push_back((uint8_t)(c >> 8)); s.push_back((uint8_t)c);
    std::vector<uint8_t> a = {(uint8_t)((3 << 5) | 2), 0x00};   // length index 3 = 12 bytes: the whole segment
    a.insert(a.end(), s.begin(), s.end());
    const auto f = mp2WithPad(a, 0x20, 0x02);
    pd.mp2Frame(f.data(), (int)f.size());
    CHECK(pd.label() == "Mp2 Radi", "MP2 label '%s'", pd.label().c_str());
    // a segment in two frames: the second without a content indicator is as long as the last subfield
    DabPadDecoder p2;
    std::vector<uint8_t> a1 = {(uint8_t)((2 << 5) | 2), 0x00};   // 8 bytes now
    a1.insert(a1.end(), s.begin(), s.begin() + 8);
    const auto f1 = mp2WithPad(a1, 0x20, 0x02);
    p2.mp2Frame(f1.data(), (int)f1.size());
    CHECK(p2.label().empty(), "label from a part");
    std::vector<uint8_t> a2(s.begin() + 8, s.end());
    a2.resize(8, 0);
    const auto f2 = mp2WithPad(a2, 0x20, 0x00);
    p2.mp2Frame(f2.data(), (int)f2.size());
    CHECK(p2.label() == "Mp2 Radi", "MP2 label in two frames '%s'", p2.label().c_str());
    // DabAudio takes the PAD from the MP2 frames
    DabAudio au;
    au.setSilent(true);
    au.select(4, false, 128);
    au.push(f.data(), (int)f.size());
    CHECK(au.dynamicLabel() == "Mp2 Radi", "DabAudio MP2 label '%s'", au.dynamicLabel().c_str());
}

static void testAacParams() {
    DabAacParams p = dabAacParams(0x50);   // 48 kHz, AAC-LC, stereo
    CHECK(p.valid && p.outRate == 48000 && p.coreRate == 48000 && !p.sbr && !p.ps && p.stereo && p.auPerSuperframe == 6 && p.profile == "AAC-LC" && p.outChannels == 2, "AAC-LC 48 kHz");
    p = dabAacParams(0x10);                // 32 kHz
    CHECK(p.outRate == 32000 && p.coreRate == 32000 && p.auPerSuperframe == 4, "AAC-LC 32 kHz");
    p = dabAacParams(0x60 | 0x10);         // 48 kHz with SBR, stereo core
    CHECK(p.sbr && !p.ps && p.coreRate == 24000 && p.outRate == 48000 && p.auPerSuperframe == 3 && p.coreChannels == 2 && p.profile == "HE-AAC (AAC-LC + SBR)", "HE-AAC 48 kHz: %s", p.profile.c_str());
    p = dabAacParams(0x20 | 0x10 | 0x08);  // 32 kHz with SBR and PS
    CHECK(p.sbr && p.ps && p.coreRate == 16000 && p.outRate == 32000 && p.auPerSuperframe == 2 && p.coreChannels == 1 && p.outChannels == 2 && p.profile == "HE-AAC v2 (AAC-LC + SBR + PS)", "HE-AAC v2: %s", p.profile.c_str());
    p = dabAacParams(0x48 | 0x02);         // PS without SBR is not valid; surround config 2
    CHECK(!p.valid && p.surround == 2, "PS without SBR");
    // from a real superframe
    dabgen::Transmitter tx(config());
    DabAudio au;
    au.setSilent(true);
    au.select(2, true, 32);
    const auto& lay = tx.layout();
    int svc = -1;
    for (size_t i = 0; i < lay.size(); i++) if (lay[i].subId == 2) svc = (int)i;
    std::vector<uint8_t> f;
    for (int64_t k = 0; k < 40; k++) { tx.logicalFrame(svc, k, f); au.push(f.data(), (int)f.size()); }
    const DabAacParams s = au.stats().aac;
    CHECK(s.valid && s.outRate == 32000 && s.coreRate == 32000 && s.auPerSuperframe == 4, "stats: %d Hz, %d AUs", s.outRate, s.auPerSuperframe);
}

// faad2 as the AAC decoder (where installed): the generator's tones come out of it right, left 1 kHz and right 3 kHz
static double tone(const std::vector<float>& st, int ch, double hz) {
    double re = 0, im = 0;
    const size_t n = st.size() / 2;
    for (size_t i = 0; i < n; i++) { const double ph = 2 * 3.14159265358979 * hz * (double)i / 48000.0; re += st[i * 2 + (size_t)ch] * std::cos(ph); im += st[i * 2 + (size_t)ch] * std::sin(ph); }
    return std::sqrt(re * re + im * im) / (double)n;
}
static void testFaad() {
    if (!dabFaadAvailable()) { printf("faad2 is not installed: its test is skipped\n"); return; }
    DabAudio::setAacBackend(2);
    dabgen::Transmitter tx(config());
    DabAudio au;
    au.setSilent(true);
    au.select(1, true, 48);
    const auto& lay = tx.layout();
    int svc = -1;
    for (size_t i = 0; i < lay.size(); i++) if (lay[i].subId == 1) svc = (int)i;
    std::vector<uint8_t> f;
    for (int64_t k = 0; k < 120; k++) { tx.logicalFrame(svc, k, f); au.push(f.data(), (int)f.size()); }
    DabAudio::setAacBackend(0);
    const DabAudioStats st = au.stats();
    CHECK(st.decoder == "faad2" && st.pcmFrames > 48000, "decoder '%s', %llu frames", st.decoder.c_str(), (unsigned long long)st.pcmFrames);
    std::vector<float> pcm;
    CHECK(au.recent(pcm, 4800) == 4800, "recent samples");
    const double l1 = tone(pcm, 0, 1000), l3 = tone(pcm, 0, 3000), r1 = tone(pcm, 1, 1000), r3 = tone(pcm, 1, 3000);
    CHECK(l1 > 0.05 && l3 < l1 * 0.1 && r3 > 0.05 && r1 < r3 * 0.1, "tones: L %.3f/%.3f R %.3f/%.3f", l1, l3, r1, r3);
}

static void testAnnouncement() {
    DabEnsemble e;
    for (int i = 1; i <= 3; i++) {
        DabService sv; sv.sid = 0xC000 + (uint32_t)i;
        DabComponent c; c.tmid = 0; c.ascty = 63; c.subId = i;
        sv.comps.push_back(c);
        e.services[sv.sid] = sv;
        DabSubchannel sub; sub.id = i; sub.size = 10; e.subs[i] = sub;
    }
    DabAnnSupport s1; s1.asu = 0x0003; s1.clusters = {5};          // service 1: alarm and road traffic, cluster 5
    DabAnnSupport s2; s2.asu = 0x0002; s2.clusters = {5};          // service 2: road traffic
    e.annSupport[0xC001] = s1; e.annSupport[0xC002] = s2;
    DabAnnSwitch sw; sw.cluster = 5; sw.asw = 0x0002; sw.subId = 3;
    e.annActive.push_back(sw);
    DabAnnouncement an;
    CHECK(dabActiveAnnouncement(e, 1, 0x7FF, an) && an.subId == 3 && an.type == 1 && an.cluster == 5, "road traffic for service 1");
    CHECK(!dabActiveAnnouncement(e, 1, 0x0001, an), "type disabled");
    CHECK(!dabActiveAnnouncement(e, 3, 0x7FF, an), "the announcing service itself (no support of its own)");
    CHECK(!dabActiveAnnouncement(e, 2, 0x0004, an), "type 2 not announced");
    e.annActive[0].subId = 1;
    CHECK(!dabActiveAnnouncement(e, 1, 0x7FF, an), "already on the sub-channel of the announcement");
    e.annActive[0].subId = 3; e.annActive[0].asw = 0x0004;   // transport flash: service 1 does not support it
    CHECK(!dabActiveAnnouncement(e, 1, 0x7FF, an), "type the service does not support");
    e.annActive[0].asw = 0x0001; e.annActive[0].cluster = 0xFF;   // an alarm reaches everybody, even a service with no support entry
    CHECK(dabActiveAnnouncement(e, 2, 0x0001, an) && an.type == 0 && an.subId == 3, "alarm cluster");
    CHECK(!dabActiveAnnouncement(e, 2, 0x0002, an), "alarm disabled");
    DabAnnSwitch sw2; sw2.cluster = 5; sw2.asw = 0x0002; sw2.subId = 2;
    e.annActive.assign({sw2, e.annActive[0]});
    CHECK(dabActiveAnnouncement(e, 1, 0x0003, an) && an.type == 0, "the alarm comes first");
}

static long fileSize(const std::string& p) { FILE* f = std::fopen(p.c_str(), "rb"); if (!f) return -1; std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fclose(f); return n; }

static void testRecording() {
    dabgen::Transmitter tx(config());
    const auto& lay = tx.layout();
    auto sendFrames = [&](DabAudio& au, int sub, int64_t from, int64_t n) {
        int svc = -1;
        for (size_t i = 0; i < lay.size(); i++) if (lay[i].subId == sub) svc = (int)i;
        std::vector<uint8_t> f;
        for (int64_t k = from; k < from + n; k++) { tx.logicalFrame(svc, k, f); au.push(f.data(), (int)f.size()); }
    };
    {   // decoded: a WAV file with a header that agrees with its size
        DabAudio au;
        au.setSilent(true);
        std::string err;
        CHECK(!au.startRecording("rectest_none", DabRecFormat::Wav, false, &err), "recording with no service selected");
        au.select(3, true, 64);
        CHECK(au.startRecording("rectest_wav", DabRecFormat::Wav, true, &err), "start WAV: %s", err.c_str());
        sendFrames(au, 3, 0, 500);
        const DabRecStats rs = au.recording();
        CHECK(rs.active && rs.bytes > 1000 && rs.seconds > 2.0 && rs.path == "rectest_wav.wav", "stats: %llu bytes %.2f s '%s'", (unsigned long long)rs.bytes, rs.seconds, rs.path.c_str());
        au.stopRecording();
        CHECK(!au.recording().active, "stopped");
        FILE* f = std::fopen("rectest_wav.wav", "rb");
        uint8_t h[44] = {};
        CHECK(f && std::fread(h, 1, 44, f) == 44, "WAV header");
        if (f) std::fclose(f);
        const uint32_t data = h[40] | (h[41] << 8) | (h[42] << 16) | ((uint32_t)h[43] << 24);
        CHECK(!std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVEfmt ", 8) && h[22] == 2 && data > 100000 && (long)data + 44 == fileSize("rectest_wav.wav"), "WAV: data %u of %ld", data, fileSize("rectest_wav.wav"));
        std::remove("rectest_wav.wav");
        const long ts = fileSize("rectest_wav.txt");
        CHECK(ts > 10, "the label log: %ld bytes", ts);
        std::remove("rectest_wav.txt");
    }
    {   // as broadcast: an MP4 file the demuxer reads back, with as many packets as access units
        DabAudio au;
        au.setSilent(true);
        au.select(3, true, 64);
        std::string err;
        CHECK(au.startRecording("rectest_aac", DabRecFormat::Encoded, false, &err), "start AAC: %s", err.c_str());
        sendFrames(au, 3, 0, 500);
        const uint64_t aus = au.stats().auOk;
        au.stopRecording();
        AVFormatContext* ic = nullptr;
        CHECK(avformat_open_input(&ic, "rectest_aac.m4a", nullptr, nullptr) == 0, "open the MP4");
        if (ic) {
            CHECK(avformat_find_stream_info(ic, nullptr) >= 0 && ic->nb_streams == 1 && ic->streams[0]->codecpar->codec_id == AV_CODEC_ID_AAC, "one AAC stream");
            AVPacket* pk = av_packet_alloc();
            uint64_t n = 0;
            while (av_read_frame(ic, pk) >= 0) { n++; av_packet_unref(pk); }
            av_packet_free(&pk);
            CHECK(n == aus && n > 100, "packets %llu, access units %llu", (unsigned long long)n, (unsigned long long)aus);
            avformat_close_input(&ic);
        }
        std::remove("rectest_aac.m4a");
    }
    {   // a DAB service: the MP2 frames as they are
        DabAudio au;
        au.setSilent(true);
        au.select(4, false, 128);
        std::string err;
        if (au.startRecording("rectest_mp2", DabRecFormat::Encoded, false, &err)) {
            sendFrames(au, 4, 0, 100);
            au.stopRecording();
            const long sz = fileSize("rectest_mp2.mp2");
            CHECK(sz > 0 && sz % 384 == 0 && sz / 384 >= 90, "MP2 file: %ld bytes", sz);
            std::remove("rectest_mp2.mp2");
        } else CHECK(false, "start MP2: %s", err.c_str());
    }
    {   // selecting another service ends the recording
        DabAudio au;
        au.setSilent(true);
        au.select(3, true, 64);
        au.startRecording("rectest_sel", DabRecFormat::Wav, false);
        au.select(1, true, 48);
        CHECK(!au.recording().active, "recording survives a new selection");
        std::remove("rectest_sel.wav");
    }
}

static void testAudio() {
    dabgen::Transmitter tx(config());
    DabAudio au;
    au.setSilent(true);
    au.select(3, true, 64);
    const auto& lay = tx.layout();
    int svc = -1;
    for (size_t i = 0; i < lay.size(); i++) if (lay[i].subId == 3) svc = (int)i;
    CHECK(svc >= 0, "sub-channel 3");
    std::vector<uint8_t> f;
    for (int64_t k = 0; k < 600; k++) { tx.logicalFrame(svc, k, f); au.push(f.data(), (int)f.size()); }
    DabSlide s;
    const bool got = au.slide(s, 0);
    CHECK(got && s.seq > 0 && sameImage(s), "DabAudio: slide %d", (int)got);
    const uint64_t seq1 = s.seq;
    CHECK(!au.slide(s, seq1), "no new picture expected");
    CHECK(au.dynamicLabel() == "OnAir Melody - test tune", "DabAudio label '%s'", au.dynamicLabel().c_str());
    const DabAudioStats st = au.stats();
    CHECK(st.auBad == 0 && st.superframesBad == 0 && st.pcmFrames > 0, "audio with PAD: %llu bad, %llu pcm frames", (unsigned long long)st.auBad, (unsigned long long)st.pcmFrames);
    au.select(3, true, 64);
    CHECK(au.dynamicLabel().empty(), "label kept after select()");
    CHECK(au.slide(s, seq1) && s.seq > seq1 && s.w == 0 && s.rgba.empty(), "slide kept after select()");
}

// a MOT header with a ContentName (and a slide show CategoryTitle when given), content type `ctype`, subtype `sub`
static std::vector<uint8_t> motHeaderNamed(size_t bodySize, int ctype, int sub, const std::string& name, const std::string& cat = "") {
    std::vector<uint8_t> ext;
    auto param = [&](int id, const std::vector<uint8_t>& v) { ext.push_back((uint8_t)(0xC0 | id)); ext.push_back((uint8_t)v.size()); ext.insert(ext.end(), v.begin(), v.end()); };
    std::vector<uint8_t> nm = {0x0F};
    nm.insert(nm.end(), name.begin(), name.end());
    param(0x0C, nm);
    if (!cat.empty()) param(0x26, std::vector<uint8_t>(cat.begin(), cat.end()));
    const size_t hs = 7 + ext.size();
    std::vector<uint8_t> h = {(uint8_t)(bodySize >> 20), (uint8_t)(bodySize >> 12), (uint8_t)(bodySize >> 4), (uint8_t)((bodySize << 4) | (hs >> 9)),
                              (uint8_t)(hs >> 1), (uint8_t)(((hs & 1) << 7) | (ctype << 1) | (sub >> 8)), (uint8_t)sub};
    h.insert(h.end(), ext.begin(), ext.end());
    return h;
}
static void sendObject(DabPadDecoder& pd, const std::vector<uint8_t>& head, const std::vector<uint8_t>& body) {
    send(pd, group(3, head), 0);
    send(pd, group(4, body), 0);
}
static const DabLogo* logoOf(const DabPadDecoder& pd, uint32_t sid) {
    for (const DabLogo& l : pd.logos()) if (l.sid == sid) return &l;
    return nullptr;
}
// Station logos: SPI service information (XML in a MOT object) naming a logo per service, the pictures in MOT objects, in either order; a
// slide show picture of the category "logo"; nothing from references that are never broadcast.
static void testSpiLogos() {
    const std::vector<uint8_t> png = dabgen::slidePng();
    const std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><serviceInformation version=\"1\"><services>"
        "<service><serviceID id=\"dab:e1.ce15.c224.0\"/>"
        "<mediaDescription><multimedia type=\"logo_unrestricted\" url=\"http://example.invalid/logos/rect.png\" width=\"320\" height=\"240\"/>"
        "<multimedia type=\"logo_colour_square\" url=\"http://example.invalid/logos/square.png\" width=\"32\" height=\"32\"/></mediaDescription></service>"
        "<service><bearer id=\"dab:e1.ce15.c225.0\"/>"
        "<mediaDescription><multimedia type=\"logo_colour_square\" url=\"http://example.invalid/logos/never.png\" width=\"32\" height=\"32\"/></mediaDescription></service>"
        "</services></serviceInformation>";
    const std::vector<uint8_t> xb(xml.begin(), xml.end());
    for (int order = 0; order < 2; order++) {      // 0: service information first, 1: the picture first
        DabPadDecoder pd;
        auto spi = [&] { sendObject(pd, motHeaderNamed(xb.size(), 5, 0, "si.xml"), xb); };
        auto pic = [&] { sendObject(pd, motHeaderNamed(png.size(), 2, 3, "square.png"), png); };
        if (order == 0) { spi(); pic(); } else { pic(); spi(); }
        const DabLogo* l = logoOf(pd, 0xC224);
        CHECK(l && l->type == "logo_colour_square" && l->name == "square.png", "SPI logo missing (order %d)", order);
        if (l) { DabSlide s; s.w = l->w; s.h = l->h; s.rgba = l->rgba; CHECK(sameImage(s), "SPI logo pixels differ (order %d)", order); }
        CHECK(!logoOf(pd, 0xC225), "logo for an unbroadcast picture");
        CHECK(pd.logoSeq() >= 1, "logoSeq");
        if (order == 0) CHECK(pd.seq() == 0, "a logo shown as slide show picture (seq %llu)", (unsigned long long)pd.seq());
    }
    {   // a slide show picture of the category "logo" belongs to the service that carries it
        DabPadDecoder pd;
        sendObject(pd, motHeaderNamed(png.size(), 2, 3, "mylogo.png", "Station Logo"), png);
        const DabLogo* l = logoOf(pd, 0);
        CHECK(l && l->name == "mylogo.png" && pd.seq() == 0, "category logo");
    }
    {   // service information without any logo: nothing
        DabPadDecoder pd;
        const std::string x2 = "<serviceInformation><services><service><serviceID id=\"dab:e1.ce15.c224.0\"/></service></services></serviceInformation>";
        sendObject(pd, motHeaderNamed(x2.size(), 5, 0, "si.xml"), std::vector<uint8_t>(x2.begin(), x2.end()));
        CHECK(pd.logos().empty() && pd.logoSeq() == 0, "logo from service information without one");
    }
}

int main() {
    testDirect();
    testForms();
    testSpiLogos();
    testLabel();
    testAudio();
    testDlPlus();
    testMp2Pad();
    testAacParams();
    testFaad();
    testAnnouncement();
    testRecording();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("test_dab_pad: all passed\n");
    return 0;
}
