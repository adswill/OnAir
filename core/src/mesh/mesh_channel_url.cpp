// Meshtastic channel URLs: see mesh_channel_url.h. Own protobuf reader and writer for the few messages involved.
#include "dect2/mesh/mesh_channel_url.h"
#include "dect2/mesh/mesh_crypto.h"
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {
struct Rd {
    const uint8_t* p; size_t n, i = 0;
    bool ok = true;
    bool more() const { return ok && i < n; }
    uint64_t varint() {
        uint64_t v = 0;
        for (int s = 0; s < 64; s += 7) {
            if (i >= n) { ok = false; return 0; }
            const uint8_t b = p[i++];
            v |= (uint64_t)(b & 0x7f) << s;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    uint32_t fixed32() {
        if (i + 4 > n) { ok = false; return 0; }
        const uint32_t v = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 | (uint32_t)p[i + 2] << 16 | (uint32_t)p[i + 3] << 24;
        i += 4;
        return v;
    }
    bool bytes(const uint8_t*& b, size_t& len) {
        const uint64_t l = varint();
        if (!ok || l > n - i) { ok = false; return false; }
        b = p + i; len = (size_t)l; i += len;
        return true;
    }
    void skip(int wt) {
        if (wt == 0) varint();
        else if (wt == 1) { if (i + 8 > n) ok = false; else i += 8; }
        else if (wt == 2) { const uint8_t* b; size_t l; bytes(b, l); }
        else if (wt == 5) fixed32();
        else ok = false;
    }
};
float asFloat(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
uint32_t fromFloat(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

bool readChannel(const uint8_t* p, size_t n, MeshtasticUrlChannel& c) {
    Rd r{p, n};
    while (r.more()) {
        const uint64_t k = r.varint();
        const int f = (int)(k >> 3), wt = (int)(k & 7);
        const uint8_t* b; size_t l;
        if (f == 2 && wt == 2) { if (r.bytes(b, l)) c.psk.assign(b, b + l); }
        else if (f == 3 && wt == 2) { if (r.bytes(b, l)) c.name.assign((const char*)b, l); }
        else if (f == 4 && wt == 5) c.id = r.fixed32();
        else if (f == 5 && wt == 0) c.uplink = r.varint() != 0;
        else if (f == 6 && wt == 0) c.downlink = r.varint() != 0;
        else if (f == 7 && wt == 2) {
            if (!r.bytes(b, l)) break;
            Rd m{b, l};
            while (m.more()) {
                const uint64_t k2 = m.varint();
                if ((k2 >> 3) == 1 && (k2 & 7) == 0) c.positionPrecision = (uint32_t)m.varint();
                else if ((k2 >> 3) == 2 && (k2 & 7) == 0) c.muted = m.varint() != 0;
                else m.skip((int)(k2 & 7));
            }
            if (!m.ok) return false;
        } else r.skip(wt);
    }
    return r.ok;
}

bool readLora(const uint8_t* p, size_t n, MeshtasticChannelSet& s) {
    Rd r{p, n};
    while (r.more()) {
        const uint64_t k = r.varint();
        const int f = (int)(k >> 3), wt = (int)(k & 7);
        if (wt == 0 && (f == 1 || f == 2 || f == 3 || f == 4 || f == 5 || f == 7 || f == 8 || f == 9 || f == 10 || f == 11)) {
            const uint64_t v = r.varint();
            switch (f) {
            case 1: s.usePreset = v != 0; break;
            case 2: s.modemPreset = (int)v; break;
            case 3: s.bandwidth = (uint32_t)v; break;
            case 4: s.spreadFactor = (uint32_t)v; break;
            case 5: s.codingRate = (uint32_t)v; break;
            case 7: s.region = (int)v; break;
            case 8: s.hopLimit = (uint32_t)v; break;
            case 9: s.txEnabled = v != 0; break;
            case 10: s.txPower = (int32_t)(uint32_t)v; break;
            case 11: s.channelNum = (uint32_t)v; break;
            }
        } else if (wt == 5 && f == 6) s.frequencyOffset = asFloat(r.fixed32());
        else if (wt == 5 && f == 14) s.overrideFrequency = asFloat(r.fixed32());
        else r.skip(wt);
    }
    return r.ok;
}

// writer
void wVar(std::string& o, uint64_t v) { while (v >= 0x80) { o += (char)(uint8_t)(v | 0x80); v >>= 7; } o += (char)(uint8_t)v; }
void wKey(std::string& o, int f, int wt) { wVar(o, (uint64_t)(f << 3 | wt)); }
void wBytes(std::string& o, int f, const std::string& b) { wKey(o, f, 2); wVar(o, b.size()); o += b; }
void wU(std::string& o, int f, uint64_t v) { if (v) { wKey(o, f, 0); wVar(o, v); } }
void wF32(std::string& o, int f, uint32_t v) { if (v) { wKey(o, f, 5); for (int i = 0; i < 4; i++) o += (char)(uint8_t)(v >> (8 * i)); } }

const char* const kPresets[] = {"LongFast", "LongSlow", "", "MediumSlow", "MediumFast", "ShortSlow", "ShortFast", "LongModerate", "ShortTurbo", "LongTurbo"};
const char* const kRegions[] = {"UNSET", "US", "EU_433", "EU_868", "CN", "JP", "ANZ", "KR", "TW", "RU", "IN", "NZ_865", "TH", "LORA_24", "UA_433", "UA_868", "MY_433", "MY_919",
                                "SG_923", "PH_433", "PH_868", "PH_915", "ANZ_433", "KZ_433", "KZ_863", "NP_865", "BR_902", "ITU1_2M", "ITU2_2M", "EU_866", "EU_874",
                                "EU_917", "EU_N_868", "ITU3_2M", "ITU1_70CM", "ITU2_70CM", "ITU3_70CM", "ITU2_125CM"};
constexpr int kNumRegions = (int)(sizeof kRegions / sizeof *kRegions);

uint32_t djb2(const std::string& s) { uint32_t h = 5381; for (unsigned char c : s) h = ((h << 5) + h) + c; return h; }
}  // namespace

std::string meshtasticPresetFromEnum(int e) { return e >= 0 && e < (int)(sizeof kPresets / sizeof *kPresets) ? kPresets[e] : ""; }
int meshtasticPresetToEnum(const std::string& n) { for (int i = 0; i < (int)(sizeof kPresets / sizeof *kPresets); i++) if (*kPresets[i] && n == kPresets[i]) return i; return -1; }
std::string meshtasticRegionFromEnum(int e) {
    if (e <= 0 || e >= kNumRegions) return "";
    for (const auto& r : meshtasticRegions()) if (r == kRegions[e]) return r;   // only the regions OnAir has a band for
    return "";
}
int meshtasticRegionToEnum(const std::string& n) { for (int i = 1; i < kNumRegions; i++) if (n == kRegions[i]) return i; return -1; }

bool meshtasticUrlDecode(const std::string& url, MeshtasticChannelSet& out, std::string& err) {
    std::string b = url;
    while (!b.empty() && (b.back() == ' ' || b.back() == '\n' || b.back() == '\r' || b.back() == '\t')) b.pop_back();
    size_t a = 0;
    while (a < b.size() && (b[a] == ' ' || b[a] == '\t')) a++;
    b = b.substr(a);
    const size_t h = b.find('#');
    if (h != std::string::npos) b = b.substr(h + 1);
    else if (b.find("://") != std::string::npos) { err = "the link has no channel part (#...)"; return false; }
    for (char& c : b) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    while (b.size() % 4) b += '=';
    std::vector<uint8_t> raw;
    if (b.size() < 4 || !meshcrypto::base64Decode(b, raw) || raw.empty()) { err = "not a Meshtastic channel link"; return false; }
    MeshtasticChannelSet s;
    Rd r{raw.data(), raw.size()};
    while (r.more()) {
        const uint64_t k = r.varint();
        const int f = (int)(k >> 3), wt = (int)(k & 7);
        const uint8_t* p; size_t l;
        if (f == 1 && wt == 2) {
            if (!r.bytes(p, l)) break;
            MeshtasticUrlChannel c;
            if (!readChannel(p, l, c)) { err = "a channel in the link is damaged"; return false; }
            s.channels.push_back(std::move(c));
        } else if (f == 2 && wt == 2) {
            if (!r.bytes(p, l)) break;
            s.hasLora = true;
            if (!readLora(p, l, s)) { err = "the LoRa part of the link is damaged"; return false; }
        } else r.skip(wt);
    }
    if (!r.ok) { err = "the link is cut short or damaged"; return false; }
    if (s.channels.empty()) { err = "the link has no channels"; return false; }
    for (const auto& c : s.channels) if (!(c.psk.size() == 0 || c.psk.size() == 1 || c.psk.size() == 16 || c.psk.size() == 32)) { err = "a channel key has a length Meshtastic does not use"; return false; }
    out = std::move(s);
    return true;
}

std::string meshtasticUrlEncode(const MeshtasticChannelSet& s) {
    std::string o;
    for (const auto& c : s.channels) {
        std::string m;
        if (!c.psk.empty()) wBytes(m, 2, std::string(c.psk.begin(), c.psk.end()));
        if (!c.name.empty()) wBytes(m, 3, c.name);
        wF32(m, 4, c.id);
        wU(m, 5, c.uplink); wU(m, 6, c.downlink);
        if (c.positionPrecision || c.muted) { std::string ms; wU(ms, 1, c.positionPrecision); wU(ms, 2, c.muted); wBytes(m, 7, ms); }
        wBytes(o, 1, m);
    }
    if (s.hasLora) {
        std::string l;
        wU(l, 1, s.usePreset); wU(l, 2, (uint64_t)s.modemPreset); wU(l, 3, s.bandwidth); wU(l, 4, s.spreadFactor); wU(l, 5, s.codingRate);
        wF32(l, 6, fromFloat(s.frequencyOffset)); wU(l, 7, (uint64_t)s.region); wU(l, 8, s.hopLimit); wU(l, 9, s.txEnabled);
        if (s.txPower) { wKey(l, 10, 0); wVar(l, (uint64_t)(int64_t)s.txPower); }
        wU(l, 11, s.channelNum); wF32(l, 14, fromFloat(s.overrideFrequency));
        wBytes(o, 2, l);
    }
    std::string b = meshcrypto::base64Encode((const uint8_t*)o.data(), o.size());
    for (char& c : b) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    while (!b.empty() && b.back() == '=') b.pop_back();
    return "https://meshtastic.org/e/#" + b;
}

std::string meshtasticUrlChannelName(const MeshtasticChannelSet& s, size_t i) {
    if (i >= s.channels.size()) return "";
    if (!s.channels[i].name.empty()) return s.channels[i].name;
    const std::string p = meshtasticPresetFromEnum(s.usePreset || !s.hasLora ? s.modemPreset : 0);
    return p.empty() ? "LongFast" : p;
}

std::string meshtasticUrlChannelKey(const MeshtasticUrlChannel& c) {
    if (c.psk.empty()) return "AA==";   // no encryption (a single 0 byte, as addMeshtasticChannel takes it)
    return meshcrypto::base64Encode(c.psk.data(), c.psk.size());
}

bool meshtasticUrlLora(const MeshtasticChannelSet& s, MeshLoraSettings& out, std::string& err) {
    const std::string region = meshtasticRegionFromEnum(s.region);
    if (region.empty()) { err = s.region == 0 ? "the link does not set a region" : "OnAir has no band for this region"; return false; }
    const std::string preset = meshtasticPresetFromEnum(s.usePreset ? s.modemPreset : 0);
    if (preset.empty()) { err = "OnAir does not know this modem preset"; return false; }
    MeshLoraSettings l;
    if (!meshtasticRegionPreset(region, preset, l)) { err = "the region has no slot for this preset"; return false; }
    if (!s.usePreset) {
        double bw = s.bandwidth;
        if (s.bandwidth == 31) bw = 31.25; else if (s.bandwidth == 62) bw = 62.5; else if (s.bandwidth == 7) bw = 7.8125; else if (s.bandwidth == 10) bw = 10.4167;
        else if (s.bandwidth == 15) bw = 15.625; else if (s.bandwidth == 20) bw = 20.8333; else if (s.bandwidth == 41) bw = 41.6667;
        if (bw <= 0 || s.spreadFactor < 5 || s.spreadFactor > 12 || s.codingRate < 5 || s.codingRate > 8) { err = "the custom modem setting is incomplete"; return false; }
        l.name = "Custom";
        l.bwHz = bw * 1000; l.sf = (int)s.spreadFactor; l.cr = (int)s.codingRate;
        l.ldro = meshLoraLdro(l.sf, l.bwHz);
    }
    double startHz = 0, endHz = 0;
    meshtasticRegionRange(region, startHz, endHz);
    if (s.overrideFrequency > 0) l.freqHz = (double)s.overrideFrequency * 1e6;
    else {
        const uint32_t n = (uint32_t)std::floor((endHz - startHz) / l.bwHz + 1e-9);
        if (n == 0) { err = "the region is narrower than the bandwidth"; return false; }
        const uint32_t slot = s.channelNum > 0 ? (s.channelNum - 1) % n : djb2(meshtasticUrlChannelName(s, 0)) % n;
        l.freqHz = std::round(startHz + l.bwHz / 2 + slot * l.bwHz);
    }
    l.freqHz += (double)s.frequencyOffset * 1e6;
    out = l;
    return true;
}

}  // namespace dect2
