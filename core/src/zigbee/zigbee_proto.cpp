// IEEE 802.15.4 / Zigbee frame layers (see zigbee_proto.h).
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/voicekeys/net_crypto.h"
#include <cstring>
#include <cstdio>
#include <cstring>
#include "dect2/common/crc.h"
#include "dect2/common/endian.h"

namespace dect2 {
namespace zigbee {

// IEEE 802.15.4-2006 table 24, chip values c0 ... c31 (c0 first, the most significant bit here)
const uint32_t kChipTable[16] = {
    0xD9C3522E, 0xED9C3522, 0x2ED9C352, 0x22ED9C35, 0x522ED9C3, 0x3522ED9C, 0xC3522ED9, 0x9C3522ED,
    0x8C96077B, 0xB8C96077, 0x7B8C9607, 0x77B8C960, 0x077B8C96, 0x6077B8C9, 0x96077B8C, 0xC96077B8,
};

uint16_t crc16(const uint8_t* d, size_t n) { return crc::crc16Kermit(d, n); }   // CRC-16/KERMIT, the 802.15.4 FCS

const char* frameTypeName(int t) {
    switch (t) {
    case Beacon: return "Beacon";
    case Data: return "Data";
    case Ack: return "Ack";
    case Command: return "MAC cmd";
    case Multipurpose: return "Multipurpose";
    case Fragment: return "Fragment";
    case Extended: return "Extended";
    default: return "Reserved";
    }
}

const char* macCommandName(int c) {
    switch (c) {
    case 1: return "Association request";
    case 2: return "Association response";
    case 3: return "Disassociation notification";
    case 4: return "Data request";
    case 5: return "PAN ID conflict notification";
    case 6: return "Orphan notification";
    case 7: return "Beacon request";
    case 8: return "Coordinator realignment";
    case 9: return "GTS request";
    default: return "Command";
    }
}

const char* nwkCommandName(int c) {
    switch (c) {
    case 1: return "Route request";
    case 2: return "Route reply";
    case 3: return "Network status";
    case 4: return "Leave";
    case 5: return "Route record";
    case 6: return "Rejoin request";
    case 7: return "Rejoin response";
    case 8: return "Link status";
    case 9: return "Network report";
    case 10: return "Network update";
    case 11: return "End device timeout request";
    case 12: return "End device timeout response";
    default: return "NWK command";
    }
}

const char* profileName(int p) {
    switch (p) {
    case 0x0000: return "ZDO";
    case 0x0101: return "Industrial plant monitoring";
    case 0x0104: return "Home Automation";
    case 0x0105: return "Commercial Building Automation";
    case 0x0107: return "Telecom Applications";
    case 0x0108: return "Personal Home & Hospital Care";
    case 0x0109: return "Smart Energy";
    case 0xA1E0: return "Green Power";
    case 0xC05E: return "ZigBee Light Link";
    default: return "";
    }
}

const char* clusterName(int c, int profile) {
    if (profile == 0) {   // ZDO: a response has bit 15 set
        switch (c & 0x7FFF) {
        case 0x0000: return c & 0x8000 ? "NWK_addr_rsp" : "NWK_addr_req";
        case 0x0001: return c & 0x8000 ? "IEEE_addr_rsp" : "IEEE_addr_req";
        case 0x0002: return c & 0x8000 ? "Node_Desc_rsp" : "Node_Desc_req";
        case 0x0003: return c & 0x8000 ? "Power_Desc_rsp" : "Power_Desc_req";
        case 0x0004: return c & 0x8000 ? "Simple_Desc_rsp" : "Simple_Desc_req";
        case 0x0005: return c & 0x8000 ? "Active_EP_rsp" : "Active_EP_req";
        case 0x0006: return c & 0x8000 ? "Match_Desc_rsp" : "Match_Desc_req";
        case 0x0013: return "Device_annce";
        case 0x0020: return c & 0x8000 ? "End_Device_Bind_rsp" : "End_Device_Bind_req";
        case 0x0021: return c & 0x8000 ? "Bind_rsp" : "Bind_req";
        case 0x0022: return c & 0x8000 ? "Unbind_rsp" : "Unbind_req";
        case 0x0030: return c & 0x8000 ? "Mgmt_NWK_Disc_rsp" : "Mgmt_NWK_Disc_req";
        case 0x0031: return c & 0x8000 ? "Mgmt_Lqi_rsp" : "Mgmt_Lqi_req";
        case 0x0032: return c & 0x8000 ? "Mgmt_Rtg_rsp" : "Mgmt_Rtg_req";
        case 0x0034: return c & 0x8000 ? "Mgmt_Leave_rsp" : "Mgmt_Leave_req";
        case 0x0036: return c & 0x8000 ? "Mgmt_Permit_Joining_rsp" : "Mgmt_Permit_Joining_req";
        case 0x0038: return c & 0x8000 ? "Mgmt_NWK_Update_rsp" : "Mgmt_NWK_Update_req";
        default: return "ZDO";
        }
    }
    switch (c) {
    case 0x0000: return "Basic";
    case 0x0001: return "Power Configuration";
    case 0x0003: return "Identify";
    case 0x0004: return "Groups";
    case 0x0005: return "Scenes";
    case 0x0006: return "On/Off";
    case 0x0008: return "Level Control";
    case 0x000A: return "Time";
    case 0x0019: return "OTA Upgrade";
    case 0x0020: return "Poll Control";
    case 0x0102: return "Window Covering";
    case 0x0201: return "Thermostat";
    case 0x0300: return "Color Control";
    case 0x0400: return "Illuminance";
    case 0x0402: return "Temperature";
    case 0x0403: return "Pressure";
    case 0x0405: return "Humidity";
    case 0x0406: return "Occupancy";
    case 0x0500: return "IAS Zone";
    case 0x0702: return "Metering";
    case 0x0B04: return "Electrical Measurement";
    case 0x1000: return "Touchlink";
    default: return "";
    }
}

std::string addrText(uint64_t e) {
    char b[32];
    snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)(e >> 56 & 0xFF), (unsigned)(e >> 48 & 0xFF), (unsigned)(e >> 40 & 0xFF),
             (unsigned)(e >> 32 & 0xFF), (unsigned)(e >> 24 & 0xFF), (unsigned)(e >> 16 & 0xFF), (unsigned)(e >> 8 & 0xFF), (unsigned)(e & 0xFF));
    return b;
}
std::string shortText(uint16_t a) { char b[8]; snprintf(b, sizeof b, "0x%04X", a); return b; }

