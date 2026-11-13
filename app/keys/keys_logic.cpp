// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "keys_logic.h"
#include "dect2/voicekeys/key_guard.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace keyswin {

using namespace dect2::voicekeys;

const std::vector<Protocol>& protocolChoices() {
    static const std::vector<Protocol> v = {Protocol::Any, Protocol::P25, Protocol::Nxdn, Protocol::Dmr, Protocol::Dpmr, Protocol::Zigbee, Protocol::ZWave, Protocol::WiSun, Protocol::WMBus, Protocol::LoRaWAN};
    return v;
}
const std::vector<Algorithm>& algorithmChoices() {
    static const std::vector<Algorithm> v = {Algorithm::Aes256Ofb, Algorithm::Aes128Ofb, Algorithm::DesOfb, Algorithm::Rc4, Algorithm::Scrambler, Algorithm::HyteraBp, Algorithm::HyteraEnhanced};
    return v;
}

namespace {
bool isSep(char c) { return c == ' ' || c == ':' || c == '\t' || c == '\r' || c == '\n'; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
}  // namespace

std::string lengthRule(Algorithm a) {
    const size_t n = algorithmKeyBytes(a);
    std::string s = algorithmName(a);
    if (a == Algorithm::Scrambler) return s + ": a 15-bit seed = 4 hex digits, the first digit 0-7";
    if (a == Algorithm::HyteraBp) return s + " (Hytera Basic Privacy, DMR): 5, 16 or 32 bytes = 10, 32 or 64 hex digits";
    if (a == Algorithm::HyteraEnhanced) return s + " (Hytera Enhanced Privacy, DMR): 5 bytes = 10 hex digits";
    if (n == 0) return s + ": any length, whole bytes (P25 ADP uses 5 bytes = 10 hex digits)";
    return s + ": " + std::to_string(n) + " bytes = " + std::to_string(n * 2) + " hex digits";
}

std::string hexProblem(Algorithm a, const std::string& hex) {
    if (trim(hex).empty()) return "enter the key as hex. " + lengthRule(a);
    for (char c : hex) {
        if (isSep(c) || std::isxdigit((unsigned char)c)) continue;
        return std::string("'") + c + "' is not a hex digit (use 0-9 and a-f; spaces and ':' between bytes are fine)";
    }
    std::vector<uint8_t> raw;
    return parseKeyHex(a, hex, raw);
}

std::string hexCounter(Algorithm a, const std::string& hex) {
    size_t digits = 0;
    for (char c : hex) { if (isSep(c)) continue; if (!std::isxdigit((unsigned char)c)) return std::string(); digits++; }
    const size_t need = algorithmKeyBytes(a);
    std::string s = std::to_string(digits / 2) + (digits % 2 ? ".5" : "");
    if (need) s += " of " + std::to_string(need) + " bytes";
    else s += " bytes";
    s += " (" + std::to_string(digits) + " hex digits)";
    return s;
}

std::string maskKey(size_t bytes) {
    return std::string(8, '*') + " (" + std::to_string(bytes) + " bytes)";
}

std::string associationText(const VoiceKey& k) {
    std::string s;
    auto add = [&](const std::string& t) { if (!s.empty()) s += ", "; s += t; };
    char b[32];
    if (k.keyId >= 0) add("key id " + std::to_string(k.keyId));
    if (k.algId >= 0) { snprintf(b, sizeof b, "algorithm 0x%02X", k.algId); add(b); }
    if (k.talkgroup >= 0) add("TG " + std::to_string(k.talkgroup));
    if (k.unitId >= 0) add("unit " + std::to_string(k.unitId));
    if (k.nac >= 0) { snprintf(b, sizeof b, "NAC 0x%03X", k.nac); add(b); }
    if (k.colourCode >= 0) add("CC " + std::to_string(k.colourCode));
    return s.empty() ? std::string("-") : s;
}

KeyForm formFromKey(const VoiceKey& k) {
    KeyForm f;
    f.name = k.name;
    f.hex = k.keyHex();
    for (size_t i = 0; i < protocolChoices().size(); i++) if (protocolChoices()[i] == k.protocol) f.protocol = (int)i;
    for (size_t i = 0; i < algorithmChoices().size(); i++) if (algorithmChoices()[i] == k.algorithm) f.algorithm = (int)i;
    auto num = [](long v) { return v < 0 ? std::string() : std::to_string(v); };
    f.keyId = num(k.keyId); f.talkgroup = num(k.talkgroup); f.unit = num(k.unitId); f.nac = num(k.nac); f.cc = num(k.colourCode);
    if (k.algId >= 0) { char b[16]; snprintf(b, sizeof b, "0x%02X", k.algId); f.algId = b; }
    f.enabled = k.enabled;
    return f;
}

std::string parseAssociation(const std::string& text, long maxValue, long& out) {
    const std::string t = trim(text);
    out = -1;
    if (t.empty()) return std::string();
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, 0);
    if (!end || *end) return "'" + t + "' is not a number (decimal, or hex with 0x)";
    if (v < 0 || v > maxValue) return "'" + t + "' is out of range (0 to " + std::to_string(maxValue) + ")";
    out = v;
    return std::string();
}

