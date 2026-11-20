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

int main() {
    testPacker();
    testTransmitReceive();
    return testSummary("drm_xhe_tx");
}
