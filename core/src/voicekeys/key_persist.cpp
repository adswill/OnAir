// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Encrypted-at-rest persistence for the voice key store (see key_persist.h). AES-256-CTR + HMAC-SHA256 from dect2::meshcrypto.
// Key material is never logged and never leaves this file in the clear.
#include "dect2/voicekeys/key_persist.h"
#include "dect2/mesh/mesh_crypto.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace dect2 {
namespace voicekeys {

const char* const kMasterAccount = "onair.voicekeys.master";

namespace {
SecretGet gGet;
SecretSet gSet;
SecretDel gDel;

void randomBytes(uint8_t* p, size_t n) {
    std::random_device rd;
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rd() & 0xFF);
}
void putU32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)((v >> (8 * i)) & 0xFF)); }
uint32_t getU32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void putStr(std::string& s, const std::string& v) { putU32(s, (uint32_t)v.size()); s += v; }
void putBytes(std::string& s, const std::vector<uint8_t>& v) { putU32(s, (uint32_t)v.size()); s.append((const char*)v.data(), v.size()); }
bool getStr(const uint8_t*& p, const uint8_t* end, std::string& out) {
    if (end - p < 4) return false; uint32_t n = getU32(p); p += 4;
    if ((size_t)(end - p) < n) return false; out.assign((const char*)p, n); p += n; return true;
}
bool getBytes(const uint8_t*& p, const uint8_t* end, std::vector<uint8_t>& out) {
    if (end - p < 4) return false; uint32_t n = getU32(p); p += 4;
    if ((size_t)(end - p) < n) return false; out.assign(p, p + n); p += n; return true;
}
const char kMagic[6] = {'O','A','V','K','1','\n'};
} // namespace

void setSecretProvider(SecretGet get, SecretSet set, SecretDel del) { gGet = std::move(get); gSet = std::move(set); gDel = std::move(del); }
bool haveSecretProvider() { return (bool)gGet && (bool)gSet; }

bool keychainMasterKey(std::vector<uint8_t>& out, bool createIfMissing) {
    if (!haveSecretProvider()) return false;
    if (gGet(kMasterAccount, out) && out.size() == 32) return true;
    out.clear();
    // the read can fail for other reasons than "no such item" (locked keychain, access denied): a new key would replace the old one and the
    // existing key file would never open again, so a new key is made only when the caller says there is nothing to lose
    if (!createIfMissing) return false;
    out.assign(32, 0);
    randomBytes(out.data(), 32);
    if (!gSet(kMasterAccount, out)) { out.clear(); return false; }
    return true;
}

void pbkdf2HmacSha256(const uint8_t* pw, size_t pwLen, const uint8_t* salt, size_t saltLen, uint32_t rounds, uint8_t* out, size_t outLen) {
    using meshcrypto::hmacSha256;
    uint32_t block = 1;
    size_t done = 0;
    while (done < outLen) {
        std::vector<uint8_t> saltBlock(salt, salt + saltLen);
        for (int i = 3; i >= 0; i--) saltBlock.push_back((uint8_t)((block >> (8 * i)) & 0xFF));
        uint8_t u[32], t[32];
        hmacSha256(pw, pwLen, saltBlock.data(), saltBlock.size(), u);
        std::memcpy(t, u, 32);
        for (uint32_t r = 1; r < rounds; r++) {
            hmacSha256(pw, pwLen, u, 32, u);
            for (int k = 0; k < 32; k++) t[k] ^= u[k];
        }
        const size_t take = outLen - done < 32 ? outLen - done : 32;
        std::memcpy(out + done, t, take);
        done += take; block++;
    }
}

std::string serializeKeys(const std::vector<VoiceKey>& keys) {
    std::string s;
    putU32(s, 1);                       // format version
    putU32(s, (uint32_t)keys.size());
    for (const auto& k : keys) {
        putStr(s, k.name);
        s.push_back((char)k.protocol);
        s.push_back((char)k.algorithm);
        putBytes(s, k.key);
        s.push_back(k.enabled ? 1 : 0);
        putU32(s, (uint32_t)k.keyId);
        putU32(s, (uint32_t)k.algId);
        putU32(s, (uint32_t)(int32_t)k.talkgroup);
        putU32(s, (uint32_t)(int32_t)k.unitId);
        putU32(s, (uint32_t)k.nac);
        putU32(s, (uint32_t)k.colourCode);
    }
    return s;
}

