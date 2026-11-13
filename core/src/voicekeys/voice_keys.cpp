// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The voice key store model + per-call matching (see voice_keys.h). No key search or recovery: matching is by the user's own table,
// and the only "trial" is the user's enabled list in stored order for a call with no key id.
#include "dect2/voicekeys/voice_keys.h"
#include "dect2/mesh/mesh_crypto.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace dect2 {
namespace voicekeys {

const char* protocolName(Protocol p) {
    switch (p) { case Protocol::Dmr: return "DMR"; case Protocol::P25: return "P25"; case Protocol::Nxdn: return "NXDN";
                 case Protocol::Dpmr: return "dPMR"; case Protocol::Zigbee: return "Zigbee"; case Protocol::ZWave: return "Z-Wave";
                 case Protocol::WiSun: return "Wi-SUN"; case Protocol::WMBus: return "wM-Bus"; case Protocol::LoRaWAN: return "LoRaWAN"; default: return "any"; }
}
bool protocolFromName(const std::string& s, Protocol& p) {
    std::string t; for (char c : s) t += (char)std::tolower((unsigned char)c);
    if (t == "any" || t.empty()) { p = Protocol::Any; return true; }
    if (t == "zigbee") { p = Protocol::Zigbee; return true; }
    if (t == "z-wave" || t == "zwave") { p = Protocol::ZWave; return true; }
    if (t == "wi-sun" || t == "wisun") { p = Protocol::WiSun; return true; }
    if (t == "wm-bus" || t == "wmbus") { p = Protocol::WMBus; return true; }
    if (t == "lorawan") { p = Protocol::LoRaWAN; return true; }
    if (t == "dmr") { p = Protocol::Dmr; return true; }
    if (t == "p25") { p = Protocol::P25; return true; }
    if (t == "nxdn") { p = Protocol::Nxdn; return true; }
    if (t == "dpmr") { p = Protocol::Dpmr; return true; }
    return false;
}

const char* algorithmName(Algorithm a) {
    switch (a) { case Algorithm::Rc4: return "RC4"; case Algorithm::DesOfb: return "DES-OFB";
                 case Algorithm::Aes128Ofb: return "AES-128-OFB"; case Algorithm::Aes256Ofb: return "AES-256-OFB";
                 case Algorithm::Scrambler: return "scrambler"; case Algorithm::HyteraBp: return "Hytera-BP";
                 case Algorithm::HyteraEnhanced: return "Hytera-Enhanced"; default: return "none"; }
}
bool algorithmFromName(const std::string& s, Algorithm& a) {
    std::string t; for (char c : s) if (!std::isspace((unsigned char)c)) t += (char)std::tolower((unsigned char)c);
    if (t == "rc4" || t == "arc4" || t == "adp") { a = Algorithm::Rc4; return true; }
    if (t == "des" || t == "des-ofb" || t == "desofb") { a = Algorithm::DesOfb; return true; }
    if (t == "aes128" || t == "aes-128" || t == "aes128-ofb" || t == "aes-128-ofb") { a = Algorithm::Aes128Ofb; return true; }
    if (t == "aes256" || t == "aes-256" || t == "aes256-ofb" || t == "aes-256-ofb" || t == "aes") { a = Algorithm::Aes256Ofb; return true; }
    if (t == "scrambler" || t == "lfsr") { a = Algorithm::Scrambler; return true; }
    if (t == "hytera-bp" || t == "hyterabp" || t == "hbp" || t == "hytera") { a = Algorithm::HyteraBp; return true; }
    if (t == "hytera-enhanced" || t == "hytera-ep" || t == "hyteraenhanced" || t == "hep") { a = Algorithm::HyteraEnhanced; return true; }
    if (t == "none" || t.empty()) { a = Algorithm::None; return true; }
    return false;
}
size_t algorithmKeyBytes(Algorithm a) {
    switch (a) {
        case Algorithm::Rc4: return 0;          // RC4 accepts any length; P25 ADP uses 5 bytes, DMR enhanced 5. 0 = any non-empty.
        case Algorithm::DesOfb: return 8;
        case Algorithm::Aes128Ofb: return 16;
        case Algorithm::Aes256Ofb: return 32;
        case Algorithm::Scrambler: return 2;    // a 15-bit seed carried in two bytes
        case Algorithm::HyteraEnhanced: return 5;   // 40-bit key
        case Algorithm::HyteraBp: return 0;     // 5, 16 or 32 bytes: checked in parseKeyHex
        default: return 0;
    }
}

std::string VoiceKey::keyHex() const { return meshcrypto::hexEncode(key.data(), key.size()); }

std::string parseKeyHex(Algorithm a, const std::string& hex, std::vector<uint8_t>& out) {
    out.clear();
    {
        size_t digits = 0;
        bool allHex = true;
        for (char c : hex) {
            if (c == ' ' || c == ':' || c == '\n' || c == '\t' || c == '\r') continue;
            if (!std::isxdigit((unsigned char)c)) allHex = false;
            digits++;
        }
        if (!allHex) return "not valid hex (use only 0-9 and a-f)";
        if (digits % 2) return "an odd number of hex digits (" + std::to_string(digits) + "): a key is whole bytes, two digits each";
    }
    if (!meshcrypto::hexDecode(hex, out)) return "not valid hex (use only 0-9 and a-f)";
    if (out.empty()) return "the key is empty";
    const size_t need = algorithmKeyBytes(a);
    if (need && out.size() != need) {
        return std::string(algorithmName(a)) + " needs " + std::to_string(need) + " key bytes (" +
               std::to_string(need * 2) + " hex digits), got " + std::to_string(out.size());
    }
    if (a == Algorithm::HyteraBp && !(out.size() == 5 || out.size() == 16 || out.size() == 32))
        return "Hytera-BP needs 5, 16 or 32 key bytes (10, 32 or 64 hex digits), got " + std::to_string(out.size());
    if (a == Algorithm::Scrambler && out.size() == 2 && (out[0] & 0x80)) {
        // the seed is 15 bits; the top bit must be clear
        return "the scrambler seed is 15 bits: the first hex digit must be 0-7";
    }
    return std::string();
}

KeyStore& KeyStore::instance() { static KeyStore s; return s; }

bool KeyStore::legalAccepted() const { std::lock_guard<std::mutex> lk(mu_); return legal_; }
void KeyStore::setLegalAccepted(bool v) { std::lock_guard<std::mutex> lk(mu_); legal_ = v; }

std::vector<VoiceKey> KeyStore::list() const { std::lock_guard<std::mutex> lk(mu_); return keys_; }
size_t KeyStore::count() const { std::lock_guard<std::mutex> lk(mu_); return keys_.size(); }

int KeyStore::add(const VoiceKey& k) { std::lock_guard<std::mutex> lk(mu_); keys_.push_back(k); return (int)keys_.size() - 1; }
bool KeyStore::update(int i, const VoiceKey& k) {
    std::lock_guard<std::mutex> lk(mu_);
    if (i < 0 || i >= (int)keys_.size()) return false;
    keys_[(size_t)i] = k; return true;
}
bool KeyStore::remove(int i) {
    std::lock_guard<std::mutex> lk(mu_);
    if (i < 0 || i >= (int)keys_.size()) return false;
    keys_.erase(keys_.begin() + i); return true;
}
void KeyStore::clear() { std::lock_guard<std::mutex> lk(mu_); keys_.clear(); }
void KeyStore::setAll(std::vector<VoiceKey> keys) { std::lock_guard<std::mutex> lk(mu_); keys_ = std::move(keys); }

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> out; std::string cur; bool q = false;
    for (char c : line) {
        if (c == '"') q = !q;
        else if (c == ',' && !q) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    for (auto& f : out) f = trim(f);
    return out;
}
long toLong(const std::string& s, long dflt) {
    if (s.empty()) return dflt;
    try { return std::stol(s, nullptr, 0); } catch (...) { return dflt; }
}
} // namespace

