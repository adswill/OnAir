// LoRa APRS and MeshCom 4 packet layer: see mesh_proto.h.
// Sources (read 2026-10-10):
//   LoRa APRS: richonguzman/LoRa_APRS_iGate (GPL-3.0) src/lora_utils.cpp (prefix "\x3c\xff\x01" on transmit, checked and stripped on
//   receive, setCRC(true), RadioLib begin() defaults otherwise), data/igate_conf.json; richonguzman/LoRa_APRS_Tracker data/tracker_conf.json
//   (433.775 SF12 / 434.855 SF9 4/7 / 439.9125 SF12 / 915 SF12, all 125 kHz); lora-aprs/LoRa_APRS_iGate data/is-cfg.json (433.775 SF12
//   125 kHz 4/5); RadioLib SX1262.h (begin defaults: sync word RADIOLIB_SX126X_SYNC_WORD_PRIVATE 0x12, preamble 8).
//   MeshCom: icssw-org/MeshCom-Firmware (MIT), branch dev: src/aprs_functions.cpp (decodeAPRS, encodeAPRS, encodeLoRaAPRS),
//   src/lora_functions.cpp (handleACK), src/country_profile.cpp and src/configuration_default.h (frequencies, SF, BW, CR),
//   src/configuration_global.h (SYNC_WORD_SX127x 0x2b, DEFAULT_PREAMPLE_LENGTH 32, MSG_TYPE_*), src/lora_setchip.cpp (CRC on).
// Written from the descriptions above; no code was copied.
#include "mesh_proto_internal.h"
#include <cctype>
#include <cstring>

namespace dect2 {

namespace {

bool callChar(unsigned char c) { return isalnum(c) || c == '-'; }

// a station call as both firmwares accept it, loosely: 1..20 letters, digits and '-', at least one digit or letter
bool plausibleCall(const std::string& s) {
    if (s.empty() || s.size() > 20) return false;
    for (unsigned char c : s) if (!callChar(c)) return false;
    return true;
}

// invalid UTF-8 and control characters become '?' (comments may carry UTF-8; broken bytes must not reach the interface)
std::string cleanText(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { o += (c < 0x20 || c == 0x7F) ? '?' : (char)c; i++; continue; }
        int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = len > 0 && i + (size_t)len <= s.size() && !(c == 0xC0 || c == 0xC1 || c > 0xF4);
        for (int k = 1; ok && k < len; k++) ok = ((unsigned char)s[i + (size_t)k] & 0xC0) == 0x80;
        if (!ok) { o += '?'; i++; continue; }
        o.append(s, i, (size_t)len);
        i += (size_t)len;
    }
    return o;
}

std::string baseCall(const std::string& c) {
    const size_t d = c.find('-');
    return d == std::string::npos ? c : c.substr(0, d);
}

void cleanInfo(aprs::Info& a) {
    a.comment = cleanText(a.comment); a.text = cleanText(a.text); a.name = cleanText(a.name); a.summary = cleanText(a.summary);
}

// "/B=085" in a MeshCom position (aprsExtractTag 'B', an integer)
int meshcomBattery(const std::string& s) {
    const size_t p = s.find("/B=");
    if (p == std::string::npos) return -1;
    size_t i = p + 3; int v = 0, nd = 0;
    while (i < s.size() && isdigit((unsigned char)s[i]) && nd < 3) { v = v * 10 + (s[i] - '0'); i++; nd++; }
    return nd ? v : -1;
}

} // namespace

std::string meshPrintable(const uint8_t* p, size_t n) {
    std::string o;
    for (size_t i = 0; i < n; i++) {
        if (p[i] >= 0x20 && p[i] < 0x7F && p[i] != '\\') o += (char)p[i];
        else o += meshFmt("\\x%02x", p[i]);
    }
    return o;
}