bool deserializeKeys(const std::string& blob, std::vector<VoiceKey>& out) {
    out.clear();
    const uint8_t* p = (const uint8_t*)blob.data();
    const uint8_t* end = p + blob.size();
    if (end - p < 8) return false;
    uint32_t ver = getU32(p); p += 4;
    if (ver != 1) return false;
    uint32_t n = getU32(p); p += 4;
    for (uint32_t i = 0; i < n; i++) {
        VoiceKey k;
        if (!getStr(p, end, k.name)) return false;
        if (end - p < 2) return false;
        k.protocol = (Protocol)*p++; k.algorithm = (Algorithm)*p++;
        if (!getBytes(p, end, k.key)) return false;
        if (end - p < 1 + 24) return false;
        k.enabled = *p++ != 0;
        k.keyId = (int)(int32_t)getU32(p); p += 4;
        k.algId = (int)(int32_t)getU32(p); p += 4;
        k.talkgroup = (long)(int32_t)getU32(p); p += 4;
        k.unitId = (long)(int32_t)getU32(p); p += 4;
        k.nac = (int)(int32_t)getU32(p); p += 4;
        k.colourCode = (int)(int32_t)getU32(p); p += 4;
        out.push_back(std::move(k));
    }
    return true;
}

std::string encryptStore(const std::vector<VoiceKey>& keys, const std::vector<uint8_t>& master32,
                         const std::string& passphrase, std::string& fileBytes) {
    const bool usePass = master32.empty();
    if (usePass && passphrase.empty()) return "no keychain and no passphrase: cannot protect the keys";
    uint8_t salt[16] = {}, iv[16] = {};
    uint32_t rounds = 200000;
    std::vector<uint8_t> master = master32;
    if (usePass) {
        randomBytes(salt, 16);
        master.assign(32, 0);
        pbkdf2HmacSha256((const uint8_t*)passphrase.data(), passphrase.size(), salt, 16, rounds, master.data(), 32);
    } else if (master.size() != 32) return "the master key is not 32 bytes";
    randomBytes(iv, 16);

    std::string plain = serializeKeys(keys);
    std::vector<uint8_t> ct(plain.begin(), plain.end());
    uint8_t ctr[16]; std::memcpy(ctr, iv, 16);
    meshcrypto::aesCtr(master.data(), 32, ctr, ct.data(), ct.size());

    uint8_t mac[32];
    meshcrypto::hmacSha256(master.data(), 32, ct.data(), ct.size(), mac);

    fileBytes.assign(kMagic, kMagic + 6);
    fileBytes.push_back((char)(usePass ? 1 : 0));
    fileBytes.append((const char*)salt, 16);
    putU32(fileBytes, rounds);
    fileBytes.append((const char*)iv, 16);
    fileBytes.append((const char*)mac, 32);
    fileBytes.append((const char*)ct.data(), ct.size());
    std::fill(master.begin(), master.end(), (uint8_t)0);
    return std::string();
}

bool fileNeedsPassphrase(const std::string& f, bool& needs) {
    if (f.size() < 7 || std::memcmp(f.data(), kMagic, 6) != 0) return false;
    needs = (uint8_t)f[6] == 1;
    return true;
}

