// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Encrypted-at-rest persistence for the voice key store. Keys are serialized and encrypted with AES-256-CTR under a master key that
// comes from the OS keychain when one is wired in (setSecretProvider), or is derived from a passphrase the user types (PBKDF2-HMAC-SHA256).
// The plaintext key material never touches disk, logs, crash reports, the remote API or the web server.
//
// File format (version 1), all little-endian:
//   magic "OAVK1\n"
//   uint8  source   : 0 = master key from the secret provider, 1 = passphrase-derived
//   uint8  salt[16]  (passphrase only; zero when source == 0)
//   uint32 rounds    (passphrase only; the PBKDF2 iteration count)
//   uint8  iv[16]    (AES-CTR nonce/counter start)
//   uint8  mac[32]   (HMAC-SHA256 of the ciphertext under the master key: detects a wrong passphrase / tampering)
//   uint8  cipher[]  (AES-256-CTR of the serialized keys)
#pragma once
#include "dect2/voicekeys/voice_keys.h"
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace voicekeys {

// The OS keychain hook. The app installs real providers (macOS Keychain, Windows DPAPI/Credential Manager, Linux libsecret). When none is
// installed, persistence requires a passphrase. `account` is a fixed label ("onair.voicekeys.master"). get() returns false when absent.
using SecretGet = std::function<bool(const std::string& account, std::vector<uint8_t>& out)>;
using SecretSet = std::function<bool(const std::string& account, const std::vector<uint8_t>& data)>;
using SecretDel = std::function<bool(const std::string& account)>;
void setSecretProvider(SecretGet get, SecretSet set, SecretDel del = {});
bool haveSecretProvider();

// The label used with the secret provider.
extern const char* const kMasterAccount;

// ---- master key from the OS keychain ----
// Fetch the 32-byte master key from the keychain, creating and storing a fresh random one the first time. False when no provider is set.
// createIfMissing == false (loading an existing file): a failed read NEVER makes a new key. With true, a new key is made only when the read failed
// and the caller knows there is no key file that still needs the old one (saveToFile checks the file it is about to replace).
bool keychainMasterKey(std::vector<uint8_t>& out32, bool createIfMissing = true);

// ---- serialize / encrypt ----
// Serialize the given keys to the plaintext blob (name, protocol, algorithm, raw key, associations, enabled). Not encrypted.
std::string serializeKeys(const std::vector<VoiceKey>& keys);
bool deserializeKeys(const std::string& blob, std::vector<VoiceKey>& out);

// Encrypt `keys` to `fileBytes`. If master32 is non-empty it is used directly (keychain path); otherwise `passphrase` is used (and must be
// non-empty). Returns empty error on success. The master key / passphrase are zeroed by the caller; this does not retain them.
std::string encryptStore(const std::vector<VoiceKey>& keys, const std::vector<uint8_t>& master32,
                         const std::string& passphrase, std::string& fileBytes);
// Decrypt. Supply master32 for a keychain file, or passphrase for a passphrase file (the file's `source` byte says which it is; the
// matching secret must be supplied). On a wrong passphrase / tampering the MAC check fails and a clear error is returned.
std::string decryptStore(const std::string& fileBytes, const std::vector<uint8_t>& master32,
                         const std::string& passphrase, std::vector<VoiceKey>& out);
// Read the file header to learn whether it needs a passphrase (source == 1) without decrypting.
bool fileNeedsPassphrase(const std::string& fileBytes, bool& needsPassphrase);

// ---- whole-store save / load against a path (uses the keychain when available, else requires passphrase) ----
// Save KeyStore::instance() to `path`. When no secret provider is set, `passphrase` must be non-empty.
std::string saveToFile(const std::string& path, const std::string& passphrase = std::string());
// Load into KeyStore::instance(). Returns empty on success; a non-empty string describes why (missing file is NOT an error -> returns "" and leaves the store empty).
std::string loadFromFile(const std::string& path, const std::string& passphrase = std::string());

// For a key file whose keychain key is gone for good (the user wiped the keychain): moves it aside to <path>.discarded so a new store can be
// saved. The old keys are NOT deleted; they just cannot be opened any more. Returns "" on success (also when there is no file).
std::string discardKeyFile(const std::string& path);

// PBKDF2-HMAC-SHA256 (RFC 2898), exposed for tests.
void pbkdf2HmacSha256(const uint8_t* pw, size_t pwLen, const uint8_t* salt, size_t saltLen, uint32_t rounds, uint8_t* out, size_t outLen);

} // namespace voicekeys
} // namespace dect2