namespace {
struct Oui { uint32_t oui; const char* name; };
// The vendors whose chips and modules are in most Zigbee devices (the OUI of the extended address)
const Oui kOui[] = {
    {0x00124B, "Texas Instruments"}, {0x000D6F, "Silicon Labs (Ember)"}, {0x000B57, "Silicon Labs"}, {0x842E14, "Silicon Labs"},
    {0x90FD9F, "Silicon Labs"}, {0x14B457, "Silicon Labs (IKEA)"}, {0x680AE2, "Silicon Labs"}, {0x60A423, "Silicon Labs"},
    {0x8CF681, "Silicon Labs"}, {0xEC1BBD, "Silicon Labs"}, {0xD0CF5E, "Silicon Labs"}, {0x001788, "Signify (Philips Hue)"},
    {0x00158D, "Lumi (Xiaomi Aqara)"}, {0x04CF8C, "Xiaomi"}, {0x54EF44, "Lumi (Xiaomi)"}, {0x286D97, "Samsung SmartThings"},
    {0x24FD5B, "Samsung SmartThings"}, {0x0013A2, "Digi (MaxStream)"}, {0x0004A3, "Microchip"}, {0x000781, "Itron"},
    {0x001EC0, "Microchip (Atmel)"}, {0x00212E, "dresden elektronik"}, {0x0021ED, "Chipcon (Texas Instruments)"},
};
} // namespace

const char* vendorOfExt(uint64_t ext) {
    const uint32_t oui = (uint32_t)(ext >> 40);
    for (const Oui& o : kOui) if (o.oui == oui) return o.name;
    return "";
}

namespace {
uint16_t le16(const uint8_t* p) { return (uint16_t)dect2::endian::loadLe16(p); }
uint32_t le32(const uint8_t* p) { return (uint32_t)dect2::endian::loadLe32(p); }
uint64_t le64(const uint8_t* p) { return (uint64_t)dect2::endian::loadLe64(p); }
}

std::string srcText(const Frame& f) {
    if (f.srcMode == 3) return addrText(f.srcExt);
    if (f.srcMode == 2) return shortText(f.srcShort);
    return "-";
}
std::string dstText(const Frame& f) {
    if (f.destMode == 3) return addrText(f.destExt);
    if (f.destMode == 2) return f.destShort == 0xFFFF ? "broadcast" : shortText(f.destShort);
    return "-";
}

