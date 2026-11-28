// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Mode Builder blocks of issue #666: Walsh coding, cyclic framing with the flywheel, custom payloads and the OFDM block. Each one has a
// loopback (generate, noise, decode, the payload comes back) at a moderate SNR; the stages alone and the JSON round trip are checked too.
#include "dect2/modebuilder/modebuilder.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace dect2::modebuilder;

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKM(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s  ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rnd(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

static Definition defOf(const std::string& json) {
    ParseResult r = parseDefinition(json);
    if (!r.ok) for (const Issue& i : r.issues) printf("  issue: %s\n", i.text().c_str());
    CHECK(r.ok);
    return r.def;
}
static bool hasError(const std::string& json, const std::string& where) {
    const ParseResult r = parseDefinition(json);
    for (const Issue& i : r.issues) if (i.error && i.text().find(where) != std::string::npos) return true;
    return false;
}

// FSK 9600 Bd, 16 bytes with a CRC-16, 76.8 kHz; `extra` goes in as more top-level keys
static std::string fskJson(const std::string& extra, const std::string& frame = R"("frame":{"length":{"mode":"fixed","bytes":16}})") {
    return R"({"onair_mode":1,"name":"blocks","sample_rate":76800,"modulation":{"type":"FSK","symbol_rate":9600},"sync":{"word":"0x2DD4","bits":16},)" +
           frame + R"(,"crc":{"preset":"CRC-16/CCITT-FALSE"})" + (extra.empty() ? "" : "," + extra) + "}";
}

// all frames come back, byte for byte, in order
static void expectLoop(const char* what, const Definition& d, const GenOptions& g, int count, double fs = 0) {
    uint32_t rng = 7;
    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < count; i++) frames.push_back(randomFrame(d, rng));
    const std::vector<cf> iq = generate(d, frames, g);
    const std::vector<Frame> out = decodeIq(d, iq, fs > 0 ? fs : d.sampleRate);
    size_t good = 0;
    for (const Frame& f : out) if (f.good()) good++;
    CHECKM(good == frames.size(), "%s: %zu of %zu frames", what, good, frames.size());
    if (good != frames.size()) {   // what the receiver saw, to find the stage that fails
        Definition dd = d;
        dd.showBadFrames = true;
        RxStats st;
        const std::vector<Frame> all = decodeIq(dd, iq, fs > 0 ? fs : d.sampleRate, 0, &st);
        printf("  debug %s: %zu frames out, syncHits %llu frames %llu bad %llu\n", what, all.size(), (unsigned long long)st.syncHits, (unsigned long long)st.frames, (unsigned long long)st.badFrames);
        for (size_t i = 0; i < all.size() && i < 4; i++) {
            int diff = 0;
            for (size_t k = 0; k < all[i].bytes.size() && i < frames.size() && k < frames[i].size(); k++) diff += __builtin_popcount((unsigned)(all[i].bytes[k] ^ frames[i][k]));
            printf("  debug frame %zu: t=%.5f snr=%.1f crcOk=%d fecFailed=%d bytes=%zu bitdiff=%d\n", i, all[i].timeSec, all[i].snrDb, (int)all[i].crcOk, (int)all[i].fecFailed, all[i].bytes.size(), diff);
        }
    }
    for (size_t i = 0; i < out.size() && i < frames.size(); i++) CHECKM(out[i].bytes == frames[i], "%s: frame %zu differs", what, i);
}

static void testWalshStage() {
    for (int k = 2; k <= 6; k++) {
        uint32_t s = 100 + (uint32_t)k;
        std::vector<uint8_t> bits(k * 37);
        for (auto& b : bits) b = (uint8_t)(rnd(s) & 1);
        std::vector<uint8_t> chips = stage::walshEncode(bits, k);
        CHECK(chips.size() == 37 * ((size_t)1 << k));
        CHECK(stage::walshDecode(chips, k, bits.size()) == bits);
        // the code words are orthogonal: distance 2^(k-1) between any two, so flipping a quarter of the chips minus one is still decoded
        const size_t n = (size_t)1 << k;
        std::vector<uint8_t> bad = chips;
        for (size_t w = 0; w < 37; w++) for (size_t j = 0; j < n / 4 - 1; j++) bad[w * n + (j * 3 + w) % n] ^= 1;
        CHECKM(stage::walshDecode(bad, k, bits.size()) == bits, "k=%d damaged chips", k);
    }
    // a padded last group: 7 bits with k = 3 -> 3 words, decoded back to 7 bits
    const std::vector<uint8_t> seven = {1, 0, 1, 1, 0, 0, 1};
    CHECK(stage::walshDecode(stage::walshEncode(seven, 3), 3, 7) == seven);
}

static void testWalshLoop() {
    for (int k : {2, 4, 6}) {
        Definition d = defOf(fskJson("\"walsh\":{\"k\":" + std::to_string(k) + "}"));
        CHECK(d.walshK == k);
        GenOptions g;
        g.snrDb = k == 2 ? 14 : 11;   // the spreading gain lets the higher k work at a lower SNR
        g.seed = 5 + (uint32_t)k;
        expectLoop(("walsh k=" + std::to_string(k)).c_str(), d, g, 8);
    }
    // with FEC and a scrambler in front of it
    Definition d = defOf(fskJson(R"("walsh":{"k":3},"fec":{"type":"hamming","code":"7,4"},"scrambler":{"type":"additive","taps":[9,5],"seed":"0x1FF"})"));
    GenOptions g;
    g.snrDb = 13;
    expectLoop("walsh + hamming + scrambler", d, g, 6);
    CHECK(hasError(fskJson(R"("walsh":{"k":7})"), "walsh.k"));
    CHECK(hasError(fskJson(R"("walsh":{"k":3})", R"("frame":{"length":{"mode":"end_flag","flag":"0x7E","min_bytes":2,"max_bytes":20}})"), "walsh"));
}

static void testCycleLoop() {
    const std::string j = fskJson(R"("cycle":{"symbols":400,"flywheel_misses":3})");
    Definition d = defOf(j);
    CHECK(d.cycleSymbols == 400);
    GenOptions g;
    g.snrDb = 16;
    g.seed = 3;
    expectLoop("cyclic frames", d, g, 7);
    CHECK(hasError(fskJson(R"("cycle":{"symbols":100})"), "cycle.symbols"));   // shorter than sync + 16 bytes + CRC
    // the flywheel: the first 3 bits of the third sync word are destroyed (replaced by noise); with up to 3 errors the cycle still holds
    uint32_t rng = 11;
    std::vector<std::vector<uint8_t>> frames;
    for (int i = 0; i < 6; i++) frames.push_back(randomFrame(d, rng));
    GenOptions q;
    q.snrDb = 200;
    std::vector<cf> iq = generate(d, frames, q);
    const double sps = d.sampleRate / d.symbolRate;
    const size_t s0 = (size_t)((q.gapSymbols + d.preamble.bits + 2 * d.cycleSymbols) * sps);
    uint32_t r2 = 99;
    for (size_t i = s0; i < s0 + (size_t)(3 * sps); i++) iq[i] = cf((float)((int)(rnd(r2) & 255) - 128) / 128.f, (float)((int)(rnd(r2) & 255) - 128) / 128.f);
    const std::vector<Frame> out = decodeIq(d, iq, d.sampleRate);
    size_t good = 0;
    for (const Frame& f : out) good += f.good();
    CHECKM(good == frames.size(), "flywheel kept %zu of %zu frames", good, frames.size());
    if (out.size() == frames.size()) CHECK(out[2].syncErrors > 0 || out[2].bytes == frames[2]);
}

static void testPayload() {
    Definition d = defOf(fskJson(R"("payload":{"text":"HELLO OnAir","offset_bytes":2})"));
    CHECK(d.payloadKind == PayloadKind::Text && d.payloadOffset == 2);
    uint32_t rng = 1;
    const std::vector<uint8_t> f = randomFrame(d, rng);
    CHECK(f.size() == 16 && std::memcmp(f.data() + 2, "HELLO OnAir", 11) == 0);
    bool present = false, ok = false;
    checkCrc(d, f, present, ok);
    CHECK(present && ok);
    GenOptions g;
    g.snrDb = 16;
    expectLoop("text payload", d, g, 5);
    uint32_t r2 = 1;
    const std::vector<cf> iq = generate(d, {randomFrame(d, r2), randomFrame(d, r2)}, g);
    const std::vector<Frame> out = decodeIq(d, iq, d.sampleRate);
    CHECK(out.size() == 2);
    for (const Frame& fr : out) CHECKM(fr.payload == "HELLO OnAir" && fr.display == "HELLO OnAir", "payload \"%s\" display \"%s\"", fr.payload.c_str(), fr.display.c_str());
    // hex, in a length-field frame (the length follows the payload)
    Definition h = defOf(fskJson(R"("payload":{"hex":"DE AD be ef 01 02","offset_bytes":1})",
                                 R"("frame":{"length":{"mode":"length_field","offset_bits":0,"width_bits":8,"min_bytes":4,"max_bytes":40}})"));
    uint32_t r3 = 1;
    const std::vector<uint8_t> hf = randomFrame(h, r3);
    CHECK(hf.size() == 1 + 6 + 2 && hf[0] == 9 && hf[1] == 0xDE && hf[6] == 0x02);
    expectLoop("hex payload in a length-field frame", h, g, 4);
    const std::vector<Frame> ho = decodeIq(h, generate(h, {hf}, g), h.sampleRate);
    CHECK(ho.size() == 1 && ho[0].payload == "DEADBEEF0102");
    // saved and read back
    const Definition back = defOf(toJson(h));
    CHECK(back.payloadKind == PayloadKind::Hex && back.payloadData == h.payloadData && back.payloadOffset == 1);
    CHECK(hasError(fskJson(R"("payload":{"text":"a","hex":"00"})"), "payload"));
    CHECK(hasError(fskJson(R"("payload":{"hex":"zz"})"), "payload"));
}

static std::string repl(std::string s, const std::string& from, const std::string& to) {
    const size_t p = s.find(from);
    if (p != std::string::npos) s.replace(p, from.size(), to);
    return s;
}

static std::string ofdmJson(int order, const std::string& extra = "") {
    return R"({"onair_mode":1,"name":"ofdm test","sample_rate":80000,"modulation":{"type":"OFDM","order":)" + std::to_string(order) +
           R"(,"symbol_rate":1000,"fft_size":64,"cyclic_prefix":16,"used_carriers":48,"pilot_every":6},"frame":{"length":{"mode":"fixed","bytes":16}},"crc":{"preset":"CRC-16/CCITT-FALSE"})" +
           (extra.empty() ? "" : "," + extra) + "}";
}

static void testOfdm() {
    for (int order : {2, 4}) {
        Definition d = defOf(ofdmJson(order));
        CHECK(d.mod == Mod::OFDM && d.ofdmFft == 64 && d.ofdmDataCarriers() == 40);
        CHECK(std::fabs(d.ofdmSampleRate() - 80000) < 1e-6);
        GenOptions g;
        g.snrDb = 20;
        g.seed = 9;
        expectLoop(order == 2 ? "OFDM BPSK" : "OFDM QPSK", d, g, 6);
        // a carrier frequency error of 4 % of the subcarrier spacing and an offset signal
        g.cfoHz = 50;
        expectLoop(order == 2 ? "OFDM BPSK with CFO" : "OFDM QPSK with CFO", d, g, 5);
    }
    // FEC, a custom payload and a faster sample rate (the block is interpolated to it and averaged back)
    Definition d = defOf(ofdmJson(4, R"("fec":{"type":"hamming","code":"7,4"},"payload":{"text":"OFDM CHAT"})"));
    GenOptions g;
    g.snrDb = 20;
    expectLoop("OFDM + hamming + payload", d, g, 4);
    uint32_t rng = 1;
    const std::vector<Frame> out = decodeIq(d, generate(d, {randomFrame(d, rng)}, g), d.sampleRate);
    CHECK(out.size() == 1 && out[0].payload == "OFDM CHAT");
    Definition fast = defOf(R"({"onair_mode":1,"name":"ofdm fast","sample_rate":240000,"modulation":{"type":"OFDM","order":2,"symbol_rate":1000,"fft_size":64,"cyclic_prefix":16,"used_carriers":48,"pilot_every":6},"frame":{"length":{"mode":"fixed","bytes":12}},"crc":{"preset":"CRC-16/CCITT-FALSE"}})");
    g.snrDb = 24;
    expectLoop("OFDM at 3x the block rate", fast, g, 4);
    // saved and read back, and the checks of meaning
    const Definition back = defOf(toJson(d));
    CHECK(back.mod == Mod::OFDM && back.ofdmCp == 16 && back.ofdmUsed == 48 && back.ofdmPilotEvery == 6 && back.order == 4);
    CHECK(hasError(repl(ofdmJson(2), "\"fft_size\":64", "\"fft_size\":48"), "fft_size"));
    CHECK(hasError(ofdmJson(2, R"("walsh":{"k":3})"), "walsh"));
    CHECK(hasError(repl(ofdmJson(2), "\"mode\":\"fixed\",\"bytes\":16", "\"mode\":\"end_flag\",\"flag\":\"0x7E\",\"min_bytes\":2,\"max_bytes\":20"), "frame.length.mode"));
}

static void testDefaultsUnchanged() {
    // a definition without the new keys writes none of them
    const std::string j = toJson(defOf(fskJson("")));
    CHECK(j.find("walsh") == std::string::npos && j.find("cycle") == std::string::npos && j.find("payload") == std::string::npos && j.find("OFDM") == std::string::npos);
}

int main() {
    testWalshStage();
    testWalshLoop();
    testCycleLoop();
    testPayload();
    testOfdm();
    testDefaultsUnchanged();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
