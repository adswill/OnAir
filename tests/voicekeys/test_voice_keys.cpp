// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Voice key store + decryption tests, synthetic frames and synthetic keys only. No captured traffic, no key search.
// Verifies: the cipher primitives against public vectors; round-trip decryption of synthetically encrypted voice frames; that a wrong
// key is flagged (never searched); that nothing is decrypted when no key is present or the legal notice is not accepted; CSV import/export;
// and encrypted-at-rest persistence (keychain-style master key and passphrase, including a wrong passphrase).
#include "dect2/voicekeys/voice_crypto.h"
#include "dect2/voicekeys/voice_keys.h"
#include "dect2/voicekeys/voice_decrypt.h"
#include "dect2/voicekeys/key_persist.h"
#include "dect2/mesh/mesh_crypto.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <random>

using namespace dect2;
using namespace dect2::voicekeys;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> H(const char* s) { std::vector<uint8_t> v; meshcrypto::hexDecode(s, v); return v; }
static std::string hex(const uint8_t* p, size_t n) { return meshcrypto::hexEncode(p, n); }

// -------- RC4 known-answer (public test vector: key "Key", plaintext "Plaintext" -> BBF316E8D940AF0AD3) --------
static void testRc4() {
    Rc4 r; const char* k = "Key";
    r.setKey((const uint8_t*)k, 3);
    uint8_t ks[9], pt[9] = {'P','l','a','i','n','t','e','x','t'};
    r.keystream(ks, 9);
    for (int i = 0; i < 9; i++) pt[i] ^= ks[i];
    CHECK(hex(pt, 9) == "bbf316e8d940af0ad3", "rc4 KAT: %s", hex(pt, 9).c_str());
}

// -------- DES known-answer (FIPS/NBS: key 0x0E329232EA6D0D73, pt 0x8787878787878787 -> ct 0x0000000000000000) --------
static void testDes() {
    Des d; auto k = H("0e329232ea6d0d73");
    CHECK(d.setKey(k.data()), "des setKey");
    auto pt = H("8787878787878787");
    uint8_t ct[8], back[8];
    d.encryptBlock(pt.data(), ct);
    CHECK(hex(ct, 8) == "0000000000000000", "des enc KAT: %s", hex(ct, 8).c_str());
    d.decryptBlock(ct, back);
    CHECK(std::memcmp(back, pt.data(), 8) == 0, "des dec round-trip");
}

// -------- end-to-end: encrypt synthetic voice bits with a key, decrypt with the same / a wrong key --------
static void synthFrame(uint32_t seed, uint8_t* bits, int n) { std::mt19937 g(seed); for (int i = 0; i < n; i++) bits[i] = (uint8_t)(g() & 1); }

static void testRoundTrip(Algorithm alg, const char* keyHex, int frameBits) {
    VoiceKey k; k.algorithm = alg; k.name = "test"; auto err = parseKeyHex(alg, keyHex, k.key);
    CHECK(err.empty(), "parse %s: %s", algorithmName(alg), err.c_str());
    uint8_t mi[9] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99};

    // "encrypt": run a fresh decryptor (the cipher is symmetric) over known plaintext frames to get ciphertext.
    Decryptor enc(k, frameBits); enc.setIv(mi, 9);
    const int frames = 9;
    std::vector<std::vector<uint8_t>> clear(frames), cipher(frames);
    for (int f = 0; f < frames; f++) {
        clear[f].resize(frameBits); synthFrame(1000 + f, clear[f].data(), frameBits);
        cipher[f] = clear[f];
        CHECK(enc.decryptFrame(cipher[f].data()), "enc frame %s", algorithmName(alg));
    }
    // decrypt with the right key: must recover the plaintext exactly.
    Decryptor dec(k, frameBits); dec.setIv(mi, 9);
    for (int f = 0; f < frames; f++) {
        std::vector<uint8_t> t = cipher[f];
        dec.decryptFrame(t.data());
        CHECK(t == clear[f], "%s round-trip frame %d", algorithmName(alg), f);
    }
    // decrypt with a WRONG key: must NOT recover plaintext, and the wrong-key heuristic must flag it (not search).
    VoiceKey wrong = k;
    if (!wrong.key.empty()) wrong.key[0] ^= 0xFF;
    Decryptor bad(wrong, frameBits); bad.setIv(mi, 9);
    bool anyDiff = false;
    for (int f = 0; f < frames; f++) {
        std::vector<uint8_t> t = cipher[f];
        bad.decryptFrame(t.data());
        if (t != clear[f]) anyDiff = true;
        int errs = 0; for (int i = 0; i < frameBits; i++) errs += (t[i] != clear[f][i]);   // stand-in for a vocoder error count
        bad.reportFrameErrors(errs > frameBits / 8 ? 10 : 0);
    }
    CHECK(anyDiff, "%s wrong key changed the output", algorithmName(alg));
    CHECK(bad.status() == DecryptStatus::WrongKey, "%s wrong key flagged (status %d)", algorithmName(alg), (int)bad.status());
}

