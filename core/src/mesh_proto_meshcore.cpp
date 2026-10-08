// MeshCore packets. Sources: meshcore-dev/MeshCore src/Packet.h and Packet.cpp (header byte, transport codes, path length byte
// with hash size in the top two bits, readFrom/writeTo), src/Mesh.cpp (ADVERT signature message, GRP_TXT/GRP_DATA handling,
// createAdvert, createGroupDatagram, TRACE layout), src/Utils.cpp (encrypt: AES-128-ECB zero padded; MAC = first 2 bytes of
// HMAC-SHA256 over the ciphertext with the 32 byte secret), src/helpers/BaseChatMesh.cpp (group text layout, channel hash =
// SHA-256 of the secret), src/helpers/AdvertDataHelpers.{h,cpp} (advert app data), docs/packet_format.md.
#include "mesh_proto_internal.h"
#include "dect2/mesh_crypto.h"
#include <cmath>
#include <cstring>

namespace dect2 {

using namespace meshcrypto;

namespace {

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }

const char* typeName(int t) {
    switch (t) {
    case 0: return "REQ";
    case 1: return "RESPONSE";
    case 2: return "TXT_MSG";
    case 3: return "ACK";
    case 4: return "ADVERT";
    case 5: return "GRP_TXT";
    case 6: return "GRP_DATA";
    case 7: return "ANON_REQ";
    case 8: return "PATH";
    case 9: return "TRACE";
    case 10: return "MULTIPART";
    case 11: return "CONTROL";
    case 15: return "RAW_CUSTOM";
    default: return nullptr;
    }
}

const char* advRole(int t) {
    switch (t) {
    case 1: return "Chat";
    case 2: return "Repeater";
    case 3: return "Room server";
    case 4: return "Sensor";
    default: return "Unknown";
    }
}

// Utils::MACThenDecrypt: HMAC key is the full 32 byte secret, AES key its first 16 bytes
bool macThenDecrypt(const MeshCoreChannel& c, const uint8_t* src, size_t n, std::vector<uint8_t>& plain) {
    if (n <= 2) return false;
    uint8_t mac[32];
    hmacSha256(c.secret, 32, src + 2, n - 2, mac);
    if (std::memcmp(mac, src, 2) != 0) return false;
    plain = aesEcbDecrypt(c.secret, 16, src + 2, n - 2);
    return !plain.empty();
}

std::string cleanLine(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i]; i++) s += (p[i] < 0x20) ? ' ' : (char)p[i];
    return s;
}

} // namespace

