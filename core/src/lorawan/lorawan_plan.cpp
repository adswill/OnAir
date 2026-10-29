// LoRaWAN channel plans and the MAC layer parser (see lorawan_plan.h).
// Facts, all public: LoRaWAN L2 1.0.4 (TS001) chapter 4 (MHDR, FHDR, FCtrl, FPort, MIC, join-request, join-accept, rejoin-request) and
// chapter 5 (MAC commands and their payload sizes, 1.1 additions), LoRaWAN Backend Interfaces 1.0 (TS002) 6.1 (DevAddr types: the leading ones
// of the address choose how many bits the NwkID has), RP002-1.0.x Regional Parameters (channel plans, data rates).
#include "dect2/lorawan/lorawan_plan.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace dect2 {
namespace lorawan {

namespace {
std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char b[160];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
}

// ---------------------------------------------------------------- plans

const char* planName(int plan) {
    static const char* const n[kPlans] = {"EU868", "US915", "AU915", "AS923-1", "AS923-2", "AS923-3", "AS923-4", "EU433"};
    return plan >= 0 && plan < kPlans ? n[plan] : "?";
}

const char* planNote(int plan) {
    switch (plan) {
    case EU868: return "Europe 863 - 870 MHz: join channels 868.1 / 868.3 / 868.5, extra 867.1 - 867.9, SF12 - SF7 at 125 kHz, SF7 at 250 kHz on 868.3, RX2 869.525";
    case US915: return "North America 902 - 928 MHz: 64 uplink channels 902.3 + 0.2 n (SF10 - SF7), 8 at 500 kHz 903.0 + 1.6 n (SF8), gateways answer on 923.3 + 0.6 n at 500 kHz";
    case AU915: return "Australia 915 - 928 MHz: 64 uplink channels 915.2 + 0.2 n, 8 at 500 kHz 915.9 + 1.6 n, gateways answer on 923.3 + 0.6 n at 500 kHz";
    case AS923_1: return "Asia-Pacific AS923-1 (Japan, Singapore, Thailand...): 923.2 / 923.4 default, 923.6 - 924.6 common extras";
    case AS923_2: return "AS923-2 (offset -1.80 MHz from AS923-1): 921.4 / 921.6 default";
    case AS923_3: return "AS923-3 (offset -6.60 MHz from AS923-1): 916.6 / 916.8 default";
    case AS923_4: return "AS923-4 (offset -5.90 MHz from AS923-1): 917.3 / 917.5 default";
    case EU433: return "Europe 433 MHz: 433.175 / 433.375 / 433.575, RX2 434.665";
    }
    return "";
}

std::vector<PlanChannel> planChannels(int plan) {
    std::vector<PlanChannel> v;
    auto add = [&](double mhz, double bw, bool up, bool down, bool dflt, int sb = 0) {
        PlanChannel c; c.mhz = mhz; c.bwKhz = bw; c.uplink = up; c.downlink = down; c.dflt = dflt; c.subBand = sb;
        v.push_back(c);
    };
    switch (plan) {
    case EU868: {
        static const double def[] = {868.1, 868.3, 868.5}, ext[] = {867.1, 867.3, 867.5, 867.7, 867.9};
        for (double f : ext) add(f, 125, true, true, false);
        for (double f : def) add(f, 125, true, true, true);
        add(868.3, 250, true, true, false);   // DR6
        add(869.525, 125, false, true, false);   // RX2
        break;
    }
    case US915:
    case AU915: {
        const double base = plan == US915 ? 902.3 : 915.2, b500 = plan == US915 ? 903.0 : 915.9;
        for (int n = 0; n < 64; n++) add(base + 0.2 * n, 125, true, false, false, n / 8 + 1);
        for (int n = 0; n < 8; n++) add(b500 + 1.6 * n, 500, true, false, false, n + 1);
        for (int n = 0; n < 8; n++) add(923.3 + 0.6 * n, 500, false, true, false, 0);
        break;
    }
    case AS923_1: case AS923_2: case AS923_3: case AS923_4: {
        const double off = plan == AS923_1 ? 0 : plan == AS923_2 ? -1.8 : plan == AS923_3 ? -6.6 : -5.9;
        static const double ext[] = {923.6, 923.8, 924.0, 924.2, 924.4, 924.6};
        add(923.2 + off, 125, true, true, true);
        add(923.4 + off, 125, true, true, true);
        for (double f : ext) add(f + off, 125, true, true, false);
        add(923.2 + off, 250, true, true, false);   // DR6
        break;
    }
    case EU433: {
        add(433.175, 125, true, true, true);
        add(433.375, 125, true, true, true);
        add(433.575, 125, true, true, true);
        add(433.175, 250, true, true, false);   // DR6
        add(434.665, 125, false, true, false);   // RX2
        break;
    }
    }
    std::sort(v.begin(), v.end(), [](const PlanChannel& a, const PlanChannel& b) { return a.mhz != b.mhz ? a.mhz < b.mhz : a.bwKhz < b.bwKhz; });
    return v;
}

std::vector<PlanChannel> selectChannels(int plan, int subBand, bool downlinks) {
    std::vector<PlanChannel> out;
    for (const PlanChannel& c : planChannels(plan)) {
        if (!c.uplink && !downlinks) continue;
        if ((plan == US915 || plan == AU915) && c.uplink && subBand > 0 && c.subBand != subBand) continue;
        out.push_back(c);
    }
    return out;
}

void planSfRange(int plan, double bwKhz, bool downlink, int& lo, int& hi) {
    const bool usau = plan == US915 || plan == AU915;
    if (bwKhz >= 400) { lo = downlink ? 7 : 8; hi = downlink ? 12 : 8; return; }   // US / AU: DR4 SF8 up, DR8 - DR13 SF12 - SF7 down
    if (bwKhz >= 200) { lo = hi = 7; return; }                                       // DR6
    lo = 7; hi = usau ? (downlink ? 12 : 10) : 12;
}

bool planHasFsk(int plan) { return plan == EU868 || plan == EU433 || plan == AS923_1 || plan == AS923_2 || plan == AS923_3 || plan == AS923_4; }

std::vector<double> planFskChannelsMhz(int plan) {
    // TheThingsNetwork/lorawan-frequency-plans: EU_863_870.yml fsk-channel 868800000 (in the 868.7 - 869.2 MHz sub-band, 0.1 % duty cycle),
    // AS_923_925.yml fsk-channel 924800000 (the plan whose channels are AS923-1's 923.2 - 924.6 MHz)
    if (plan == EU868) return {868.8};
    if (plan == AS923_1) return {924.8};
    return {};
}

// ---------------------------------------------------------------- MAC layer

const char* mtypeName(int t) {
    static const char* const n[] = {"Join request", "Join accept", "Unconfirmed up", "Unconfirmed down", "Confirmed up", "Confirmed down", "Rejoin request", "Proprietary"};
    return t >= 0 && t < 8 ? n[t] : "?";
}

std::string eui(uint64_t v) { return fmt("%016llX", (unsigned long long)v); }

std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02X", p[i]); if (i) s += ' '; s += b; }
    return s;
}