namespace {

void parseAps(Frame& f, const uint8_t* p, size_t n) {
    if (n < 1) return;
    const int fc = p[0];
    f.apsType = fc & 3;
    f.apsDelivery = fc >> 2 & 3;
    f.apsSecured = (fc & 0x20) != 0;
    const bool ext = (fc & 0x80) != 0;
    size_t i = 1;
    if (f.apsType == 0) {   // data
        if (f.apsDelivery == 0 || f.apsDelivery == 2) { if (n < i + 1) return; f.apsDestEp = p[i++]; }
        else if (f.apsDelivery == 3) { if (n < i + 2) return; f.apsDestEp = -1; i += 2; }
        if (n < i + 6) return;
        f.apsCluster = le16(p + i); i += 2;
        f.apsProfile = le16(p + i); i += 2;
        f.apsSrcEp = p[i++];
        f.apsCounter = p[i++];
        (void)ext;
        f.hasAps = true;
    } else if (f.apsType == 1) {   // command
        if (n < i + 1) return;
        f.apsCounter = p[i];
        f.hasAps = true;
    } else if (f.apsType == 2) {   // acknowledgement
        if (n >= i + 6) {
            f.apsDestEp = p[i]; i += 1;
            f.apsCluster = le16(p + i); i += 2;
            f.apsProfile = le16(p + i); i += 2;
            f.apsSrcEp = p[i++];
            if (n > i) f.apsCounter = p[i];
        }
        f.hasAps = true;
    }
    if (f.apsSecured) f.encrypted = true;
}

bool parseNwk(Frame& f, const uint8_t* p, size_t n) {
    if (n < 8) return false;
    const int fc = le16(p);
    const int proto = fc >> 2 & 15;
    if (proto != 1 && proto != 2) return false;
    f.nwkType = fc & 3;
    if (f.nwkType == 2) return false;   // reserved
    f.nwkProtocol = proto;
    f.nwkRoute = (fc >> 6 & 3) != 0;
    f.nwkMulticast = (fc >> 8 & 1) != 0;
    f.nwkSecured = (fc >> 9 & 1) != 0;
    f.nwkSourceRoute = (fc >> 10 & 1) != 0;
    f.nwkHasDestExt = (fc >> 11 & 1) != 0;
    f.nwkHasSrcExt = (fc >> 12 & 1) != 0;
    f.nwkDest = le16(p + 2);
    f.nwkSrc = le16(p + 4);
    f.nwkRadius = p[6];
    f.nwkSeq = p[7];
    size_t i = 8;
    if (f.nwkHasDestExt) { if (n < i + 8) return false; f.nwkDestExt = le64(p + i); i += 8; }
    if (f.nwkHasSrcExt) { if (n < i + 8) return false; f.nwkSrcExt = le64(p + i); i += 8; }
    if (f.nwkMulticast) { if (n < i + 1) return false; i += 1; }
    if (f.nwkSourceRoute) {
        if (n < i + 2) return false;
        const size_t cnt = p[i];
        if (n < i + 2 + 2 * cnt) return false;
        f.nwkRelayIndex = p[i + 1];
        for (size_t k = 0; k < cnt; k++) f.nwkRelays.push_back(le16(p + i + 2 + 2 * k));
        i += 2 + 2 * cnt;
    }
    f.hasNwk = true;
    if (f.nwkSecured) {   // auxiliary header: control, frame counter, source address, key sequence number; the rest is encrypted
        f.encrypted = true;
        f.nwkAuxAt = (int)i;
        return true;
    }
    if (f.nwkType == 0) parseAps(f, p + i, n - i);
    else if (f.nwkType == 1 && n > i) f.nwkCommand = p[i];
    return true;
}

void parseBeaconPayload(Frame& f, const uint8_t* p, size_t n) {
    if (n < 15 || p[0] != 0) return;
    f.zbBeacon = true;
    f.protocolId = p[0];
    f.stackProfile = p[1] & 15;
    f.nwkVersion = p[1] >> 4 & 15;
    f.routerCapacity = (p[2] >> 2 & 1) != 0;
    f.depth = p[2] >> 3 & 15;
    f.endDeviceCapacity = (p[2] >> 7 & 1) != 0;
    f.extPanId = le64(p + 3);
    f.nwkUpdateId = p[14];
}

} // namespace

