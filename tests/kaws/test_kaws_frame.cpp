// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS framing: callsign / grid / text coding, CRC cross-check with the Rust crate, blocks, beacons, chat, ack, data, voice superframe, classification
#include "dect2/kaws/kaws_frame.h"
#include "dect2/rs/kaws.h"
#include <cstdio>
#include <cstring>
using namespace dect2;
using namespace dect2::kaws;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

static Bits randomBits(size_t n, uint64_t seed) {
    kris::XorShift64 r{seed ? seed : 1};
    Bits b(n);
    for (auto& x : b) x = (uint8_t)(r.next() >> 33) & 1u;
    return b;
}

int main() {
    // CRC: the C++ foundation and the Rust crate agree on the known vectors
    const uint8_t chk[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(onair_kaws_crc16(chk, 9) == 0x29B1u, "crc16 %04x", onair_kaws_crc16(chk, 9));
    CHECK(kris::crc16(chk, 9) == 0x29B1u, "kris crc16");
    CHECK(onair_kaws_crc24(chk, 9) == 0x21CF02u, "crc24 %06x", onair_kaws_crc24(chk, 9));
    CHECK(onair_kaws_version() == 1u, "version");

    // weave: the first 16 directions are the spec string 1000011100100011 (1 = falling)
    int dirs[16];
    CHECK(onair_kaws_weave(16, dirs) == 16, "weave");
    const char* want = "1000011100100011";
    for (int i = 0; i < 16; i++) CHECK((dirs[i] < 0) == (want[i] == '1'), "weave bit %d", i);

    // callsign base 40
    CHECK(callsignValid("LY1ABC") && callsignValid("ly1abc/p") && !callsignValid("") && !callsignValid("TOOLONG123") && !callsignValid("A_B") && !callsignValid("   "), "valid");
    for (const char* cs : {"LY1ABC", "DL1ABC/P", "A", "9A1AAAAAA", "K1.-/", "W1AW"}) {
        uint64_t v = callsignEncode(cs);
        CHECK(v < (1ull << 48), "fits 48 bits %s", cs);
        CHECK(callsignDecode(v) == cs, "round trip %s -> %s", cs, callsignDecode(v).c_str());
    }
    CHECK(callsignEncode("lY1abc") == callsignEncode("LY1ABC"), "case folding");
    CHECK(callsignEncode("ZZZZZZZZZ") < (1ull << 48), "largest value < 2^48");
    CHECK(callsignEncode("LY1ABC") != callsignEncode("LY1ABD"), "distinct");
    CHECK(recipientHash("LY1ABC") < (1u << 22) && recipientHash("LY1ABC") != recipientHash("LY1ABD"), "hash");
    CHECK(recipientHash("ly1abc") == recipientHash("LY1ABC"), "hash case");

    // grid
    uint32_t g = 0;
    CHECK(gridEncode("KO24RM", g) && g < 18662400u && gridDecode(g) == "KO24RM", "grid");
    CHECK(gridEncode("AA00AA", g) && g == 0 && gridEncode("RR99XX", g) && g == 18662399u, "grid ends");
    CHECK(!gridEncode("SA00AA", g) && !gridEncode("AA00AY", g) && !gridEncode("KO24", g), "grid invalid");
    CHECK(18662400u < (1u << 25), "25 bits");

    // text alphabet: 64 distinct symbols, both directions
    for (int i = 0; i < 64; i++) CHECK(textCode(textChar(i)) == i, "text %d", i);
    CHECK(textCode('a') == textCode('A') && textChar(textCode('~')) == '~', "text fold");

    // blocks
    const int Ks[] = {128, 256, 336, 512, 510, 360, 600, 900};
    for (int K : Ks) {
        Bits pl = randomBits((size_t)(K - kBlockOverhead), (uint64_t)K);
        Bits blk;
        CHECK(makeBlock(K, 5, 9, pl, blk) && (int)blk.size() == K, "block K=%d", K);
        uint8_t id = 0, ix = 0;
        Bits back;
        CHECK(parseBlock(K, blk, id, ix, back) && id == 5 && ix == 9 && back == pl, "parse K=%d", K);
        for (size_t flip : {(size_t)0, (size_t)7, (size_t)(K / 2), (size_t)(K - 1)}) {
            Bits bad = blk;
            bad[flip] ^= 1;
            CHECK(!parseBlock(K, bad, id, ix, back), "corrupt K=%d bit %zu", K, flip);
        }
        CHECK(!makeBlock(K, 5, 9, Bits((size_t)(K - kBlockOverhead + 1), 0), blk), "too long");
    }

    // short beacon: 104 payload bits, fills the P0 block exactly
    BeaconShort bs;
    bs.call = "LY1ABC"; bs.grid = "KO24RM"; bs.powerStep = 40; bs.fineLat = 17; bs.fineLon = 60; bs.linkQuality = 201;
    Bits pb = encodeBeaconShort(bs);
    CHECK(pb.size() == 104, "short beacon %zu bits", pb.size());
    BeaconShort bs2;
    CHECK(decodeBeaconShort(pb, bs2) && bs2.call == "LY1ABC" && bs2.grid == "KO24RM" && bs2.powerStep == 40 && bs2.fineLat == 17 && bs2.fineLon == 60 && bs2.linkQuality == 201, "short beacon fields");
    CHECK(frameTypeOf(pb) == FrameType::BeaconShort && expectedMessageBits(pb) == 104, "type / length");
    Bits blk0;
    CHECK(makeBlock(128, 0, 0, pb, blk0) && classifyBlock(128, blk0) == BlockKind::Structured, "beacon fills P0 K=128");

    // full beacon: 147 + 6 n bits, fits one P1 block (232 usable) with 10 characters
    BeaconFull bf;
    bf.call = "LY1ABC"; bf.grid = "KO24RM"; bf.lat = 54.6872; bf.lon = 25.2797; bf.powerStep = 100; bf.linkQuality = 7; bf.text = "HELLO 73";
    Bits fb = encodeBeaconFull(bf);
    CHECK(fb.size() == 147 + 6 * 8, "full beacon %zu bits", fb.size());
    BeaconFull bf2;
    CHECK(decodeBeaconFull(fb, bf2) && bf2.call == "LY1ABC" && bf2.grid == "KO24RM" && bf2.text == "HELLO 73" && bf2.powerStep == 100, "full beacon fields");
    CHECK(std::abs(bf2.lat - 54.6872) < 1e-5 && std::abs(bf2.lon - 25.2797) < 1e-5, "lat/lon %.6f %.6f", bf2.lat, bf2.lon);
    bf.lat = -33.8688; bf.lon = -151.2093;
    CHECK(decodeBeaconFull(encodeBeaconFull(bf), bf2) && std::abs(bf2.lat + 33.8688) < 1e-5 && std::abs(bf2.lon + 151.2093) < 1e-5, "negative lat/lon");
    bf.text = "ABCDEFGHIJKLMNOP";
    CHECK(encodeBeaconFull(bf).size() == 147 + 60, "text capped at 10 characters");
    CHECK(147 + 60 <= 256 - kBlockOverhead, "fits P1");

    // chat: 401 bits for 50 characters (the spec's 417 bits include the 16-bit CRC that the block carries)
    Chat ch;
    ch.from = "LY1ABC"; ch.to = "DL2XYZ"; ch.seq = 12; ch.ackRef = 11;
    ch.text = "CQ CQ DE LY1ABC KO24 - NICE TO SEE YOU ON 14.093 MHZ";
    ch.text.resize(50);
    Bits cb = encodeChat(ch);
    CHECK(cb.size() == 401, "chat 50 chars = %zu bits", cb.size());
    Chat ch2;
    CHECK(decodeChat(cb, ch2) && ch2.from == "LY1ABC" && ch2.text == ch.text && ch2.seq == 12 && ch2.ackRef == 11 && ch2.toHash == recipientHash("DL2XYZ") && !ch2.firstMessage, "chat fields");
    ch.firstMessage = true;
    Bits cb2 = encodeChat(ch);
    CHECK(cb2.size() == 401 + 26, "first message adds 26 bits (%zu)", cb2.size());
    CHECK(decodeChat(cb2, ch2) && ch2.to == "DL2XYZ", "full recipient");
    ch.utf8 = true; ch.firstMessage = false; ch.text = "caf\xC3\xA9 ok";
    Bits cb3 = encodeChat(ch);
    CHECK(decodeChat(cb3, ch2) && ch2.utf8 && ch2.text == ch.text, "utf8 text");
    CHECK(expectedMessageBits(cb) == cb.size() && expectedMessageBits(cb2) == cb2.size() && expectedMessageBits(cb3) == cb3.size(), "expected lengths");
    ch.utf8 = false; ch.text = std::string(150, 'A');
    CHECK(decodeChat(encodeChat(ch), ch2) && ch2.text.size() == 100, "100 characters maximum");
    // chat message over blocks of every data profile, reassembled
    ch.text = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 .,?!";
    Bits big = encodeChat(ch);
    for (int K : {128, 256, 336, 512, 510, 360}) {
        std::vector<Bits> blocks = splitMessage(K, 3, big);
        CHECK(!blocks.empty(), "split K=%d", K);
        Bits joined;
        for (auto& b : blocks) { uint8_t i, x; Bits pl; CHECK(parseBlock(K, b, i, x, pl) && i == 3, "blk"); joined.insert(joined.end(), pl.begin(), pl.end()); }
        joined.resize(expectedMessageBits(joined));
        CHECK(joined == big && decodeChat(joined, ch2) && ch2.text == ch.text, "chat over K=%d (%zu blocks)", K, blocks.size());
    }
    CHECK(splitMessage(128, 1, Bits(104 * 16 + 1, 0)).empty(), "more than 16 blocks refused");

    // ack: 110 bits, raw, zero padded to K; P0 K=128 fits
    Ack ak;
    ak.from = "DL2XYZ"; ak.msgId = 9; ak.bitmap = 0x0000A5C3u; ak.snrDb = -22; ak.suggestedProfile = 3;
    for (int K : {128, 256, 900}) {
        Bits ab = encodeAck(ak, K);
        Ack a2;
        CHECK((int)ab.size() == K && decodeAck(ab, a2) && a2.from == "DL2XYZ" && a2.msgId == 9 && a2.bitmap == 0xA5C3u && a2.snrDb == -22 && a2.suggestedProfile == 3, "ack K=%d", K);
        CHECK(classifyBlock(K, ab) == BlockKind::Ack, "classify ack K=%d", K);
        Bits bad = ab;
        bad[20] ^= 1;
        CHECK(!decodeAck(bad, a2) && classifyBlock(K, bad) == BlockKind::Invalid, "corrupt ack");
    }
    ak.snrDb = 99; CHECK(decodeAck(encodeAck(ak, 128), ak) && ak.snrDb == 28, "snr clamp high");
    ak.snrDb = -99; CHECK(decodeAck(encodeAck(ak, 128), ak) && ak.snrDb == -35, "snr clamp low");

    // data frame with CRC-24
    std::vector<uint8_t> file(300);
    for (size_t i = 0; i < file.size(); i++) file[i] = (uint8_t)(i * 7 + 3);
    Bits db = encodeData(file);
    std::vector<uint8_t> f2;
    CHECK(decodeData(db, f2) && f2 == file && expectedMessageBits(db) == db.size(), "data frame");
    db[100] ^= 1;
    CHECK(!decodeData(db, f2), "data crc24 detects a flip");

    // voice side channel and superframe
    VoiceSide vs;
    vs.call = "LY1ABC"; vs.callNumber = 5; vs.slowData = randomBits(108, 77);
    Bits sb = voiceSideEncode(vs);
    VoiceSide vs2;
    CHECK(sb.size() == 180 && voiceSideDecode(sb, vs2) && vs2.call == "LY1ABC" && vs2.callNumber == 5 && vs2.slowData == vs.slowData, "voice side 180 bits");
    sb[100] ^= 1;
    CHECK(!voiceSideDecode(sb, vs2), "voice side crc");
    for (int prof : {6, 7}) {
        int cbits = voiceCodecBits(prof);
        std::vector<Bits> frames;
        for (int i = 0; i < kVoiceFrames; i++) frames.push_back(randomBits((size_t)cbits, 100 + (uint64_t)i));
        Bits blk, side = voiceSideEncode(vs);
        CHECK(voiceBuild(prof, frames, side, blk) && (int)blk.size() == (prof == 6 ? 600 : 900), "voice block P%d = %zu bits", prof, blk.size());
        std::vector<Bits> f3;
        Bits s3;
        CHECK(voiceParse(prof, blk, f3, s3) && f3 == frames && s3 == side, "voice parse P%d", prof);
    }
    CHECK(voiceCodecBits(3) == 0, "no codec bits for data profiles");

    // block classification: a structured block is not an ack and vice versa
    CHECK(classifyBlock(256, Bits(256, 0)) == BlockKind::Invalid, "all-zero block is invalid");

    printf(fails ? "kaws_frame: %d FAILED\n" : "kaws_frame: all passed\n", fails);
    return fails ? 1 : 0;
}
