// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NFC / RFID: protocol-layer checks and a physical-layer loopback (the synthetic generator into the matched demodulator), clean
// and with noise and a carrier offset. Receive only; the exchange is fabricated, nothing is cloned.
#include "dect2/nfc/nfc_proto.h"
#include "dect2/nfc/nfc_rx.h"
#include "dect2/nfc/nfc_gen.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::nfc;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)   // Flawfinder: ignore

static std::string hex(const std::vector<uint8_t>& b) { std::string s; char c[4]; for (uint8_t v : b) { snprintf(c, sizeof c, "%02X", v); s += c; } return s; }

static void testProtocol() {
    // CRC self-consistency and that A and B differ
    std::vector<uint8_t> d = {0x93, 0x70, 0x04, 0xA1, 0xB2, 0xC3, 0x44};
    uint16_t ca = crcA(d.data(), d.size());
    std::vector<uint8_t> f = d; f.push_back((uint8_t)(ca & 0xFF)); f.push_back((uint8_t)(ca >> 8));
    CHECK(crcAOk(f), "crcA self-check");
    CHECK(crcA(d.data(), d.size()) != crcB(d.data(), d.size()), "crcA and crcB differ");
    uint16_t cb = crcB(d.data(), d.size());
    std::vector<uint8_t> fb = d; fb.push_back((uint8_t)(cb & 0xFF)); fb.push_back((uint8_t)(cb >> 8));
    CHECK(crcBOk(fb), "crcB self-check");

    // Miller symbol codec round-trip
    for (std::vector<uint8_t> bytes : std::vector<std::vector<uint8_t>>{{0x26}, {0x93, 0x20}, {0x04, 0x00}, {0x60, 0x00, 0x12, 0x34}}) {
        const int bits = bytes.size() == 1 && bytes[0] == 0x26 ? 7 : (int)bytes.size() * 8;
        std::vector<uint8_t> seq = bytesToMillerSeq(bytes, bits);
        int ob = 0; bool par = false;
        std::vector<uint8_t> back = millerSeqToBytes(seq, ob, par);
        CHECK(back == bytes && par, "Miller round-trip %s -> %s par=%d", hex(bytes).c_str(), hex(back).c_str(), par);
    }
    // a deliberate parity break is caught
    {
        std::vector<uint8_t> bits = bytesToBits({0x55}, 8);
        bits[8] ^= 1; int ob = 0; bool par = true;
        bitsToBytes(bits, ob, par);
        CHECK(!par, "parity break detected");
    }

    // frame classification
    CHECK(parseTypeA({0x26}, 7, true).kind == AKind::Reqa, "REQA");
    CHECK(parseTypeA({0x52}, 7, true).kind == AKind::Wupa, "WUPA");
    {
        ATypeA at = parseTypeA({0x04, 0x00}, 16, false);
        CHECK(at.kind == AKind::Atqa && atqaUidBytes(at.atqa) == 4, "ATQA 4-byte");
    }
    {
        ATypeA sk = parseTypeA({0x20}, 8, false);
        CHECK(sk.kind == AKind::Sak && sk.iso144434 && !sk.uidComplete == false, "SAK 0x20 ISO14443-4");
    }
    {
        std::vector<uint8_t> req = {0x05, 0x00, 0x08};
        uint16_t c = crcB(req.data(), req.size()); req.push_back((uint8_t)(c & 0xFF)); req.push_back((uint8_t)(c >> 8));
        ATypeB b = parseTypeB(req, true);
        CHECK(b.valid && b.isReqb && b.slots == 1 && b.wup, "REQB/WUPB parse");
    }
    {
        std::vector<uint8_t> resp = {0x00, 0x00, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x04, 0xE0};   // flags, dsfid, UID LSB first
        uint16_t c = crcB(resp.data(), resp.size()); resp.push_back((uint8_t)(c & 0xFF)); resp.push_back((uint8_t)(c >> 8));
        AVicinity v = parseVicinity(resp, false);
        CHECK(v.kind == VKind::Response && !v.uid.empty() && v.uid.front() == 0xE0, "15693 inventory UID 0xE0 first");
    }

    // NDEF URI and text
    {
        std::vector<uint8_t> mem = {0x03, 0x0A, 0xD1, 0x01, 0x06, 'U', 0x03, 'a', '.', 'c', 'o', 'm', 0xFE};
        NdefMessage m = parseType2Ndef(mem);
        CHECK(m.valid && m.records.size() == 1 && m.records[0].uri == "http://a.com", "NDEF URI");
    }
    {
        // text record "en" + "Hi"
        std::vector<uint8_t> payload = {0x02, 'e', 'n', 'H', 'i'};
        std::vector<uint8_t> mem = {0x03, (uint8_t)(payload.size() + 4), 0xD1, 0x01, (uint8_t)payload.size(), 'T'};
        for (uint8_t b : payload) mem.push_back(b);
        mem.push_back(0xFE);
        NdefMessage m = parseType2Ndef(mem);
        CHECK(m.valid && m.records.size() == 1 && m.records[0].text == "Hi" && m.records[0].lang == "en", "NDEF text");
    }
}

struct Seen { std::vector<RawFrame> frames; };