std::string decryptStore(const std::string& f, const std::vector<uint8_t>& master32,
                         const std::string& passphrase, std::vector<VoiceKey>& out) {
    if (f.size() < 6 + 1 + 16 + 4 + 16 + 32 || std::memcmp(f.data(), kMagic, 6) != 0) return "not an OnAir key file";
    const uint8_t* p = (const uint8_t*)f.data() + 6;
    const uint8_t source = *p++;
    const uint8_t* salt = p; p += 16;
    uint32_t rounds = getU32(p); p += 4;
    const uint8_t* iv = p; p += 16;
    const uint8_t* mac = p; p += 32;
    const size_t ctLen = (const uint8_t*)f.data() + f.size() - p;

    std::vector<uint8_t> master;
    if (source == 1) {
        if (passphrase.empty()) return "this key file needs a passphrase";
        // the round count is read from the file: an imported file with 4 billion rounds would hang the program, one with 0 would be a free guess
        if (rounds < 1000 || rounds > 5000000) return "the key file is damaged";
        master.assign(32, 0);
        pbkdf2HmacSha256((const uint8_t*)passphrase.data(), passphrase.size(), salt, 16, rounds, master.data(), 32);
    } else {
        if (master32.size() != 32) return "the keychain master key is unavailable";
        master = master32;
    }
    uint8_t want[32];
    meshcrypto::hmacSha256(master.data(), 32, p, ctLen, want);
    if (std::memcmp(want, mac, 32) != 0) { std::fill(master.begin(), master.end(), (uint8_t)0); return source == 1 ? "wrong passphrase" : "the key file does not match this keychain"; }

    std::vector<uint8_t> pt(p, p + ctLen);
    uint8_t ctr[16]; std::memcpy(ctr, iv, 16);
    meshcrypto::aesCtr(master.data(), 32, ctr, pt.data(), pt.size());
    std::fill(master.begin(), master.end(), (uint8_t)0);
    std::string blob((const char*)pt.data(), pt.size());
    if (!deserializeKeys(blob, out)) return "the key file is corrupt";
    return std::string();
}

std::string saveToFile(const std::string& path, const std::string& passphrase) {
    std::vector<uint8_t> master;
    if (haveSecretProvider()) {
        // an existing key file that was sealed with the keychain key keeps that key: if it cannot be read now, save nothing
        bool existingKeychainFile = false;
        {
            std::ifstream in(path, std::ios::binary);
            char hdr[7] = {};
            if (in && in.read(hdr, 7) && std::memcmp(hdr, kMagic, 6) == 0 && (uint8_t)hdr[6] == 0) existingKeychainFile = true;
        }
        if (!keychainMasterKey(master, !existingKeychainFile)) return existingKeychainFile ? "the OS keychain key for the existing key file cannot be read; nothing was saved" : "could not reach the OS keychain";
    }
    std::string fileBytes;
    const std::string err = encryptStore(store().list(), master, passphrase, fileBytes);
    std::fill(master.begin(), master.end(), (uint8_t)0);
    if (!err.empty()) return err;
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) return "could not open " + path + " for writing";
    o.write(fileBytes.data(), (std::streamsize)fileBytes.size());
    const bool wrote = (bool)o;
    o.close();
#ifndef _WIN32
    { std::error_code ec; std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec); }   // 0600
#endif
    return wrote ? std::string() : "could not write " + path;
}

std::string discardKeyFile(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return std::string();
    std::filesystem::remove(path + ".discarded", ec);
    std::filesystem::rename(path, path + ".discarded", ec);
    return ec ? "could not move the old key file aside: " + ec.message() : std::string();
}

std::string loadFromFile(const std::string& path, const std::string& passphrase) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();   // no file yet: not an error
    std::ostringstream ss; ss << in.rdbuf();
    const std::string f = ss.str();
    if (f.empty()) return std::string();
    bool needsPass = false;
    if (!fileNeedsPassphrase(f, needsPass)) return "not an OnAir key file";
    std::vector<uint8_t> master;
    if (!needsPass) { if (!keychainMasterKey(master, false)) return "this key file needs the OS keychain, which is unavailable"; }
    std::vector<VoiceKey> keys;
    const std::string err = decryptStore(f, master, passphrase, keys);
    std::fill(master.begin(), master.end(), (uint8_t)0);
    if (!err.empty()) return err;
    store().setAll(std::move(keys));
    return std::string();
}

} // namespace voicekeys
} // namespace dect2