// Zigbee NWK security (ZigBee spec section 4.3, CCM* level 5). The on-air security level bits are zero; nonce and AAD use level 5.
bool decryptNwk(const uint8_t* d, size_t total, Frame& f, const uint8_t key[16], std::vector<uint8_t>& plain) {
    if (f.nwkAuxAt < 0 || f.payloadAt + f.payloadLen > total) return false;
    const uint8_t* nwk = d + f.payloadAt;
    const size_t nLen = f.payloadLen, a = (size_t)f.nwkAuxAt;
    if (nLen < a + 5) return false;
    const uint8_t sc = (uint8_t)((nwk[a] & ~7) | 5);
    const int keyId = (nwk[a] >> 3) & 3;
    const bool extNonce = (nwk[a] >> 5) & 1;
    size_t i = a + 1 + 4;
    if (!extNonce || nLen < i + 8) return false;
    uint8_t nonce[13];
    std::memcpy(nonce, nwk + i, 8);
    i += 8;
    if (keyId == 1) { if (nLen < i + 1) return false; i++; }
    if (nLen < i + 4) return false;
    std::memcpy(nonce + 8, nwk + a + 1, 4);
    nonce[12] = sc;
    std::vector<uint8_t> aad(nwk, nwk + i);
    aad[a] = sc;
    if (!voicekeys::ccmStarDecrypt(key, nonce, aad.data(), aad.size(), nwk + i, nLen - i - 4, 4, plain)) return false;
    f.decrypted = true;
    f.encrypted = false;
    if (f.nwkType == 0) parseAps(f, plain.data(), plain.size());
    char b[160];
    if (f.hasAps) snprintf(b, sizeof b, "NWK decrypted, APS %s cluster 0x%04X, %zu bytes", profileName(f.apsProfile), (unsigned)f.apsCluster, plain.size());
    else snprintf(b, sizeof b, "NWK decrypted, %zu bytes", plain.size());
    f.info = b;
    return true;
}