MeshDecodeResult MeshProto::decodeMeshCore(const uint8_t* p, size_t n, const MeshRadioInfo&) const {
    MeshDecodeResult r;
    MeshPacketInfo& pi = r.packet;
    pi.protocol = MeshProtocol::MeshCore;
    pi.size = n;
    if (n < 3) { pi.note = "too short for a MeshCore packet"; return r; }
    const uint8_t header = p[0];
    const int route = header & 3, type = (header >> 2) & 15, ver = header >> 6;
    size_t i = 1;
    if (route == 0 || route == 3) {                                    // transport codes
        if (n < 1 + 4 + 1) { pi.note = "truncated transport codes"; return r; }
        i += 4;
    }
    const uint8_t pl = p[i++];
    const int hsz = (pl >> 6) + 1, hcnt = pl & 63;
    if (hsz == 4 || hcnt * hsz > 64) { pi.note = "bad path length byte"; return r; }
    const size_t pathBytes = (size_t)hcnt * hsz;
    if (i + pathBytes >= n) { pi.note = "no payload"; return r; }
    const uint8_t* path = p + i;
    i += pathBytes;
    const uint8_t* pay = p + i;
    const size_t m = n - i;
    if (m > 184) { pi.note = "payload longer than 184 bytes"; return r; }

    pi.routeType = route;
    pi.payloadType = type;
    pi.payloadVersion = ver;
    pi.pathHashCount = hcnt;
    pi.pathHashSize = hsz;
    for (int k = 0; k < hcnt; k++) {
        if (k) pi.path += ",";
        pi.path += meshHex(path + (size_t)k * hsz, (size_t)hsz);
    }
    const char* tn = typeName(type);
    pi.type = tn ? tn : meshFmt("TYPE_%d", type);
    r.ok = true;
    const bool flood = route == 0 || route == 1;
    const std::string via = hcnt ? meshFmt(", %d hop%s", hcnt, hcnt == 1 ? "" : "s") : (flood ? ", direct from sender" : "");

    switch (type) {
    case 4: {                                                          // ADVERT
        if (m < 32 + 4 + 64 + 1) { pi.note = "incomplete advert"; break; }
        const uint8_t* pub = pay;
        const uint32_t ts = le32(pay + 32);
        const uint8_t* sig = pay + 36;
        const uint8_t* app = pay + 100;
        size_t al = m - 100;
        if (al > 32) al = 32;                                          // Mesh.cpp clamps to MAX_ADVERT_DATA_SIZE
        std::vector<uint8_t> msg(pub, pub + 32);
        put32(msg, ts);
        msg.insert(msg.end(), app, app + al);
        const bool ok = ed25519Verify(sig, msg.data(), msg.size(), pub);
        pi.from = meshHex(pub, 6);
        pi.decrypted = true;                                           // adverts are not encrypted
        // AdvertDataParser
        const uint8_t flags = app[0];
        size_t k = 1;
        MeshNodeUpdate u;
        u.protocol = MeshProtocol::MeshCore;
        u.nodeId = pi.from;
        u.publicKey = meshHex(pub, 32);
        u.role = advRole(flags & 15);
        u.signatureChecked = true;
        u.signatureOk = ok;
        u.positionTime = ts;
        bool bad = false;
        if (flags & 0x10) {
            if (k + 8 > al) bad = true;
            else {
                const int32_t la = (int32_t)le32(app + k), lo = (int32_t)le32(app + k + 4);
                k += 8;
                if (la != 0 || lo != 0) { u.hasPosition = true; u.lat = la / 1e6; u.lon = lo / 1e6; }
            }
        }
        if (!bad && (flags & 0x20)) { if (k + 2 > al) bad = true; else k += 2; }
        if (!bad && (flags & 0x40)) { if (k + 2 > al) bad = true; else k += 2; }
        if (!bad && (flags & 0x80)) u.longName = cleanLine(app + k, al - k);
        if (bad) { pi.note = "incomplete advert data"; break; }
        pi.detail = (u.longName.empty() ? "(no name)" : u.longName) + ", " + u.role;
        if (u.hasPosition) pi.detail += meshFmt(", %.5f, %.5f", u.lat, u.lon);
        pi.detail += ok ? ", signature ok" : ", signature NOT valid";
        pi.detail += via;
        if (!ok) { pi.note = "signature not valid"; break; }           // the firmware drops these
        r.nodes.push_back(u);
        break;
    }
    case 5:
    case 6: {                                                          // GRP_TXT, GRP_DATA
        if (m < 4) { pi.note = "incomplete group packet"; break; }
        const uint8_t h = pay[0];
        pi.channelHash = h;
        pi.channel = meshFmt("0x%02x", h);
        std::vector<MeshCoreChannel> chans;
        {
            std::lock_guard<std::mutex> g(s_->m);
            chans = s_->mc;
        }
        bool any = false;
        for (const MeshCoreChannel& c : chans) {
            if (c.hash != h) continue;
            any = true;
            std::vector<uint8_t> d;
            if (!macThenDecrypt(c, pay + 1, m - 1, d)) continue;
            pi.decrypted = true;
            pi.channel = c.name;
            if (type == 5) {
                if (d.size() < 5) { pi.note = "short group text"; break; }
                if ((d[4] >> 2) != 0) { pi.note = meshFmt("unsupported text type %d", d[4] >> 2); break; }
                const std::string line = cleanLine(d.data() + 5, d.size() - 5);
                MeshTextMessage t;
                t.protocol = MeshProtocol::MeshCore;
                t.channel = c.name;
                t.senderTime = le32(d.data());
                t.hops = hcnt;
                const size_t sep = line.find(": ");
                if (sep != std::string::npos) { t.from = line.substr(0, sep); t.text = line.substr(sep + 2); }
                else t.text = line;
                pi.from = t.from;
                pi.to = c.name;
                pi.detail = (t.from.empty() ? "" : t.from + ": ") + t.text + via;
                r.messages.push_back(t);
            } else {
                if (d.size() < 3) { pi.note = "short group data"; break; }
                const unsigned dt = d[0] | (d[1] << 8);
                pi.detail = meshFmt("data type 0x%04x, %u bytes", dt, (unsigned)d[2]) + via;
            }
            break;
        }
        if (!pi.decrypted && pi.note.empty()) pi.note = any ? "MAC mismatch (wrong key or damaged)" : meshFmt("unknown channel 0x%02x", h);
        break;
    }
    case 0: case 1: case 2: case 8: {                                  // end-to-end encrypted, dest hash + src hash + MAC + data
        if (m < 4) { pi.note = "incomplete packet"; break; }
        pi.to = meshHex(pay, 1);
        pi.from = meshHex(pay + 1, 1);
        pi.note = "encrypted (direct)";
        pi.detail = "to " + pi.to + " from " + pi.from + via;
        break;
    }
    case 7: {                                                          // ANON_REQ: dest hash, sender public key, MAC, data
        if (m < 1 + 32 + 2) { pi.note = "incomplete packet"; break; }
        pi.to = meshHex(pay, 1);
        pi.from = meshHex(pay + 1, 6);
        pi.note = "encrypted (direct)";
        pi.detail = "to " + pi.to + " from " + pi.from + via;
        break;
    }
    case 3: {                                                          // ACK: 4 byte checksum
        if (m < 4) { pi.note = "incomplete ack"; break; }
        pi.decrypted = true;
        pi.detail = meshFmt("ack %08x", le32(pay)) + via;
        break;
    }
    case 9: {                                                          // TRACE: tag, auth, flags, hashes to visit; SNRs sit in the path field
        if (m < 9) { pi.note = "incomplete trace"; break; }
        pi.decrypted = true;
        const int sz = 1 << (pay[8] & 3);
        const int hops = (int)((m - 9) / sz);
        std::string s = meshFmt("trace %08x, %d node%s", le32(pay), hops, hops == 1 ? "" : "s");
        if (hcnt) {                                                    // each entry of the path is an SNR in quarter dB
            s += ", SNR";
            for (int k = 0; k < hcnt; k++) s += meshFmt(" %.2f", (int8_t)path[k] / 4.0);
            s += " dB";
        }
        pi.detail = s;
        pi.path.clear();
        break;
    }
    case 11:
        pi.decrypted = true;
        pi.detail = meshFmt("control, first byte 0x%02x", pay[0]);
        break;
    default:
        pi.detail = meshFmt("%zu bytes", m);
        break;
    }
    return r;
}

