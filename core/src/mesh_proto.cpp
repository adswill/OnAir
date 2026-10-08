// Meshtastic and MeshCore packet layer: channels, LoRa settings of both protocols, shared helpers.
// Sources: meshtastic/firmware src/mesh/RadioInterface.cpp (regions, slot hash, applyModemConfig), src/mesh/MeshRadio.h
// (modemPresetToParams, defaultpsk), src/mesh/Channels.cpp (key expansion, channel hash), src/DisplayFormatters.cpp (preset names);
// meshcore-dev/MeshCore src/helpers/BaseChatMesh.cpp (addChannel), examples/companion_radio/MyMesh.cpp (Public channel PSK),
// src/helpers/radiolib (sync word, preamble), api.meshcore.nz/api/v1/config (community presets, read 2026-10).
#include "mesh_proto_internal.h"
#include "dect2/mesh_crypto.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace dect2 {

using namespace meshcrypto;

const char* meshEnumName(const MeshEnumName* t, int v) {
    for (; t->name; t++) if (t->value == v) return t->name;
    return nullptr;
}

std::string meshHex(const uint8_t* p, size_t n) { return hexEncode(p, n); }

std::string meshFmt(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

// MeshRadio.h: defaultpsk
static const uint8_t kDefaultPsk[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
const uint8_t* meshtasticDefaultKey() { return kDefaultPsk; }

// examples/companion_radio/MyMesh.cpp: PUBLIC_GROUP_PSK "izOH6cXN6mrJ5e26oRXNcg=="
static const uint8_t kMeshcorePublic[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
const uint8_t* meshcorePublicSecret() { return kMeshcorePublic; }

std::string meshtasticNodeIdString(uint32_t num) { return meshFmt("!%08x", num); }

std::string meshtasticHwModelName(int hw) {
    const char* n = meshEnumName(kMeshHwModels, hw);
    return n ? n : meshFmt("HW_%d", hw);
}

int meshtasticHwModelValue(const std::string& name) {
    for (const MeshEnumName* t = kMeshHwModels; t->name; t++) if (name == t->name) return t->value;
    return -1;
}

// Channels.cpp: generateHash (xor of the name, then xor of the key bytes)
uint8_t meshtasticChannelHash(const std::string& name, const std::vector<uint8_t>& key) {
    uint8_t h = 0;
    for (char c : name) h ^= (uint8_t)c;
    for (uint8_t b : key) h ^= b;
    return h;
}

// Channels.cpp getKey(): one byte N expands to the default key with the last byte raised by N-1 (0 = no encryption);
// other short keys are padded with zeros to 16 or 32 bytes
bool meshtasticExpandPsk(const std::vector<uint8_t>& psk, std::vector<uint8_t>& key) {
    key.clear();
    if (psk.empty()) return true;
    if (psk.size() == 1) {
        if (psk[0] == 0) return true;
        key.assign(kDefaultPsk, kDefaultPsk + 16);
        key[15] = (uint8_t)(key[15] + psk[0] - 1);
        return true;
    }
    if (psk.size() > 32) return false;
    key = psk;
    if (key.size() < 16) key.resize(16, 0);
    else if (key.size() > 16 && key.size() < 32) key.resize(32, 0);
    return true;
}

bool meshLoraLdro(int sf, double bwHz) {
    if (bwHz <= 0) return false;
    return std::ldexp(1.0, sf) / bwHz * 1000.0 >= 16.0;       // symbol time in ms, as RadioLib sets it
}

bool meshcoreMakeChannel(const std::string& name, const uint8_t* secret, size_t len, MeshCoreChannel& out) {
    if (len != 16 && len != 32) return false;
    out = MeshCoreChannel();
    out.name = name;
    std::memcpy(out.secret, secret, len);
    out.keyLen = len;
    uint8_t h[32];
    sha256(secret, len, h);                                    // BaseChatMesh::addChannel: hash over the key length
    out.hash = h[0];
    return true;
}

// ---- MeshProto: channel lists ----
MeshProto::MeshProto() : s_(std::make_unique<State>()) {
    // The default channel uses the key of PSK index 1 and, with an empty name, the name of the modem preset
    // (Channels::getName); so each preset name carries the default key.
    static const char* const names[] = {"LongFast", "LongSlow", "LongTurbo", "LongMod", "LongModerate", "MediumFast", "MediumSlow",
                                        "ShortFast", "ShortSlow", "ShortTurbo"};
    for (const char* n : names) {
        MeshtasticChannel c;
        c.name = n;
        c.key.assign(kDefaultPsk, kDefaultPsk + 16);
        c.hash = meshtasticChannelHash(c.name, c.key);
        s_->mt.push_back(c);
    }
    MeshCoreChannel pub;
    meshcoreMakeChannel("Public", kMeshcorePublic, 16, pub);
    s_->mc.push_back(pub);
}

MeshProto::~MeshProto() = default;

bool MeshProto::addMeshtasticChannel(const std::string& name, const std::string& base64Psk) {
    std::vector<uint8_t> raw, key;
    if (!base64Decode(base64Psk, raw) && !hexDecode(base64Psk, raw)) return false;
    if (!meshtasticExpandPsk(raw, key)) return false;
    MeshtasticChannel c;
    c.name = name;
    c.key = key;
    c.hash = meshtasticChannelHash(name, key);
    c.user = true;
    std::lock_guard<std::mutex> g(s_->m);
    for (auto& e : s_->mt) if (e.name == name && e.key == key) return true;       // already known
    s_->mt.push_back(c);
    return true;
}

// secret: hex (32 or 64 digits) or base64 (16 or 32 bytes); a name that starts with '#' and an empty secret derives the key
// from the name the way the MeshCore apps do for hashtag channels (first 16 bytes of SHA-256 of the name; app convention,
// not from the firmware source)
bool MeshProto::addMeshCoreChannel(const std::string& name, const std::string& secret) {
    std::vector<uint8_t> raw;
    if (secret.empty() && !name.empty() && name[0] == '#') {
        uint8_t h[32];
        sha256((const uint8_t*)name.data(), name.size(), h);
        raw.assign(h, h + 16);
    } else {
        std::string t;
        for (char c : secret) if (c != ' ' && c != ':') t += c;
        const bool allHex = !t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return std::isxdigit((unsigned char)c) != 0; });
        if (allHex && (t.size() == 32 || t.size() == 64)) hexDecode(t, raw);
        else if (!base64Decode(t, raw)) return false;
    }
    MeshCoreChannel c;
    if (!meshcoreMakeChannel(name, raw.data(), raw.size(), c)) return false;
    c.user = true;
    std::lock_guard<std::mutex> g(s_->m);
    for (auto& e : s_->mc) if (e.hash == c.hash && std::memcmp(e.secret, c.secret, 32) == 0) { e.name = name; return true; }
    s_->mc.push_back(c);
    return true;
}

void MeshProto::clearUserChannels() {
    std::lock_guard<std::mutex> g(s_->m);
    s_->mt.erase(std::remove_if(s_->mt.begin(), s_->mt.end(), [](const MeshtasticChannel& c) { return c.user; }), s_->mt.end());
    s_->mc.erase(std::remove_if(s_->mc.begin(), s_->mc.end(), [](const MeshCoreChannel& c) { return c.user; }), s_->mc.end());
}

std::vector<std::string> MeshProto::channelNames(MeshProtocol p) const {
    std::lock_guard<std::mutex> g(s_->m);
    std::vector<std::string> v;
    if (p == MeshProtocol::Meshtastic) for (auto& c : s_->mt) v.push_back(c.name);
    else for (auto& c : s_->mc) v.push_back(c.name);
    return v;
}

// ---- LoRa settings ----
static std::string lower(std::string s) {
    std::string o;
    for (char c : s) if (c != '_' && c != ' ' && c != '-') o += (char)std::tolower((unsigned char)c);
    return o;
}

namespace {
struct PresetRow { const char* name; int sf; double bwKHz; int cr; };
// MeshRadio.h modemPresetToParams (non-wide regions), names from DisplayFormatters.cpp
const PresetRow kPresets[] = {
    {"ShortTurbo", 7, 500, 5},  {"ShortFast", 7, 250, 5},   {"ShortSlow", 8, 250, 5},
    {"MediumFast", 9, 250, 5},  {"MediumSlow", 10, 250, 5}, {"LongTurbo", 11, 500, 8},
    {"LongFast", 11, 250, 5},   {"LongModerate", 11, 125, 8}, {"LongSlow", 12, 125, 8},
};
struct Region { const char* name; double startMHz, endMHz; };
// RadioInterface.cpp regions[] (freqStart, freqEnd; the spacing is 0 in all of them)
const Region kRegions[] = {
    {"US", 902.0, 928.0},      {"EU_433", 433.0, 434.0},    {"EU_868", 869.4, 869.65},  {"CN", 470.0, 510.0},
    {"JP", 920.5, 923.5},      {"ANZ", 915.0, 928.0},       {"ANZ_433", 433.05, 434.79}, {"RU", 868.7, 869.2},
    {"KR", 920.0, 923.0},      {"TW", 920.0, 925.0},        {"IN", 865.0, 867.0},       {"NZ_865", 864.0, 868.0},
    {"TH", 920.0, 925.0},      {"UA_433", 433.0, 434.7},    {"UA_868", 868.0, 868.6},   {"MY_433", 433.0, 435.0},
    {"MY_919", 919.0, 924.0},  {"SG_923", 917.0, 925.0},    {"PH_433", 433.0, 434.7},   {"PH_868", 868.0, 869.4},
    {"PH_915", 915.0, 918.0},  {"KZ_433", 433.075, 434.775}, {"KZ_863", 863.0, 868.0},  {"NP_865", 865.0, 868.0},
    {"BR_902", 902.0, 907.5},
};
const PresetRow* findPreset(const std::string& n) {
    const std::string k = lower(n);
    for (const PresetRow& p : kPresets) if (lower(p.name) == k) return &p;
    if (k == "longmod") return &kPresets[7];
    return nullptr;
}
// RadioInterface.cpp hash(): djb2
uint32_t djb2(const std::string& s) {
    uint32_t h = 5381;
    for (unsigned char c : s) h = ((h << 5) + h) + c;
    return h;
}
} // namespace

bool meshtasticPreset(const std::string& preset, MeshLoraSettings& out) {
    const PresetRow* p = findPreset(preset);
    if (!p) return false;
    out = MeshLoraSettings();
    out.name = p->name;
    out.freqHz = 0;
    out.sf = p->sf;
    out.bwHz = p->bwKHz * 1000.0;
    out.cr = p->cr;
    out.preamble = 16;                                  // RadioInterface.h: preambleLength = 16
    out.syncWord = 0x2B;                                // RadioLibInterface.h: syncWord = 0x2b
    out.ldro = meshLoraLdro(out.sf, out.bwHz);
    return true;
}

// numChannels = floor((end - start) / (spacing + bw)); slot = hash(name) % numChannels; f = start + bw/2 + slot*bw
double meshtasticSlotHz(const std::string& region, const std::string& preset) {
    const PresetRow* p = findPreset(preset);
    if (!p) return 0;
    std::string r = lower(region);
    for (const Region& g : kRegions) {
        if (lower(g.name) != r) continue;                   // lower() drops '_' so "EU868" and "EU_868" both match
        const double bw = p->bwKHz / 1000.0;
        if (g.endMHz - g.startMHz < bw) return 0;           // the firmware would fall back to LongFast here
        const uint32_t n = (uint32_t)std::floor((g.endMHz - g.startMHz) / bw + 1e-9);
        if (n == 0) return 0;
        const uint32_t slot = djb2(p->name) % n;
        return std::round((g.startMHz + bw / 2 + slot * bw) * 1e6);
    }
    return 0;
}

// MeshCore: the community presets (api.meshcore.nz/api/v1/config, 2026-10). The sync word is RADIOLIB_SX126X_SYNC_WORD_PRIVATE
// (0x12) and the preamble is 32 symbols up to SF8, else 16 (src/helpers/radiolib, RadioLibWrappers.h preambleLengthForSF).
MeshLoraSettings meshcoreDefaults(const std::string& region) {
    struct Row { const char* name; double mhz; int sf; double bwKHz; int cr; };
    static const Row rows[] = {
        {"EU", 869.618, 8, 62.5, 8},   {"UK", 869.618, 8, 62.5, 8},   {"CH", 869.618, 8, 62.5, 8},
        {"NL", 869.618, 7, 62.5, 5},   {"PT", 869.618, 7, 62.5, 6},   {"EU433", 433.650, 8, 62.5, 8},
        {"EU_DEPRECATED", 869.525, 11, 250, 5},
        {"US", 910.525, 7, 62.5, 5},   {"CA", 910.525, 7, 62.5, 5},   {"AU", 915.800, 10, 250, 5},
        {"NZ", 917.375, 7, 62.5, 5},
    };
    const std::string k = lower(region);
    const Row* r = &rows[0];
    for (const Row& x : rows) if (lower(x.name) == k) { r = &x; break; }
    MeshLoraSettings s;
    s.name = std::string("MeshCore ") + r->name;
    s.freqHz = std::round(r->mhz * 1e6);
    s.sf = r->sf;
    s.bwHz = r->bwKHz * 1000.0;
    s.cr = r->cr;
    s.preamble = r->sf <= 8 ? 32 : 16;
    s.syncWord = 0x12;
    s.ldro = meshLoraLdro(s.sf, s.bwHz);
    return s;
}

} // namespace dect2
