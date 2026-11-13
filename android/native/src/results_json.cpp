// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Telemetry of the active mode as JSON for the Kotlin screens (cards + tables). Modes with a dedicated exporter fill tables with the desktop's
// column names; every other mode gets the common header, its one-line summary and its decoded lists as text (genericResults).
#include "results_json.h"
#include "dect2/net/remote_api.h"
#include "dect2/engine/modes.h"
#include "dect2/results/rx_json.h"
#include "dect2/adsb/adsb_tel.h"
#include "dect2/ais/ais_tel.h"
#include "dect2/acars/acars_tel.h"
#include "dect2/pager/pager_tel.h"
#include "dect2/packet/packet_tel.h"
#include "dect2/general/general_tel.h"
#include "dect2/fm/fm_tel.h"
#include "dect2/cdr/cdr_tel.h"
#include "dect2/dtmb/dtmb_tel.h"
#include "dect2/engine/engine.h"
#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <cmath>
#include <type_traits>
#include <vector>

namespace onair {
using namespace dect2;

namespace {
std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap);
    return b;
}
std::string q(const std::string& s) { return '"' + jsonEscape(s) + '"'; }

struct Out {
    std::string cards, tables;
    void card(const std::string& k, const std::string& v) { if (!cards.empty()) cards += ','; cards += "[" + q(k) + "," + q(v) + "]"; }
};
struct Table {
    std::string cols, rows;
    int n = 0;
    explicit Table(std::initializer_list<const char*> c) { for (const char* x : c) { if (!cols.empty()) cols += ','; cols += q(x); } }
    void row(std::initializer_list<std::string> r) {
        if (n++) rows += ',';
        rows += '[';
        bool first = true;
        for (const std::string& x : r) { if (!first) rows += ','; first = false; rows += q(x); }
        rows += ']';
    }
    std::string json(const char* name) const { return "{\"name\":" + q(name) + ",\"cols\":[" + cols + "],\"rows\":[" + rows + "]}"; }
};
void addTable(Out& o, const char* name, const Table& t) { if (!o.tables.empty()) o.tables += ','; o.tables += t.json(name); }

