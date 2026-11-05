// Mesh transmit identity and packets: see mesh_tx.h.
#include "dect2/mesh/mesh_tx.h"
#include "dect2/mesh/mesh_crypto.h"
#include "dect2/mesh/mesh_proto.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace dect2 {

namespace {
std::string lowerS(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
bool isHex8(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s) if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}
const uint8_t kDefaultKey[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
}

bool meshTxParsePsk(const std::string& pskIn, std::vector<uint8_t>& key) {
    key.clear();
    const std::string p = trim(pskIn), l = lowerS(p);
    if (l.empty() || l == "default" || l == "aq==" || l == "aq") { key.assign(kDefaultKey, kDefaultKey + 16); return true; }
    if (l == "none" || l == "off" || l == "ag==") return true;
    std::vector<uint8_t> raw;
    if (!(p.size() == 32 || p.size() == 64) || !meshcrypto::hexDecode(p, raw)) {
        raw.clear();
        if (!meshcrypto::base64Decode(p, raw)) return false;
    }
    if (raw.size() == 16 || raw.size() == 32) { key = raw; return true; }
    if (raw.size() == 1) return meshtasticExpandPsk(raw, key);        // a short key: the firmware's rules (1 = the default key, 2 .. 10 = it with its last byte changed)
    return false;
}

bool meshTxParseNodeId(const std::string& idIn, uint32_t& num) {
    std::string s = trim(idIn);
    if (!s.empty() && s[0] == '!') s.erase(0, 1);
    if (!isHex8(s)) return false;
    num = (uint32_t)std::strtoul(s.c_str(), nullptr, 16);
    return true;
}

uint32_t meshTxNodeNum(const MeshTxIdentity& id) {
    uint32_t n = 0;
    if (meshTxParseNodeId(id.nodeId, n) && n != 0 && n != 0xFFFFFFFFu) return n;
    // a stable number: FNV-1a over the callsign (or the long name), kept clear of the reserved low numbers and the broadcast address
    const std::string& src = !trim(id.callsign).empty() ? id.callsign : id.longName;
    uint32_t h = 2166136261u;
    for (char c : lowerS(trim(src))) { h ^= (uint8_t)c; h *= 16777619u; }
    if (h < 0x100) h += 0x1000;
    if (h == 0xFFFFFFFFu) h = 0xFFFFFFFEu;
    return h;
}

std::string meshTxText(const MeshTxIdentity& id, const std::string& text) {
    const std::string cs = trim(id.callsign);
    if (id.callsignInMessage && !cs.empty()) return cs + ": " + text;
    return text;
}

static size_t utf8Chars(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

std::vector<std::string> meshTxWarnings(const MeshTxIdentity& id, const std::string& text) {
    std::vector<std::string> w;
    if (utf8Chars(id.shortName) > 4) w.push_back("The short name is longer than 4 characters: it is cut to 4.");
    if (id.shortName.empty()) w.push_back("The short name is empty: other nodes will show the node ID.");
    if (id.longName.size() > 39) w.push_back("The long name is longer than 39 bytes: it is cut.");
    if (id.longName.empty()) w.push_back("The long name is empty.");
    uint32_t n = 0;
    if (!trim(id.nodeId).empty() && !meshTxParseNodeId(id.nodeId, n)) w.push_back("The node ID is not 8 hex digits (like !1a2b3c4d): one is made from the callsign instead.");
    else if (!trim(id.nodeId).empty() && (n == 0 || n == 0xFFFFFFFFu)) w.push_back("The node ID 0 and !ffffffff are reserved: one is made from the callsign instead.");
    std::vector<uint8_t> key;
    if (!meshTxParsePsk(id.psk, key)) w.push_back("The PSK is not default, none, hex or base64 of 1, 16 or 32 bytes: the default key is used.");
    else if (key.empty()) w.push_back("No PSK: the message goes out unencrypted, anyone can read it.");
    if (id.hopLimit < 0 || id.hopLimit > 7) w.push_back("The hop limit must be 0 to 7: it is limited to that.");
    else if (id.hopLimit > 3) w.push_back("A hop limit above 3 loads the whole mesh with your message; 3 is the usual setting.");
    if (id.channelName.empty()) w.push_back("The channel name is empty: the default name for the preset is used.");
    if (id.callsignInMessage && trim(id.callsign).empty()) w.push_back("The callsign box is ticked but no callsign is set: nothing is added.");
    const std::string t = meshTxText(id, text);
    if (t.size() > 200) w.push_back("The text is longer than 200 bytes: it is cut (a Meshtastic message holds 233 bytes).");
    return w;
}

std::vector<MeshTxFrame> meshTxPackets(const MeshTxIdentity& id, const std::string& textIn, uint32_t seed) {
    std::vector<MeshTxFrame> out;
    std::vector<uint8_t> key;
    if (!meshTxParsePsk(id.psk, key)) key.assign(kDefaultKey, kDefaultKey + 16);   // a bad PSK is warned about, never refused
    std::string chan = trim(id.channelName);
    if (chan.empty()) chan = id.preset.empty() ? "LongFast" : id.preset;
    const int hop = std::max(0, std::min(7, id.hopLimit));
    const uint32_t num = meshTxNodeNum(id);
    std::string lng = id.longName.substr(0, 39);
    std::string shrt;                                       // 4 characters, not bytes
    {
        size_t chars = 0;
        for (size_t i = 0; i < id.shortName.size(); i++) {
            if (((unsigned char)id.shortName[i] & 0xC0) != 0x80) { if (++chars > 4) break; }
            shrt += id.shortName[i];
        }
    }
    uint32_t pid = seed ? seed : 1;
    if (id.sendNodeInfo) {
        // NODEINFO: the User message in a Data protobuf, port 4, to everyone
        std::vector<uint8_t> user;
        auto putVar = [](std::vector<uint8_t>& b, uint64_t v) { while (v >= 0x80) { b.push_back((uint8_t)(v | 0x80)); v >>= 7; } b.push_back((uint8_t)v); };
        auto putStr = [&](int field, const std::string& s) { user.push_back((uint8_t)(field << 3 | 2)); putVar(user, s.size()); user.insert(user.end(), s.begin(), s.end()); };
        putStr(1, meshtasticNodeIdString(num));
        putStr(2, lng);
        putStr(3, shrt);
        user.push_back((uint8_t)(5 << 3)); putVar(user, 255);          // hw_model PRIVATE_HW
        MeshTxFrame f;
        f.what = "node info";
        f.bytes = meshtasticRaw(num, 0xFFFFFFFFu, pid, hop, hop, false, chan, key, meshtasticData(4, user));
        out.push_back(std::move(f));
        pid++;
    }
    std::string text = meshTxText(id, textIn);
    if (text.size() > 200) text.resize(200);
    MeshTxFrame f;
    f.what = "text";
    f.bytes = meshtasticRaw(num, 0xFFFFFFFFu, pid, hop, hop, false, chan, key, meshtasticData(1, std::vector<uint8_t>(text.begin(), text.end())));
    out.push_back(std::move(f));
    return out;
}

} // namespace dect2