// -------- the store: no decryption without the legal notice, and nothing when no key is present --------
static void testStorePolicy() {
    KeyStore& s = KeyStore::instance();
    s.clear(); s.setLegalAccepted(false);
    CallKeyInfo info; info.protocol = Protocol::P25; info.keyId = 5;
    CHECK(s.candidates(info).empty(), "no candidates before the legal notice is accepted");

    VoiceKey k; k.protocol = Protocol::P25; k.algorithm = Algorithm::Aes256Ofb; k.keyId = 5; k.name = "friends";
    parseKeyHex(Algorithm::Aes256Ofb, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", k.key);
    s.add(k);
    CHECK(s.candidates(info).empty(), "still nothing before acceptance even with a matching key");
    s.setLegalAccepted(true);
    CHECK(s.candidates(info).size() == 1, "matching key found after acceptance");

    // a call with a DIFFERENT key id and no trial fallback (it carries an id) must not match the id-5 key
    CallKeyInfo other = info; other.keyId = 9;
    CHECK(s.candidates(other).empty(), "a different key id does not match (no search)");

    // lookupForCall: not encrypted -> nothing
    auto l0 = lookupForCall(info, /*encrypted=*/false, 144);
    CHECK(l0.status == DecryptStatus::NotEncrypted && !l0.decryptor, "clear call is never decrypted");
    // encrypted + match -> a decryptor
    auto l1 = lookupForCall(info, true, 144);
    CHECK(l1.status == DecryptStatus::Decrypted && l1.decryptor, "encrypted call with a key builds a decryptor");
    // encrypted + no key -> NoKey
    s.clear();
    auto l2 = lookupForCall(info, true, 144);
    CHECK(l2.status == DecryptStatus::NoKey && !l2.decryptor, "encrypted call with no key -> NoKey");
}

static void testCsv() {
    KeyStore& s = KeyStore::instance();
    s.clear();
    std::vector<std::string> errs;
    const char* csv =
        "name,protocol,algorithm,key,keyid,algid,talkgroup,unit,nac,cc,enabled\n"
        "club,DMR,RC4,0011223344,,,,,,,1\n"
        "sar,P25,AES-256-OFB,00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff,3,0x84,101,,0x293,,1\n"
        "bad,P25,AES-256-OFB,xyz,,,,,,,1\n";
    int n = s.importCsv(csv, errs);
    CHECK(n == 2, "imported 2 good keys (got %d)", n);
    CHECK(errs.size() == 1, "one bad line reported (got %zu)", errs.size());
    const std::string out = s.exportCsv();
    CHECK(out.find("club") != std::string::npos && out.find("sar") != std::string::npos, "export names present");
    CHECK(out.find("0011223344") != std::string::npos, "export key present (the Keys window the user opened)");
}

static void testPersist() {
    KeyStore& s = KeyStore::instance();
    s.clear();
    VoiceKey k; k.name = "n1"; k.protocol = Protocol::Nxdn; k.algorithm = Algorithm::Scrambler; k.keyId = 7;
    parseKeyHex(Algorithm::Scrambler, "1acf", k.key); s.add(k);

    // passphrase path
    std::string file;
    auto e = encryptStore(s.list(), {}, "correct horse", file);
    CHECK(e.empty(), "encrypt with passphrase: %s", e.c_str());
    bool needs = false; CHECK(fileNeedsPassphrase(file, needs) && needs, "file marked passphrase");
    CHECK(file.find("1acf") == std::string::npos, "the raw key does not appear in the encrypted file");
    std::vector<VoiceKey> back;
    CHECK(decryptStore(file, {}, "correct horse", back).empty() && back.size() == 1 && back[0].name == "n1", "decrypt with right passphrase");
    CHECK(!decryptStore(file, {}, "wrong passphrase", back).empty(), "wrong passphrase rejected");
    {   // the PBKDF2 round count (bytes 23..26: magic 6, flag 1, salt 16) comes from the file: absurd values are refused at once
        std::string evil = file;
        for (int i = 0; i < 4; i++) evil[23 + i] = (char)0xFF;
        CHECK(!decryptStore(evil, {}, "correct horse", back).empty(), "4 billion rounds refused");
        for (int i = 0; i < 4; i++) evil[23 + i] = 0;
        CHECK(!decryptStore(evil, {}, "correct horse", back).empty(), "0 rounds refused");
    }

    // keychain-style master key path
    std::vector<uint8_t> master(32); for (int i = 0; i < 32; i++) master[i] = (uint8_t)(i * 7 + 1);
    std::string file2;
    CHECK(encryptStore(s.list(), master, "", file2).empty(), "encrypt with master key");
    CHECK(fileNeedsPassphrase(file2, needs) && !needs, "master-key file not marked passphrase");
    std::vector<VoiceKey> back2;
    CHECK(decryptStore(file2, master, "", back2).empty() && back2.size() == 1, "decrypt with master key");
    std::vector<uint8_t> wrongMaster = master; wrongMaster[0] ^= 1;
    CHECK(!decryptStore(file2, wrongMaster, "", back2).empty(), "wrong master key rejected");
}

static void testKeychainLoss() {
    // a keychain read that fails (locked, denied) must not make a new master key: the old key file would never open again
    KeyStore& s = KeyStore::instance();
    s.clear();
    VoiceKey k; k.name = "keep"; k.protocol = Protocol::Nxdn; k.algorithm = Algorithm::Scrambler; k.keyId = 1;
    parseKeyHex(Algorithm::Scrambler, "1acf", k.key); s.add(k);
    std::vector<uint8_t> stored;       // the "keychain"
    bool failReads = false;
    int sets = 0;
    setSecretProvider([&](const std::string&, std::vector<uint8_t>& out) { if (failReads || stored.empty()) return false; out = stored; return true; },
                      [&](const std::string&, const std::vector<uint8_t>& d) { stored = d; sets++; return true; });
    const std::string path = (std::filesystem::temp_directory_path() / "onair_vk_test_keys.oavk").string();
    std::remove(path.c_str());
    CHECK(saveToFile(path).empty() && sets == 1 && stored.size() == 32, "first save makes the key");
    const std::vector<uint8_t> first = stored;
    s.clear();
    CHECK(loadFromFile(path).empty() && s.list().size() == 1, "load with the key");
    // the keychain stops answering: load fails and nothing is replaced
    failReads = true;
    s.clear();
    CHECK(!loadFromFile(path).empty() && sets == 1 && stored == first, "a failed read on load makes no new key");
    // and a save over the existing keychain file is refused instead of replacing the key
    s.add(k);
    CHECK(!saveToFile(path).empty() && sets == 1 && stored == first, "a failed read on save makes no new key");
    failReads = false;
    s.clear();
    CHECK(loadFromFile(path).empty() && s.list().size() == 1, "the old file still opens with the old key");
    // the keychain item is gone for good: the user discards the old file (moved aside, not deleted) and can save again
    CHECK(discardKeyFile(path).empty() && !std::ifstream(path).good() && std::ifstream(path + ".discarded").good(), "discard moves the old file aside");
    failReads = true;
    s.add(k);
    CHECK(saveToFile(path).empty(), "after a discard a save may make a new key");
    std::remove((path + ".discarded").c_str());
    failReads = false;
    // with no file at all a new key may be made
    std::remove(path.c_str());
    stored.clear(); sets = 0;
    s.add(k);
    CHECK(saveToFile(path).empty() && sets == 1, "no file: a new key is made");
    std::remove(path.c_str());
    setSecretProvider({}, {}, {});
    s.clear();
}

static void testPbkdf2() {
    // RFC 6070 is for SHA-1; use a self-consistent check: same inputs -> same output, different salt -> different output.
    uint8_t a[32], b[32];
    pbkdf2HmacSha256((const uint8_t*)"pw", 2, (const uint8_t*)"salt1234", 8, 1000, a, 32);
    pbkdf2HmacSha256((const uint8_t*)"pw", 2, (const uint8_t*)"salt1234", 8, 1000, b, 32);
    CHECK(std::memcmp(a, b, 32) == 0, "pbkdf2 deterministic");
    pbkdf2HmacSha256((const uint8_t*)"pw", 2, (const uint8_t*)"salt9999", 8, 1000, b, 32);
    CHECK(std::memcmp(a, b, 32) != 0, "pbkdf2 salt matters");
}

int main() {
    testRc4();
    testDes();
    testRoundTrip(Algorithm::Rc4, "0011223344", 144);        // P25 ADP-style 40-bit RC4 over an IMBE frame
    testRoundTrip(Algorithm::DesOfb, "0e329232ea6d0d73", 144);
    testRoundTrip(Algorithm::Aes256Ofb, "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", 144);
    testRoundTrip(Algorithm::Scrambler, "1acf", 72);         // NXDN/dPMR scrambler over a VCH
    testStorePolicy();
    testCsv();
    testPersist();
    testKeychainLoss();
    testPbkdf2();
    if (fails == 0) printf("voice_keys: all tests passed\n");
    else printf("voice_keys: %d FAILED\n", fails);
    return fails ? 1 : 0;
}
