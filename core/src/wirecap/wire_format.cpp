// Wireshark output: the byte formats (wire_format.h).
#include "dect2/wirecap/wire_format.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::wire {

namespace {
void le16(Bytes& o, uint32_t v) { o.push_back((uint8_t)v); o.push_back((uint8_t)(v >> 8)); }
void le32(Bytes& o, uint32_t v) { le16(o, v & 0xFFFF); le16(o, v >> 16); }
void be16(Bytes& o, uint32_t v) { o.push_back((uint8_t)(v >> 8)); o.push_back((uint8_t)v); }
void be32(Bytes& o, uint32_t v) { be16(o, v >> 16); be16(o, v & 0xFFFF); }
void put(Bytes& o, const uint8_t* p, size_t n) { if (n) o.insert(o.end(), p, p + n); }
void pad4(Bytes& o) { while (o.size() % 4) o.push_back(0); }

void option(Bytes& o, uint16_t code, const std::string& v) {
    le16(o, code); le16(o, (uint32_t)v.size());
    put(o, (const uint8_t*)v.data(), v.size());
    pad4(o);
}
// a block: type, total length, body (padded to 32 bits), total length
Bytes block(uint32_t type, Bytes body) {
    pad4(body);
    Bytes o;
    o.reserve(body.size() + 12);
    le32(o, type); le32(o, (uint32_t)(body.size() + 12));
    put(o, body.data(), body.size());
    le32(o, (uint32_t)(body.size() + 12));
    return o;
}
} // namespace

Bytes pcapFileHeader(uint32_t linktype, uint32_t snaplen) {
    Bytes o;
    le32(o, 0xA1B2C3D4u); le16(o, 2); le16(o, 4); le32(o, 0); le32(o, 0); le32(o, snaplen); le32(o, linktype);
    return o;
}

void appendPcapRecord(Bytes& o, int64_t t, const uint8_t* p, size_t n) {
    le32(o, (uint32_t)(t / 1000000)); le32(o, (uint32_t)(t % 1000000)); le32(o, (uint32_t)n); le32(o, (uint32_t)n);
    put(o, p, n);
}

Bytes pcapngSectionHeader(const std::string& application) {
    Bytes b;
    le32(b, 0x1A2B3C4Du); le16(b, 1); le16(b, 0);
    le32(b, 0xFFFFFFFFu); le32(b, 0xFFFFFFFFu);          // section length: not given
    option(b, 4, application);                           // shb_userappl
    le32(b, 0);                                          // opt_endofopt
    return block(0x0A0D0D0Au, std::move(b));
}

Bytes pcapngInterface(uint32_t linktype, const std::string& name, const std::string& description, uint32_t snaplen) {
    Bytes b;
    le16(b, linktype); le16(b, 0); le32(b, snaplen);
    option(b, 2, name);                                  // if_name
    option(b, 3, description);                           // if_description
    le32(b, 0);
    return block(1, std::move(b));
}

Bytes pcapngPacket(uint32_t iface, int64_t t, const uint8_t* p, size_t n) {
    Bytes b;
    b.reserve(n + 24);
    le32(b, iface); le32(b, (uint32_t)((uint64_t)t >> 32)); le32(b, (uint32_t)t); le32(b, (uint32_t)n); le32(b, (uint32_t)n);
    put(b, p, n);
    return block(6, std::move(b));
}

Bytes udpInIpv4(uint16_t dstPort, const uint8_t* p, size_t n, uint16_t srcPort) {
    const size_t total = 28 + n;
    Bytes o;
    o.reserve(total);
    o.push_back(0x45); o.push_back(0);
    be16(o, (uint32_t)std::min<size_t>(total, 65535));
    be16(o, 0); be16(o, 0x4000);                         // identification, don't fragment
    o.push_back(64); o.push_back(17);                    // TTL, UDP
    be16(o, 0);                                          // header checksum, below
    be32(o, 0x7F000001u); be32(o, 0x7F000001u);
    uint32_t s = 0;
    for (int i = 0; i < 20; i += 2) s += (uint32_t)o[(size_t)i] << 8 | o[(size_t)i + 1];
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    o[10] = (uint8_t)(~s >> 8); o[11] = (uint8_t)~s;
    be16(o, srcPort); be16(o, dstPort);
    be16(o, (uint32_t)std::min<size_t>(n + 8, 65535));
    be16(o, 0);                                          // checksum: none (allowed in IPv4)
    put(o, p, n);
    return o;
}

