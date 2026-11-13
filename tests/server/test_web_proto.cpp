// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web protocol: the JSON reader, every client message's validation, the forbidden types, WebSocket framing both ways, SHA-1 / base64
// (RFC vectors), IMA ADPCM, the binary frames, and a fuzz run: hundreds of thousands of mutated and random messages through the parser and the
// frame reader must never crash, never hang, and never produce an accepted message with a value out of its range.
#include "../../server/web/web_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace onair_web;
#include "../common/test_util.h"

static bool ok(const std::string& t) { ClientMsg m; std::string c, w; return parseClientMsg(t, m, c, w); }
static std::string codeOf(const std::string& t) { ClientMsg m; std::string c, w; parseClientMsg(t, m, c, w); return c; }

static void testJson() {
    Json j;
    std::string e;
    CHECK(jsonParse("{\"a\":[1,2.5e3,-0.5],\"b\":{\"c\":null,\"d\":true},\"s\":\"x\\u00e9\\ud83d\\ude00\"}", j, e), "valid: %s", e.c_str());
    CHECK(j.get("s") && j.get("s")->str == "x\xc3\xa9\xf0\x9f\x98\x80", "unicode escapes");
    const char* bad[] = {"", "{", "{\"a\":}", "{\"a\":1,}", "[1,2", "{\"a\":1}x", "{'a':1}", "{\"a\":01}", "{\"a\":1.}", "{\"a\":.5}", "{\"a\":-}",
                         "{\"a\":1e}", "{\"a\":NaN}", "{\"a\":1e999}", "{\"a\":\"\\x\"}", "{\"a\":\"\\ud800\"}", "{\"a\":\"\\udc00x\"}", "{\"a\":\"\x01\"}",
                         "{\"a\":\"\xc0\xaf\"}", "{\"a\":\"\xed\xa0\x80\"}", "{\"a\":\"\xff\"}", "{\"a\":1,\"a\":2}", "{\"a\":\"\\u0000\"}", "[[[[[1]]]]]",
                         "tru", "nul", "{\"a\":truex}"};
    for (const char* b : bad) CHECK(!jsonParse(b, j, e), "must refuse: %s", b);
    std::string big = "[";
    for (int i = 0; i < 200; i++) big += "1,";
    big += "1]";
    CHECK(!jsonParse(big, j, e), "too many values");
    CHECK(!jsonParse(std::string(5000, ' ') + "1", j, e), "too long");
    CHECK(!jsonParse("\"" + std::string(2000, 'a') + "\"", j, e), "string too long");
    CHECK(jsonEsc("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001", "escape: %s", jsonEsc("a\"b\\c\n\x01").c_str());
    CHECK(jsonNumber(NAN) == "null" && jsonNumber(1.5) == "1.5", "numbers");
}

static void testMessages() {
    ClientMsg m; std::string c, w;
    CHECK(parseClientMsg("{\"type\":\"hello\",\"proto\":1,\"name\":\"  Kris <b>&\"}", m, c, w) && m.type == ClientMsg::Hello && m.name == "Kris b", "hello: %s / name '%s'", w.c_str(), m.name.c_str());
    CHECK(!ok("{\"type\":\"hello\",\"proto\":2}"), "other protocol version");
    CHECK(!ok("{\"type\":\"hello\"}"), "hello without proto");
    CHECK(parseClientMsg("{\"type\":\"tune\",\"freq_hz\":145800000,\"mode\":\"nfm\",\"low_hz\":-6000,\"high_hz\":6000}", m, c, w) && m.hasFreq && m.hasDemod && m.hasPass, "tune: %s", w.c_str());
    CHECK(codeOf("{\"type\":\"tune\",\"mode\":\"fm2\"}") == err::OutOfRange, "unknown mode");
    CHECK(!ok("{\"type\":\"tune\"}"), "empty tune");
    CHECK(!ok("{\"type\":\"tune\",\"freq_hz\":-1}"), "negative frequency");
    CHECK(!ok("{\"type\":\"tune\",\"freq_hz\":\"100\"}"), "frequency as text");
    CHECK(!ok("{\"type\":\"tune\",\"low_hz\":100}"), "half a passband");
    CHECK(!ok("{\"type\":\"tune\",\"low_hz\":100,\"high_hz\":120}"), "passband narrower than 50 Hz");
    CHECK(!ok("{\"type\":\"tune\",\"freq_hz\":1e6,\"extra\":1}"), "unknown field");
    CHECK(ok("{\"type\":\"rx\",\"squelch_db\":-90,\"agc\":\"fast\",\"gain_db\":10,\"deemph_us\":75}"), "rx");
    CHECK(!ok("{\"type\":\"rx\",\"deemph_us\":60}"), "bad de-emphasis");
    CHECK(!ok("{\"type\":\"rx\",\"agc\":\"medium\"}"), "bad agc");
    CHECK(!ok("{\"type\":\"rx\",\"gain_db\":100}"), "gain out of range");
    CHECK(ok("{\"type\":\"audio\",\"on\":true,\"codec\":\"pcm16\",\"rate\":24000}"), "audio");
    CHECK(!ok("{\"type\":\"audio\",\"on\":true,\"rate\":11025}"), "odd audio rate");
    CHECK(!ok("{\"type\":\"audio\",\"on\":true,\"codec\":\"opus\"}"), "unknown codec");
    CHECK(!ok("{\"type\":\"audio\"}"), "audio without on");
    CHECK(!ok("{\"type\":\"audio\",\"on\":1}"), "on must be a boolean");
    CHECK(ok("{\"type\":\"spectrum\",\"on\":true,\"bins\":512,\"fps\":5,\"start_hz\":100e6,\"stop_hz\":101e6}"), "spectrum");
    CHECK(!ok("{\"type\":\"spectrum\",\"on\":true,\"bins\":8192}"), "too many bins");
    CHECK(!ok("{\"type\":\"spectrum\",\"on\":true,\"fps\":0}"), "zero fps");
    CHECK(!ok("{\"type\":\"spectrum\",\"on\":true,\"bins\":512.5}"), "fractional bins");
    CHECK(!ok("{\"type\":\"spectrum\",\"on\":true,\"start_hz\":100e6,\"stop_hz\":100e6}"), "empty span");
    CHECK(ok("{\"type\":\"subscribe\",\"decoder\":\"adsb\"}") && ok("{\"type\":\"unsubscribe\",\"decoder\":\"aprs\",\"freq_hz\":144800000}"), "subscribe");
    CHECK(!ok("{\"type\":\"subscribe\",\"decoder\":\"../etc\"}") && !ok("{\"type\":\"subscribe\",\"decoder\":\"ADSB\"}"), "decoder ids");
    CHECK(ok("{\"type\":\"ping\",\"t\":123.5}") && ok("{\"type\":\"bye\"}"), "ping, bye");
    CHECK(codeOf("{\"type\":\"nope\"}") == err::UnknownType, "unknown type");
    CHECK(codeOf("[1]") == err::BadMessage && codeOf("{") == err::BadJson, "not an object");
    CHECK(codeOf("{\"type\":1}") == err::BadMessage, "type must be text");
}

static void testForbidden() {
    for (const char* t : {"tx", "transmit", "ptt", "arm", "tx_arm", "key", "keys", "import_key", "key_import", "voicekeys", "decrypt", "record", "recording", "upload", "exec", "admin"}) {
        CHECK(codeOf(std::string("{\"type\":\"") + t + "\",\"on\":true}") == err::Forbidden, "%s must be forbidden", t);
        CHECK(isForbiddenType(t), "%s in the list", t);
    }
    for (const auto& t : clientTypes()) CHECK(!isForbiddenType(t), "%s is a normal type", t.c_str());
}

static void testWs() {
    CHECK(base64Enc(sha1Raw("abc")) == "qZk+NkcGgWq6PiVxeFDCbJzQ2J0=", "sha1(abc)");
    CHECK(wsAccept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "RFC 6455 accept key");
    CHECK(base64Enc("") == "" && base64Enc("f") == "Zg==" && base64Enc("fo") == "Zm8=" && base64Enc("foobar") == "Zm9vYmFy", "base64");
    CHECK(ctEq("abc", "abc") && !ctEq("abc", "abd") && !ctEq("abc", "abcd") && !ctEq("", ""), "ctEq");
    const std::string p = "{\"type\":\"ping\"}";
    std::string f = wsMaskedFrame(1, p, 0x11223344);
    WsIn r = wsParse(f);
    CHECK(r.kind == 2 && r.op == 1 && r.payload == p && r.used == f.size(), "masked text frame");
    r = wsParse(f.substr(0, f.size() - 1));
    CHECK(r.kind == 0, "incomplete frame waits");
    std::string big(3000, 'x');
    f = wsMaskedFrame(1, big, 7);
    r = wsParse(f);
    CHECK(r.kind == 2 && r.payload == big, "16-bit length");
    r = wsParse(wsMaskedFrame(1, std::string(5000, 'x'), 7));
    CHECK(r.kind == 1 && r.closeCode == 1009, "too big");
    r = wsParse(wsServerFrame(1, p));
    CHECK(r.kind == 1 && r.closeCode == 1002, "unmasked refused");
    std::string frag = wsMaskedFrame(1, p, 1);
    frag[0] = 0x01;   // FIN clear
    CHECK(wsParse(frag).kind == 1 && wsParse(frag).closeCode == 1003, "fragment refused");
    std::string rsv = wsMaskedFrame(1, p, 1);
    rsv[0] |= 0x40;
    CHECK(wsParse(rsv).closeCode == 1002, "reserved bits");
    CHECK(wsParse(wsMaskedFrame(9, std::string(126, 'p'), 1)).closeCode == 1002, "long control frame");
    std::string len64 = "\x81\xFF" + std::string(8, '\0');
    len64[2] = 0x7F; len64[9] = 1;
    CHECK(wsParse(len64).kind == 1 && wsParse(len64).closeCode == 1009, "64-bit length");
    const std::string sf = wsServerFrame(2, std::string(70000, 'a'));
    CHECK((uint8_t)sf[0] == 0x82 && (uint8_t)sf[1] == 127 && sf.size() == 70010, "server frame 64-bit length");
}

static void testAudio() {
    std::vector<int16_t> x(4800);
    for (size_t i = 0; i < x.size(); i++) x[i] = (int16_t)(12000 * std::sin(2 * M_PI * 700 * (double)i / 12000) + 3000 * std::sin(2 * M_PI * 2100 * (double)i / 12000));
    AdpcmState st;
    std::string f = audioFrame(7, AudioCodec::Adpcm, 12000, -40.f, false, x, st);
    CHECK(f.size() == kAudioHeader + x.size() / 2, "adpcm size %zu", f.size());
    AudioCodec c; int rate; bool sq; std::vector<int16_t> y;
    CHECK(parseAudioFrame(f, c, rate, sq, y) && c == AudioCodec::Adpcm && rate == 12000 && !sq && y.size() == x.size(), "adpcm frame parses");
    double e = 0, s = 0;
    for (size_t i = 100; i < x.size(); i++) { s += (double)x[i] * x[i]; e += (double)(x[i] - y[i]) * (x[i] - y[i]); }
    const double snr = 10 * std::log10(s / (e + 1));
    CHECK(snr > 20, "ADPCM SNR %.1f dB", snr);
    // the second frame decodes on its own (the state travels in its header)
    std::string f2 = audioFrame(8, AudioCodec::Adpcm, 12000, -40.f, false, x, st);
    CHECK(parseAudioFrame(f2, c, rate, sq, y), "second frame");
    e = 0;
    for (size_t i = 0; i < x.size(); i++) e += (double)(x[i] - y[i]) * (x[i] - y[i]);
    CHECK(10 * std::log10(s / (e + 1)) > 20, "second frame on its own");
    f = audioFrame(9, AudioCodec::Pcm16, 8000, -10.f, false, x, st);
    CHECK(parseAudioFrame(f, c, rate, sq, y) && y == x, "pcm16 exact");
    f = audioFrame(10, AudioCodec::Pcm16, 8000, -100.f, true, x, st);
    CHECK(f.size() == kAudioHeader && parseAudioFrame(f, c, rate, sq, y) && sq && y.empty(), "squelched frame has no payload");
    std::vector<uint8_t> bins(300, 9);
    const std::string sp = spectrumFrame(5, 1e8, 1.024e8, -120, -20, bins);
    CHECK(sp.size() == kSpectrumHeader + 300 && (uint8_t)sp[0] == kFrameSpectrum && (uint8_t)sp[2] == (300 & 255) && (uint8_t)sp[3] == 1, "spectrum frame");
}

// mutations of valid messages plus random bytes: never a crash, never an accepted value out of range
static void fuzz() {
    const std::vector<std::string> seeds = {
        "{\"type\":\"hello\",\"proto\":1,\"password\":\"secret123\",\"name\":\"x\"}",
        "{\"type\":\"tune\",\"freq_hz\":145800000,\"mode\":\"nfm\",\"low_hz\":-6000,\"high_hz\":6000}",
        "{\"type\":\"rx\",\"squelch_db\":-90,\"agc\":\"fast\",\"gain_db\":10,\"deemph_us\":75}",
        "{\"type\":\"audio\",\"on\":true,\"codec\":\"adpcm\",\"rate\":12000}",
        "{\"type\":\"spectrum\",\"on\":true,\"bins\":512,\"fps\":5,\"start_hz\":100e6,\"stop_hz\":101e6}",
        "{\"type\":\"subscribe\",\"decoder\":\"adsb\",\"freq_hz\":1090000000}",
        "{\"type\":\"ping\",\"t\":1}"};
    const char* alphabet = "{}[]\":,0123456789.-+eE truefalsnul\\u\"abcxyz_\x01\x7f\xc3\xa9\xff";
    std::mt19937 rng(12345);
    int accepted = 0, bad = 0;
    for (int it = 0; it < 300000; it++) {
        std::string s = seeds[rng() % seeds.size()];
        const int muts = 1 + (int)(rng() % 6);
        for (int k = 0; k < muts; k++) {
            const int op = (int)(rng() % 5);
            const size_t pos = s.empty() ? 0 : rng() % s.size();
            if (op == 0 && !s.empty()) s.erase(pos, 1 + rng() % 3);
            else if (op == 1) s.insert(pos, 1, alphabet[rng() % strlen(alphabet)]);
            else if (op == 2 && !s.empty()) s[pos] = (char)(rng() & 0xFF);
            else if (op == 3) s.insert(pos, s.substr(rng() % (s.size() + 1), rng() % 20));
            else if (op == 4 && it % 50 == 0) s = std::string(rng() % 64, (char)(rng() & 0xFF));
        }
        ClientMsg m; std::string c, w;
        if (parseClientMsg(s, m, c, w)) {
            accepted++;
            // invariants of every accepted message
            bool good = m.freqHz >= 0 && m.freqHz <= 1e11 && m.squelchDb >= -200 && m.squelchDb <= 0 && m.gainDb >= -20 && m.gainDb <= 60 &&
                        m.bins >= 64 && m.bins <= 4096 && m.fps >= 1 && m.fps <= 30 && m.rateHz >= 8000 && m.rateHz <= 48000 &&
                        m.name.size() <= (size_t)kMaxNameLen && m.password.size() <= 128 && m.decoder.size() <= 24 &&
                        (!m.hasPass || m.highHz - m.lowHz >= 50) && (int)m.demod >= 0 && m.demod < Demod::Count;
            for (char ch : m.name) if ((unsigned char)ch < 0x20 || ch == '<' || ch == '>') good = false;
            if (!good) bad++;
        } else if (c.empty()) bad++;   // a refusal always says why
        // the frame reader on the same bytes as a "frame"
        WsIn r = wsParse(s, kMaxClientMsg);
        if (r.kind == 2 && r.used > s.size()) bad++;
    }
    CHECK(bad == 0, "fuzz: %d bad results", bad);
    CHECK(accepted > 1000, "fuzz: only %d mutated messages accepted (the mutations should keep some valid)", accepted);
    // random frames: a frame header followed by junk, any length field
    for (int it = 0; it < 200000; it++) {
        std::string s((size_t)(rng() % 40), '\0');
        for (char& ch : s) ch = (char)(rng() & 0xFF);
        WsIn r = wsParse(s);
        if (r.kind == 2 && (r.used > s.size() || r.payload.size() > kMaxClientMsg)) bad++;
        if (r.kind == 1 && r.closeCode != 1002 && r.closeCode != 1003 && r.closeCode != 1009) bad++;
    }
    CHECK(bad == 0, "frame fuzz: %d bad results", bad);
    printf("fuzz: %d of 300000 mutated messages accepted, all within range\n", accepted);
}

int main() {
    {
        onair_web::ClientMsg cm; std::string code, why;
        CHECK(onair_web::parseClientMsg("{\"type\":\"control\",\"take\":true}", cm, code, why) && cm.type == onair_web::ClientMsg::Control && cm.take, "control message parses");
        CHECK(!onair_web::parseClientMsg("{\"type\":\"control\"}", cm, code, why) && code == onair_web::err::BadMessage, "control needs take");
        CHECK(!onair_web::parseClientMsg("{\"type\":\"control\",\"take\":1}", cm, code, why), "take must be a boolean");
    }
    testJson();
    testMessages();
    testForbidden();
    testWs();
    testAudio();
    fuzz();
    return testSummary("web_proto");
}