int KeyStore::parseCsv(const std::string& text, std::vector<VoiceKey>& staged, std::vector<std::string>& errors) {
    std::istringstream in(text);
    std::string line;
    int imported = 0, lineNo = 0;
    bool headerChecked = false;
    while (std::getline(in, line)) {
        lineNo++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const auto f = splitCsvLine(line);
        if (!headerChecked) {
            headerChecked = true;
            std::string c0; for (char c : (f.empty() ? "" : f[0])) c0 += (char)std::tolower((unsigned char)c);
            if (c0 == "name") continue;   // a header row
        }
        if (f.size() < 4) { errors.push_back("line " + std::to_string(lineNo) + ": need at least name,protocol,algorithm,key"); continue; }
        VoiceKey k;
        k.name = f[0];
        if (!protocolFromName(f[1], k.protocol)) { errors.push_back("line " + std::to_string(lineNo) + ": unknown protocol '" + f[1] + "'"); continue; }
        if (!algorithmFromName(f[2], k.algorithm)) { errors.push_back("line " + std::to_string(lineNo) + ": unknown algorithm '" + f[2] + "'"); continue; }
        const std::string err = parseKeyHex(k.algorithm, f[3], k.key);
        if (!err.empty()) { errors.push_back("line " + std::to_string(lineNo) + ": " + err); continue; }
        if (f.size() > 4) k.keyId = (int)toLong(f[4], -1);
        if (f.size() > 5) k.algId = (int)toLong(f[5], -1);
        if (f.size() > 6) k.talkgroup = toLong(f[6], -1);
        if (f.size() > 7) k.unitId = toLong(f[7], -1);
        if (f.size() > 8) k.nac = (int)toLong(f[8], -1);
        if (f.size() > 9) k.colourCode = (int)toLong(f[9], -1);
        if (f.size() > 10) { std::string e; for (char c : f[10]) e += (char)std::tolower((unsigned char)c); k.enabled = !(e == "0" || e == "no" || e == "false" || e == "off"); }
        if (k.name.empty()) k.name = std::string(protocolName(k.protocol)) + " key " + std::to_string(staged.size() + 1);
        staged.push_back(std::move(k));
        imported++;
    }
    return imported;
}

