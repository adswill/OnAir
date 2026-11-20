// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM xHE-AAC transmit path: the audio super frame packer (clause 5.3.1) against the parser the receiver uses, including delayed borders, and a whole
// transmitter -> receiver run that must bring the transmitted access units back in order. The access units are pseudo random bytes: this checks the
// framing, the SDC and the channel coding of the stream, not what a USAC decoder makes of it (that needs a real encoder; see docs/CODECS.md).
#include "dect2/drm/drm_audio.h"
#include "dect2/drm/drm_fec.h"
#include "dect2/drm/drm_gen.h"
#include "dect2/drm/drm_rx.h"
#include <map>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::drm;

#include "../common/test_util.h"

// pack many super frames of one length from access units of the given sizes; every complete frame the parser returns must be the next access unit, CRC good
static void roundTrip(const char* what, int len, int minAu, int maxAu, bool wantDelayed) {
    std::mt19937 rng(1234);
    XheSuperFramePacker pk;
    XheSuperFrameParser ps;
    std::vector<std::vector<uint8_t>> sent;
    size_t got = 0;
    int delayed = 0, packed = 0, refused = 0;
    for (int n = 0; n < 400; n++) {
        while ((int)pk.queuedBytes() < len) {
            std::vector<uint8_t> au((size_t)(minAu + (int)(rng() % (unsigned)(maxAu - minAu + 1))));
            for (auto& b : au) b = (uint8_t)rng();
            sent.push_back(au);
            pk.push(au);
        }
        std::vector<uint8_t> sf;
        if (!pk.pack(len, sf)) { refused++; continue; }
        packed++;
        CHECK((int)sf.size() == len, "%s: super frame has the asked length (%zu)", what, sf.size());
        const int b = sf[0] >> 4;
        for (int k = 0; k < b; k++) { const int e = ((sf[sf.size() - 2 * (size_t)(k + 1)] << 8) | sf[sf.size() - 2 * (size_t)(k + 1) + 1]) >> 4; if (e == 0xFFE || e == 0xFFF) delayed++; }
        std::vector<XheFrame> fr;
        CHECK(ps.parse(sf.data(), (int)sf.size(), fr), "%s: the parser reads super frame %d", what, n);
        for (const XheFrame& f : fr) {
            CHECK(f.crcOk, "%s: CRC of frame %zu", what, got);
            CHECK(got < sent.size() && f.au == sent[got], "%s: frame %zu is the access unit that went in", what, got);
            got++;
        }
    }
    CHECK(refused == 0, "%s: no super frame refused (%d)", what, refused);
    CHECK(got > 100, "%s: frames came out (%zu)", what, got);
    if (wantDelayed) CHECK(delayed > 0, "%s: delayed borders were exercised", what);
}

static void testPacker() {
    roundTrip("long frames", 700, 150, 400, false);
    roundTrip("short frames (delayed borders)", 220, 8, 40, true);
    roundTrip("one frame per super frame or fewer", 300, 280, 700, false);
    XheSuperFramePacker pk;
    std::vector<uint8_t> sf;
    pk.push({0x80, 1, 2});
    CHECK(!pk.pack(100, sf), "too little queued is refused");
    CHECK(pk.queuedBytes() == 5, "and nothing is consumed");
}