std::string hhmmss(int64_t wall) {
    time_t t = (time_t)wall; struct tm tm; gmtime_r(&t, &tm);
    return fmt("%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}
std::string ago(double s) { return s < 90 ? fmt("%.0f s", s) : s < 5400 ? fmt("%.0f min", s / 60) : fmt("%.1f h", s / 3600); }
std::string num(double v, const char* f) { return fmt(f, v); }

void adsb(Out& o, const AdsbTelemetry& a) {
    o.card("Aircraft", std::to_string(a.aircraftCount));
    o.card("With position", std::to_string(a.withPosition));
    o.card("Messages per second", fmt("%.1f", a.msgsPerSec));
    o.card("Level", fmt("%.1f dBFS", a.levelDbfs));
    o.card("Noise", fmt("%.1f dBFS", a.noiseDbfs));
    if (a.maxRangeNm > 0) o.card("Max range", fmt("%.0f nm", a.maxRangeNm));
    Table t({"ICAO", "Callsign", "Alt ft", "Spd kt", "Hdg", "VS fpm", "Lat", "Lon", "Dist nm", "Sqk", "Msgs", "Age s"});
    int n = 0;
    for (const AdsbAircraft& x : a.aircraft) {
        if (++n > 120) break;
        t.row({fmt("%06X", x.icao & 0xFFFFFF), x.callsign,
               x.hasAlt ? std::to_string(x.altFt) : "", x.hasSpeed ? fmt("%.0f", x.speedKt) : "", x.hasHeading ? fmt("%.0f", x.headingDeg) : "",
               x.hasVrate ? std::to_string(x.vrateFpm) : "", x.hasPos ? fmt("%.4f", x.lat) : "", x.hasPos ? fmt("%.4f", x.lon) : "",
               x.hasRange ? fmt("%.0f", x.distNm) : "", x.hasSquawk ? fmt("%04d", x.squawk) : "", std::to_string(x.messages), fmt("%.0f", x.ageSec)});
    }
    addTable(o, "Aircraft", t);
}

void ais(Out& o, const AisTelemetry& a) {
    o.card("Stations", std::to_string(a.vesselCount));
    o.card("AIS 1 per minute", fmt("%.1f", a.burstsPerMin[0]));
    o.card("AIS 2 per minute", fmt("%.1f", a.burstsPerMin[1]));
    Table t({"Name", "MMSI", "Type", "SOG kn", "Heard", "Pos", "Ch"});
    int n = 0;
    for (const AisVessel& v : a.vessels) {
        if (++n > 150) break;
        t.row({v.name.empty() ? v.callsign : v.name, std::to_string(v.mmsi), v.shipType >= 0 ? aisShipTypeText(v.shipType) : "",
               v.sog >= 0 ? fmt("%.1f", v.sog) : "", ago(v.ageSec), v.hasPos ? fmt("%.4f, %.4f", v.lat, v.lon) : "", std::string(1, v.channel)});
    }
    addTable(o, "Vessels", t);
}

void acars(Out& o, const AcarsTelemetry& a) {
    o.card("Aircraft", std::to_string(a.aircraft.size()));
    o.card("Messages", std::to_string(a.messages.size()));
    Table m({"Time UTC", "MHz", "Reg", "Flight", "Label", "Meaning", "Text"});
    int n = 0;
    for (const AcarsMessage& x : a.messages) {
        if (++n > 100) break;
        m.row({hhmmss(x.wallTime), fmt("%.3f", x.freqHz / 1e6), x.reg, x.flightId, x.label, x.labelText, x.text});
    }
    addTable(o, "Messages", m);
    Table ac({"Registration", "Flight", "Blocks", "Last label", "MHz", "Level dB", "Position", "Alt ft"});
    n = 0;
    for (const AcarsAircraft& x : a.aircraft) {
        if (++n > 60) break;
        ac.row({x.reg, x.flight, std::to_string(x.messages), x.lastLabel, fmt("%.3f", x.freqHz / 1e6), fmt("%.0f", x.levelDb),
                x.hasPos ? fmt("%.3f, %.3f", x.lat, x.lon) : "", x.hasAlt ? std::to_string(x.altFt) : ""});
    }
    addTable(o, "Aircraft", ac);
}

void pager(Out& o, const PagerTelemetry& p) {
    o.card("Messages", std::to_string(p.messagesTotal));
    if (p.lastSpeed >= 0) o.card("Last lock", pagerSpeedName(p.lastSpeed));
    o.card("Level", fmt("%.1f dBFS", p.levelDb));
    Table t({"Time UTC", "Protocol", "Address", "Fn", "Type", "Message"});
    if (p.messages) {
        int n = 0;
        for (const PagerMessage& m : *p.messages) {
            if (++n > 100) break;
            t.row({hhmmss(m.wallTime), pagerSpeedName(m.speed), std::to_string(m.address), m.function >= 0 ? std::to_string(m.function) : "", pagerTypeName(m.type), m.text});
        }
    }
    addTable(o, "Messages", t);
}

void packet(Out& o, const PacketTelemetry& p) {
    o.card("Frames", std::to_string(p.blocksOk));
    o.card("APRS frames", std::to_string(p.aprsFrames));
    o.card("Stations", std::to_string(p.stations.size()));
    Table st({"Callsign", "Symbol", "Last heard", "Position", "Comment"});
    int n = 0;
    for (const PacketStation& s : p.stations) {
        if (++n > 100) break;
        st.row({s.call, std::string(1, s.symTable ? s.symTable : ' ') + std::string(1, s.symCode ? s.symCode : ' '), fmt("%.0f s", std::max(0.0, p.timeSec - s.lastHeardSec)),
                s.hasPos ? fmt("%.4f, %.4f", s.lat, s.lon) : "", s.comment});
    }
    addTable(o, "Stations", st);
    Table fr({"Time", "From > To,Path", "Info"});
    n = 0;
    for (auto it = p.frames.rbegin(); it != p.frames.rend() && n < 60; ++it, ++n)
        fr.row({fmt("%.0f s", it->timeSec), it->from + " > " + it->to + (it->path.empty() ? "" : "," + it->path), it->summary.empty() ? it->info : it->summary});
    addTable(o, "Frames", fr);
}

const char* const kDemod[] = {"NFM", "WFM", "AM", "USB", "LSB", "CW"};
void general(Out& o, const GeneralTelemetry& g) {
    o.card("Demodulator", g.demod >= 0 && g.demod < 6 ? kDemod[g.demod] : "?");
    o.card("Squelch", g.squelchOpen ? "open" : "closed");
    o.card("Channel level", fmt("%.1f dBFS", g.levelDb));
    if (g.noiseDb > -199) o.card("Noise", fmt("%.1f dBFS", g.noiseDb));
    o.card("Audio", fmt("%.1f dBFS", g.audioDb));
}
void fm(Out& o, const FmTelemetry& f) {
    o.card("Sound", f.stereo ? "Stereo" : "Mono");
    o.card("Deviation", fmt("%.1f kHz", f.devKhz));
    o.card("Level", fmt("%.1f dBFS", f.levelDbfs));
    if (f.pilotPct > 0) o.card("Pilot", fmt("%.1f %%", f.pilotPct));
    o.card("RDS", f.rdsSync ? fmt("%.0f %% blocks ok", f.rdsBlockOkPct) : "no sync");
    if (!f.ptyText.empty()) o.card("Programme type", f.ptyText);
    if (f.trafficAlert) o.card("Traffic", "announcement");
}

// ---- exporters added for the voice networks, aviation/satellite data links, DAB, SAME, BLE, ISM, mesh and the HF digital family ----
std::string idOrQ(int64_t v) { return v < 0 ? std::string("?") : std::to_string(v); }
std::string yn(bool b) { return b ? "yes" : ""; }
void common(Out& o, int state, float snr, float level, double cfo, uint64_t ok, uint64_t bad) {
    o.card("State", state >= 2 ? "locked" : state == 1 ? "partly locked" : "searching");
    if (snr != 0) o.card("SNR", fmt("%.1f dB", snr));
    if (level > -199) o.card("Level", fmt("%.1f dBFS", level));
    o.card("CFO", fmt("%+.0f Hz", cfo));
    o.card("Blocks ok / bad", std::to_string(ok) + " / " + std::to_string(bad));
}
void eventsTable(Out& o, const std::vector<std::string>& ev, const char* name = "Events") {
    Table t({"Event"});
    int n = 0;
    for (const std::string& e : ev) { if (++n > 80) break; t.row({e}); }
    if (n) addTable(o, name, t);
}
// the last lines of a text stream as a one-column table
void textTable(Out& o, const char* name, const std::string& s, int maxLines = 40) {
    if (s.empty()) return;
    std::vector<std::string> lines;
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        std::string ln = s.substr(p, e - p);
        while (ln.size() > 90) { lines.push_back(ln.substr(0, 90)); ln = ln.substr(90); }
        if (!ln.empty()) lines.push_back(ln);
        p = e + 1;
    }
    Table t({"Text"});
    size_t from = lines.size() > (size_t)maxLines ? lines.size() - maxLines : 0;
    for (size_t i = lines.size(); i-- > from;) t.row({lines[i]});
    addTable(o, name, t);
}

void dmr(Out& o, const DmrTelemetry& d) {
    static const char* const kind[] = {"Group voice", "Private voice", "All call", "Data", "Control"};
    static const char* const st[5] = {"-", "voice", "data", "idle", "control"};
    common(o, d.state, d.snrDb, d.levelDbfs, d.cfoHz, d.blocksOk, d.blocksBad);
    if (!d.link.empty()) o.card("Link", d.link);
    if (d.cc >= 0) o.card("Colour code", std::to_string(d.cc));
    o.card("Slot 1 / 2", std::string(st[d.slot[0].state % 5]) + " / " + st[d.slot[1].state % 5]);
    o.card("BER", fmt("%.2f %%", d.ber * 100));
    o.card("Calls heard", std::to_string(d.calls));
    o.card("Voice decoder", d.ambe ? "available" : "not installed (silent)");
    if (!d.cachInfo.empty()) o.card("CACH", d.cachInfo);
    Table t({"Slot", "Kind", "Source", "Target", "CC", "Seconds", "Voice frames", "FEC errors", "Flags", "Alias / note"});
    int n = 0;
    for (auto it = d.callLog.rbegin(); it != d.callLog.rend() && n < 80; ++it, ++n) {
        const DmrCall& c = *it;
        std::string fl = std::string(c.active ? "ON AIR " : "") + (c.emergency ? "EMERG " : "") + (c.privacy ? "PRIV " : "") + (c.lateEntry ? "late " : "");
        t.row({c.slot ? std::to_string(c.slot) : "", kind[c.kind % 5], c.idsKnown ? std::to_string(c.src) : "?", c.idsKnown ? std::to_string(c.dst) : "?",
               c.cc >= 0 ? std::to_string(c.cc) : "", fmt("%.1f", c.endSec - c.startSec), std::to_string(c.voiceFrames), std::to_string(c.fecErrors), fl,
               c.alias.empty() ? c.note : c.alias});
    }
    addTable(o, "Calls", t);
    Table m({"Slot", "Source", "Target", "Format", "CRC", "Text"});
    n = 0;
    for (auto it = d.messages.rbegin(); it != d.messages.rend() && n < 16; ++it, ++n)
        m.row({std::to_string(it->slot), std::to_string(it->src), std::to_string(it->dst), it->format, it->crcOk ? "ok" : "bad", it->text});
    if (n) addTable(o, "Messages", m);
}

void p25(Out& o, const P25Telemetry& p) {
    common(o, p.state, p.snrDb, p.levelDb, p.cfoHz, p.blocksOk, p.blocksBad);
    if (p.nac >= 0) o.card("NAC", fmt("%03X", p.nac));
    if (p.control) o.card("Control channel", "heard");
    if (p.site.valid) o.card("System / site", fmt("WACN %05X  sys %03X  site %d-%d", p.site.wacn < 0 ? 0 : p.site.wacn, p.site.sysId < 0 ? 0 : p.site.sysId, p.site.rfss, p.site.site));
    o.card("BER", fmt("%.2f %%", p.berPct));
    o.card("Voice decoder", p.imbe ? "available" : "not installed (silent)");
    Table t({"Kind", "Source", "Target", "Seconds", "Frames", "BER %", "Freq MHz", "Flags", "Note"});
    static const char* const kind[] = {"Voice", "Grant", "Data"};
    int n = 0;
    auto add = [&](const P25Call& c) {
        if (n++ >= 80) return;
        std::string fl = std::string(c.active ? "ON AIR " : "") + (c.emergency ? "EMERG " : "") + (c.encrypted ? "ENC " : "") + (c.lateEntry ? "late " : "");
        t.row({kind[c.kind % 3], p25Id(c.src), std::string(c.group ? "TG " : "") + p25Id(c.dst), fmt("%.1f", c.durSec), std::to_string(c.voiceFrames), fmt("%.1f", c.berPct),
               c.freqHz > 0 ? fmt("%.4f", c.freqHz / 1e6) : "", fl, c.text});
    };
    if (p.now.active) add(p.now);
    for (const P25Call& c : p.log) add(c);
    addTable(o, "Calls", t);
    Table tg({"Talkgroup", "Grants", "Calls", "Last unit", "Freq MHz", "Flags"});
    n = 0;
    for (const P25Talkgroup& g : p.talkgroups) {
        if (++n > 80) break;
        tg.row({std::to_string(g.tg), std::to_string(g.grants), std::to_string(g.calls), p25Id(g.lastSrc), g.lastFreqHz > 0 ? fmt("%.4f", g.lastFreqHz / 1e6) : "",
                std::string(g.active ? "ON AIR " : "") + (g.encrypted ? "ENC " : "") + (g.emergency ? "EMERG" : "")});
    }
    if (n) addTable(o, "Talkgroups", tg);
    eventsTable(o, p.events, "Control messages");
}

void nxdn(Out& o, const NxdnTelemetry& p) {
    common(o, p.state, p.snrDb, p.levelDb, p.cfoHz, p.blocksOk, p.blocksBad);
    if (p.baud) o.card("Rate", p.baud == 4800 ? "NXDN96 (4800)" : "NXDN48 (2400)");
    if (p.ran >= 0) o.card("RAN", std::to_string(p.ran));
    o.card("BER", fmt("%.2f %%", p.berPct));
    o.card("Voice decoder", p.ambe ? "available" : "not installed (silent)");
    Table t({"Kind", "Source", "Target", "Type", "Seconds", "Frames", "BER %", "Flags", "Text"});
    static const char* const kind[] = {"Voice", "Status", "Data"};
    int n = 0;
    auto add = [&](const NxdnCall& c) {
        if (n++ >= 80) return;
        t.row({kind[c.kind % 3], nxdnId(c.src), nxdnId(c.dst), c.callType == 1 ? "group" : "unit", fmt("%.1f", c.durSec), std::to_string(c.voiceFrames), fmt("%.1f", c.berPct),
               std::string(c.active ? "ON AIR " : "") + (c.cipher ? "ENC " : "") + (c.lateEntry ? "late" : ""), c.text});
    };
    if (p.now.active) add(p.now);
    for (const NxdnCall& c : p.log) add(c);
    addTable(o, "Calls", t);
    eventsTable(o, p.events, "Control messages");
}

void dpmr(Out& o, const DpmrTelemetry& p) {
    common(o, p.state, p.snrDb, p.levelDb, p.cfoHz, p.blocksOk, p.blocksBad);
    if (p.cc >= 0) o.card("Colour code", std::to_string(p.cc));
    if (p.channel) o.card("PMR446 channel", std::to_string(p.channel) + (p.scanning ? " (scanning)" : ""));
    o.card("BER", fmt("%.2f %%", p.berPct));
    o.card("Voice decoder", p.ambe ? "available" : "not installed (silent)");
    Table t({"Kind", "Source", "Target", "CC", "Ch", "Seconds", "Frames", "BER %", "Flags", "Text"});
    static const char* const kind[] = {"Voice", "Status", "Message", "Packet"};
    int n = 0;
    auto add = [&](const DpmrCall& c) {
        if (n++ >= 80) return;
        t.row({kind[c.kind % 4], dpmrId(c.src), dpmrId(c.dst), c.cc >= 0 ? std::to_string(c.cc) : "", c.channel ? std::to_string(c.channel) : "", fmt("%.1f", c.durSec),
               std::to_string(c.voiceFrames), fmt("%.1f", c.berPct), std::string(c.active ? "ON AIR " : "") + (c.emergency ? "EMERG " : "") + (c.lateEntry ? "late" : ""), c.text});
    };
    if (p.now.active) add(p.now);
    for (const DpmrCall& c : p.log) add(c);
    addTable(o, "Calls", t);
    eventsTable(o, p.events);
}

void dstar(Out& o, const DstarTelemetry& p) {
    common(o, p.state, p.snrDb, p.levelDb, p.cfoHz, p.blocksOk, p.blocksBad);
    o.card("Headers ok / bad", std::to_string(p.headersOk) + " / " + std::to_string(p.headersBad));
    o.card("BER", fmt("%.2f %%", p.berPct));
    o.card("Voice decoder", p.ambe ? "available" : "not installed (silent)");
    Table t({"MY", "UR", "RPT1", "RPT2", "Seconds", "Frames", "BER %", "Position", "Message"});
    int n = 0;
    auto add = [&](const DstarCall& c) {
        if (n++ >= 80) return;
        t.row({c.my + (c.sfx.empty() ? "" : "/" + c.sfx), c.ur, c.rpt1, c.rpt2, fmt("%.1f", c.durSec), std::to_string(c.voiceFrames), fmt("%.1f", c.berPct),
               c.hasPos ? fmt("%.4f, %.4f", c.lat, c.lon) : "", c.text});
    };
    if (p.now.active) add(p.now);
    for (const DstarCall& c : p.log) add(c);
    addTable(o, "Heard", t);
    Table m({"Time UTC", "MY", "UR", "Message"});
    n = 0;
    for (const DstarMessage& x : p.messages) { if (++n > 40) break; m.row({hhmmss(x.wallTime), x.my, x.ur, x.text}); }
    if (n) addTable(o, "Messages", m);
    Table g({"Time UTC", "Call", "Position", "Speed km/h", "Comment"});
    n = 0;
    for (const DstarPosition& x : p.positions) { if (++n > 40) break; g.row({hhmmss(x.wallTime), x.call, fmt("%.4f, %.4f", x.lat, x.lon), x.velValid ? fmt("%.0f", x.speedKmh) : "", x.comment}); }
    if (n) addTable(o, "Positions", g);
}

void tetra(Out& o, const TetraTelemetry& p) {
    common(o, p.state >= 2 ? 2 : p.state ? 1 : 0, p.snrDb, p.levelDb, p.cfoHz, p.blocksOk, p.blocksBad);
    o.card("Carriers seen", std::to_string(p.carriers.size()));
    o.card("Cells", std::to_string(p.cells.size()));
    Table t({"MHz", "MCC", "MNC", "CC", "LA", "Main", "Level dB", "SNR dB", "Load", "Encryption", "Seen"});
    int n = 0;
    for (const TetraCell& c : p.cells) {
        if (++n > 60) break;
        t.row({fmt("%.4f", (c.nominalHz > 0 ? c.nominalHz : c.freqHz) / 1e6), c.mcc >= 0 ? std::to_string(c.mcc) : "", c.mnc >= 0 ? std::to_string(c.mnc) : "",
               c.colourCode >= 0 ? std::to_string(c.colourCode) : "", c.la >= 0 ? std::to_string(c.la) : "", c.isMain ? "MCCH" : "", fmt("%.0f", c.levelDb), fmt("%.0f", c.snrDb),
               std::to_string(c.cellLoad), c.encryption() ? "yes" : "", std::to_string(c.seen)});
    }
    addTable(o, "Cells", t);
    Table e({"Time UTC", "Event"});
    n = 0;
    for (const TetraEvent& x : p.events) { if (++n > 80) break; e.row({hhmmss(x.wallTime), x.text}); }
    if (n) addTable(o, "Events", e);
}

void hfdl(Out& o, const HfdlTelemetry& h) {
    o.card("Aircraft", std::to_string(h.aircraft.size()));
    o.card("Ground stations", std::to_string(h.stations.size()));
    o.card("Squitters", std::to_string(h.squitters));
    o.card("Link PDUs", std::to_string(h.lpdus));
    o.card("ACARS messages", std::to_string(h.acarsMsgs));
    Table m({"Time UTC", "kHz", "bps", "Dir", "Ground stn", "Aircraft", "Kind", "Flight", "Label", "Text"});
    int n = 0;
    for (const HfdlMessage& x : h.messages) {
        if (++n > 100) break;
        m.row({hhmmss(x.wallTime), fmt("%.1f", x.freqHz / 1e3), std::to_string(x.bps), x.uplink ? "up" : "down", std::to_string(x.gsId),
               x.reg.empty() ? (x.icao ? fmt("%06X", x.icao) : "") : x.reg, x.kind, x.flight, x.label, x.acars ? x.acarsText : x.summary});
    }
    addTable(o, "Messages", m);
    Table ac({"Registration", "ICAO", "Flight", "Msgs", "kHz", "Level dB", "Position", "Alt ft", "Logged on"});
    n = 0;
    for (const HfdlAircraft& x : h.aircraft) {
        if (++n > 80) break;
        ac.row({x.reg, x.icao ? fmt("%06X", x.icao) : "", x.flight, std::to_string(x.messages), fmt("%.1f", x.freqHz / 1e3), fmt("%.0f", x.levelDb),
                x.hasPos ? fmt("%.3f, %.3f", x.lat, x.lon) : "", x.hasAlt ? std::to_string(x.altFt) : "", yn(x.loggedOn)});
    }
    addTable(o, "Aircraft", ac);
    Table gs({"Id", "Name", "Squitters", "Msgs", "kHz", "Level dB"});
    n = 0;
    for (const HfdlGroundStation& x : h.stations) { if (++n > 60) break; gs.row({std::to_string(x.id), x.name, std::to_string(x.squitters), std::to_string(x.messages), fmt("%.1f", x.freqHz / 1e3), fmt("%.0f", x.levelDb)}); }
    addTable(o, "Ground stations", gs);
}

void vdl2(Out& o, const Vdl2Telemetry& v) {
    o.card("Aircraft", std::to_string(v.aircraft.size()));
    o.card("Ground stations", std::to_string(v.stations.size()));
    Table m({"Time UTC", "MHz", "Dir", "Source", "Dest", "Frame", "Protocol", "Flight", "Label", "Text"});
    int n = 0;
    for (const Vdl2Message& x : v.messages) {
        if (++n > 100) break;
        m.row({hhmmss(x.wallTime), fmt("%.3f", x.freqHz / 1e6), x.fromAir ? "down" : x.fromGround ? "up" : "", fmt("%06X", x.src), fmt("%06X", x.dst), x.frameName, x.proto,
               x.flight.empty() ? x.reg : x.flight, x.label, x.acars ? x.acarsText : x.summary});
    }
    addTable(o, "Messages", m);
    Table ac({"ICAO", "Registration", "Flight", "Msgs", "MHz", "Level dB", "Position", "Alt ft", "Speed kt"});
    n = 0;
    for (const Vdl2Aircraft& x : v.aircraft) {
        if (++n > 80) break;
        ac.row({fmt("%06X", x.icao), x.reg, x.flight, std::to_string(x.messages), fmt("%.3f", x.freqHz / 1e6), fmt("%.0f", x.levelDb), x.hasPos ? fmt("%.3f, %.3f", x.lat, x.lon) : "",
                x.hasAlt ? std::to_string(x.altFt) : "", x.hasKin ? fmt("%.0f", x.groundKt) : ""});
    }
    addTable(o, "Aircraft", ac);
    Table gs({"Address", "Msgs", "MHz", "Level dB", "Airports"});
    n = 0;
    for (const Vdl2GroundStation& x : v.stations) { if (++n > 40) break; gs.row({fmt("%06X", x.addr), std::to_string(x.messages), fmt("%.3f", x.freqHz / 1e6), fmt("%.0f", x.levelDb), x.airports}); }
    if (n) addTable(o, "Ground stations", gs);
}

void aero(Out& o, const AeroTelemetry& a) {
    common(o, a.state, a.snrDb, -200, a.cfoHz, a.blocksOk, a.blocksBad);
    o.card("Messages", std::to_string(a.messagesTotal));
    o.card("Logons", std::to_string(a.logonsTotal));
    o.card("Frames", std::to_string(a.frames));
    Table m({"Time UTC", "Dir", "Aircraft", "Registration", "Flight", "Label", "Text"});
    int n = 0;
    for (auto it = a.messages.rbegin(); it != a.messages.rend() && n < 100; ++it, ++n)
        m.row({hhmmss(it->wallTime), it->uplink ? "up" : "down", fmt("%06X", it->aesId), it->registration, it->flight, it->label, it->decoded.empty() ? it->text : it->decoded});
    addTable(o, "Messages", m);
    Table ac({"AES", "Registration", "Flight", "Msgs", "Last label", "Position", "Alt ft", "Logged on"});
    n = 0;
    for (const AeroAircraft& x : a.aircraft) {
        if (++n > 80) break;
        ac.row({fmt("%06X", x.aesId), x.registration, x.flight, std::to_string(x.messages), x.lastLabel, x.hasPos ? fmt("%.3f, %.3f", x.lat, x.lon) : "", x.hasAlt ? std::to_string(x.altFt) : "", yn(x.loggedOn)});
    }
    addTable(o, "Aircraft", ac);
    Table ch({"Offset kHz", "Rate bps", "Eb/N0 dB", "Level dB", "Frames", "BER"});
    for (const AeroChannelInfo& c : a.channels) ch.row({fmt("%.1f", c.offsetHz / 1e3), std::to_string(c.bitRate), fmt("%.1f", c.ebn0Db), fmt("%.0f", c.levelDb), std::to_string(c.frames), fmt("%.3f", c.channelBer)});
    if (!a.channels.empty()) addTable(o, "Channels", ch);
}

void inmc(Out& o, const InmcTelemetry& a) {
    common(o, a.state, a.snrDb, -200, a.cfoHz, a.blocksOk, a.blocksBad);
    o.card("Frame lock", a.frameLock ? "yes" : "no");
    o.card("Es/N0", fmt("%.1f dB", a.esn0Db));
    o.card("Messages", std::to_string(a.messageCount));
    Table m({"Time", "Service", "Priority", "Area", "Satellite", "LES", "Packets", "Complete", "Text"});
    int n = 0;
    for (const InmcMessage& x : a.messages) {
        if (++n > 100) break;
        m.row({x.rxTime, x.serviceText, std::to_string(x.priority), x.area, x.sat >= 0 ? std::to_string(x.sat) : "", x.lesName.empty() ? idOrQ(x.lesId) : x.lesName,
               std::to_string(x.packets), yn(x.complete), x.text});
    }
    addTable(o, "Messages", m);
    Table ch({"Region", "LES", "Type", "Eb/N0 dB", "Frames ok", "Frames bad", "Messages"});
    for (const InmcChannelInfo& c : a.channels) ch.row({c.region, c.lesName, c.channelTypeName, fmt("%.1f", c.ebn0Db), std::to_string(c.framesOk), std::to_string(c.framesBad), std::to_string(c.messages)});
    if (!a.channels.empty()) addTable(o, "Channels", ch);
}

void dab(Out& o, const DabTelemetry& d, const DabEnsemble* e) {
    o.card("Sync", d.sync ? "locked" : "searching");
    o.card("SNR", fmt("%.1f dB", d.snrDb));
    o.card("FIC blocks ok / bad", std::to_string(d.fibOk) + " / " + std::to_string(d.fibBad));
    if (!d.ensembleLabel.empty()) o.card("Ensemble", d.ensembleLabel);
    o.card("Audio decoder", d.audio.decoding ? d.audio.codec : "FFmpeg not built in (no sound)");
    if (!e || !e->valid) return;
    Table t({"Service", "Id", "Type", "kbit/s", "Subch", "Programme type", "Now playing"});
    int n = 0;
    for (const auto& kv : e->services) {
        if (++n > 120) break;
        const DabService& s = kv.second;
        const DabComponent* a = s.audio();
        int br = 0;
        if (a) { auto it = e->subs.find(a->subId); if (it != e->subs.end()) br = it->second.bitrate; }
        t.row({s.label, fmt("%X", s.sid), a ? (a->ascty == 63 ? "DAB+" : "DAB") : s.dmb() ? "DMB" : "data", br ? std::to_string(br) : "", a ? std::to_string(a->subId) : "",
               s.pty >= 0 ? std::to_string(s.pty) : "", s.dls});
    }
    addTable(o, "Services", t);
}

void same(Out& o, const SameTelemetry& s) {
    common(o, s.state, s.snrDb, s.levelDb, s.cfoHz, s.blocksOk, s.blocksBad);
    o.card("Alerts", std::to_string(s.alerts.size()));
    o.card("Attention tone", s.toneNow ? "now" : "no");
    Table t({"Time UTC", "Event", "Level", "Originator", "Areas", "Valid min", "Issued", "From", "SNR dB", "Repeats"});
    int n = 0;
    for (const SameAlert& a : s.alerts) {
        if (++n > 60) break;
        if (!a.valid) { t.row({hhmmss(a.wall), a.eom ? "end of message" : "damaged: " + a.err, "", "", "", "", "", "", fmt("%.0f", a.snrDb), std::to_string(a.bursts)}); continue; }
        std::string loc;
        for (const auto& l : a.msg.locs) { if (!loc.empty()) loc += "; "; loc += l.text.empty() ? l.code : l.text; }
        t.row({hhmmss(a.wall), a.msg.eventName.empty() ? a.msg.event : a.msg.eventName, a.msg.eventKind, a.msg.orgName.empty() ? a.msg.org : a.msg.orgName, loc,
               std::to_string(a.msg.purgeMin), fmt("day %d %02d:%02d", a.msg.issueDay, a.msg.issueHour, a.msg.issueMin), a.msg.callsign, fmt("%.0f", a.snrDb), std::to_string(a.bursts)});
    }
    addTable(o, "Alerts", t);
}

void ble(Out& o, const BleTelemetry& b) {
    common(o, b.state, b.snrDb, b.levelDb, b.cfoHz, b.blocksOk, b.blocksBad);
    o.card("Devices", std::to_string(b.devices.size()));
    Table t({"Address", "Name", "Vendor", "Beacon", "Services", "RSSI", "Packets", "Connectable", "Last s"});
    int n = 0;
    for (const BleDevice& d : b.devices) {
        if (++n > 120) break;
        t.row({d.addr + (d.random ? " (r)" : ""), d.name, d.vendor, d.beacon, d.services, fmt("%.0f", d.rssi), std::to_string(d.packets), yn(d.connectable), fmt("%.0f", std::max(0.0, b.timeSec - d.lastSec))});
    }
    addTable(o, "Devices", t);
}

void ism(Out& o, const IsmTelemetry& s) {
    common(o, s.state, s.snrDb, s.levelDb, s.cfoHz, s.blocksOk, s.blocksBad);
    o.card("Devices", std::to_string(s.devices.size()));
    o.card("Bursts", std::to_string(s.bursts));
    Table t({"Model", "Id", "Ch", "Readings", "Battery", "MHz", "RSSI", "Count", "Last s"});
    int n = 0;
    for (const IsmDevice& d : s.devices) {
        if (++n > 100) break;
        std::string rd;
        for (const IsmField& f : d.fields) {
            if (!rd.empty()) rd += "  ";
            rd += f.name + " " + (f.text.empty() ? fmt(("%." + std::to_string(f.decimals < 0 ? 0 : f.decimals > 6 ? 6 : f.decimals) + "f").c_str(), f.value) : f.text) + (f.unit.empty() ? "" : " " + f.unit);
        }
        t.row({d.model, d.id, d.channelName.empty() ? (d.channel >= 0 ? std::to_string(d.channel) : "") : d.channelName, rd, d.battery >= 0 ? std::to_string(d.battery) : "",
               fmt("%.3f", d.freqHz / 1e6), fmt("%.0f", d.rssiDb), std::to_string(d.count), fmt("%.0f", std::max(0.0, s.timeSec - d.lastSec))});
    }
    addTable(o, "Sensors", t);
}

void iridium(Out& o, const IridiumTelemetry& r) {
    common(o, r.state, r.snrDb, -200, r.cfoHz, r.blocksOk, r.blocksBad);
    o.card("Bursts", std::to_string(r.bursts));
    o.card("Demodulated", std::to_string(r.demodulated));
    o.card("Downlink / uplink", std::to_string(r.downlink) + " / " + std::to_string(r.uplink));
    o.card("Ring alerts / paged", std::to_string(r.ringAlerts.size()) + " / " + std::to_string(r.pagedTotal));
    Table s({"Satellite", "Frames", "Offset Hz", "Position", "Alt km", "Beams"});
    int n = 0;
    for (const IridiumSatInfo& x : r.sats) {
        if (++n > 60) break;
        s.row({std::to_string(x.id), std::to_string(x.frames), fmt("%.0f", x.freqOffsetHz), x.hasPos ? fmt("%.2f, %.2f", x.lat, x.lon) : "", x.hasPos ? fmt("%.0f", x.altKm) : "", std::to_string(x.beams.size())});
    }
    addTable(o, "Satellites", s);
    Table pg({"RIC", "Seq", "Complete", "Text"});
    n = 0;
    for (auto it = r.messages.rbegin(); it != r.messages.rend() && n < 60; ++it, ++n) pg.row({std::to_string(it->ric), std::to_string(it->seq), yn(it->complete), it->text});
    if (n) addTable(o, "Pages", pg);
    Table ac({"Dir", "Registration", "Flight", "Label", "Text"});
    n = 0;
    for (auto it = r.acars.rbegin(); it != r.acars.rend() && n < 60; ++it, ++n) ac.row({it->downlink ? "down" : "up", it->reg, it->flight, it->label, it->text});
    if (n) addTable(o, "ACARS", ac);
}

void mesh(Out& o, const MeshTelemetry& m) {
    common(o, m.state, m.snrDb, -200, m.cfoHz, m.blocksOk, m.blocksBad);
    o.card("Nodes", std::to_string(m.nodes.size()));
    o.card("Messages", std::to_string(m.messages.size()));
    o.card("Packets", std::to_string(m.packets.size()));
    Table nd({"Id", "Name", "Short", "Hardware", "Role", "Position", "Battery %", "SNR dB", "Hops", "Packets"});
    int n = 0;
    for (const MeshNode& x : m.nodes) {
        if (++n > 100) break;
        nd.row({x.id, x.longName, x.shortName, x.hwModel, x.role, x.hasPosition ? fmt("%.4f, %.4f", x.lat, x.lon) : "", x.batteryPct >= 0 ? fmt("%.0f", x.batteryPct) : "",
                fmt("%.1f", x.lastSnrDb), x.hopsAway >= 0 ? std::to_string(x.hopsAway) : "", std::to_string(x.packets)});
    }
    addTable(o, "Nodes", nd);
    Table ms({"Channel", "From", "To", "Hops", "Text"});
    n = 0;
    for (auto it = m.messages.rbegin(); it != m.messages.rend() && n < 80; ++it, ++n) ms.row({it->channel, it->fromName.empty() ? it->from : it->fromName, it->to, it->hops >= 0 ? std::to_string(it->hops) : "", it->text});
    if (n) addTable(o, "Messages", ms);
    Table pk({"Type", "From", "To", "SF", "SNR dB", "Size", "CRC", "Note"});
    n = 0;
    for (auto it = m.packets.rbegin(); it != m.packets.rend() && n < 60; ++it, ++n)
        pk.row({it->type, it->from, it->to, std::to_string(it->sf), fmt("%.1f", it->snrDb), std::to_string(it->size), it->crcOk ? "ok" : "bad", it->note});
    if (n) addTable(o, "Packets", pk);
}

// FT8 / FT4 / ... decode list, plus the RTTY and keyboard-mode text streams that share the HF digital engine
void hfdig(Out& o, const HfdigTelemetry& h) {
    common(o, h.state, h.snrDb, h.levelDb, h.cfoHz, h.blocksOk, h.blocksBad);
    static const char* const names[kFtxModes] = {"FT8", "FT4", "FT2", "JT65", "JT9", "Q65", "FST4", "MSK144", "FST4W", "WSPR"};
    if (h.ftx.total) o.card("Decodes", std::to_string(h.ftx.total));
    if (!h.ftx.decodes.empty()) {
        Table t({"Time UTC", "Mode", "SNR dB", "dT s", "Hz", "Message"});
        int n = 0;
        for (auto it = h.ftx.decodes.rbegin(); it != h.ftx.decodes.rend() && n < 120; ++it, ++n) {
            const int mi = it->mode;
            t.row({hhmmss((int64_t)it->slotUtc), mi >= 0 && mi < kFtxModes ? names[mi] : "?", fmt("%.0f", it->snrDb), fmt("%.1f", it->dt), fmt("%.0f", it->hz), it->msg});
        }
        addTable(o, "Decodes", t);
    }
    textTable(o, "RTTY", h.rtty.text);
    textTable(o, "Text", h.kbd.text);
    if (!h.kbd.callsign.empty()) o.card("Callsign", h.kbd.callsign);
}

void psk(Out& o, const PskTelemetry& p) {
    common(o, p.state, p.snrDb, p.levelDb, p.cfoHz, p.blocksOk, p.blocksBad);
    Table t({"Offset Hz", "SNR dB", "Quality", "Chars", "Calls", "Text"});
    int n = 0;
    for (const PskSignal& s : p.signals) {
        if (++n > 40) break;
        std::string c;
        for (const std::string& x : s.calls) { if (!c.empty()) c += ' '; c += x; }
        std::string tx = s.text.size() > 160 ? s.text.substr(s.text.size() - 160) : s.text;
        t.row({fmt("%.0f", s.offsetHz), fmt("%.0f", s.snrDb), fmt("%.0f", s.quality * 100), std::to_string(s.chars), c, tx});
    }
    addTable(o, "Signals", t);
}

void cw(Out& o, const CwTelemetry& p) {
    common(o, p.state, p.snrDb, p.levelDb, p.cfoHz, p.blocksOk, p.blocksBad);
    Table t({"Offset Hz", "WPM", "SNR dB", "Locked", "Chars", "Calls", "Text"});
    int n = 0;
    for (const CwSignal& s : p.signals) {
        if (++n > 40) break;
        std::string c;
        for (const std::string& x : s.calls) { if (!c.empty()) c += ' '; c += x; }
        std::string tx = s.text.size() > 160 ? s.text.substr(s.text.size() - 160) : s.text;
        t.row({fmt("%.0f", s.offsetHz), fmt("%.0f", s.wpm), fmt("%.0f", s.snrDb), yn(s.locked), std::to_string(s.chars), c, tx});
    }
    addTable(o, "Signals", t);
}

void js8(Out& o, const Js8Telemetry& p) {
    common(o, p.state, p.snrDb, p.levelDb, p.cfoHz, p.blocksOk, p.blocksBad);
    Table t({"Time UTC", "SNR dB", "Hz", "From", "To", "Text"});
    int n = 0;
    for (auto it = p.decodes.rbegin(); it != p.decodes.rend() && n < 100; ++it, ++n)
        t.row({hhmmss((int64_t)it->slotUtc), fmt("%.0f", it->snrDb), fmt("%.0f", it->hz), it->from, it->to, it->text.empty() ? it->chars : it->text});
    addTable(o, "Decodes", t);
    Table s({"Call", "Grid", "SNR dB", "Frames", "Last text"});
    n = 0;
    for (const Js8Station& x : p.stations) { if (++n > 60) break; s.row({x.call, x.grid, fmt("%.0f", x.snrDb), std::to_string(x.frames), x.lastText}); }
    if (n) addTable(o, "Stations", s);
}

// CDR (China Digital Radio, #463): the services with what they carry, and why there is no sound when the DRA+ add-on is missing. `listen` is
// the service the bridge asked the receiver to play (-1: the first audio service, as the desktop does; 0: none).
void cdrResults(Out& o, const CdrTelemetry& t, int listen, std::string& extra) {
    static const char* st[4] = {"searching for a beacon", "beacon found", "system information decoded", "multiplex decoded"};
    o.card("State", t.state >= 0 && t.state < 4 ? st[t.state] : "-");
    o.card("SNR", fmt("%.1f dB", t.snrDb));
    o.card("Carrier error", fmt("%.0f Hz", t.cfoHz));
    if (t.tm > 0) o.card("Transmission mode", std::to_string(t.tm));
    if (t.innerKhz > 0 || t.outerKhz > 0) o.card("Digital part", fmt("%d .. %d kHz either side", t.innerKhz, t.outerKhz));
    o.card("LDPC ok / bad", std::to_string(t.blocksOk) + " / " + std::to_string(t.blocksBad));
    if (!t.network.empty()) o.card("Network", t.network);
    if (!t.country.empty()) o.card("Country", t.country);
    int playing = 0;      // the same choice as CdrReceiver makes for -1: the first audio service
    for (const auto& s : t.services)
        if (s.audio && (listen == (int)s.id || listen == -1)) { playing = s.id; break; }
    bool anyAudio = false;
    for (const auto& s : t.services) anyAudio |= s.audio;
    std::string sound;
    if (!anyAudio) sound = t.services.empty() ? "no services yet" : "no audio service";
    else if (!t.draFound) sound = t.draProblem.empty() ? "silent: the DRA+ add-on is not installed" : "silent: the DRA+ add-on cannot be used";
    else if (t.draPcmFrames > 0) sound = fmt("DRA+ %.1f kHz", t.draRateHz / 1000.0);
    else sound = "DRA+ add-on loaded, waiting for sound";
    o.card("Sound", sound);
    auto ch = [](int c) { return c == 1 ? std::string("mono") : c == 2 ? std::string("stereo") : c == 3 ? std::string("5.1") : std::string("-"); };
    Table tb({"Id", "Kind", "Format", "Kbps", "Language", "Text", "Playing"});
    std::string svc;
    for (const auto& s : t.services) {
        std::string format, lang;
        if (s.audio && !s.streams.empty()) {
            const auto& a = s.streams[0];
            format = a.bitrate > 0 ? fmt("DRA+ %.0f kbit/s, %g kHz %s", a.bitrate / 1000.0, a.sampleRate / 1000.0, ch(a.channelsCode).c_str())
                                   : fmt("DRA+, %g kHz %s", a.sampleRate / 1000.0, ch(a.channelsCode).c_str());
            lang = a.language;
        }
        const char* kind = s.audio ? "audio" : s.data ? "data" : "-";
        tb.row({fmt("%04X", s.id), kind, format, fmt("%.1f", s.kbps), lang, s.text, (int)s.id == playing ? "yes" : ""});
        if (!svc.empty()) svc += ',';
        svc += "{\"id\":" + std::to_string(s.id) + ",\"audio\":" + (s.audio ? "true" : "false") + ",\"name\":" + q(fmt("%s %04X", kind, s.id)) + ",\"format\":" + q(format) + "}";
    }
    if (!t.services.empty()) addTable(o, "Services", tb);
    if (!t.notes.empty()) { Table n({"Note"}); for (const auto& x : t.notes) n.row({x}); addTable(o, "Notes", n); }
    extra = ",\"cdr\":{\"draFound\":" + std::string(t.draFound ? "true" : "false") + ",\"draProblem\":" + q(t.draProblem) + ",\"draPath\":" + q(t.draPath) +
            ",\"anyAudio\":" + (anyAudio ? "true" : "false") + ",\"listen\":" + std::to_string(listen) + ",\"playing\":" + std::to_string(playing) +
            ",\"pcmFrames\":" + std::to_string(t.draPcmFrames) + ",\"services\":[" + svc + "]}";
}

// Cards for the video standards from their telemetry (lock, SNR, MER, error counters): the phone has no plots yet
void tvCards(Out& o, const RxTelemetry& rx) {
    auto cnt = [](uint64_t a, uint64_t b, uint64_t c) { return std::to_string(a) + " / " + std::to_string(b) + " / " + std::to_string(c); };
    switch (rx.standard) {
    case 0:
        o.card("L1 pre / post", std::string(rx.l1preOk ? "ok" : "no") + " / " + (rx.l1postOk ? "ok" : "no"));
        o.card("FFT size", std::to_string(rx.fftN));
        o.card("Guard samples", std::to_string(rx.guard));
        o.card("Sample-rate offset", fmt("%+.1f ppm", rx.sroPpm));
        o.card("P2 SNR", fmt("%.1f dB", rx.p2SnrDb));
        o.card("Data SNR", fmt("%.1f dB", rx.dataSnrDb));
        if (rx.plpValid) { o.card("PLP", std::to_string(rx.plpId)); o.card("PLP MER", fmt("%.1f dB", rx.plpMerDb)); o.card("BER before LDPC", fmt("%.2e", rx.plpPreBer)); o.card("PLP frames", std::to_string(rx.plpFrames)); }
        o.card("Services", std::to_string(rx.plpList.size()));
        break;
    case 1:
        o.card("TPS", rx.dvbt.tpsOk ? "ok" : "no");
        o.card("FEC sync", rx.dvbt.fecSync ? "yes" : "no");
        o.card("FFT size", std::to_string(rx.fftN));
        o.card("Data SNR", fmt("%.1f dB", rx.dataSnrDb));
        o.card("Reed-Solomon clean / fixed / failed", cnt(rx.dvbt.rsClean, rx.dvbt.rsCorrected, rx.dvbt.rsFailed));
        o.card("Packets", std::to_string(rx.dvbt.packets));
        break;
    case 2:
        o.card("Pilot / field sync / TS", std::string(rx.atsc.pilot ? "yes" : "no") + " / " + (rx.atsc.fieldSync ? "yes" : "no") + " / " + (rx.atsc.tsOk ? "yes" : "no"));
        o.card("SNR", fmt("%.1f dB", rx.atsc.snrDb));
        o.card("Data SNR", fmt("%.1f dB", rx.atsc.dataSnrDb));
        o.card("Reed-Solomon clean / fixed / failed", cnt(rx.atsc.rsClean, rx.atsc.rsCorrected, rx.atsc.rsFailed));
        break;
    case 7:
        o.card("Symbol rate", fmt("%.2f Msym/s", rx.dvbs.symbolRate / 1e6));
        o.card("SNR", fmt("%.1f dB", rx.dvbs.snrDb));
        o.card("MER", fmt("%.1f dB", rx.dvbs.merDb));
        o.card("Data", rx.dvbs.dataValid ? "valid" : "none");
        break;
    case 8: {
        const DtmbTelemetry& d = rx.dtmb;
        static const char* hdr[3] = {"PN420", "PN595", "PN945"};
        static const char* map[5] = {"4QAM-NR", "4QAM", "16QAM", "32QAM", "64QAM"};
        static const char* rt[3] = {"0.4", "0.6", "0.8"};
        o.card("SNR", fmt("%.1f dB", d.snrDb));
        o.card("MER", fmt("%.1f dB", d.merDb));
        o.card("Carrier to noise", fmt("%.1f dB", d.snrPnDb));
        o.card("TS lock", d.tsLock ? "yes" : "no");
        // #435: the phone showed only the four cards above; the frame and system information are what the desktop shows next to them
        o.card("Frame header", d.header >= 0 && d.header < 3 ? std::string(hdr[d.header]) + (d.phaseRotates ? ", phase rotates" : ", fixed phase") : "searching");
        if (d.carriers > 0) o.card("Carriers", d.carriers == 1 ? "single carrier (C=1)" : "multi-carrier (C=3780)");
        if (d.siOk && d.mapping >= 0 && d.mapping < 5 && d.rate >= 0 && d.rate < 3)
            o.card("System information", fmt("%s, rate %s, interleaver %d (index %d)", map[d.mapping], rt[d.rate], d.interleaver, d.siIndex));
        else o.card("System information", d.header >= 0 ? "not decoded yet" : "-");
        if (d.netMbps > 0) o.card("Payload rate", fmt("%.2f Mbit/s", d.netMbps));
        o.card("LDPC ok / bad", std::to_string(d.blocksOk) + " / " + std::to_string(d.blocksBad));
        o.card("TS packets", std::to_string(d.packets));
        if (d.frames) o.card("Frames without a usable header", fmt("%.0f %%", d.frameLossPct));
        if (d.cwDropped) o.card("Codewords dropped (phone too slow)", std::to_string(d.cwDropped));
        if (d.cfoHz != 0) o.card("Carrier offset", fmt("%.0f Hz", d.cfoHz));
        if (d.clockPpm != 0) o.card("Sample clock", fmt("%+.1f ppm", d.clockPpm));
        if (d.echoSpanUs > 0) o.card("Echo span", fmt("%.1f us", d.echoSpanUs));
        break;
    }
    case 41:
        o.card("Symbol rate", fmt("%.2f Msym/s", rx.dvbc.symbolRate / 1e6));
        o.card("QAM", std::to_string((int)rx.dvbc.qam));
        o.card("MER", fmt("%.1f dB", rx.dvbc.merDb));
        o.card("Level", fmt("%.1f dBFS", rx.dvbc.levelDbfs));
        break;
    default: break;
    }
}
} // namespace