std::string macCommandName(uint8_t cid, bool up) {
    if (cid >= 0x80) return fmt("proprietary 0x%02X", cid);
    // TS001 1.0.4 table 4 (1.1 adds ResetInd, RekeyInd, ADRParamSetup, ForceRejoin, RejoinParamSetup)
    switch (cid) {
    case 0x01: return up ? "ResetInd" : "ResetConf";
    case 0x02: return up ? "LinkCheckReq" : "LinkCheckAns";
    case 0x03: return up ? "LinkADRAns" : "LinkADRReq";
    case 0x04: return up ? "DutyCycleAns" : "DutyCycleReq";
    case 0x05: return up ? "RXParamSetupAns" : "RXParamSetupReq";
    case 0x06: return up ? "DevStatusAns" : "DevStatusReq";
    case 0x07: return up ? "NewChannelAns" : "NewChannelReq";
    case 0x08: return up ? "RXTimingSetupAns" : "RXTimingSetupReq";
    case 0x09: return up ? "TxParamSetupAns" : "TxParamSetupReq";
    case 0x0A: return up ? "DlChannelAns" : "DlChannelReq";
    case 0x0B: return up ? "RekeyInd" : "RekeyConf";
    case 0x0C: return up ? "ADRParamSetupAns" : "ADRParamSetupReq";
    case 0x0D: return up ? "DeviceTimeReq" : "DeviceTimeAns";
    case 0x0E: return up ? "CID 0x0E" : "ForceRejoinReq";
    case 0x0F: return up ? "RejoinParamSetupAns" : "RejoinParamSetupReq";
    case 0x10: return up ? "PingSlotInfoReq" : "PingSlotInfoAns";
    case 0x11: return up ? "PingSlotChannelAns" : "PingSlotChannelReq";
    case 0x12: return up ? "BeaconTimingReq" : "BeaconTimingAns";
    case 0x13: return up ? "BeaconFreqAns" : "BeaconFreqReq";
    }
    return fmt("CID 0x%02X", cid);
}