Frame parse(const uint8_t* d, size_t total) {
    Frame f;
    if (total < 3) return f;
    const size_t n = total >= 2 ? total - 2 : total;   // without the FCS
    const int fc = le16(d);
    f.type = fc & 7;
    f.secured = (fc >> 3 & 1) != 0;
    f.pending = (fc >> 4 & 1) != 0;
    f.ackReq = (fc >> 5 & 1) != 0;
    f.panCompress = (fc >> 6 & 1) != 0;
    f.seqSuppressed = (fc >> 8 & 1) != 0;
    f.ieList = (fc >> 9 & 1) != 0;
    f.destMode = fc >> 10 & 3;
    f.version = fc >> 12 & 3;
    f.srcMode = fc >> 14 & 3;
    if (f.destMode == 1 || f.srcMode == 1 || f.version == 3) { f.info = "reserved addressing"; return f; }
    size_t i = 2;
    if (f.version < 2 || !f.seqSuppressed) { if (n < i + 1) return f; f.seq = d[i++]; }
    // which PAN IDs are present
    bool destPan = false, srcPan = false;
    if (f.version < 2) {
        destPan = f.destMode != 0;
        srcPan = f.srcMode != 0 && !f.panCompress;
        if (f.destMode == 0 && f.srcMode != 0) srcPan = !f.panCompress;
    } else {
        const bool dp = f.destMode != 0, sp = f.srcMode != 0, c = f.panCompress;
        if (!dp && !sp) destPan = c;
        else if (!dp && sp) srcPan = !c;
        else if (dp && !sp) destPan = !c;
        else if (f.destMode == 3 && f.srcMode == 3) destPan = c;
        else { destPan = true; srcPan = !c; }
    }
    if (destPan) { if (n < i + 2) return f; f.destPan = le16(d + i); i += 2; f.hasDestPan = true; }
    if (f.destMode == 2) { if (n < i + 2) return f; f.destShort = le16(d + i); i += 2; }
    else if (f.destMode == 3) { if (n < i + 8) return f; f.destExt = le64(d + i); i += 8; }
    if (srcPan) { if (n < i + 2) return f; f.srcPan = le16(d + i); i += 2; f.hasSrcPan = true; }
    else if (f.srcMode != 0 && f.hasDestPan) { f.srcPan = f.destPan; f.hasSrcPan = true; }
    if (f.srcMode == 2) { if (n < i + 2) return f; f.srcShort = le16(d + i); i += 2; }
    else if (f.srcMode == 3) { if (n < i + 8) return f; f.srcExt = le64(d + i); i += 8; }
    size_t mic = 0;
    if (f.secured) {   // auxiliary security header
        if (n < i + 1) return f;
        const int sc = d[i++];
        const int level = sc & 7, keyMode = sc >> 3 & 3;
        const bool noCounter = f.version >= 2 && (sc >> 5 & 1);
        if (!noCounter) { if (n < i + 4) return f; f.frameCounter = le32(d + i); i += 4; }
        i += keyMode == 0 ? 0 : keyMode == 1 ? 1 : keyMode == 2 ? 5 : 9;
        if (n < i) return f;
        f.secLevel = level; f.secKeyMode = keyMode;
        if (keyMode == 1) f.secKeyIndex = d[i - 1];
        mic = level == 0 ? 0 : (size_t)(4 << ((level & 3) - 1 >= 0 ? (level & 3) - 1 : 0));
        if ((level & 3) == 0) mic = 0;
        f.encrypted = level >= 4;
        if (level == 0) f.encrypted = false;
    }
    if (n < i + mic) return f;
    f.payloadAt = i;
    f.payloadLen = n - i - mic;
    f.ok = true;
    const uint8_t* p = d + i;
    const size_t pl = f.payloadLen;
    char b[200];
    const std::string who = srcText(f) + " > " + dstText(f);
    if (f.type == Ack) { snprintf(b, sizeof b, "Ack #%d", f.seq); f.info = b; return f; }
    if (f.encrypted) {
        snprintf(b, sizeof b, "%s, MAC payload encrypted (%zu bytes)", frameTypeName(f.type), pl);
        f.info = b;
        return f;
    }
    if (f.type == Beacon) {
        size_t k = 0;
        if (pl >= 2) {
            const int sf = le16(p);
            f.superframe = true;
            f.beaconOrder = sf & 15; f.superframeOrder = sf >> 4 & 15;
            f.panCoordinator = (sf >> 14 & 1) != 0;   // bit 14 = PAN coordinator, bit 15 = association permit (2006)
            f.assocPermit = (sf >> 15 & 1) != 0;
            f.coordinator = f.panCoordinator;
            k = 2;
            if (pl >= k + 1) {   // GTS fields
                const int cnt = p[k] & 7;
                k += 1;
                if (cnt) k += 1 + (size_t)cnt * 3;
            }
            if (pl >= k + 1) {   // pending address fields
                const int s = p[k] & 7, e = p[k] >> 4 & 7;
                k += 1 + (size_t)s * 2 + (size_t)e * 8;
            }
            if (k <= pl) parseBeaconPayload(f, p + k, pl - k);
        }
        if (f.zbBeacon) {
            snprintf(b, sizeof b, "Zigbee beacon: PAN 0x%04X, ext PAN %s, stack profile %d%s%s, join %s", f.srcPan, addrText(f.extPanId).c_str(), f.stackProfile,
                     f.routerCapacity ? ", router slots" : "", f.endDeviceCapacity ? ", end-device slots" : "", f.assocPermit ? "open" : "closed");
        } else {
            snprintf(b, sizeof b, "Beacon: PAN 0x%04X%s%s", f.srcPan, f.panCoordinator ? ", PAN coordinator" : "", f.assocPermit ? ", join allowed" : "");
        }
        f.info = b;
        return f;
    }
    if (f.type == Command) {
        if (pl >= 1) f.command = p[0];
        std::string extra;
        if (f.command == 1 && pl >= 2) {
            const int c = p[1];
            snprintf(b, sizeof b, ": %s%s%s", c & 2 ? "full-function" : "reduced-function", c & 4 ? ", mains powered" : ", battery", c & 8 ? ", rx on when idle" : "");
            extra = b;
        } else if (f.command == 2 && pl >= 4) {
            const int st = p[3];
            snprintf(b, sizeof b, ": address 0x%04X, %s", le16(p + 1), st == 0 ? "joined" : st == 1 ? "PAN at capacity" : st == 2 ? "access denied" : "refused");
            extra = b;
        }
        f.info = std::string(macCommandName(f.command)) + extra;
        return f;
    }
    if (f.type == Data) {
        if (pl && parseNwk(f, p, pl)) {
            if (f.encrypted) {
                snprintf(b, sizeof b, "Zigbee NWK %s 0x%04X > 0x%04X, radius %d, seq %d, payload encrypted (network key)", f.nwkType == 1 ? "command" : "data", f.nwkSrc, f.nwkDest, f.nwkRadius, f.nwkSeq);
            } else if (f.nwkType == 1) {
                snprintf(b, sizeof b, "Zigbee NWK %s 0x%04X > 0x%04X, radius %d", nwkCommandName(f.nwkCommand), f.nwkSrc, f.nwkDest, f.nwkRadius);
            } else if (f.hasAps && f.apsType == 0) {
                const char* cn = clusterName(f.apsCluster, f.apsProfile);
                const char* pn = profileName(f.apsProfile);
                snprintf(b, sizeof b, "Zigbee 0x%04X > 0x%04X, APS cluster 0x%04X%s%s%s, profile 0x%04X%s%s%s, ep %d > %d%s", f.nwkSrc, f.nwkDest, f.apsCluster, *cn ? " " : "", cn,
                         "", f.apsProfile, *pn ? " (" : "", pn, *pn ? ")" : "", f.apsSrcEp, f.apsDestEp, f.apsSecured ? ", APS encrypted" : "");
            } else {
                snprintf(b, sizeof b, "Zigbee NWK data 0x%04X > 0x%04X, radius %d, seq %d", f.nwkSrc, f.nwkDest, f.nwkRadius, f.nwkSeq);
            }
            f.info = b;
        } else {
            snprintf(b, sizeof b, "Data, %zu bytes (not Zigbee NWK)", pl);
            f.info = b;
        }
        return f;
    }
    f.info = who;
    return f;
}

