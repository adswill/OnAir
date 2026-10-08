// Known-answer tests for the crypto behind the Meshtastic and MeshCore packet layers.
// Sources: FIPS 197 appendix C, NIST SP 800-38A F.1.1/F.5.1/F.5.5, RFC 3686 section 6 (as used in the Meshtastic firmware's
// test/test_crypto), FIPS 180-4 examples, RFC 4231 test cases 1-4 and 6, RFC 8032 section 7.1 tests 1 and 2.
#include "dect2/mesh_crypto.h"
#include <cstdio>
#include <cstring>
using namespace dect2::meshcrypto;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<uint8_t> H(const char* s) { std::vector<uint8_t> v; hexDecode(s, v); return v; }
static std::vector<uint8_t> S(const char* s) { return std::vector<uint8_t>(s, s + strlen(s)); }
static std::string hex(const uint8_t* p, size_t n) { return hexEncode(p, n); }

static void sha(const std::vector<uint8_t>& in, const char* want256, const char* what) {
    uint8_t o[32];
    sha256(in.data(), in.size(), o);
    CHECK(hex(o, 32) == want256, "sha256 %s: %s", what, hex(o, 32).c_str());
    // incremental in odd pieces must agree
    Sha256 s;
    for (size_t i = 0; i < in.size(); i += 7) s.update(in.data() + i, in.size() - i < 7 ? in.size() - i : 7);
    s.final(o);
    CHECK(hex(o, 32) == want256, "sha256 pieces %s", what);
}

static void aes(const char* key, const char* pt, const char* ct, const char* what) {
    const auto k = H(key), p = H(pt);
    Aes a;
    CHECK(a.setKey(k.data(), k.size()), "key %s", what);
    uint8_t o[16], back[16];
    a.encryptBlock(p.data(), o);
    CHECK(hex(o, 16) == ct, "aes enc %s: %s", what, hex(o, 16).c_str());
    a.decryptBlock(o, back);
    CHECK(std::memcmp(back, p.data(), 16) == 0, "aes dec %s", what);
}

static void ctr(const char* key, const char* iv, const char* pt, const char* ct, const char* what) {
    const auto k = H(key), v = H(iv);
    auto d = H(pt);
    CHECK(aesCtr(k.data(), k.size(), v.data(), d.data(), d.size()), "ctr key %s", what);
    CHECK(hex(d.data(), d.size()) == ct, "ctr %s: %s", what, hex(d.data(), d.size()).c_str());
}

static void hmac(const std::vector<uint8_t>& key, const std::vector<uint8_t>& msg, const char* want, const char* what) {
    uint8_t o[32];
    hmacSha256(key.data(), key.size(), msg.data(), msg.size(), o);
    CHECK(hex(o, 32) == want, "hmac %s: %s", what, hex(o, 32).c_str());
}