namespace {
// Bytes after the CID, or -1 for a CID this parser does not know the size of
int macLen(uint8_t cid, bool up) {
    switch (cid) {
    case 0x01: return 1;
    case 0x02: return up ? 0 : 2;
    case 0x03: return up ? 1 : 4;
    case 0x04: return up ? 0 : 1;
    case 0x05: return up ? 1 : 4;
    case 0x06: return up ? 2 : 0;
    case 0x07: return up ? 1 : 5;
    case 0x08: return up ? 0 : 1;
    case 0x09: return up ? 0 : 1;
    case 0x0A: return up ? 1 : 4;
    case 0x0B: return 1;
    case 0x0C: return up ? 0 : 1;
    case 0x0D: return up ? 0 : 5;
    case 0x0E: return up ? -1 : 2;
    case 0x0F: return 1;
    case 0x10: return up ? 1 : 0;
    case 0x11: return up ? 1 : 5;
    case 0x12: return up ? 0 : 3;
    case 0x13: return up ? 1 : 3;
    }
    return -1;
}

std::string macDetail(uint8_t cid, bool up, const uint8_t* d, int n) {
    if (cid == 0x06 && up && n == 2) {
        const int margin = ((d[1] & 0x3F) ^ 0x20) - 0x20;
        if (d[0] == 0) return fmt("external power, margin %d dB", margin);
        if (d[0] == 255) return fmt("battery unknown, margin %d dB", margin);
        return fmt("battery %d %%, margin %d dB", (int)std::lround((d[0] - 1) * 100.0 / 253.0), margin);
    }
    if (cid == 0x02 && !up && n == 2) return fmt("margin %d dB, %d gateways", d[0], d[1]);
    if (cid == 0x03 && !up && n == 4) return fmt("DR%d, TXPower %d, mask 0x%04X, NbTrans %d", d[0] >> 4, d[0] & 15, d[1] | d[2] << 8, d[3] & 15);
    if (cid == 0x03 && up && n == 1) return fmt("%s%s%s", d[0] & 4 ? "power ok " : "power rejected ", d[0] & 2 ? "rate ok " : "rate rejected ", d[0] & 1 ? "mask ok" : "mask rejected");
    if (cid == 0x07 && !up && n == 5) return fmt("channel %d, %.4f MHz, DR%d - DR%d", d[0], (d[1] | d[2] << 8 | d[3] << 16) * 100.0 / 1e6, d[4] & 15, d[4] >> 4);
    if (cid == 0x08 && !up && n == 1) return fmt("RX1 delay %d s", (d[0] & 15) == 0 ? 1 : d[0] & 15);
    if (cid == 0x0D && !up && n == 5) return fmt("GPS time %u s", le32(d));
    if (cid == 0x04 && !up && n == 1) return fmt("max duty cycle 1/%d", 1 << (d[0] & 15));
    return "";
}
} // namespace