MeshDecodeResult meshDecodeLoraAprs(const uint8_t* p, size_t n) {
    MeshDecodeResult r;
    r.packet.protocol = MeshProtocol::LoraAprs;
    r.packet.size = n;
    r.raw = meshPrintable(p, n);
    if (n < 3 || p[0] != 0x3C || p[1] != 0xFF || p[2] != 0x01) { r.packet.note = "no LoRa APRS prefix"; return r; }
    std::string s((const char*)p + 3, n - 3);
    while (!s.empty() && (s.back() == 0 || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    const size_t gt = s.find('>'), colon = s.find(':');
    if (gt == std::string::npos || colon == std::string::npos || gt == 0 || colon < gt + 2) { r.packet.note = "not a TNC2 packet"; return r; }
    const std::string src = s.substr(0, gt), hdr = s.substr(gt + 1, colon - gt - 1);
    const size_t comma = hdr.find(',');
    const std::string dest = hdr.substr(0, comma), path = comma == std::string::npos ? std::string() : hdr.substr(comma + 1);
    if (!plausibleCall(src) || !plausibleCall(dest)) { r.packet.note = "bad call in the TNC2 header"; return r; }
    for (unsigned char c : path) if (!(callChar(c) || c == ',' || c == '*')) { r.packet.note = "bad path in the TNC2 header"; return r; }
    r.ok = true;
    r.hasAprs = true;
    r.aprsSource = src; r.aprsDest = dest; r.aprsPath = path;
    r.aprsInfo = s.substr(colon + 1);
    r.packet.from = src; r.packet.to = dest; r.packet.path = path;
    r.packet.decrypted = true;
    if (aprs::parse(baseCall(dest), r.aprsInfo, r.aprs)) {
        cleanInfo(r.aprs);
        r.packet.type = r.aprs.type;
        r.packet.detail = r.aprs.summary;
        if (r.aprs.type == "Message" && !r.aprs.isAck && !r.aprs.isRej) {
            MeshTextMessage m;
            m.protocol = MeshProtocol::LoraAprs;
            m.channel = "APRS"; m.from = src; m.to = r.aprs.name; m.text = r.aprs.text;
            r.messages.push_back(m);
        }
    } else {
        r.aprs = aprs::Info();
        r.packet.type = "APRS?";
        r.packet.detail = cleanText(r.aprsInfo.substr(0, 80));
    }
    return r;
}

MeshDecodeResult meshDecodeMeshCom(const uint8_t* p, size_t n) {
    MeshDecodeResult r;
    r.packet.protocol = MeshProtocol::MeshCom;
    r.packet.size = n;
    r.raw = meshPrintable(p, n);
    if (n == 0) return r;
    const uint8_t type = p[0];
    if (type == 0x41) {
        // ack_functions.h isPlausibleAckFrame: 12 bytes or more, byte 5 = 0x80 | remaining hops (at most MAX_HOP_LIMIT 7).
        // Byte 10 is 0x01 and byte 11 0x00 in 99.6 % of the firmware authors' field log; the firmware does not check them, so we
        // demand them too only for frames that would otherwise be accepted on two bytes alone (random data).
        if (n < 12 || (p[5] & 0x80) != 0x80 || (p[5] & 0x7F) > 7 || p[10] != 0x01 || p[11] != 0x00) { r.packet.note = "not a MeshCom ack"; return r; }
        r.ok = true;
        r.packet.type = "ACK";
        r.packet.packetId = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
        const uint32_t acked = (uint32_t)p[6] | (uint32_t)p[7] << 8 | (uint32_t)p[8] << 16 | (uint32_t)p[9] << 24;
        r.packet.hopLimit = p[5] & 0x7F;
        r.packet.detail = meshFmt("ack of %08X", acked);
        r.packet.decrypted = true;
        return r;
    }
    if (type != ':' && type != '!' && type != '@') { r.packet.note = "not a MeshCom packet"; return r; }
    if (n < 16) { r.packet.note = "short MeshCom packet"; return r; }
    size_t i = 6;
    std::string srcPath;
    for (; i < n && i - 6 < 120 && p[i] != '>'; i++) {
        if (p[i] < 0x20 || p[i] > 0x7E) break;
        srcPath += (char)p[i];
    }
    if (i >= n || p[i] != '>') { r.packet.note = "MeshCom: no '>' after the source"; return r; }
    i++;
    const size_t d0 = i;
    std::string dest;
    for (; i < n && i - d0 < 120 && p[i] != type; i++) {
        if (p[i] < 0x20 || p[i] > 0x7E) break;
        dest += (char)p[i];
    }
    if (i >= n || p[i] != type) { r.packet.note = "MeshCom: no type character after the destination"; return r; }
    i++;
    std::string payload;
    for (; i < n && p[i] != 0; i++) payload += (char)p[i];
    if (i >= n) { r.packet.note = "MeshCom: payload not ended"; return r; }
    i++;
    if (i + 4 > n) { r.packet.note = "MeshCom: trailer cut off"; return r; }
    const uint8_t hw = p[i], mod = p[i + 1];
    uint32_t sum = 0;
    for (size_t k = 0; k < i + 2; k++) sum += p[k];
    const uint32_t fcs = (uint32_t)p[i + 2] << 8 | p[i + 3];
    if ((sum & 0xFFFF) != fcs) { r.packet.note = "MeshCom: checksum wrong"; return r; }
    // source path: "CALL" or "CALL,RELAY1,..."; the first is the sender
    const std::string src = srcPath.substr(0, srcPath.find(','));
    if (!plausibleCall(src)) { r.packet.note = "MeshCom: bad source call"; return r; }
    std::string destCall = dest.substr(dest.rfind(',') == std::string::npos ? 0 : dest.rfind(',') + 1);
    size_t j = i + 4;
    const int fw = j < n ? p[j++] : -1;
    r.ok = true;
    r.hasAprs = true;
    r.aprsSource = src;
    r.aprsPath = srcPath.size() > src.size() ? srcPath.substr(src.size() + 1) : std::string();
    r.aprsDest = dest;
    r.aprsInfo = std::string(1, (char)type) + payload;
    r.packet.from = src; r.packet.to = destCall; r.packet.path = r.aprsPath;
    r.packet.packetId = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24;
    r.packet.hopLimit = p[5] & 0x0F;
    r.packet.decrypted = true;
    std::string flags;
    if (p[5] & 0x80) flags += ", server";
    if (p[5] & 0x40) flags += ", track";
    if (p[5] & 0x20) flags += ", app offline";
    if (p[5] & 0x10) flags += ", mesh";
    const std::string tail = meshFmt("HW %u, MOD %u, country %u", hw, mod & 0x0F, mod >> 4) + (fw >= 0 ? meshFmt(", FW %d", fw) : std::string()) + flags;
    if (type == ':') {
        r.packet.type = "TEXT";
        MeshTextMessage m;
        m.protocol = MeshProtocol::MeshCom;
        m.channel = destCall == "*" ? "MeshCom all" : "MeshCom " + destCall;
        m.from = src; m.to = destCall; m.text = cleanText(payload); m.packetId = r.packet.packetId;
        r.messages.push_back(m);
        r.packet.detail = cleanText(payload.substr(0, 60)) + " (" + tail + ")";
    } else if (type == '!') {
        r.packet.type = "POSITION";
        if (aprs::parse(baseCall(destCall), r.aprsInfo, r.aprs)) cleanInfo(r.aprs);
        else r.aprs = aprs::Info();
        r.batteryPct = meshcomBattery(payload);
        r.packet.detail = (r.aprs.hasPos ? aprs::fmtLat(r.aprs.lat) + " " + aprs::fmtLon(r.aprs.lon) : std::string("position not readable")) +
                          (r.batteryPct >= 0 ? meshFmt(", battery %d %%", r.batteryPct) : std::string()) + " (" + tail + ")";
    } else {
        r.packet.type = "HEY";
        r.packet.detail = cleanText(payload.substr(0, 60)) + " (" + tail + ")";
    }
    return r;
}

std::vector<MeshLoraSettings> loraAprsPresets() {
    struct Row { const char* name; double mhz; int sf, cr; };
    static const Row rows[] = {
        {"LoRa APRS EU", 433.775, 12, 5}, {"LoRa APRS PL", 434.855, 9, 7}, {"LoRa APRS UK", 439.9125, 12, 5}, {"LoRa APRS 915", 915.0, 12, 5},
    };
    std::vector<MeshLoraSettings> v;
    for (const auto& r : rows) {
        MeshLoraSettings s;
        s.name = r.name; s.freqHz = r.mhz * 1e6; s.sf = r.sf; s.bwHz = 125000; s.cr = r.cr; s.preamble = 8; s.syncWord = 0x12;
        s.ldro = meshLoraLdro(s.sf, s.bwHz);
        v.push_back(s);
    }
    return v;
}

std::vector<MeshLoraSettings> meshcomPresets() {
    // country_profile.cpp (ESP32 columns): frequency, bandwidth, SF; CR 4/6 everywhere; preamble 32 for the default (EU), 8 in the table
    struct Row { const char* name; double mhz, bwKhz; int sf, pre; };
    static const Row rows[] = {
        {"MeshCom EU", 433.175, 250, 11, 32}, {"MeshCom UK", 439.9125, 125, 10, 8}, {"MeshCom LA", 433.925, 125, 10, 8},
        {"MeshCom 868", 869.525, 250, 11, 8}, {"MeshCom 915", 906.875, 250, 11, 8}, {"MeshCom VR2", 435.775, 250, 11, 8},
        {"MeshCom 435", 435.750, 250, 11, 8}, {"MeshCom 436", 436.250, 250, 11, 8}, {"MeshCom 442", 442.000, 250, 11, 8},
    };
    std::vector<MeshLoraSettings> v;
    for (const auto& r : rows) {
        MeshLoraSettings s;
        s.name = r.name; s.freqHz = r.mhz * 1e6; s.sf = r.sf; s.bwHz = r.bwKhz * 1e3; s.cr = 6; s.preamble = r.pre; s.syncWord = 0x2B;
        s.ldro = meshLoraLdro(s.sf, s.bwHz);
        v.push_back(s);
    }
    return v;
}

std::vector<uint8_t> loraAprsFrame(const std::string& tnc2) {
    std::vector<uint8_t> v = {0x3C, 0xFF, 0x01};
    v.insert(v.end(), tnc2.begin(), tnc2.end());
    return v;
}

std::vector<uint8_t> meshcomBuild(const MeshComFrame& f) {
    std::vector<uint8_t> b;
    b.push_back((uint8_t)f.type);
    for (int k = 0; k < 4; k++) b.push_back((uint8_t)(f.msgId >> (8 * k)));
    b.push_back((uint8_t)((f.maxHop & 0x0F) | (f.server ? 0x80 : 0) | (f.track ? 0x40 : 0) | (f.appOffline ? 0x20 : 0) | (f.mesh ? 0x10 : 0)));
    const std::string head = f.sourcePath + ">" + f.dest + f.type + f.payload;
    b.insert(b.end(), head.begin(), head.end());
    b.push_back(0);
    b.push_back(f.hw);
    b.push_back(f.mod);
    uint32_t sum = 0;
    for (uint8_t c : b) sum += c;
    b.push_back((uint8_t)(sum >> 8));
    b.push_back((uint8_t)sum);
    b.push_back(f.fw);
    b.push_back(f.lastHw);
    b.push_back(f.subVersion ? (uint8_t)f.subVersion : (uint8_t)'#');
    b.push_back(0x7E);
    return b;
}

} // namespace dect2