std::string keyFromForm(const KeyForm& f, VoiceKey& out) {
    if (f.protocol < 0 || f.protocol >= (int)protocolChoices().size()) return "choose a protocol";
    if (f.algorithm < 0 || f.algorithm >= (int)algorithmChoices().size()) return "choose an algorithm";
    VoiceKey k;
    k.name = trim(f.name);
    k.protocol = protocolChoices()[f.protocol];
    k.algorithm = algorithmChoices()[f.algorithm];
    const std::string hp = hexProblem(k.algorithm, f.hex);
    if (!hp.empty()) return "Key: " + hp;
    if (!parseKeyHex(k.algorithm, f.hex, k.key).empty()) return "Key: not valid";
    if (k.name.empty()) k.name = std::string(protocolName(k.protocol)) + " key";
    long v;
    std::string e;
    if (!(e = parseAssociation(f.keyId, 65535, v)).empty()) return "Key id: " + e;
    k.keyId = (int)v;
    if (!(e = parseAssociation(f.algId, 255, v)).empty()) return "Algorithm id: " + e;
    k.algId = (int)v;
    if (!(e = parseAssociation(f.talkgroup, 16777215, v)).empty()) return "Talkgroup: " + e;
    k.talkgroup = v;
    if (!(e = parseAssociation(f.unit, 16777215, v)).empty()) return "Unit id: " + e;
    k.unitId = v;
    if (!(e = parseAssociation(f.nac, 4095, v)).empty()) return "NAC: " + e;
    k.nac = (int)v;
    if (!(e = parseAssociation(f.cc, 15, v)).empty()) return "Colour code: " + e;
    k.colourCode = (int)v;
    k.enabled = f.enabled;
    out = std::move(k);
    return std::string();
}

CsvPreview previewKeyList(std::vector<VoiceKey> keys, const std::vector<VoiceKey>& existing, std::vector<std::string> errors) {
    CsvPreview p;
    p.errors = std::move(errors);
    int n = 0;
    for (auto& k : keys) {
        n++;
        if (k.algorithm == Algorithm::None) { p.errors.push_back("key " + std::to_string(n) + " (" + k.name + "): the algorithm is missing"); continue; }
        PreviewRow r;
        auto same = [&](const VoiceKey& e) { return e.name == k.name && e.protocol == k.protocol && e.algorithm == k.algorithm && e.key == k.key; };
        for (const auto& e : existing) if (same(e)) { r.duplicate = true; break; }
        for (const auto& o : p.rows) if (same(o.key)) { r.duplicate = true; break; }
        r.key = std::move(k);
        p.rows.push_back(std::move(r));
    }
    return p;
}

CsvPreview previewCsv(const std::string& text, const std::vector<VoiceKey>& existing) {
    // An OnAir encrypted export starts with its magic: it needs the passphrase and is not text.
    if (text.compare(0, 6, "OAVK1\n") == 0) { CsvPreview p; p.encryptedFile = true; return p; }
    std::vector<VoiceKey> keys;
    std::vector<std::string> errors;
    KeyStore::parseCsv(text, keys, errors);
    return previewKeyList(std::move(keys), existing, std::move(errors));
}

std::string previewLine(const PreviewRow& r) {
    const VoiceKey& k = r.key;
    return k.name + " | " + protocolName(k.protocol) + " | " + algorithmName(k.algorithm) + " | " + maskKey(k.key.size()) + " | " + associationText(k) +
           (k.enabled ? "" : " | disabled") + (r.duplicate ? " | already there: skipped" : "");
}

std::vector<VoiceKey> rowsToAdd(const CsvPreview& p) {
    std::vector<VoiceKey> v;
    for (const auto& r : p.rows) if (!r.duplicate) v.push_back(r.key);
    return v;
}

CallStatus classifyStatus(const std::string& s) {
    if (s.empty() || s == "clear") return CallStatus::Clear;
    if (s == "encrypted: add your key") return CallStatus::AddKey;
    if (s.compare(0, 19, "decrypted with key ") == 0 || s == "decrypted") return CallStatus::Decrypted;
    if (s == "key mismatch") return CallStatus::Mismatch;
    if (s == "encrypted: unsupported algorithm") return CallStatus::Unsupported;
    return CallStatus::Other;
}

std::string statusKeyName(const std::string& s) {
    const std::string pre = "decrypted with key ";
    return s.compare(0, pre.size(), pre) == 0 ? s.substr(pre.size()) : std::string();
}

const char* storeStateText(StoreState s) {
    switch (s) {
        case StoreState::NeedsNotice: return "keys off: accept the notice first";
        case StoreState::Locked: return "keys locked";
        case StoreState::Unlocked: return "keys unlocked";
    }
    return "?";
}

std::string storeFilePath(const std::string& dataDir) {
    std::string d = dataDir;
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d + keyFolderName() + "/" + keyFileName();
}

std::string exportFileName(bool encrypted) {
    return std::string(keyExportPrefix()) + (encrypted ? keyFileExt() : ".csv");
}

const std::vector<std::string>& legalNotice() {
    static const std::vector<std::string> v = {
        "OnAir can decrypt digital-voice calls only with keys you import yourself, for example to listen to friends on a private encrypted "
        "channel you are authorised to monitor. There is no automatic decrypter.",
        "Use only on channels you are authorised to listen to. OnAir never recovers, guesses, searches for or cracks a key, never attacks a "
        "weak cipher, and never ships or downloads keys. It decrypts a call only when you have loaded a key that matches it.",
        "Your keys stay on this computer, encrypted with a key held by the operating system's keychain or by a passphrase you type. They are "
        "never uploaded and never appear in logs, bug reports, the remote API or the web server.",
        "Listening to or decrypting communications you are not entitled to hear may be illegal where you live. You are responsible for how you use this.",
    };
    return v;
}

const char* exportWarning(bool encrypted) {
    return encrypted
        ? "This file holds your keys, locked with the passphrase you give. Anyone who has the file and the passphrase has the keys: keep both safe, and do not attach the file to a bug report."
        : "This file holds your keys as plain text. Anyone who gets it can read them. Delete it when you are done, store it somewhere private, and never attach it to a bug report. A passphrase-protected export is safer.";
}

}  // namespace keyswin