Frame parse(const uint8_t* p, size_t n) {
    Frame f;
    auto fail = [&](const char* why) { f.ok = false; f.why = why; return f; };
    if (n < 5) return fail("too short");
    f.mtype = p[0] >> 5;
    f.major = p[0] & 3;
    if (((p[0] >> 2) & 7) != 0) return fail("RFU bits of MHDR set");
    if (f.major != 0) return fail("unknown major version");
    f.uplink = f.mtype == 0 || f.mtype == 2 || f.mtype == 4 || f.mtype == 6;
    f.confirmed = f.mtype == 4 || f.mtype == 5;
    if (f.mtype != 7) f.mic = (uint32_t)p[n - 4] << 24 | (uint32_t)p[n - 3] << 16 | (uint32_t)p[n - 2] << 8 | p[n - 1];
    switch (f.mtype) {
    case 0:
        if (n != 23) return fail("join request must be 23 bytes");
        f.joinEui = le64(p + 1); f.devEui = le64(p + 9); f.devNonce = (uint16_t)(p[17] | p[18] << 8);
        f.ok = true;
        f.summary = "JoinEUI " + eui(f.joinEui) + " DevEUI " + eui(f.devEui) + fmt(" DevNonce %u", f.devNonce);
        return f;
    case 1:
        if (n != 17 && n != 33) return fail("join accept must be 17 or 33 bytes");
        f.joinAcceptCfList = n == 33;
        f.ok = true;
        f.summary = fmt("join accept, %zu bytes encrypted%s", n - 1, f.joinAcceptCfList ? " (with a channel list)" : "");
        return f;
    case 6:
        if (n < 19) return fail("rejoin request too short");
        f.rejoinType = p[1];
        if ((f.rejoinType == 0 || f.rejoinType == 2) && n == 19) {
            f.netId = (uint32_t)p[2] | (uint32_t)p[3] << 8 | (uint32_t)p[4] << 16; f.devEui = le64(p + 5); f.devNonce = (uint16_t)(p[13] | p[14] << 8);
        } else if (f.rejoinType == 1 && n == 24) {
            f.joinEui = le64(p + 2); f.devEui = le64(p + 10); f.devNonce = (uint16_t)(p[18] | p[19] << 8);
        } else return fail("rejoin request of the wrong size");
        f.ok = true;
        f.summary = fmt("rejoin type %d, DevEUI ", f.rejoinType) + eui(f.devEui) + fmt(", RJcount %u", f.devNonce);
        return f;
    case 7:
        f.ok = true;
        f.summary = fmt("proprietary, %zu bytes", n - 1);
        f.frmLen = n - 1;
        return f;
    default: break;
    }
    // data frame
    if (n < 12) return fail("data frame too short");
    f.devAddr = le32(p + 1);
    const uint8_t fc = p[5];
    f.adr = fc & 0x80;
    f.ack = fc & 0x20;
    f.foptsLen = fc & 15;
    if (f.uplink) { f.adrAckReq = fc & 0x40; f.classB = fc & 0x10; } else f.fpending = fc & 0x10;
    f.fcnt = (uint16_t)(p[6] | p[7] << 8);
    const size_t fhdrEnd = 8 + (size_t)f.foptsLen;
    if (fhdrEnd > n - 4) return fail("FOpts longer than the frame");
    {   // DevAddr type: the leading ones (TS002 6.1): NwkID bits 6, 6, 9, 11, 12, 13, 15, 17; NwkAddr bits 25, 24, 20, 17, 15, 13, 10, 7
        static const int idBits[8] = {6, 6, 9, 11, 12, 13, 15, 17}, addrBits[8] = {25, 24, 20, 17, 15, 13, 10, 7};
        int t = 0;
        while (t < 7 && (f.devAddr >> (31 - t)) & 1) t++;
        f.addrType = t;
        f.nwkId = (f.devAddr >> addrBits[t]) & ((1u << idBits[t]) - 1);
    }
    // MAC commands in FOpts
    {
        size_t i = 8;
        while (i < fhdrEnd) {
            MacCommand c;
            c.cid = p[i++];
            c.name = macCommandName(c.cid, f.uplink);
            const int len = c.cid >= 0x80 ? -1 : macLen(c.cid, f.uplink);
            if (len < 0 || i + (size_t)len > fhdrEnd) { f.mac.push_back(c); break; }   // unknown size or cut short: the rest cannot be split
            c.data.assign(p + i, p + i + len);
            c.detail = macDetail(c.cid, f.uplink, p + i, len);
            i += (size_t)len;
            f.mac.push_back(std::move(c));
        }
    }
    const size_t micAt = n - 4;
    if (micAt > fhdrEnd) {
        f.fport = p[fhdrEnd];
        f.frmLen = micAt - fhdrEnd - 1;
        f.macInPayload = f.fport == 0;
    }
    std::string fl;
    auto flag = [&](bool on, const char* s) { if (on) { if (!fl.empty()) fl += ", "; fl += s; } };
    flag(f.adr, "ADR"); flag(f.adrAckReq, "ADRAckReq"); flag(f.ack, "ACK"); flag(f.classB, "ClassB"); flag(f.fpending, "FPending");
    f.flags = fl;
    f.ok = true;
    std::string s = fmt("DevAddr %08X FCnt %u", f.devAddr, f.fcnt);
    if (f.fport >= 0) s += fmt(" FPort %d, %zu bytes", f.fport, f.frmLen);
    if (!f.mac.empty()) { s += " ["; for (size_t i = 0; i < f.mac.size(); i++) s += (i ? ", " : "") + f.mac[i].name; s += "]"; }
    f.summary = s;
    return f;
}