int KeyStore::importCsv(const std::string& text, std::vector<std::string>& errors) {
    std::vector<VoiceKey> staged;
    const int imported = parseCsv(text, staged, errors);
    if (!staged.empty()) { std::lock_guard<std::mutex> lk(mu_); for (auto& k : staged) keys_.push_back(std::move(k)); }
    return imported;
}

std::string KeyStore::exportCsv() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream o;
    o << "name,protocol,algorithm,key,keyid,algid,talkgroup,unit,nac,cc,enabled\n";
    for (const auto& k : keys_) {
        auto opt = [](long v) { return v < 0 ? std::string() : std::to_string(v); };
        o << '"' << k.name << '"' << ',' << protocolName(k.protocol) << ',' << algorithmName(k.algorithm) << ','
          << k.keyHex() << ',' << opt(k.keyId) << ',' << opt(k.algId) << ',' << opt(k.talkgroup) << ',' << opt(k.unitId)
          << ',' << opt(k.nac) << ',' << opt(k.colourCode) << ',' << (k.enabled ? 1 : 0) << '\n';
    }
    return o.str();
}

std::vector<VoiceKey> KeyStore::candidates(const CallKeyInfo& info, bool allowTrial) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<VoiceKey> out;
    if (!legal_) return out;   // decryption stays off until the user accepts the notice

    auto protoOk = [&](const VoiceKey& k) { return k.protocol == Protocol::Any || info.protocol == Protocol::Any || k.protocol == info.protocol; };
    auto fieldOk = [](int key, int call) { return key < 0 || call < 0 || key == call; };
    auto lfieldOk = [](long key, long call) { return key < 0 || call < 0 || key == call; };

    const bool callHasKeyId = info.keyId >= 0;

    // First pass: explicit association match (preferred, deterministic).
    for (const auto& k : keys_) {
        if (!k.enabled || !protoOk(k)) continue;
        const bool idMatch = fieldOk(k.keyId, info.keyId) && fieldOk(k.algId, info.algId);
        const bool fieldsMatch = fieldOk(k.nac, info.nac) && fieldOk(k.colourCode, info.colourCode) &&
                                 lfieldOk(k.talkgroup, info.talkgroup) && lfieldOk(k.unitId, info.unitId);
        if (idMatch && fieldsMatch) {
            // when the call carries a key id, require the key to actually name that id (not just be "any")
            if (callHasKeyId && k.keyId < 0 && k.talkgroup < 0 && k.unitId < 0 && k.nac < 0 && k.colourCode < 0) continue;
            out.push_back(k);
        }
    }
    if (!out.empty()) return out;

    // Second pass: only when the call names no key id, offer the user's enabled list for this protocol in stored order (a trial).
    if (allowTrial && !callHasKeyId) {
        for (const auto& k : keys_) if (k.enabled && protoOk(k)) out.push_back(k);
    }
    return out;
}

} // namespace voicekeys
} // namespace dect2
