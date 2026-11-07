// Hytera Basic / Enhanced Privacy: known-answer vectors computed with the DSD-FME algorithm (ISC, reference harness reproduced from
// dsd_mbe.c / crypt-rc4.c), plus synthetic encrypt-then-decrypt of 49-bit frames through the key store and Decryptor.
// Synthetic only: no live Hytera recording was available, so this proves the implementation matches DSD-FME, not a real radio.
#include "dect2/voicekeys/hytera.h"
#include "dect2/voicekeys/voice_decrypt.h"
#include "dect2/voicekeys/voice_keys.h"
#include <cstdio>
#include <random>
using namespace dect2::voicekeys;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint64_t pack(const uint8_t* b) { uint64_t v = 0; for (int i = 0; i < 49; i++) v = (v << 1) | b[i]; return v; }
static std::vector<uint8_t> seq(uint8_t a, int n, uint8_t step = 1) { std::vector<uint8_t> v; for (int i = 0; i < n; i++) v.push_back((uint8_t)(a + i * step)); return v; }

static void bpKat() {
    uint8_t o[49];
    const std::vector<uint8_t> k40 = {0x11, 0x22, 0x33, 0x44, 0x55};
    struct { int f; uint64_t v; } a[] = {{0, 0x022446688AA22ULL}, {3, 0x0455112233445ULL}, {17, 0x1544488CD1154ULL}};
    for (auto& t : a) { CHECK(hyteraBpFrameKeystream(k40, t.f, o) && pack(o) == t.v, "bp40 frame %d %013llX", t.f, (unsigned long long)pack(o)); }
    const std::vector<uint8_t> k128 = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xFF, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    CHECK(hyteraBpFrameKeystream(k128, 5, o) && pack(o) == 0x1BFC004488CD1ULL, "bp128 %013llX", (unsigned long long)pack(o));
    std::vector<uint8_t> k256 = {1, 2, 3, 4, 5, 6, 7, 8, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                                 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    CHECK(hyteraBpFrameKeystream(k256, 17, o) && pack(o) == 0x044484C505458ULL, "bp256 %013llX", (unsigned long long)pack(o));
    CHECK(!hyteraBpFrameKeystream(seq(1, 7), 0, o), "7-byte key refused");
}

static void enhKat() {
    uint8_t ks[135];
    const uint8_t mi[5] = {1, 2, 3, 4, 5};
    CHECK(hyteraEnhancedKeystream({0xAA, 0xBB, 0xCC, 0xDD, 0xEE}, mi, ks), "enh ok");
    const uint8_t head[16] = {0x4B, 0x93, 0x8A, 0x36, 0x1C, 0xD7, 0xE7, 0xB0, 0x25, 0x18, 0x70, 0xBB, 0x1B, 0xD8, 0x26, 0x2A};
    CHECK(memcmp(ks, head, 16) == 0, "enhanced head");
    const uint8_t tail[7] = {0x2F, 0x31, 0xF8, 0x69, 0x04, 0x1D, 0x78};
    CHECK(memcmp(ks + 128, tail, 7) == 0, "enhanced tail");
}

// encrypt a synthetic superframe with the reference helper, decrypt through the Decryptor built from a VoiceKey
static void roundTrip(Algorithm alg, const std::vector<uint8_t>& key, bool useMi) {
    VoiceKey vk; vk.name = "test"; vk.protocol = Protocol::Dmr; vk.algorithm = alg; vk.key = key;
    const uint8_t mi[5] = {9, 8, 7, 6, 5};
    std::mt19937 g(7);
    uint8_t plain[18][49], enc[18][49];
    for (int f = 0; f < 18; f++) for (int i = 0; i < 49; i++) plain[f][i] = (uint8_t)(g() & 1);
    for (int i = 0; i < 49; i++) plain[4][i] = (uint8_t)((0xF801A99F8CE080ULL >> (55 - i)) & 1);   // one silence frame stays clear
    memcpy(enc, plain, sizeof enc);
    HyteraDecryptor h(alg == Algorithm::HyteraBp ? HyteraDecryptor::Basic : HyteraDecryptor::Enhanced, key);
    if (useMi) h.setMessageIndicator(mi);
    h.startSuperframe();
    for (int f = 0; f < 18; f++) h.applyFrame(enc[f]);
    CHECK(memcmp(enc[0], plain[0], 49) != 0, "frame 0 changed");
    CHECK(memcmp(enc[4], plain[4], 49) == 0, "silence frame untouched");
    Decryptor d(vk, 49);
    CHECK(d.status() != DecryptStatus::Unsupported, "decryptor accepted %s", algorithmName(alg));
    if (useMi) d.setIv(mi, 5);
    d.startSuperframe();
    for (int f = 0; f < 18; f++) d.decryptFrame(enc[f]);
    CHECK(memcmp(enc, plain, sizeof enc) == 0, "round trip %s", algorithmName(alg));
    // wrong key: not recovered
    VoiceKey bad = vk; bad.key[0] ^= 0x80;
    Decryptor w(bad, 49); if (useMi) w.setIv(mi, 5);
    uint8_t again[18][49]; memcpy(again, plain, sizeof again);
    HyteraDecryptor h2(alg == Algorithm::HyteraBp ? HyteraDecryptor::Basic : HyteraDecryptor::Enhanced, key);
    if (useMi) h2.setMessageIndicator(mi);
    for (int f = 0; f < 18; f++) h2.applyFrame(again[f]);
    for (int f = 0; f < 18; f++) w.decryptFrame(again[f]);
    CHECK(memcmp(again, plain, sizeof again) != 0, "wrong key must not decrypt");
}

static void storeRules() {
    std::vector<uint8_t> o;
    CHECK(parseKeyHex(Algorithm::HyteraBp, "1122334455", o).empty(), "bp 5 bytes ok");
    CHECK(!parseKeyHex(Algorithm::HyteraBp, "112233", o).empty(), "bp 3 bytes refused");
    CHECK(parseKeyHex(Algorithm::HyteraEnhanced, "AABBCCDDEE", o).empty(), "enh 5 bytes ok");
    CHECK(!parseKeyHex(Algorithm::HyteraEnhanced, "AABBCCDDEE00", o).empty(), "enh 6 bytes refused");
    Algorithm a; CHECK(algorithmFromName("Hytera-BP", a) && a == Algorithm::HyteraBp, "name bp");
    CHECK(algorithmFromName("hytera-enhanced", a) && a == Algorithm::HyteraEnhanced, "name enh");
    CHECK(algorithmFromName(algorithmName(Algorithm::HyteraEnhanced), a) && a == Algorithm::HyteraEnhanced, "name round trip");
    VoiceKey vk; vk.algorithm = Algorithm::HyteraBp; vk.key = seq(1, 5);
    Decryptor wrongBits(vk, 72);
    CHECK(wrongBits.status() == DecryptStatus::Unsupported, "72-bit frames refused for Hytera");
}

int main() {
    bpKat(); enhKat();
    roundTrip(Algorithm::HyteraBp, seq(0x11, 5, 0x11), false);
    roundTrip(Algorithm::HyteraBp, seq(0x20, 16), false);
    roundTrip(Algorithm::HyteraBp, seq(0x40, 32, 3), false);
    roundTrip(Algorithm::HyteraEnhanced, seq(0xAA, 5, 0x11), true);
    storeRules();
    printf(fails ? "hytera: %d FAILED\n" : "hytera: all passed\n", fails);
    return fails ? 1 : 0;
}
