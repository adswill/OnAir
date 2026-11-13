// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The data feed formats: SBS-1 BaseStation, Beast binary, KISS, APRS-IS, acarsdec and dumpvdl2 JSON.
// References: the dump1090 / readsb "net_io.c" writers (SBS, Beast), the KISS protocol (Chepponis / Karn 1987),
// aprs-is.net "IGate" and "q Construct" pages, acarsdec's JSON output, dumpvdl2's JSON output (as acarshub reads them).
#include "dect2/net/feeds.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstring>
#include "dect2/common/str.h"

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "dev"
#endif

namespace dect2::feedfmt {

namespace {
std::string trimRight(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '_' || s.back() == '@')) s.pop_back();
    return s;
}
std::string upper(std::string s) {
    for (char& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return s;
}
void addStr(std::string& o, const char* key, const std::string& v, bool comma = true) {
    if (comma) o += ',';
    o += '"'; o += key; o += "\":\""; o += jsonEscape(v); o += '"';
}
void addRaw(std::string& o, const char* key, const std::string& v) {
    o += ",\""; o += key; o += "\":"; o += v;
}
std::string num(double v, const char* fmt) {
    char b[48]; snprintf(b, sizeof b, fmt, v); return b;
}
} // namespace

std::string jsonEscape(const std::string& s) { return dect2::str::jsonEscape7bit(s); }   // the decoders' texts are 7 bit; keep the JSON valid UTF-8 whatever comes

// ---------------------------------------------------------------- ADS-B