static Seen runLoop(int session, double snrDb, double cfoHz) {
    GenConfig g; g.fs = 4e6; g.session = session; g.snrDb = 1e9; g.carrierHz = 0;
    std::vector<cf32> buf = renderSession(g);
    // add noise / offset if asked
    if (snrDb < 200 || cfoHz != 0) {
        const double noiseAmp = snrDb < 200 ? 0.3 * std::pow(10.0, -snrDb / 20.0) : 0.0;
        uint32_t rng = 777; auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return ((int)(rng >> 9) / 4194304.0f) - 1.0f; };
        double ph = 0; const double step = 2 * M_PI * cfoHz / g.fs;
        for (auto& v : buf) {
            if (step != 0) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += step; }
            if (noiseAmp > 0) v += cf32((float)(noiseAmp * rnd()), (float)(noiseAmp * rnd()));
        }
    }
    Seen s;
    Demod d(g.fs);
    d.setFrameCallback([&](const RawFrame& f) { s.frames.push_back(f); });
    // feed in blocks; add trailing idle so the last burst flushes
    for (size_t i = 0; i < buf.size(); i += 4096) d.feed(buf.data() + i, std::min<size_t>(4096, buf.size() - i));
    std::vector<cf32> tail(20000, cf32(0.3f, 0)); d.feed(tail.data(), tail.size());
    return s;
}

static void testLoopbackA(double snr, double cfo) {
    Seen s = runLoop(0, snr, cfo);
    bool reqa = false, atqa = false, uid = false, sak = false, ndef = false;
    for (const RawFrame& f : s.frames) {
        if (f.proto != Proto::TypeA) continue;
        ATypeA a = parseTypeA(f.bytes, f.bits, f.fromReader);
        if (a.kind == AKind::Reqa) reqa = true;
        if (a.kind == AKind::Atqa && atqaUidBytes(a.atqa) == 4) atqa = true;
        if (!f.fromReader && f.bytes.size() == 5 && f.bytes[0] == 0x04 && f.bytes[1] == 0xA1 && f.bytes[2] == 0xB2 && f.bytes[3] == 0xC3) uid = true;
        if (a.kind == AKind::Sak) sak = true;
        if (a.kind == AKind::Apdu && !f.fromReader && f.bytes.size() >= 16) {
            std::vector<uint8_t> mem(f.bytes.begin(), f.bytes.end() - (a.crcPresent ? 2 : 0));
            NdefMessage m = parseType2Ndef(mem);
            if (m.valid) for (auto& r : m.records) if (r.uri.find("nfc.example") != std::string::npos) ndef = true;
        }
    }
    CHECK(reqa, "loopback A (snr=%.0f cfo=%.0f): REQA seen (%d frames)", snr, cfo, (int)s.frames.size());
    CHECK(atqa, "loopback A (snr=%.0f cfo=%.0f): ATQA seen", snr, cfo);
    CHECK(uid, "loopback A (snr=%.0f cfo=%.0f): UID 04A1B2C3 seen", snr, cfo);
    CHECK(sak, "loopback A (snr=%.0f cfo=%.0f): SAK seen", snr, cfo);
    CHECK(ndef, "loopback A (snr=%.0f cfo=%.0f): NDEF URI nfc.example decoded", snr, cfo);
}

static void testLoopbackV() {
    Seen s = runLoop(1, 1e9, 0);
    bool uid = false;
    for (const RawFrame& f : s.frames) {
        if (f.proto != Proto::V15693 || f.fromReader) continue;
        AVicinity v = parseVicinity(f.bytes, false);
        if (!v.uid.empty() && v.uid.front() == 0xE0) uid = true;
    }
    CHECK(uid, "loopback 15693: inventory UID 0xE0 recovered (%d frames)", (int)s.frames.size());
}

static void testNoise() {
    // 40 noise-only runs must not fabricate a tag / CRC-valid frame
    int falseFrames = 0;
    for (int t = 0; t < 40; t++) {
        std::vector<cf32> buf(200000);
        uint32_t rng = 1000u + t; auto rnd = [&] { rng = rng * 1664525u + 1013904223u; return ((int)(rng >> 9) / 4194304.0f) - 1.0f; };
        for (auto& v : buf) v = cf32(0.3f + 0.05f * rnd(), 0.05f * rnd());   // carrier + noise, no frames
        Demod d(4e6); int n = 0; d.setFrameCallback([&](const RawFrame& f) {
            if ((f.proto == Proto::TypeA || f.proto == Proto::TypeB) && f.bytes.size() >= 3 && crcAOk(f.bytes)) n++;
            if (f.proto == Proto::V15693 && f.bytes.size() >= 3 && crcBOk(f.bytes)) n++;
        });
        for (size_t i = 0; i < buf.size(); i += 4096) d.feed(buf.data() + i, std::min<size_t>(4096, buf.size() - i));
        falseFrames += n;
    }
    CHECK(falseFrames == 0, "noise-only: %d CRC-valid frames fabricated (want 0)", falseFrames);
}

int main() {
    testProtocol();
    testLoopbackA(1e9, 0);
    testLoopbackA(20, 0);
    testLoopbackA(20, 12000);
    testLoopbackV();
    testNoise();
    std::printf("%s: %d failure(s)\n", fails ? "test_nfc_decode FAILED" : "test_nfc_decode OK", fails);
    return fails ? 1 : 0;
}