int main() {
    // FIPS 197 appendix C.1 and C.3
    aes("000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff", "69c4e0d86a7b0430d8cdb78070b4c55a", "FIPS197 C.1");
    aes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "00112233445566778899aabbccddeeff",
        "8ea2b7ca516745bfeafc49904b496089", "FIPS197 C.3");
    // SP 800-38A F.1.1 (ECB-AES128) and the AES-256 ECB vectors the Meshtastic firmware test takes from NIST AES_ECB.pdf
    aes("2b7e151628aed2a6abf7158809cf4f3c", "6bc1bee22e409f96e93d7e117393172a", "3ad77bb40d7a3660a89ecaf32466ef97", "38A F.1.1 b1");
    aes("2b7e151628aed2a6abf7158809cf4f3c", "ae2d8a571e03ac9c9eb76fac45af8e51", "f5d3d58503b9699de785895a96fdbaaf", "38A F.1.1 b2");
    aes("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", "6bc1bee22e409f96e93d7e117393172a", "f3eed1bdb5d2a03c064b5a7e3db181f8", "NIST ECB256 b1");
    aes("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", "ae2d8a571e03ac9c9eb76fac45af8e51", "591ccb10d410ed26dc5ba74a31362870", "NIST ECB256 b2");
    aes("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", "30c81c46a35ce411e5fbc1191a0a52ef", "b6ed21b99ca6f4f9f153e7b1beafed1d", "NIST ECB256 b3");
    { // ECB with a partial last block is zero padded (MeshCore Utils::encrypt)
        const auto k = H("2b7e151628aed2a6abf7158809cf4f3c");
        const auto p = H("6bc1bee22e409f96e93d7e117393172a" "ae2d");
        auto e = aesEcbEncrypt(k.data(), k.size(), p.data(), p.size());
        CHECK(e.size() == 32, "ecb padded size %zu", e.size());
        auto d = aesEcbDecrypt(k.data(), k.size(), e.data(), e.size());
        CHECK(d.size() == 32 && std::memcmp(d.data(), p.data(), 18) == 0 && d[18] == 0 && d[31] == 0, "ecb padded round trip");
    }
    // SP 800-38A F.5.1 (CTR-AES128) and F.5.5 (CTR-AES256), four blocks, the counter carries into byte 14
    ctr("2b7e151628aed2a6abf7158809cf4f3c", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
        "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee", "38A F.5.1");
    ctr("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
        "601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c52b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6", "38A F.5.5");
    // RFC 3686 section 6 vectors #1 and #7 as used by the Meshtastic firmware (test/test_crypto, test_AES_CTR)
    ctr("AE6852F8121067CC4BF7A5765577F39E", "00000030000000000000000000000001", "53696e676c6520626c6f636b206d7367" /* "Single block msg" */,
        "e4095d4fb7a7b3792d6175a3261311b8", "RFC3686 AES128");
    ctr("776BEFF2851DB06F4C8A0542C8696F6C6A81AF1EEC96B4D37FC1D689E6C1C104", "00000060DB5672C97AA8F0B200000001", "53696e676c6520626c6f636b206d7367",
        "145ad01dbf824ec7560863dc71e3e0c0", "RFC3686 AES256");
    { // CTR with a length that is not a block multiple: only the used keystream bytes are applied
        const auto k = H("2b7e151628aed2a6abf7158809cf4f3c"), iv = H("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
        auto d = H("6bc1bee22e409f96e93d7e117393172aae2d8a");
        aesCtr(k.data(), k.size(), iv.data(), d.data(), d.size());
        CHECK(hex(d.data(), d.size()) == "874d6191b620e3261bef6864990db6ce9806f6", "ctr partial block");
    }
    CHECK(![] { Aes a; uint8_t k[24] = {}; return a.setKey(k, 24); }(), "AES-192 is not offered");

    // SHA-256: FIPS 180-4 examples, and the vectors in the firmware test (empty, d3, 11af)
    sha(S("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    sha(S("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "448 bit");
    sha(S(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    sha(H("d3"), "28969cdfa74a12c82f3bad960b0b000aca2ac329deea5c2328ebc6f2ba9802c1", "d3");
    sha(H("11af"), "5ca7133fa735326081558ac312c620eeca9970d1e70a4b95533d956f072d1f98", "11af");
    { // one million 'a' (FIPS 180-4)
        std::vector<uint8_t> m(1000000, 'a');
        sha(m, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "1M a");
    }
    // SHA-512 (FIPS 180-4)
    { uint8_t o[64]; const auto m = S("abc"); sha512(m.data(), m.size(), o);
      CHECK(hex(o, 64) == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", "sha512 abc"); }
    { uint8_t o[64]; sha512(nullptr, 0, o);
      CHECK(hex(o, 64) == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", "sha512 empty"); }
    { uint8_t o[64]; const auto m = S("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu");
      sha512(m.data(), m.size(), o);
      CHECK(hex(o, 64) == "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909", "sha512 896 bit"); }

    // HMAC-SHA256: RFC 4231 test cases 1, 2, 3, 4, 6
    hmac(std::vector<uint8_t>(20, 0x0b), S("Hi There"), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "RFC4231 #1");
    hmac(S("Jefe"), S("what do ya want for nothing?"), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "RFC4231 #2");
    hmac(std::vector<uint8_t>(20, 0xaa), std::vector<uint8_t>(50, 0xdd), "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", "RFC4231 #3");
    hmac(H("0102030405060708090a0b0c0d0e0f10111213141516171819"), std::vector<uint8_t>(50, 0xcd),
         "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", "RFC4231 #4");
    hmac(std::vector<uint8_t>(131, 0xaa), S("Test Using Larger Than Block-Size Key - Hash Key First"),
         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "RFC4231 #6");
    // a 32 byte key padded with zeros, as MeshCore's HMAC key for a 16 byte channel secret is built (value from Python hmac/hashlib)
    { std::vector<uint8_t> key(32, 0); const auto s = H("8b3387e9c5cdea6ac9e5edbaa115cd72"); std::memcpy(key.data(), s.data(), 16);
      hmac(key, S("abc"), "767e4fa0a768dbeb0486736341256999072c1dce49ad9fb448c8de1098b9429f", "padded key"); }

    // Ed25519: RFC 8032 section 7.1 tests 1 and 2
    struct Ed { const char *sk, *pk, *msg, *sig; };
    const Ed eds[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    };
    for (const Ed& e : eds) {
        const auto sk = H(e.sk), msg = H(e.msg), want = H(e.sig);
        uint8_t pk[32], sig[64];
        ed25519PublicKey(sk.data(), pk);
        CHECK(hex(pk, 32) == e.pk, "ed25519 public key: %s", hex(pk, 32).c_str());
        ed25519Sign(sk.data(), msg.data(), msg.size(), sig);
        CHECK(hex(sig, 64) == e.sig, "ed25519 signature: %s", hex(sig, 64).c_str());
        CHECK(ed25519Verify(want.data(), msg.data(), msg.size(), pk), "ed25519 verify RFC signature");
        auto bad = want; bad[10] ^= 1;
        CHECK(!ed25519Verify(bad.data(), msg.data(), msg.size(), pk), "ed25519 rejects a changed R");
        bad = want; bad[40] ^= 4;
        CHECK(!ed25519Verify(bad.data(), msg.data(), msg.size(), pk), "ed25519 rejects a changed S");
        auto m2 = msg; m2.push_back(0);
        CHECK(!ed25519Verify(want.data(), m2.data(), m2.size(), pk), "ed25519 rejects a changed message");
    }
    { // longer message, cross-checked with OpenSSL 3.6.4 (pkeyutl -sign -rawin on the PKCS#8 form of the seed)
        const auto sk = H("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
        const auto msg = S("MeshCore advert signing check, 80 bytes of message to cross-check against OpenSSL 3: 0123456789");
        uint8_t pk[32], sig[64];
        ed25519PublicKey(sk.data(), pk);
        ed25519Sign(sk.data(), msg.data(), msg.size(), sig);
        CHECK(hex(pk, 32) == "79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664", "openssl cross-check public key");
        CHECK(hex(sig, 64) == "91596fe643e3d9df317824ecc2ffefee2d96ddd8401732d209242a4b0409cd02a9be77dd6eadfc3bea16f32e7a3ada9b6fcc82a49e7c5fc54ebcbdfd9922d309", "openssl cross-check signature");
        CHECK(ed25519Verify(sig, msg.data(), msg.size(), pk), "verify own signature");
    }

    // base64 and hex helpers
    { std::vector<uint8_t> v;
      CHECK(base64Decode("1PG7OiApB1nwvP+rz05pAQ==", v) && hex(v.data(), v.size()) == "d4f1bb3a20290759f0bcffabcf4e6901", "base64 default PSK");
      CHECK(base64Decode("AQ==", v) && v.size() == 1 && v[0] == 1, "base64 AQ==");
      CHECK(base64Decode("izOH6cXN6mrJ5e26oRXNcg==", v) && hex(v.data(), v.size()) == "8b3387e9c5cdea6ac9e5edbaa115cd72", "base64 MeshCore Public");
      CHECK(base64Encode(v.data(), v.size()) == "izOH6cXN6mrJ5e26oRXNcg==", "base64 encode");
      CHECK(!base64Decode("ab*d", v), "base64 rejects junk");
      CHECK(hexDecode("00 ff:A5", v) && v.size() == 3 && v[2] == 0xa5, "hex");
      CHECK(!hexDecode("abc", v), "hex odd length"); }

    if (fails) return 1;
    printf("mesh_crypto_kat ok\n");
    return 0;
}