// The transmitter: SDC type 9 with coding 3, then the frames at the receiver
static void testTransmitReceive() {
    DrmTxConfig c;
    c.audioCoding = 3; c.audioMode = 0; c.audioRateHz = 32000; c.seed = 77;
    c.audioXheConfig = {0x00};
    DrmTransmitter tx(c, nullptr);
    CHECK(tx.ok(), "the transmitter accepts an xHE-AAC configuration");
    if (!tx.ok()) return;
    DrmTxConfig bad = c; bad.audioRateHz = 12000;
    CHECK(!DrmTransmitter(bad, nullptr).ok(), "an AAC-only rate is refused for xHE-AAC");
    std::unique_ptr<DrmAudioSource> ref = makeDrmXhePatternSource(c.seed);
    std::vector<std::vector<uint8_t>> units;
    std::map<std::vector<uint8_t>, size_t> index;
    for (size_t i = 0; i < 6000; i++) { std::vector<uint8_t> au; ref->nextAccessUnit(au); index.emplace(au, i); units.push_back(au); }
    DrmReceiver rx;
    rx.setSilent(true);
    rx.configure(tx.sampleRate());
    XheSuperFrameParser ps;
    int good = 0, inOrder = 0, bad2 = 0;
    long last = -1;
    rx.setStreamCallback([&](int, const uint8_t* b, int len, int, const DrmTelemetry&) {
        std::vector<XheFrame> fr;
        if (len <= 4 || !ps.parse(b, len - 4, fr)) { bad2++; return; }
        for (const XheFrame& f : fr) {
            const auto it = index.find(f.au);
            if (!f.crcOk || it == index.end()) { bad2++; continue; }
            good++;
            if ((long)it->second == last + 1) inOrder++;
            last = (long)it->second;
        }
    });
    std::vector<cf32> out;
    for (int i = 0; i < 14; i++) { out.clear(); tx.superFrame(out); rx.feed(out.data(), out.size()); }
    CHECK(good >= 20, "the receiver brought transmitted access units back (%d good, %d bad)", good, bad2);
    CHECK(inOrder >= good - 3, "and in order (%d of %d)", inOrder, good);
}

struct Bits { std::vector<uint8_t> b; int n = 0; void put(unsigned v, int k) { for (int i = k - 1; i >= 0; i--) { if ((n & 7) == 0) b.push_back(0); if ((v >> i) & 1) b.back() |= (uint8_t)(0x80 >> (n & 7)); n++; } } };

// the static config -> the encoder's AudioSpecificConfig -> the static config again must give the bytes we started with
static void roundTripConfig(const char* what, int mode, int rateCode, const std::vector<uint8_t>& staticCfg) {
    SdcAudio a; a.coding = 3; a.mode = mode; a.rateCode = rateCode; a.config = staticCfg;
    const std::vector<uint8_t> asc = xheAscFromSdc(a);
    CHECK(!asc.empty(), "%s: ASC built", what);
    SdcAudio b;
    CHECK(xheSdcFromAsc(asc, b), "%s: the ASC is read back", what);
    CHECK(b.coding == 3 && b.mode == mode && b.rateCode == rateCode, "%s: mode %d rate code %d", what, b.mode, b.rateCode);
    CHECK(b.config == staticCfg, "%s: static config bytes are the same", what);
}
static void testConfigInverse() {
    Bits m;   // mono, 2:1 SBR, default header, one extension element without config
    m.put(2, 2); m.put(1, 1); m.put(0, 3); m.put(5, 4); m.put(9, 4); m.put(0, 2);
    m.put(1, 2); m.put(3, 4); m.put(0, 4); m.put(0, 1); m.put(0, 1);
    roundTripConfig("mono 38.4 kHz", 0, 6, m.b);
    Bits st;  // stereo, 2:1 SBR, parametric stereo config 1 with temporal shape, no extension
    st.put(2, 2); st.put(0, 1); st.put(1, 1); st.put(0, 2); st.put(5, 4); st.put(9, 4); st.put(0, 2);
    st.put(1, 2); st.put(3, 3); st.put(2, 3); st.put(1, 1); st.put(0, 1); st.put(1, 1); st.put(0, 1); st.put(7, 5); st.put(0, 2);
    roundTripConfig("stereo 32 kHz", 2, 5, st.b);
    Bits plain;   // mono, 1024 samples without SBR
    plain.put(0, 2); plain.put(1, 1); plain.put(0, 2);
    roundTripConfig("mono 24 kHz no SBR", 0, 4, plain.b);
    SdcAudio no, back;
    no.coding = 3; no.mode = 0; no.rateCode = 4; no.config = plain.b;
    std::vector<uint8_t> asc = xheAscFromSdc(no);
    asc[0] ^= 0x20;
    CHECK(!xheSdcFromAsc(asc, back), "an ASC of another audio object type is refused");
    CHECK(!xheSdcFromAsc({}, back), "an empty ASC is refused");
}

int main() {
    testConfigInverse();
    testPacker();
    testTransmitReceive();
    return testSummary("drm_xhe_tx");
}