// ---------------------------------------------------------------- builders

std::vector<uint8_t> build(const Build& b) {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)((b.mtype & 7) << 5));
    const bool up = b.mtype == 2 || b.mtype == 4;
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(b.devAddr >> (8 * i)));
    const size_t fl = std::min<size_t>(15, b.fopts.size());
    v.push_back((uint8_t)((b.adr ? 0x80 : 0) | (up && b.adrAckReq ? 0x40 : 0) | (b.ack ? 0x20 : 0) | (!up && b.fpending ? 0x10 : 0) | fl));
    v.push_back((uint8_t)(b.fcnt & 0xFF)); v.push_back((uint8_t)(b.fcnt >> 8));
    v.insert(v.end(), b.fopts.begin(), b.fopts.begin() + (long)fl);
    if (b.fport >= 0) { v.push_back((uint8_t)b.fport); v.insert(v.end(), b.frm.begin(), b.frm.end()); }
    for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(b.mic >> (8 * i)));
    return v;
}

std::vector<uint8_t> buildJoinRequest(uint64_t joinEui, uint64_t devEui, uint16_t devNonce, uint32_t mic) {
    std::vector<uint8_t> v{0x00};
    for (int i = 0; i < 8; i++) v.push_back((uint8_t)(joinEui >> (8 * i)));
    for (int i = 0; i < 8; i++) v.push_back((uint8_t)(devEui >> (8 * i)));
    v.push_back((uint8_t)devNonce); v.push_back((uint8_t)(devNonce >> 8));
    for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(mic >> (8 * i)));
    return v;
}

std::vector<uint8_t> buildJoinAccept(size_t totalLen, uint8_t seed) {
    std::vector<uint8_t> v{0x20};
    uint32_t x = seed * 2654435761u + 12345u;
    while (v.size() < totalLen) { x = x * 1664525u + 1013904223u; v.push_back((uint8_t)(x >> 24)); }
    return v;
}

} // namespace lorawan
} // namespace dect2