Bytes makeGsmtap(uint8_t type, uint8_t subType, const uint8_t* p, size_t n, uint16_t arfcn, uint8_t timeslot, uint32_t fn, int8_t signalDbm, int8_t snrDb) {
    Bytes o;
    o.reserve(16 + n);
    o.push_back(1); o.push_back(4);                      // version, header length in 32-bit words
    o.push_back(type); o.push_back(timeslot);
    be16(o, arfcn);
    o.push_back((uint8_t)signalDbm); o.push_back((uint8_t)snrDb);
    be32(o, fn);
    o.push_back(subType); o.push_back(0); o.push_back(0); o.push_back(0);
    put(o, p, n);
    return o;
}

Bytes packBitsMsbFirst(const uint8_t* bits, size_t n) {
    Bytes o((n + 7) / 8, 0);
    for (size_t i = 0; i < n; i++) if (bits[i] & 1) o[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    return o;
}

Bytes radiotap80211(const uint8_t* f, size_t n, int freqMhz, float dbm) {
    const bool hasFreq = freqMhz > 0, hasDbm = dbm > -127.5f && dbm < 0;
    uint32_t present = 1u << 1;                          // flags
    if (hasFreq) present |= 1u << 3;                     // channel
    if (hasDbm) present |= 1u << 5;                      // antenna signal, dBm
    Bytes body;
    body.push_back(0);                                   // flags: no FCS at the end
    if (hasFreq) {
        body.push_back(0);                               // channel is aligned on 2 bytes (the fields start at offset 8)
        le16(body, (uint32_t)freqMhz);
        le16(body, freqMhz < 3000 ? 0x0080u : 0x0100u);  // 2 GHz or 5 GHz spectrum
    }
    if (hasDbm) body.push_back((uint8_t)(int8_t)std::lround(dbm));
    Bytes o;
    o.reserve(8 + body.size() + n);
    o.push_back(0); o.push_back(0);
    le16(o, (uint32_t)(8 + body.size()));
    le32(o, present);
    put(o, body.data(), body.size());
    put(o, f, n);
    return o;
}

Bytes loraTap(const uint8_t* phy, size_t n, double freqHz, double bwHz, int sf, float snrDb, uint8_t syncWord) {
    Bytes o;
    o.reserve(15 + n);
    o.push_back(0); o.push_back(0);                      // version 0, padding
    be16(o, 15);
    be32(o, freqHz > 0 ? (uint32_t)std::llround(freqHz) : 0);
    o.push_back((uint8_t)std::clamp<long>(std::lround(bwHz / 125000.0), 0, 255));
    o.push_back((uint8_t)std::clamp(sf, 0, 255));
    o.push_back(255); o.push_back(255); o.push_back(255);   // packet, maximum and current RSSI: not known (our level is not calibrated to dBm)
    o.push_back((uint8_t)(int8_t)std::clamp<long>(std::lround(snrDb * 4), -128, 127));   // quarter dB
    o.push_back(syncWord);
    put(o, phy, n);
    return o;
}

Bytes bleLl(const uint8_t* p, size_t n, uint32_t aa) {
    Bytes o;
    o.reserve(4 + n);
    le32(o, aa);
    put(o, p, n);
    return o;
}

Bytes zwaveUser(const uint8_t* mpdu, size_t n, int rate) {
    Bytes o;
    o.reserve(1 + n);
    o.push_back((uint8_t)(rate >= 1 && rate <= 3 ? rate - 1 : 0));
    put(o, mpdu, n);
    return o;
}

const char* luaDissectors() {
    return R"LUA(-- OnAir: Wireshark helpers (written by OnAir, Wireshark window: "Add OnAir to Wireshark").
-- Mode S / ADS-B and Z-Wave have no dissector in Wireshark, so OnAir writes them under private link
-- types (USER0, USER1) and this file reads them. NMEA 0183 has a dissector that is off by default
-- on UDP: this file turns it on for the port OnAir uses for AIS. Described in docs/WIRESHARK-3.8.md.
local user0 = (wtap_encaps and wtap_encaps.USER0) or (wtap and wtap.USER0) or 45
local user1 = (wtap_encaps and wtap_encaps.USER1) or (wtap and wtap.USER1) or 46

-- USER0: a Mode S frame, 7 or 14 bytes, with its parity
local adsb = Proto("onair_modes", "Mode S / ADS-B (OnAir)")
local f_df = ProtoField.uint8("onair_modes.df", "Downlink format", base.DEC, nil, 0xF8)
local f_ca = ProtoField.uint8("onair_modes.ca", "Capability / third field", base.DEC, nil, 0x07)
local f_icao = ProtoField.uint24("onair_modes.icao", "Address (DF 11, 17, 18)", base.HEX)
local f_me = ProtoField.bytes("onair_modes.me", "Message (ME)")
local f_tc = ProtoField.uint8("onair_modes.tc", "Type code", base.DEC, nil, 0xF8)
local f_pi = ProtoField.uint24("onair_modes.parity", "Parity", base.HEX)
adsb.fields = { f_df, f_ca, f_icao, f_me, f_tc, f_pi }
function adsb.dissector(tvb, pinfo, tree)
    local n = tvb:captured_len()
    if n ~= 7 and n ~= 14 then return 0 end
    pinfo.cols.protocol = "Mode S"
    local t = tree:add(adsb, tvb())
    local df = math.floor(tvb(0, 1):uint() / 8)
    t:add(f_df, tvb(0, 1))
    t:add(f_ca, tvb(0, 1))
    if df == 11 or df == 17 or df == 18 then
        t:add(f_icao, tvb(1, 3))
        pinfo.cols.info = string.format("DF %d, %06X", df, tvb(1, 3):uint())
    else
        pinfo.cols.info = string.format("DF %d", df)
    end
    if n == 14 and (df == 17 or df == 18) then
        t:add(f_me, tvb(4, 7))
        t:add(f_tc, tvb(4, 1))
    end
    t:add(f_pi, tvb(n - 3, 3))
    return n
end
DissectorTable.get("wtap_encap"):add(user0, adsb)

-- USER1: one byte for the rate, then a Z-Wave MPDU
local zw = Proto("onair_zwave", "Z-Wave (OnAir)")
local rates = { [0] = "9.6 kbit/s", [1] = "40 kbit/s", [2] = "100 kbit/s" }
local z_rate = ProtoField.uint8("onair_zwave.rate", "Rate", base.DEC, rates)
local z_home = ProtoField.uint32("onair_zwave.home", "Home ID", base.HEX)
local z_src = ProtoField.uint8("onair_zwave.src", "Source node", base.DEC)
local z_rest = ProtoField.bytes("onair_zwave.rest", "Frame control, destination, payload and check (S0 / S2 payloads stay encrypted)")
zw.fields = { z_rate, z_home, z_src, z_rest }
function zw.dissector(tvb, pinfo, tree)
    if tvb:captured_len() < 6 then return 0 end
    pinfo.cols.protocol = "Z-Wave"
    local t = tree:add(zw, tvb())
    t:add(z_rate, tvb(0, 1))
    t:add(z_home, tvb(1, 4))
    t:add(z_src, tvb(5, 1))
    if tvb:captured_len() > 6 then t:add(z_rest, tvb(6)) end
    pinfo.cols.info = string.format("Home %08X, node %d", tvb(1, 4):uint(), tvb(5, 1):uint())
    return tvb:captured_len()
end
DissectorTable.get("wtap_encap"):add(user1, zw)

-- AIS: NMEA sentences in UDP, port 10110
local nmea = Dissector.get("nmea0183")
if nmea then DissectorTable.get("udp.port"):add(10110, nmea) end
)LUA";
}

} // namespace dect2::wire