// ---- modes without a hand-written exporter: generated from the telemetry structs (tools/gen_results_map.py) ------------------------------
namespace {
template <class T> std::string cell(const T& v) {
    if constexpr (std::is_same_v<T, std::string>) return v.size() > 400 ? v.substr(0, 400) : v;
    else if constexpr (std::is_floating_point_v<T>) return std::isfinite(v) ? fmt("%.6g", (double)v) : std::string();
    else if constexpr (std::is_arithmetic_v<T>) return std::to_string(v);
    else return std::string();
}
// a card only when the field says something (not empty, not zero)
template <class T> void cardIf(Out& o, const char* k, const T& v) {
    if constexpr (std::is_same_v<T, std::string>) { if (!v.empty()) o.card(k, cell(v)); }
    else { if (v != T{}) o.card(k, cell(v)); }
}
void genericResults(Out& o, const RxTelemetry& rx) {
    switch (rx.standard) {
#include "results_generic_map.inc"
    default: break;
    }
}
} // namespace

std::string resultsJson(const RxTelemetry& rx, bool running, const dect2::DabEnsemble* dabEns, const dect2::SampleLoss* loss, int cdrListen) {
    Out o;
    std::string extra;
    switch (rx.standard) {
    case 6: fm(o, rx.fm); extra = ",\"fm\":{\"ps\":" + q(rx.fm.psName) + ",\"rt\":" + q(rx.fm.radioText) + ",\"stereo\":" + (rx.fm.stereo ? "true" : "false") +
                                   ",\"rdsSync\":" + (rx.fm.rdsSync ? "true" : "false") + ",\"pty\":" + q(rx.fm.ptyText) + ",\"pi\":" + std::to_string(rx.fm.piCode) +
                                   ",\"snrDb\":" + jsonNum(rx.fm.snrDb) + ",\"levelDbfs\":" + jsonNum(rx.fm.levelDbfs) + "}"; break;
    case 12: adsb(o, rx.adsb); break;
    case 15: ais(o, rx.ais); break;
    case 17: acars(o, rx.acars); break;
    case 24: pager(o, rx.pager); break;
    case 25: packet(o, rx.packet); break;
    case 56: general(o, rx.general); extra = ",\"general\":{\"demod\":" + std::to_string(rx.general.demod) + ",\"squelchOpen\":" + (rx.general.squelchOpen ? "true" : "false") +
                                       ",\"levelDb\":" + jsonNum(rx.general.levelDb) + ",\"audioDb\":" + jsonNum(rx.general.audioDb) + ",\"squelchDb\":" + jsonNum(rx.general.squelchDb) + "}"; break;
    case 0: case 1: case 2: case 7: case 8: case 41: tvCards(o, rx); break;
    case 23: cdrResults(o, rx.cdr, cdrListen, extra); break;
    case 3: dab(o, rx.dab, dabEns); break;
    case 10: dmr(o, rx.dmr); break;
    case 51: p25(o, rx.p25); break;
    case 39: nxdn(o, rx.nxdn); break;
    case 46: dpmr(o, rx.dpmr); break;
    case 34: dstar(o, rx.dstar); break;
    case 47: tetra(o, rx.tetra); break;
    case 36: hfdl(o, rx.hfdl); break;
    case 28: vdl2(o, rx.vdl2); break;
    case 19: aero(o, rx.aero); break;
    case 18: inmc(o, rx.inmc); break;
    case 77: same(o, rx.same); break;
    case 52: ble(o, rx.ble); break;
    case 29: ism(o, rx.ism); break;
    case 20: iridium(o, rx.iridium); break;
    case 21: mesh(o, rx.mesh); break;
    case 26: hfdig(o, rx.hfdig); break;
    case 37: psk(o, rx.psk); break;
    case 35: cw(o, rx.cw); break;
    case 38: js8(o, rx.js8); break;
    default: genericResults(o, rx); break;
    }
    // #435: a network radio that does not deliver every sample (SpyServer over Wi-Fi at 10 Msps) breaks the wideband modes without an error of
    // their own; say how much of the signal never arrived, for every mode, and give the app a plain note for its banner
    if (loss && loss->live) {
        const double lostSec = loss->radioSec + loss->sourceLostSec;
        const bool recent = (loss->sinceRadioSec >= 0 && loss->sinceRadioSec < 15) || (loss->sinceSourceLostSec >= 0 && loss->sinceSourceLostSec < 15);
        if (lostSec > 0) o.card(loss->network ? "Signal lost on the network link" : "Signal lost on the USB link", fmt("%.2f s", lostSec));
        if (loss->deliveredPct >= 0 && loss->deliveredPct < 99.5f) o.card("Radio delivers", fmt("%.0f %% of the sample rate", loss->deliveredPct));
        if (loss->cpuSec > 0) o.card("Signal dropped (phone too slow)", fmt("%.2f s", loss->cpuSec));
        std::string note;
        if ((recent && lostSec > 0.05) || loss->rateLow)
            note = loss->network ? "The radio's network link is losing part of the signal" + (loss->deliveredPct >= 0 ? fmt(" (only %.0f %% arrives)", loss->deliveredPct) : std::string()) +
                                       ". Wideband modes (TV, DAB, DTMB) need every sample: use a wired link, a lower sample rate, or plug the radio into the phone over USB."
                                 : "The USB link is losing part of the signal. Try another cable or a powered hub, or a lower sample rate.";
        else if (loss->sinceCpuSec >= 0 && loss->sinceCpuSec < 15 && loss->cpuSec > 0.05)
            note = "The phone is too slow for this sample rate and drops part of the signal. Close other apps or use a lower sample rate.";
        if (!note.empty()) extra += ",\"linkNote\":" + q(note);
    }
    const std::string sum = modeSummary(rx);
    std::string core = rxTelemetryJson(rx, nullptr);   // {"seq":..,"standard":..,"state":..,"locked":..,"snrDb":..,"details":{..}}
    std::string j = "{\"running\":" + std::string(running ? "true" : "false") + ",\"available\":" + (rx.seq ? "true" : "false") + "," + core.substr(1, core.size() - 2);
    j += ",\"summary\":" + q(sum) + ",\"cards\":[" + o.cards + "],\"tables\":[" + o.tables + "]" + extra + "}";
    return j;
}

} // namespace onair