std::string sbsLine(const FeedAdsbFrame& f, int64_t wallMs) {
    const adsb::Msg& m = f.msg;
    int type = 0;
    if (f.msg.df == 17 || f.msg.df == 18) {
        const int tc = m.tc;
        if (tc >= 1 && tc <= 4) type = 1;
        else if (tc >= 5 && tc <= 8) type = 2;
        else if ((tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) type = 3;
        else if (tc == 19) type = 4;
    } else if (m.df == 4 || m.df == 20) type = 5;
    else if (m.df == 5 || m.df == 21) type = 6;
    else if (m.df == 0 || m.df == 16) type = 7;
    else if (m.df == 11) type = 8;
    if (!type) return "";

    const time_t secs = (time_t)(wallMs / 1000);
    const int ms = (int)(wallMs % 1000);
    tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &secs);
#else
    localtime_r(&secs, &lt);
#endif
    char when[40];
    snprintf(when, sizeof when, "%04d/%02d/%02d,%02d:%02d:%02d.%03d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, ms);

    std::string call, alt, gs, trk, lat, lon, vr, sq, alert, emerg, spi, gnd;
    if (type == 1 && m.hasIdent) call = trimRight(m.callsign);
    if (m.hasAlt && type != 1 && type != 2 && type != 4) alt = std::to_string(m.altFt);
    if (type == 2) {
        if (m.hasMove) gs = num(m.moveKt, "%.0f");
        if (m.hasTrack) trk = num(m.trackDeg, "%.0f");
    }
    if (type == 4) {
        if (m.hasVel && m.speedKind == 0) gs = num(m.speedKt, "%.0f");
        if (m.hasHeading && m.headingIsTrack) trk = num(m.headingDeg, "%.0f");
        if (m.hasVrate) vr = std::to_string(m.vrateFpm);
    }
    if ((type == 2 || type == 3) && f.hasPos) { lat = num(f.lat, "%.5f"); lon = num(f.lon, "%.5f"); }
    if (type == 6 && m.hasSquawk) { char b[8]; snprintf(b, sizeof b, "%04d", m.squawk); sq = b; }
    if (type == 5 || type == 6) {
        alert = m.alert ? "-1" : "0";
        spi = m.spi ? "-1" : "0";
        emerg = (type == 6 && m.hasSquawk && (m.squawk == 7500 || m.squawk == 7600 || m.squawk == 7700)) ? "-1" : "0";
    }
    gnd = m.ground ? "-1" : "0";

    char hex[12];
    snprintf(hex, sizeof hex, "%s%06X", f.nonIcao ? "~" : "", f.icao & 0xFFFFFF);
    std::string o = "MSG," + std::to_string(type) + ",1,1," + hex + ",1," + when + "," + when;
    for (const std::string* s : {&call, &alt, &gs, &trk, &lat, &lon, &vr, &sq, &alert, &emerg, &spi, &gnd}) { o += ','; o += *s; }
    o += "\r\n";
    return o;
}

uint8_t beastSignal(float levelDbfs) {
    const double a = std::pow(10.0, levelDbfs / 20.0) * 255.0;
    return (uint8_t)std::lround(std::min(255.0, std::max(0.0, a)));
}

std::string beastFrame(const uint8_t* bytes, int bits, uint64_t ts12, uint8_t signal) {
    const int n = bits == 56 ? 7 : 14;
    std::string o;
    o.reserve(2 + 2 * (6 + 1 + n));
    o += (char)0x1A;
    o += n == 7 ? '2' : '3';
    auto put = [&](uint8_t b) { o += (char)b; if (b == 0x1A) o += (char)0x1A; };
    for (int i = 5; i >= 0; i--) put((uint8_t)(ts12 >> (8 * i)));
    put(signal);
    for (int i = 0; i < n; i++) put(bytes[i]);
    return o;
}

// ---------------------------------------------------------------- APRS

std::string kissFrame(const uint8_t* d, size_t n) {
    std::string o;
    o.reserve(n + 8);
    o += (char)0xC0; o += (char)0x00;
    for (size_t i = 0; i < n; i++) {
        if (d[i] == 0xC0) { o += (char)0xDB; o += (char)0xDC; }
        else if (d[i] == 0xDB) { o += (char)0xDB; o += (char)0xDD; }
        else o += (char)d[i];
    }
    o += (char)0xC0;
    return o;
}

std::string aprsIsLine(const std::string& from, const std::string& to, const std::string& path, const std::string& info, const std::string& myCall, bool uiF0) {
    if (!uiF0 || from.empty() || to.empty() || myCall.empty()) return "";
    std::string body = info;
    const size_t cut = body.find_first_of("\r\n");
    if (cut != std::string::npos) body.resize(cut);
    if (body.empty() || body[0] == '}' || body[0] == '?') return "";   // third-party traffic and queries are not gated
    std::string p;
    size_t s = 0;
    while (s <= path.size() && !path.empty()) {
        size_t e = path.find(',', s);
        if (e == std::string::npos) e = path.size();
        std::string tok = path.substr(s, e - s);
        std::string bare = tok;
        if (!bare.empty() && bare.back() == '*') bare.pop_back();
        bare = upper(bare);
        if (bare == "TCPIP" || bare == "TCPXX" || bare == "NOGATE" || bare == "RFONLY" || bare.rfind("TCPIP", 0) == 0) return "";
        if (bare.size() == 3 && bare[0] == 'Q' && bare[1] == 'A') return "";   // already carries a q construct: came from the Internet
        if (!tok.empty()) { p += ','; p += tok; }
        s = e + 1;
        if (e == path.size()) break;
    }
    return from + ">" + to + p + ",qAR," + upper(myCall) + ":" + body + "\r\n";
}

std::string aprsIsLogin(const std::string& call, const std::string& pass) {
    return "user " + upper(call) + " pass " + pass + " vers OnAir " ONAIR_VERSION "\r\n";
}

// ---------------------------------------------------------------- ACARS / VDL2

std::string acarsJson(const AcarsMessage& m, const std::string& station) {
    std::string o = "{\"timestamp\":" + num((double)m.wallTime, "%.3f");
    addStr(o, "station_id", station);
    addRaw(o, "channel", "0");
    addRaw(o, "freq", num(m.freqHz / 1e6, "%.3f"));
    addRaw(o, "level", num(m.levelDb, "%.1f"));
    addRaw(o, "error", std::to_string(m.parityFixed));
    if (m.mode) addStr(o, "mode", std::string(1, m.mode));
    addStr(o, "label", m.label);
    if (m.blockId) addStr(o, "block_id", std::string(1, m.blockId));
    if (!m.ack || m.ack == '!') addRaw(o, "ack", "false");
    else addStr(o, "ack", std::string(1, m.ack));
    if (!m.reg.empty()) addStr(o, "tail", m.reg);
    if (m.downlink) {
        if (!m.flightId.empty()) addStr(o, "flight", m.flightId);
        if (!m.msgNum.empty()) addStr(o, "msgno", m.msgNum + (m.msgSeq ? std::string(1, m.msgSeq) : ""));
    }
    if (!m.text.empty()) addStr(o, "text", m.text);
    if (m.finalBlock) addRaw(o, "end", "true");
    o += ",\"app\":{\"name\":\"OnAir\",\"ver\":\"" ONAIR_VERSION "\"}}";
    return o;
}

std::string vdl2Json(const Vdl2Message& m, const std::string& station) {
    auto addrType = [](int t) -> const char* {
        switch (t) {
        case 1: return "Aircraft";
        case 4: case 5: return "Ground station";
        case 7: return "All stations";
        default: return "reserved";
        }
    };
    auto hex6 = [](uint32_t a) { char b[8]; snprintf(b, sizeof b, "%06X", a & 0xFFFFFF); return std::string(b); };
    std::string o = "{\"vdl2\":{\"app\":{\"name\":\"OnAir\",\"ver\":\"" ONAIR_VERSION "\"}";
    addStr(o, "station", station);
    o += ",\"t\":{\"sec\":" + std::to_string((long long)m.wallTime) + ",\"usec\":0}";
    addRaw(o, "freq", std::to_string((long long)std::llround(m.freqHz)));
    addRaw(o, "hdr_bits_fixed", "0");
    addRaw(o, "octets_corrected_by_fec", std::to_string(m.rsFixed));
    addRaw(o, "idx", "0");
    addRaw(o, "sig_level", num(m.levelDb, "%.1f"));
    addRaw(o, "noise_level", num(m.levelDb - m.snrDb, "%.1f"));
    addRaw(o, "freq_skew", num(m.cfoHz, "%.1f"));
    o += ",\"avlc\":{\"src\":{";
    addStr(o, "addr", hex6(m.src), false);
    addStr(o, "type", addrType(m.srcType));
    if (m.srcType == 1) addStr(o, "status", m.onGround ? "On ground" : "Airborne");
    o += "},\"dst\":{";
    addStr(o, "addr", hex6(m.dst), false);
    addStr(o, "type", addrType(m.dstType));
    o += "}";
    addStr(o, "cr", m.response ? "Response" : "Command");
    addStr(o, "frame_type", m.frameKind ? std::string(1, m.frameKind) : std::string("U"));
    if (m.frameKind == 'S' || m.frameKind == 'U') addStr(o, "cmd", m.frameName);
    if (m.nr >= 0) addRaw(o, "rseq", std::to_string(m.nr));
    if (m.ns >= 0) addRaw(o, "sseq", std::to_string(m.ns));
    addRaw(o, "poll", m.pf ? "true" : "false");
    if (m.acars) {
        o += ",\"acars\":{\"err\":";
        o += m.acarsCrcOk ? "false" : "true";
        addRaw(o, "crc_ok", m.acarsCrcOk ? "true" : "false");
        addRaw(o, "more", "false");
        addStr(o, "reg", m.reg);
        addStr(o, "mode", "2");
        addStr(o, "label", m.label);
        addStr(o, "blk_id", m.blockId ? std::string(1, m.blockId) : std::string(""));
        addStr(o, "ack", "!");
        addStr(o, "flight", m.flight);
        addStr(o, "msg_num", m.msgNum);
        addStr(o, "msg_text", m.acarsText);
        o += "}";
    }
    o += "}}}";
    return o;
}

// ---------------------------------------------------------------- GNSS NMEA, BLE / LoRa JSON lines

namespace {
std::string nmeaLine(const std::string& body) {
    unsigned cs = 0;
    for (unsigned char c : body) cs ^= c;
    char t[8];
    snprintf(t, sizeof t, "*%02X\r\n", cs & 0xFF);
    return "$" + body + t;
}
void latLonNmea(double lat, double lon, std::string& slat, char& ns, std::string& slon, char& ew) {
    ns = lat >= 0 ? 'N' : 'S';
    ew = lon >= 0 ? 'E' : 'W';
    lat = std::fabs(lat); lon = std::fabs(lon);
    const int latd = (int)lat, lond = (int)lon;
    char b[32];
    snprintf(b, sizeof b, "%02d%07.4f", latd, (lat - latd) * 60.0); slat = b;
    snprintf(b, sizeof b, "%03d%07.4f", lond, (lon - lond) * 60.0); slon = b;
}
std::string utcHms(const GnssFix& f) {
    if (!f.timeValid) return "";
    char b[16];
    snprintf(b, sizeof b, "%02d%02d%06.3f", f.hour, f.minute, f.second);
    return b;
}
std::string utcDmy(const GnssFix& f) {
    if (!f.timeValid) return "";
    char b[16];
    snprintf(b, sizeof b, "%02d%02d%02d", f.day, f.month, f.year >= 0 ? f.year % 100 : 0);
    return b;
}
} // namespace

std::string gnssRmc(const GnssFix& f) {
    if (!f.valid) return "";
    std::string slat, slon; char ns = 'N', ew = 'E';
    latLonNmea(f.latDeg, f.lonDeg, slat, ns, slon, ew);
    const double kn = f.hasVelocity ? f.speedMps * 1.943844 : 0;
    const double cog = f.hasVelocity ? f.courseDeg : 0;
    char body[160];
    snprintf(body, sizeof body, "GPRMC,%s,A,%s,%c,%s,%c,%.2f,%.2f,%s,,,A", utcHms(f).c_str(), slat.c_str(), ns, slon.c_str(), ew, kn, cog, utcDmy(f).c_str());
    return nmeaLine(body);
}

std::string gnssGga(const GnssFix& f) {
    if (!f.valid) return "";
    std::string slat, slon; char ns = 'N', ew = 'E';
    latLonNmea(f.latDeg, f.lonDeg, slat, ns, slon, ew);
    char body[192];
    snprintf(body, sizeof body, "GPGGA,%s,%s,%c,%s,%c,1,%02d,%.1f,%.1f,M,0.0,M,,", utcHms(f).c_str(), slat.c_str(), ns, slon.c_str(), ew,
             std::max(0, std::min(12, f.nSats)), f.hdop > 0 ? f.hdop : 99.9, f.heightM);
    return nmeaLine(body);
}

std::string bleJson(const std::string& mac, const std::string& name, float rssi) {
    char r[32];
    snprintf(r, sizeof r, "%.1f", rssi);
    return std::string("{\"type\":\"ble\",\"mac\":\"") + jsonEscape(mac) + "\",\"name\":\"" + jsonEscape(name) + "\",\"rssi\":" + r + "}";
}

std::string loraJson(const char* kind, const std::string& hex, float snrDb, double freqHz, int sf, const std::string& extra) {
    char sn[32], fr[48], sfs[16];
    snprintf(sn, sizeof sn, "%.1f", snrDb);
    snprintf(fr, sizeof fr, "%.0f", freqHz);
    snprintf(sfs, sizeof sfs, "%d", sf);
    std::string o = "{\"type\":\"";
    o += jsonEscape(kind ? kind : "lora");
    o += "\",\"hex\":\"";
    o += jsonEscape(hex);
    o += "\",\"snr\":";
    o += sn;
    o += ",\"freq\":";
    o += fr;
    o += ",\"sf\":";
    o += sfs;
    if (!extra.empty()) { o += ','; o += extra; }
    o += '}';
    return o;
}

} // namespace dect2::feedfmt
