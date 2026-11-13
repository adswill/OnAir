// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Voice key store: the named decryption keys the USER imports so OnAir can listen to digital-voice channels the user is authorised to hear.
//
// OWNER / LEGAL CONTRACT (do not weaken):
//   - Nothing is ever decrypted by default. A call is only decrypted when the user has loaded a key that matches it.
//   - OnAir has NO automatic decrypter, NO key search, recovery, cracking or brute force. The only "trying" that happens is attempting the
//     user's own SMALL enabled key list, in the order the user stored them, and ONLY for a call that carries no key id to match on.
//   - Keys live locally only. They are encrypted at rest, never uploaded, never written to logs, crash reports, bug-report zips, IQ metadata,
//     the remote API or the web server. Web sessions never see or use keys.
//   - Restricted / secret algorithms (e.g. TETRA TEA1-4) are never implemented here; only ciphers with public specifications.
//
// This store holds the model and the matching logic. Persistence (encrypt-at-rest) is in key_persist.h; the receivers read keys through
// the process-global store() and call voice_decrypt.h.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {
namespace voicekeys {

enum class Protocol : uint8_t { Any = 0, Dmr, P25, Nxdn, Dpmr, Zigbee, ZWave, WiSun, WMBus, LoRaWAN };
const char* protocolName(Protocol p);
bool protocolFromName(const std::string& s, Protocol& p);

// The cipher families OnAir can apply with a user key. Only publicly specified algorithms appear here.
enum class Algorithm : uint8_t {
    None = 0,
    Rc4,         // RC4 / ARCFOUR keystream (P25 ADP; DMR enhanced-privacy-style where documented)
    DesOfb,      // single DES in OFB (P25 DES-OFB; NXDN/standard DES)
    Aes128Ofb,   // AES-128 in OFB (DMR/NXDN AES where the standard publishes the framing)
    Aes256Ofb,   // AES-256 in OFB (P25 AES; DMR/NXDN AES-256)
    Scrambler,   // 15-bit LFSR scrambler seeded by the key (NXDN and dPMR scramblers)
    HyteraBp,    // Hytera Basic Privacy: 40 / 128 / 256 bit repeating key stream over the 49 vocoder bits (DMR, hytera.h). Not an ETSI standard
    HyteraEnhanced, // Hytera Enhanced Privacy: 40-bit RC4 key plus the 40-bit message indicator (DMR, hytera.h). Not an ETSI standard
};
const char* algorithmName(Algorithm a);
bool algorithmFromName(const std::string& s, Algorithm& a);
// The exact key length this algorithm needs, in bytes (Rc4 for P25 ADP is 5; Scrambler uses a 15-bit seed = 2 bytes). 0 means "any non-empty".
size_t algorithmKeyBytes(Algorithm a);

// One user key. Association fields are optional (-1 = unset); they let a call pick its key automatically.
struct VoiceKey {
    std::string name;                 // the user's label; shown in status text ("decrypted with key <name>")
    Protocol protocol = Protocol::Any;
    Algorithm algorithm = Algorithm::None;
    std::vector<uint8_t> key;         // the raw key bytes (validated by algorithm + length)
    bool enabled = true;
    // optional association (match these against the call's fields). -1 means "do not use this field to match".
    int keyId = -1;                   // P25 key id / NXDN key id / DMR key index
    int algId = -1;                   // P25 algorithm id (0x80 clear, 0x81 DES-OFB, 0x84 AES-256, 0xAA ADP ...); -1 = any
    long talkgroup = -1;
    long unitId = -1;
    int nac = -1;                     // P25 NAC
    int colourCode = -1;              // DMR colour code
    // no raw key material is ever logged; keyHex() is only for the Keys window the user already opened.
    std::string keyHex() const;
};

// Validate a hex string for an algorithm: trims spaces/':'; on success fills out and returns empty, else returns a human error.
std::string parseKeyHex(Algorithm a, const std::string& hex, std::vector<uint8_t>& out);

// What a per-call lookup found.
struct Match {
    enum Status { NoKey, Found, Trial } status = NoKey;   // Trial: no key id on the call, trying the user's enabled list in order
    int index = -1;                                       // index into the store's keys (Found/Trial)
};

// The call's identifying fields, as much as the receiver knows. Unset = -1.
struct CallKeyInfo {
    Protocol protocol = Protocol::Any;
    int keyId = -1, algId = -1, nac = -1, colourCode = -1;
    long talkgroup = -1, unitId = -1;
};

// The process-global store. Thread-safe. The app owns CRUD + persistence; receivers read.
class KeyStore {
public:
    static KeyStore& instance();

    // ---- the legal gate: decryption stays off until the user accepts the first-use notice (set by the UI, persisted by the app) ----
    bool legalAccepted() const;
    void setLegalAccepted(bool v);

    // ---- CRUD (interface thread) ----
    std::vector<VoiceKey> list() const;                 // a copy (never hands out internal references)
    size_t count() const;
    int add(const VoiceKey& k);                         // returns the new index
    bool update(int index, const VoiceKey& k);
    bool remove(int index);
    void clear();
    void setAll(std::vector<VoiceKey> keys);            // used by load

    // ---- import / export ----
    // CSV columns (header optional, case-insensitive): name,protocol,algorithm,key,keyid,algid,talkgroup,unit,nac,cc,enabled
    // Returns the number imported; errors appended to `errors` (one per bad line). Never throws.
    int importCsv(const std::string& text, std::vector<std::string>& errors);
    // The same parser without touching any store (the Keys window previews with it). Appends to `out`; returns the number parsed.
    static int parseCsv(const std::string& text, std::vector<VoiceKey>& out, std::vector<std::string>& errors);
    std::string exportCsv() const;

    // ---- per-call selection (feed thread; must not block on I/O) ----
    // Returns the keys to try for this call, newest match policy:
    //   * if the call carries a key id/alg id, only enabled keys whose association matches are returned (usually one).
    //   * otherwise, when allowTrialWithoutKeyId, the user's whole enabled list for that protocol is returned in stored order (the UI
    //     tells the user this is happening). Never returns anything when the legal notice is not accepted.
    std::vector<VoiceKey> candidates(const CallKeyInfo& info, bool allowTrialWithoutKeyId = true) const;

private:
    KeyStore() = default;
    mutable std::mutex mu_;
    std::vector<VoiceKey> keys_;
    bool legal_ = false;
};

inline KeyStore& store() { return KeyStore::instance(); }

} // namespace voicekeys
} // namespace dect2
