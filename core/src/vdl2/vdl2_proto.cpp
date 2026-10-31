// VDL Mode 2 bit layer, AVLC and the protocols above it: see vdl2_proto.h.
#include "dect2/vdl2/vdl2_proto.h"
#include "vdl2_internal.h"
#include "dect2/acars/acars_proto.h"
#include "dect2/aero/aero_adsc.h"
#include "dect2/aero/aero_pos.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "dect2/common/str.h"

namespace dect2 {

const int kVdl2PreamblePhase[kVdl2PreambleSyms] = {0, 3, -3, 1, 1, 2, 0, 4, -3, 4, -2, 3, 1, -2, -3, 0};
const uint8_t kVdl2GrayBits[8] = {0, 1, 3, 2, 6, 7, 5, 4};

int vdl2StepForBits(int b) {
    for (int i = 0; i < 8; i++) if (kVdl2GrayBits[i] == (b & 7)) return i;
    return 0;
}

// ---------------------------------------------------------------- scrambler, header

// ---------------------------------------------------------------- Reed-Solomon


// ---------------------------------------------------------------- burst body

namespace {
void putOctet(std::vector<uint8_t>& bits, uint8_t v) { for (int b = 0; b < 8; b++) bits.push_back((v >> b) & 1); }
} // namespace

std::vector<uint8_t> vdl2BurstBits(const std::vector<uint8_t>& dataBits) {
    const uint32_t len = (uint32_t)dataBits.size();
    Vdl2Layout l;
    std::vector<uint8_t> out;
    if (!vdl2Layout(len, l)) return out;
    std::vector<uint8_t> data(l.dataOct, 0);
    for (size_t i = 0; i < dataBits.size(); i++) data[i / 8] |= (uint8_t)((dataBits[i] & 1) << (i % 8));
    // blocks (rows), data row by row
    std::vector<std::vector<uint8_t>> rows(l.blocks, std::vector<uint8_t>(kVdl2RsN, 0));
    for (int i = 0; i < l.dataOct; i++) rows[i / kVdl2RsK][i % kVdl2RsK] = data[i];
    for (int r = 0; r < l.blocks; r++) vdl2RsEncode(rows[r].data(), &rows[r][kVdl2RsK]);
    const uint32_t hw = vdl2HeaderWord(len);
    for (int b = 24; b >= 0; b--) out.push_back((hw >> b) & 1);
    for (int c = 0; c < kVdl2RsK; c++)
        for (int r = 0; r < l.blocks; r++) {
            if (r == l.blocks - 1 && c >= l.lastLen) continue;
            putOctet(out, rows[r][c]);
        }
    const int fecRows = l.lastFec ? l.blocks : l.blocks - 1;
    for (int c = 0; c < kVdl2RsN - kVdl2RsK; c++)
        for (int r = 0; r < fecRows; r++) {
            if (r == l.blocks - 1 && c >= l.lastFec) continue;
            putOctet(out, rows[r][kVdl2RsK + c]);
        }
    vdl2Scramble(out, 0);
    return out;
}


// ---------------------------------------------------------------- HDLC


std::vector<uint8_t> vdl2HdlcBits(const std::vector<std::vector<uint8_t>>& frames) {
    std::vector<uint8_t> out;
    auto flag = [&] { putOctet(out, 0x7E); };
    flag();
    for (const auto& f : frames) {
        int ones = 0;
        for (uint8_t v : f)
            for (int b = 0; b < 8; b++) {
                const int bit = (v >> b) & 1;
                out.push_back((uint8_t)bit);
                if (bit) { if (++ones == 5) { out.push_back(0); ones = 0; } } else ones = 0;
            }
        flag();
    }
    return out;
}


// ---------------------------------------------------------------- AVLC


uint8_t vdl2ControlI(int ns, int nr, bool p) { return (uint8_t)(((ns & 7) << 1) | (p ? 0x10 : 0) | ((nr & 7) << 5)); }

std::vector<uint8_t> vdl2Frame(const Vdl2Addr& dst, const Vdl2Addr& src, uint8_t control, const std::vector<uint8_t>& info) {
    std::vector<uint8_t> f(9);
    vdl2PutAddr(&f[0], dst, false);
    vdl2PutAddr(&f[4], src, true);
    f[8] = control;
    f.insert(f.end(), info.begin(), info.end());
    const uint16_t c = vdl2Fcs(f.data(), f.size());
    f.push_back((uint8_t)(c & 0xFF)); f.push_back((uint8_t)(c >> 8));
    return f;
}

// ---- protocol readers ----
namespace {


void addLine(std::string& d, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void addLine(std::string& d, const char* fmt, ...) {
    char b[400];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    d += b; d += '\n';
}


void analyseAcarsText(Vdl2Message& m, const std::string& label, const std::string& text) {
    const bool uplink = !m.fromAir;
    AeroAdscMessage adsc;
    if (aeroDecodeAdsc(text, uplink, adsc)) {
        const std::string adscText = aeroDescribeAdsc(adsc);
        if (!adscText.empty()) {
            addLine(m.detail, "ADS-C (FANS-1/A):");
            m.detail += adscText;
            if (m.detail.back() != '\n') m.detail += '\n';
        }
        if (adsc.crcOk) {
            if (adsc.hasEarth && adsc.trackValid) {
                m.hasKin = true;
                m.kinTrackDeg = adsc.trackDeg;
                m.groundKt = adsc.groundKt;
                m.vsFpm = adsc.vsFpm;
            }
            if (adsc.hasPos) {
                m.hasPos = true;
                m.lat = adsc.lat;
                m.lon = adsc.lon;
                m.hasAlt = true;
                m.altFt = adsc.altFt;
                m.posKind = std::string("ADS-C ") + aeroAdscTagName(adsc.posTag);
            }
            if (!adsc.flightId.empty() && m.flight.empty()) m.flight = adsc.flightId;
        }
        if (adsc.found) m.summary = "ACARS " + label + " ADS-C" + (m.hasPos ? " position report" : m.hasKin ? " earth reference" : "");
    }
    if (!m.hasPos) {
        AeroPosition pos;
        if (aeroParseTextPosition(label, text, pos) && pos.valid) {
            m.hasPos = true;
            m.lat = pos.lat;
            m.lon = pos.lon;
            m.hasAlt = pos.hasAlt;
            m.altFt = pos.altFt;
            m.posKind = pos.kind;
            if (!pos.flightId.empty() && m.flight.empty()) m.flight = pos.flightId;
            if (pos.hasTrack && pos.hasSpeed) {
                m.hasKin = true;
                m.kinTrackDeg = pos.trackDeg;
                m.groundKt = pos.speedKt;
            }
        }
    }
    // FANS-1/A CPDLC (ARINC 622 IMI AT1 / CR1 / CC1 / DR1): found and named, the ASN.1 PER body is not decoded
    for (const char* imi : {".AT1", ".CR1", ".CC1", ".DR1"}) {
        const size_t at = text.find(imi);
        if (at != std::string::npos) {
            const std::string what = imi[1] == 'A' ? "CPDLC message" : imi[1] == 'C' && imi[2] == 'R' ? "CPDLC connect request" : imi[2] == 'C' ? "CPDLC connect confirm" : "CPDLC disconnect";
            addLine(m.detail, "FANS-1/A %s (IMI %s), ground facility %s, %zu hex digits of ASN.1 PER (not decoded)", what.c_str(), imi + 1, text.substr(at > 7 ? at - 7 : 0, at > 7 ? 7 : at).c_str(),
                    text.size() - at - 4);
            m.summary = "ACARS " + label + " FANS-1/A " + what;
            break;
        }
    }
    if (m.hasPos) addLine(m.detail, "  position %.4f %.4f%s (%s)", m.lat, m.lon, m.hasAlt ? (" " + std::to_string(m.altFt) + " ft").c_str() : "", m.posKind.c_str());
}

void readAcars(const uint8_t* p, size_t n, Vdl2Message& m) {
    // p: from the SOH; mode .. suffix, check bytes, an optional DEL
    m.proto = "ACARS";
    m.acars = true;
    size_t end = n;
    if (end && p[end - 1] == 0x7F) end--;
    if (end < 1 + 13 + 2) { m.summary = "ACARS: cut short"; addLine(m.detail, "ACARS: %zu octets, too short", n); return; }
    const uint8_t* blk = p + 1;
    const size_t bl = end - 1 - 2;                     // mode .. suffix
    m.acarsCrcOk = acarsCrc(blk, bl + 2) == 0;
    std::vector<uint8_t> t7(bl);
    for (size_t i = 0; i < bl; i++) t7[i] = blk[i] & 0x7F;
    AcarsMessage a;
    if (!acarsParseBlock(t7.data(), (int)bl, a)) { m.summary = "ACARS: not a valid block"; addLine(m.detail, "ACARS: the block layout is not valid"); return; }
    m.acarsFinal = a.finalBlock; m.acarsSeq = a.msgSeq; m.downlinkBlock = a.downlink;
    m.reg = a.reg; m.flight = a.flightId; m.label = a.label; m.labelText = a.labelText; m.msgNum = a.msgNum; m.acarsText = a.text; m.blockId = a.blockId;
    std::string s = "ACARS " + a.label;
    if (!a.labelText.empty()) s += " (" + a.labelText + ")";
    if (!a.flightId.empty()) s += " " + a.flightId;
    std::string one;
    for (char c : a.text) one += (c == '\n' || c == '\r') ? ' ' : c;
    if (!one.empty()) s += ": " + (one.size() > 80 ? one.substr(0, 80) + "..." : one);
    m.summary = s;
    addLine(m.detail, "ACARS: mode %c, registration %s, label %s%s%s, block id %c%s", a.mode ? a.mode : '-', a.reg.empty() ? "-" : a.reg.c_str(), a.label.c_str(),
            a.labelText.empty() ? "" : " ", a.labelText.c_str(), a.blockId ? a.blockId : '-', m.acarsCrcOk ? "" : ", check sequence FAILED");
    if (!a.msgNum.empty() || !a.flightId.empty()) addLine(m.detail, "  message %s, flight %s", a.msgNum.empty() ? "-" : a.msgNum.c_str(), a.flightId.empty() ? "-" : a.flightId.c_str());
    if (!a.decoded.empty()) addLine(m.detail, "  reading: %s", a.decoded.c_str());
    analyseAcarsText(m, a.label, a.text);
}

} // namespace

// The ACARS reader (ARINC 618 block, ADS-C, position text) is not part of the Rust crate; the Rust-backed vdl2ParseFrame calls it through this.
void vdl2ReanalyseJoined(Vdl2Message& m) {
    m.hasPos = false; m.hasKin = false; m.hasAlt = false; m.posKind.clear();
    addLine(m.detail, "Joined message of %d blocks%s:", m.acarsBlocks, m.acarsIncomplete ? " (the last block did not arrive)" : "");
    std::string one;
    for (char c : m.acarsText) one += (c == '\n' || c == '\r') ? ' ' : c;
    std::string s = "ACARS " + m.label;
    if (!m.labelText.empty()) s += " (" + m.labelText + ")";
    if (!m.flight.empty()) s += " " + m.flight;
    if (!one.empty()) s += ": " + (one.size() > 80 ? one.substr(0, 80) + "..." : one);
    m.summary = s;
    analyseAcarsText(m, m.label, m.acarsText);
}
void vdl2ReadAcars(const uint8_t* p, size_t n, Vdl2Message& m) { readAcars(p, n, m); }


// ---------------------------------------------------------------- builders

std::vector<uint8_t> vdl2AcarsInfo(const std::string& reg, const std::string& label, char blockId, const std::string& text, char ack, char mode, bool lastBlock) {
    AcarsBlockSpec s;
    s.lastBlock = lastBlock;
    s.mode = mode;
    s.reg = reg;
    s.ack = (uint8_t)ack;
    s.label[0] = label.size() > 0 ? label[0] : 'H';
    s.label[1] = label.size() > 1 ? label[1] : '1';
    s.blockId = blockId;
    s.text = text;
    s.hasText = !text.empty();
    std::vector<uint8_t> f = acarsBuildFrame(s, 0);    // SYN SYN SOH ... DEL
    std::vector<uint8_t> out = {0xFF, 0xFF};
    out.insert(out.end(), f.begin() + 2, f.end());
    return out;
}

std::vector<uint8_t> vdl2X25ClnpData(int lcn, int ps, int pr, const std::vector<uint8_t>& src, const std::vector<uint8_t>& dst, const std::vector<uint8_t>& user) {
    std::vector<uint8_t> o = {(uint8_t)(0x10 | ((lcn >> 8) & 0xF)), (uint8_t)(lcn & 0xFF), (uint8_t)(((pr & 7) << 5) | ((ps & 7) << 1))};
    std::vector<uint8_t> h = {0x81, 0, 0x01, 60, 0x1C, 0, 0, 0, 0};
    h.push_back((uint8_t)dst.size()); h.insert(h.end(), dst.begin(), dst.end());
    h.push_back((uint8_t)src.size()); h.insert(h.end(), src.begin(), src.end());
    h[1] = (uint8_t)h.size();
    std::vector<uint8_t> tp = {2, 0xF0, 0x80};          // COTP DT, end of TSDU, sequence 0
    tp.insert(tp.end(), user.begin(), user.end());
    const size_t seg = h.size() + tp.size();
    h[5] = (uint8_t)(seg >> 8); h[6] = (uint8_t)seg;
    o.insert(o.end(), h.begin(), h.end());
    o.insert(o.end(), tp.begin(), tp.end());
    return o;
}

std::vector<uint8_t> vdl2X25CallRequest(int lcn, const std::string& called, const std::string& calling) {
    std::vector<uint8_t> o = {(uint8_t)(0x10 | ((lcn >> 8) & 0xF)), (uint8_t)(lcn & 0xFF), 0x0B, (uint8_t)((calling.size() << 4) | (called.size() & 0xF))};
    const std::string all = called + calling;
    for (size_t i = 0; i < all.size(); i += 2) {
        const int hi = all[i] - '0', lo = i + 1 < all.size() ? all[i + 1] - '0' : 0;
        o.push_back((uint8_t)((hi << 4) | lo));
    }
    o.push_back(0);                                     // no facilities
    o.push_back(0xC1); o.push_back(0x00);               // ICAO SNDCF, no compression options
    return o;
}

namespace {
void putLoc(std::vector<uint8_t>& g, double lat, double lon) {
    const int la = (int)std::lround(lat * 10) & 0xFFF, lo = (int)std::lround(lon * 10) & 0xFFF;
    g.push_back((uint8_t)(la >> 4)); g.push_back((uint8_t)(((la & 0xF) << 4) | (lo >> 8))); g.push_back((uint8_t)(lo & 0xFF));
}
std::vector<uint8_t> xid(const std::vector<uint8_t>& vdlParams) {
    std::vector<uint8_t> o = {0x82, 0x80, 0x00, 0x03, 0x01, 0x01, 0x00};   // public group: parameter set id
    o.push_back(0xF0); o.push_back((uint8_t)(vdlParams.size() >> 8)); o.push_back((uint8_t)vdlParams.size());
    o.insert(o.end(), vdlParams.begin(), vdlParams.end());
    return o;
}
void param(std::vector<uint8_t>& g, uint8_t id, const std::vector<uint8_t>& v) { g.push_back(id); g.push_back((uint8_t)v.size()); g.insert(g.end(), v.begin(), v.end()); }
} // namespace

std::vector<uint8_t> vdl2XidGsif(double lat, double lon, const std::string& airports) {
    std::vector<uint8_t> g;
    param(g, 0x00, {'V', '2'});
    std::vector<uint8_t> loc; putLoc(loc, lat, lon);
    param(g, 0xC8, loc);
    param(g, 0xC1, std::vector<uint8_t>(airports.begin(), airports.end()));
    return xid(g);
}

std::vector<uint8_t> vdl2XidAircraftLe(double lat, double lon, int altFt, const std::string& dest) {
    std::vector<uint8_t> g;
    param(g, 0x00, {'V', '2'});
    param(g, 0x01, {0x00});
    std::vector<uint8_t> loc; putLoc(loc, lat, lon); loc.push_back((uint8_t)std::max(0, std::min(255, (altFt + 500) / 1000)));
    param(g, 0x84, loc);
    param(g, 0x83, std::vector<uint8_t>(dest.begin(), dest.end()));
    return xid(g);
}

} // namespace dect2