// ------------------------------------------------------------------ builders

namespace {
void put16(std::vector<uint8_t>& v, uint16_t x) { dect2::endian::putLe16(v, x); }
void put64(std::vector<uint8_t>& v, uint64_t x) { dect2::endian::putLe64(v, x); }
}

std::vector<uint8_t> buildMac(const MacBuild& b) {
    std::vector<uint8_t> v;
    int fc = b.type & 7;
    if (b.secured) fc |= 1 << 3;
    if (b.pending) fc |= 1 << 4;
    if (b.ackReq) fc |= 1 << 5;
    const bool both = b.destMode != 0 && b.srcMode != 0;
    const bool comp = b.panCompress && both;
    if (comp) fc |= 1 << 6;
    fc |= (b.destMode & 3) << 10 | (b.version & 3) << 12 | (b.srcMode & 3) << 14;
    put16(v, (uint16_t)fc);
    v.push_back(b.seq);
    if (b.destMode) put16(v, b.destPan);
    if (b.destMode == 2) put16(v, b.destShort); else if (b.destMode == 3) put64(v, b.destExt);
    if (b.srcMode && !comp) put16(v, b.srcPan);
    if (b.srcMode == 2) put16(v, b.srcShort); else if (b.srcMode == 3) put64(v, b.srcExt);
    v.insert(v.end(), b.payload.begin(), b.payload.end());
    const uint16_t c = crc16(v.data(), v.size());
    put16(v, c);
    return v;
}

std::vector<uint8_t> zbBeaconPayload(int sp, int ver, bool router, bool endDev, int depth, uint64_t extPan, int upd) {
    std::vector<uint8_t> v = {0, (uint8_t)(sp & 15 | (ver & 15) << 4), (uint8_t)((router ? 4 : 0) | (depth & 15) << 3 | (endDev ? 0x80 : 0))};
    put64(v, extPan);
    v.push_back(0xFF); v.push_back(0xFF); v.push_back(0xFF);   // tx offset
    v.push_back((uint8_t)upd);
    return v;
}

std::vector<uint8_t> buildNwk(const NwkBuild& b) {
    std::vector<uint8_t> v;
    int fc = b.type & 3 | (b.protocol & 15) << 2 | (b.route ? 1 << 6 : 0);
    if (b.secured) fc |= 1 << 9;
    if (b.destExt) fc |= 1 << 11;
    if (b.srcExt) fc |= 1 << 12;
    if (!b.relays.empty()) fc |= 1 << 10;
    put16(v, (uint16_t)fc);
    put16(v, b.dest); put16(v, b.src);
    v.push_back((uint8_t)b.radius); v.push_back((uint8_t)b.seq);
    if (b.destExt) put64(v, b.destExtAddr);
    if (b.srcExt) put64(v, b.srcExtAddr);
    if (!b.relays.empty()) {
        v.push_back((uint8_t)b.relays.size()); v.push_back((uint8_t)b.relayIndex);
        for (uint16_t r : b.relays) put16(v, r);
    }
    v.insert(v.end(), b.payload.begin(), b.payload.end());
    return v;
}

} // namespace zigbee
} // namespace dect2