// ---- builders ----

bool meshcoreNodeKeys(const MeshCoreNode& n, uint8_t seed[32], uint8_t pub[32]) {
    uint8_t in[8] = {'m', 'c', 't', 'e', 's', 't', n.seed, 0};
    sha256(in, 7, seed);
    ed25519PublicKey(seed, pub);
    return true;
}

std::vector<uint8_t> meshcoreAdvert(const MeshCoreNode& nd, uint32_t unixTime) {
    uint8_t seed[32], pub[32];
    meshcoreNodeKeys(nd, seed, pub);
    // AdvertDataBuilder::encodeTo
    std::vector<uint8_t> app;
    app.push_back((uint8_t)((nd.repeater ? 2 : 1) | 0x10 | (nd.name.empty() ? 0 : 0x80)));
    put32(app, (uint32_t)(int32_t)(nd.lat * 1e6));
    put32(app, (uint32_t)(int32_t)(nd.lon * 1e6));
    const size_t room = 32 - app.size();
    app.insert(app.end(), nd.name.begin(), nd.name.begin() + (nd.name.size() < room ? nd.name.size() : room));
    std::vector<uint8_t> msg(pub, pub + 32);
    put32(msg, unixTime);
    msg.insert(msg.end(), app.begin(), app.end());
    uint8_t sig[64];
    ed25519Sign(seed, msg.data(), msg.size(), sig);
    std::vector<uint8_t> out;
    out.push_back((uint8_t)((4 << 2) | 1));                // ADVERT, flood
    out.push_back(0);                                      // path length 0
    out.insert(out.end(), pub, pub + 32);
    put32(out, unixTime);
    out.insert(out.end(), sig, sig + 64);
    out.insert(out.end(), app.begin(), app.end());
    return out;
}

std::vector<uint8_t> meshcoreGroupText(const uint8_t secret16[16], const MeshCoreNode& from, uint32_t unixTime, const std::string& text,
                                       const std::vector<uint8_t>& path) {
    std::vector<uint8_t> plain;
    put32(plain, unixTime);
    plain.push_back(0);                                    // TXT_TYPE_PLAIN, attempt 0
    const std::string line = from.name + ": " + text;
    // BaseChatMesh::sendGroupMessage; keep the packet under 184 payload bytes (1 hash + 2 MAC + whole blocks)
    const size_t maxPlain = (184 - 3) / 16 * 16;
    plain.insert(plain.end(), line.begin(), line.end());
    if (plain.size() > maxPlain) plain.resize(maxPlain);
    uint8_t key32[32] = {};
    std::memcpy(key32, secret16, 16);
    const std::vector<uint8_t> ct = aesEcbEncrypt(key32, 16, plain.data(), plain.size());
    uint8_t mac[32];
    hmacSha256(key32, 32, ct.data(), ct.size(), mac);
    uint8_t h[32];
    sha256(secret16, 16, h);
    std::vector<uint8_t> out;
    out.push_back((uint8_t)((5 << 2) | 1));                // GRP_TXT, flood
    out.push_back((uint8_t)(path.size() & 63));
    out.insert(out.end(), path.begin(), path.begin() + (path.size() < 63 ? path.size() : 63));
    out.push_back(h[0]);
    out.push_back(mac[0]);
    out.push_back(mac[1]);
    out.insert(out.end(), ct.begin(), ct.end());
    return out;
}

std::vector<uint8_t> meshcorePublicText(const MeshCoreNode& from, uint32_t unixTime, const std::string& text, const std::vector<uint8_t>& path) {
    return meshcoreGroupText(meshcorePublicSecret(), from, unixTime, text, path);
}

std::vector<uint8_t> meshcoreAck(uint32_t crc, const std::vector<uint8_t>& path) {
    std::vector<uint8_t> out;
    out.push_back((uint8_t)((3 << 2) | 1));
    out.push_back((uint8_t)(path.size() & 63));
    out.insert(out.end(), path.begin(), path.begin() + (path.size() < 63 ? path.size() : 63));
    put32(out, crc);
    return out;
}

} // namespace dect2
