// Insights (View > Insights; docs/SMART5-ham-3.9.md, SMART5-voice-3.9.md, SMART5-pager-3.9.md, SMART5-hfutil-3.9.md): what the smart analysers of
// core/include/dect2/insight found in this receiver's own decodes. On by default; they are fed through the automation analyser hook and the FT8 view,
// and every finding also goes on as an automation event "insight.<kind>". Offline; per talkgroup or system, never per radio.
#include "../d40/d40_ui.h"
#include "../app.h"
#include "dect2/insight/live.h"
#include "dect2/hfdl/hfdl_proto.h"
#include <cmath>
#include <cctype>
#include <fstream>
#include <sstream>
#include "../smart5/smart5_ui.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>
#include <set>

namespace in = dect2::insight;

namespace {

struct State {
    bool started = false, on = true;
    double nextTick = 0, nextSave = 0, lastFtx = 0;
    std::deque<in::Insight> rows;   // newest last
    std::set<std::pair<double, std::string>> ftxSeen;   // FT8 decodes already handed to the hub (slot, message)
    // the field feeds (docs/SMART5-field-3.9.md): what of each receiver's telemetry was handed over already
    double nextFeed = 0, lastFeedWall = 0;
    std::map<uint32_t, uint32_t> flarmPackets;
    uint64_t hfdlSerial = 0;
    double meshSec = -1, lwSec = -1;
    std::map<std::string, int64_t> meterSeen;
} S;

std::string fieldPath(const char* name) { return plat::dataDir() + "/insight_" + name + ".txt"; }
std::string readFile(const std::string& p) { std::ifstream in(p); std::stringstream ss; ss << in.rdbuf(); return ss.str(); }
void writeFile(const std::string& p, const std::string& text) { std::ofstream out(p, std::ios::trunc); out << text; }

std::set<std::string> mineSet() {
    std::set<std::string> m; std::istringstream in(plat::prefs().getS("wmbusMine", ""));
    for (std::string l; std::getline(in, l);) if (!l.empty()) m.insert(l);
    return m;
}

void fieldLoad() {
    in::live().with([](in::Hub& h) {
        h.field().hfdl().load(readFile(fieldPath("hfdl")));
        h.field().navtexInForce().load(readFile(fieldPath("navtex")));
        for (const auto& k : mineSet()) {
            std::string f = k; for (auto& c : f) if (!std::isalnum((unsigned char)c)) c = '_';
            h.field().leakFor(k).load(readFile(fieldPath(("meter_" + f).c_str())));
        }
    });
}
void fieldSave() {
    std::string hf, nt; std::vector<std::pair<std::string, std::string>> meters;
    in::live().with([&](in::Hub& h) {
        hf = h.field().hfdl().save(); nt = h.field().navtexInForce().save();
        for (const auto& [k, m] : h.field().leaks()) { std::string f = k; for (auto& c : f) if (!std::isalnum((unsigned char)c)) c = '_'; meters.push_back({"meter_" + f, m.save()}); }
    });
    writeFile(fieldPath("hfdl"), hf); writeFile(fieldPath("navtex"), nt);
    for (const auto& [n, t] : meters) writeFile(fieldPath(n.c_str()), t);
}

// once a second: the new parts of the running receiver's telemetry go to the field analysers (FLARM, HFDL, mesh, LoRaWAN, my meters)
void fieldFeed(App& a, double now) {
    if (now < S.nextFeed) return;
    S.nextFeed = now + 1;
    const double dt = S.lastFeedWall > 0 ? std::min(5.0, now - S.lastFeedWall) : 1;
    S.lastFeedWall = now;
    if (receiverLocationSet()) in::live().with([](in::Hub& h) { h.field().setHome(plat::prefs().getD("adsbLat", 0), plat::prefs().getD("adsbLon", 0)); });
    const int st = a.rx.standard;
    in::live().with([&](in::Hub& h) {
        auto& f = h.field();
        if (st == 67) {   // FLARM / OGN: thermals
            for (const auto& ac : a.rx.flarm.aircraft) {
                if (!ac.hasPos) continue;
                uint32_t& last = S.flarmPackets[ac.id];
                if (ac.packets == last) continue;
                last = ac.packets;
                dect2::TrafficFix x; x.id = ac.id; x.t = now - ac.ageSec; x.lat = ac.lat; x.lon = ac.lon; x.altM = ac.altM;
                x.speedMs = ac.speedKmh / 3.6; x.climbMs = ac.climbMs; x.reportsGround = !ac.airborne;
                f.flarmFix(x, h.pending());
            }
        } else if (st == 36) {   // HFDL: what was listened to, and the ground stations heard
            std::vector<int> khz;
            for (const auto& c : a.rx.hfdl.channels) if (c.inBand) khz.push_back((int)std::lround(c.freqHz / 1e3));
            f.hfdlListened(now, khz, dt);
            const auto& m = a.rx.hfdl.messages;   // newest first
            if (!m.empty() && m.front().serial < S.hfdlSerial) S.hfdlSerial = 0;   // the receiver restarted
            for (auto it = m.rbegin(); it != m.rend(); ++it) {
                if (it->serial <= S.hfdlSerial) continue;
                S.hfdlSerial = it->serial;
                if (!it->uplink || it->gsId <= 0) continue;
                const auto* gs = dect2::hfdlStation(it->gsId);
                f.hfdlHeard(it->wallTime > 0 ? (double)it->wallTime : now, it->gsId, (int)std::lround(it->freqHz / 1e3), it->snrDb, it->squitter,
                            gs ? gs->name : "", gs ? gs->lat : 0, gs ? gs->lon : 0, gs != nullptr, h.pending());
            }
        } else if (st == 21) {   // mesh: census (Meshtastic, MeshCore, LoRa APRS, MeshCom) and the LoRaWAN frames heard there
            const auto& t = a.rx.mesh;
            if (t.timeSec < S.meshSec) S.meshSec = -1;
            std::map<std::string, std::string> roles;
            for (const auto& n : t.nodes) if (!n.role.empty()) roles[n.id] = n.role;
            for (const auto& p : t.packets) {   // oldest first
                if (p.timeSec <= S.meshSec) continue;
                S.meshSec = p.timeSec;
                const double tw = p.unixTime > 0 ? p.unixTime : now - (t.timeSec - p.timeSec);
                if (p.protocol == 5) {
                    dect2::LwHealthFrame x; x.t = tw; x.mtype = p.mtype; x.sf = p.sf; x.crcOk = p.crcOk;
                    if (p.mtype >= 2 && p.mtype <= 5) { x.devAddr = p.from; x.fcnt = (long)p.packetId; } else x.devEui = p.from;
                    f.lorawan(x, h.pending());
                    continue;
                }
                dect2::MeshCensusFrame x; x.t = tw; x.from = p.from; x.type = p.type; x.packetId = p.packetId; x.hopStart = p.hopStart; x.hopLimit = p.hopLimit;
                x.sf = p.sf; x.bwHz = p.bwHz; x.cr = p.cr; x.payloadBytes = p.size; x.preamble = p.protocol == 1 ? 16 : 8; x.crcOk = p.crcOk;
                auto r = roles.find(p.from); if (r != roles.end()) x.role = r->second;
                f.mesh(x, h.pending());
            }
        } else if (st == 201) {   // LoRaWAN mode
            const auto& lg = a.rx.lorawan.log;   // newest first
            if (!lg.empty() && lg.front().sec < S.lwSec) S.lwSec = -1;
            for (auto it = lg.rbegin(); it != lg.rend(); ++it) {
                if (it->sec <= S.lwSec) continue;
                S.lwSec = it->sec;
                if (!it->lorawan) continue;
                dect2::LwHealthFrame x; x.t = it->wallTime > 0 ? (double)it->wallTime : now; x.mtype = it->mtype; x.sf = it->fsk ? 0 : it->sf; x.crcOk = !it->hasCrc || it->crcOk;
                if (it->hasDevAddr) { char b[12]; snprintf(b, sizeof b, "%08X", (unsigned)it->devAddr); x.devAddr = b; x.fcnt = it->fcnt; }
                if (it->mtype == 0 && it->devEui) { char b[20]; snprintf(b, sizeof b, "%016llX", (unsigned long long)it->devEui); x.devEui = b; }
                f.lorawan(x, h.pending());
            }
        } else if (st == 87) {   // wireless M-Bus: only the meters marked as the user's own
            const auto mine = mineSet();
            if (mine.empty()) return;
            for (const auto& d : a.rx.wmbus.devices) {
                const std::string key = d.mfr + " " + d.id;
                if (!mine.count(key) || d.encrypted) continue;
                int64_t& seen = S.meterSeen[key];
                if (d.lastSeen <= seen) continue;
                seen = d.lastSeen;
                for (const auto& r : d.records) {
                    if (!r.numeric || r.func != 0 || r.storage != 0 || r.tariff != 0 || r.quantity.find("Volume") == std::string::npos) continue;
                    double litres;
                    if (dect2::WaterLeakCheck::litresFrom(r.number, r.value, litres)) { f.meter(key, d.lastSeen, litres, h.pending()); break; }
                }
            }
        }
    });
}

std::string heatPath() { return plat::dataDir() + "/insight_heatmap.txt"; }

int tzOffset() {
    const time_t now = time(nullptr);
    tm lt = *localtime(&now);
    tm gt = *gmtime(&now);
    gt.tm_isdst = lt.tm_isdst;
    return (int)difftime(mktime(&lt), mktime(&gt));
}

std::string utc(double t, bool date = false) {
    const time_t tt = (time_t)t;
    tm g = *gmtime(&tt);
    char b[32];
    strftime(b, sizeof b, date ? "%Y-%m-%d %H:%M" : "%H:%M", &g);
    return b;
}

void station() {
    in::live().setStation(plat::prefs().getS("ham.call", ""), plat::prefs().getS("ham.grid", ""));
}

void start() {
    S.started = true;
    S.on = plat::prefs().getB("insight.on", true);
    in::live().setTzOffset(tzOffset());
    in::live().loadHeatmap(heatPath());
    fieldLoad();
    station();
    in::live().enable(S.on);
}

void tick(App& a) {
    if (!S.started) start();
    const double now = (double)time(nullptr);
    if (S.on) fieldFeed(a, now);
    if (!S.on || now < S.nextTick) return;
    S.nextTick = now + 30;
    station();
    for (auto& i : in::live().tick(now, {a.freqMhz * 1e6})) {
        S.rows.push_back(i);
        if (S.rows.size() > 500) S.rows.pop_front();
    }
    if (now >= S.nextSave) { if (S.nextSave > 0) { in::live().saveHeatmap(heatPath()); fieldSave(); } S.nextSave = now + 600; }
}

ImVec4 sevColour(int s) { return s >= 2 ? pal::badRed() : s == 1 ? pal::warnAmber() : ImGui::GetStyleColorVec4(ImGuiCol_Text); }

void findingsTab() {
    if (S.rows.empty()) { ImGui::TextDisabled("%s", tr("Nothing yet. Findings appear here as the decoders hear things.")); return; }
    if (!ImGui::BeginTable("##insrows", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time (UTC)"), ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(tr("Area"), ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(tr("Finding"));
    ImGui::TableHeadersRow();
    for (auto it = S.rows.rbegin(); it != S.rows.rend(); ++it) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(utc(it->time, true).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(it->area.c_str());
        ImGui::TableNextColumn(); ImGui::TextColored(sevColour(it->severity), "%s", it->text.c_str());
    }
    ImGui::EndTable();
}

void hamTab(in::Hub& h) {
    if (plat::prefs().getS("ham.call", "").empty())
        ImGui::TextDisabled("%s", tr("Set your call sign and locator in the Ham radio mode to see who hears you."));
    const auto rows = h.whoHearsMe().list();
    ImGui::SeparatorText(tr("Who hears me (FT8, FT4, JS8)"));
    if (rows.empty()) ImGui::TextDisabled("%s", tr("No station has called or answered you yet."));
    else if (ImGui::BeginTable("##whm", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 220 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Call")); ImGui::TableSetupColumn(tr("Band")); ImGui::TableSetupColumn(tr("Their report"));
        ImGui::TableSetupColumn(tr("Your SNR")); ImGui::TableSetupColumn(tr("Balance")); ImGui::TableSetupColumn(tr("Distance")); ImGui::TableSetupColumn(tr("Last (UTC)"));
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.call.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.band.c_str());
            ImGui::TableNextColumn(); if (r.hasReport) ImGui::Text("%+d dB", r.theirReport); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%+.0f dB", r.mySnr);
            ImGui::TableNextColumn(); if (r.hasReport) ImGui::Text("%+d dB", r.balance()); else ImGui::TextDisabled("-");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Their report of you minus your SNR of them: above 0 they hear you better than you hear them"));
            ImGui::TableNextColumn(); if (r.distanceKm >= 0) ImGui::Text("%.0f km %s", r.distanceKm, in::compass8(r.bearing)); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(utc(r.lastTime).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText(tr("Antenna pattern from decodes"));
    const auto sec = h.antennaPattern().sectors(10);
    ImGui::TextDisabled(tr("%zu located decodes; the distance is taken out, what is left per direction:"), h.antennaPattern().size());
    for (const auto& s : sec) {
        if (s.count < 10) continue;
        ImGui::Text("%3d-%3d deg  %+5.1f dB  +/- %.1f  (%d)", s.fromDeg, s.fromDeg + 30, s.meanDb, s.stdErrDb, s.count);
    }
    ImGui::SeparatorText(tr("APRS network health"));
    int over = 0, path = 0;
    for (const auto& s : h.aprsHealth().stations()) { over += s.overBeacon; path += !s.pathIssue.empty(); }
    ImGui::Text(tr("%d stations beacon too often, %d use an obsolete or long path"), over, path);
    for (const auto& s : h.aprsHealth().stations())
        if (s.overBeacon || !s.pathIssue.empty())
            ImGui::BulletText("%s%s%s", s.call.c_str(), s.overBeacon ? (std::string("  ") + tr("every") + " " + in::AprsHealth::secText(s.medianSec)).c_str() : "",
                              s.pathIssue.empty() ? "" : ("  " + s.pathIssue).c_str());
}

void voiceTab(in::Hub& h, double now) {
    const auto tgs = h.heatmap().talkgroups();
    ImGui::SeparatorText(tr("Talkgroups: busiest hours (local time)"));
    if (tgs.empty()) ImGui::TextDisabled("%s", tr("No talkgroup calls yet (DMR, P25, NXDN, TETRA, trunked systems)."));
    else if (ImGui::BeginTable("##tgs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 220 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Talkgroup")); ImGui::TableSetupColumn(tr("Busiest")); ImGui::TableSetupColumn(tr("Next busy"));
        ImGui::TableHeadersRow();
        for (const auto& tg : tgs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(tg.c_str());
            ImGui::TableNextColumn();
            std::string b;
            for (const auto& x : h.heatmap().busiest(tg, 3)) {
                char t[48];
                snprintf(t, sizeof t, "%s%s %02d:00 (%.0f s)", b.empty() ? "" : ", ", in::weekdayName(x.first / 24), x.first % 24, x.second);
                b += t;
            }
            ImGui::TextUnformatted(b.c_str());
            ImGui::TableNextColumn();
            const double nb = h.heatmap().nextBusy(tg, now);
            if (nb > 0) ImGui::Text(tr("in %.0f h"), (nb - now) / 3600); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText(tr("Trunk load (Erlang B)"));
    for (const auto& kv : h.trunks()) {
        const auto& hrs = kv.second.hours();
        if (hrs.empty()) { ImGui::BulletText(tr("%s: %d channels seen, the first hour is not over yet"), kv.first.c_str(), kv.second.channels()); continue; }
        const auto& r = hrs.back();
        ImGui::BulletText(tr("%s, hour from %s UTC: %d calls, %.2f Erlang carried, %.2f offered, %d channels, blocking %.1f %%, peak %d at once"), kv.first.c_str(),
                          utc(r.start).c_str(), r.calls, r.carriedErl, r.offeredErl, r.channels, r.blocking * 100, r.peakConcurrent);
    }
}

void pagerTab(in::Hub& h) {
    const auto v = h.pagerPatterns().automaticList();
    ImGui::SeparatorText(tr("Automatic and test pages"));
    if (v.empty()) ImGui::TextDisabled("%s", tr("None found yet: a capcode needs seven pages of the same text at a steady pace."));
    for (const auto& x : v) ImGui::BulletText(tr("%s every %s"), x.first.c_str(), in::PagerPatterns::dur(x.second).c_str());
}

void hfTab(in::Hub& h) {
    const auto v = h.schedule().all();
    ImGui::SeparatorText(tr("Learned broadcast schedules (UTC)"));
    if (v.empty()) ImGui::TextDisabled("%s", tr("None yet: a slot is learned after decodes at the same time on three days."));
    for (const auto& l : v) {
        std::string t;
        for (int m : l.minutes) { char b[24]; snprintf(b, sizeof b, "%s%02d:%02d", t.empty() ? "" : " ", m / 60, m % 60); t += b; }
        ImGui::BulletText("%s %.1f kHz: %s", l.station.c_str(), l.freqHz / 1e3, t.c_str());
    }
}

void aviationTab(in::Hub& h, double now) {
    auto& f = h.field();
    ImGui::SeparatorText(tr("Thermals (FLARM, OGN): last 30 minutes"));
    const auto th = f.thermals().active(now, 1800);
    if (th.empty()) ImGui::TextDisabled("%s", tr("None yet: a thermal is a glider circling and climbing. They also show on the FLARM map."));
    else if (ImGui::BeginTable("##therm", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 160 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Climb")); ImGui::TableSetupColumn(tr("Altitude band")); ImGui::TableSetupColumn(tr("Gliders"));
        ImGui::TableSetupColumn(tr("Where")); ImGui::TableSetupColumn(tr("Last used (UTC)"));
        ImGui::TableHeadersRow();
        double hl, ho; const bool home = f.home(hl, ho);
        for (const auto& t : th) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.1f m/s", t.climbMs);
            ImGui::TableNextColumn(); ImGui::Text("%.0f - %.0f m", t.baseM, t.topM);
            ImGui::TableNextColumn(); ImGui::Text("%zu", t.aircraft.size());
            ImGui::TableNextColumn();
            if (home) ImGui::Text("%.1f km %s", in::distanceKm(hl, ho, t.lat, t.lon), in::compass8(in::bearingDeg(hl, ho, t.lat, t.lon)));
            else ImGui::Text("%.4f %.4f", t.lat, t.lon);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(utc(t.lastT).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText(tr("HFDL propagation: share of squitters heard per UTC hour"));
    auto& p = f.hfdl();
    const auto ids = p.stations();
    if (ids.empty()) { ImGui::TextDisabled("%s", tr("No HFDL ground station heard yet. Learned over days; kept between runs.")); }
    else {
        const int hour = dect2::HfdlPropagation::hourOf(now);
        const auto bf = p.bestFrequency(hour);
        if (bf.khz) ImGui::Text(tr("Best frequency this hour: %d kHz (%.1f stations' worth of squitters)"), bf.khz, bf.rate);
        double hl, ho; const bool home = f.home(hl, ho);
        if (ImGui::BeginTable("##hfdlp", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 220 * gUi))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("Station")); ImGui::TableSetupColumn(tr("Distance")); ImGui::TableSetupColumn(tr("Best now"));
            ImGui::TableSetupColumn(tr("00 - 23 UTC"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (int id : ids) {
                const auto* gs = dect2::hfdlStation(id);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(gs ? gs->name : std::to_string(id).c_str());
                ImGui::TableNextColumn();
                if (home && gs) ImGui::Text("%.0f km %s", in::distanceKm(hl, ho, gs->lat, gs->lon), in::compass8(in::bearingDeg(hl, ho, gs->lat, gs->lon))); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                const auto b = p.best(id, hour);
                if (b.khz) ImGui::Text("%d kHz %.0f %%", b.khz, b.rate * 100); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                // one cell per hour: the best rate on any frequency, brighter = more squitters heard
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 o = ImGui::GetCursorScreenPos();
                const float w = std::max(6.f * gUi, (ImGui::GetContentRegionAvail().x - 2) / 24.f), hh = ImGui::GetTextLineHeight();
                for (int hr = 0; hr < 24; hr++) {
                    const double rt = p.best(id, hr).rate;
                    const ImVec2 a0(o.x + hr * w, o.y), a1(o.x + (hr + 1) * w - 1, o.y + hh);
                    dl->AddRectFilled(a0, a1, rt > 0 ? IM_COL32(60, 200, 120, (int)(60 + 195 * std::min(1.0, rt))) : IM_COL32(128, 128, 128, 40));
                    if (hr == hour) dl->AddRect(a0, a1, IM_COL32(255, 255, 255, 200));
                }
                ImGui::Dummy(ImVec2(24 * w, hh));
            }
            ImGui::EndTable();
        }
    }
    ImGui::TextDisabled("%s", tr("Manoeuvre alerts (emergency squawks, steep descents and climbs, sharp turns, go-arounds) appear under Findings."));
}

void marineTab(in::Hub& h, double now) {
    auto& f = h.field();
    ImGui::SeparatorText(tr("AIS ducting"));
    auto& d = f.ducting();
    if (!d.hasHome()) ImGui::TextDisabled("%s", tr("Set the receiver position (Settings) to measure AIS ranges."));
    else {
        ImGui::Text(tr("Normal range %.0f km (%zu samples), ducting beyond %.0f km, farthest ever %.0f km"), d.normalKm(), d.learned(), d.thresholdKm(), d.recordKm());
        if (d.active()) {
            ImGui::TextColored(pal::warnAmber(), tr("Ducting now, since %s UTC"), utc(d.activeSince()).c_str());
            for (const auto& v : d.farVessels(now)) ImGui::BulletText("MMSI %09u  %.0f km", (unsigned)v.mmsi, v.km);
        } else ImGui::TextDisabled("%s", tr("No ducting now."));
    }
    ImGui::SeparatorText(tr("NAVTEX warnings in force"));
    auto& n = f.navtexInForce();
    const auto st = n.stations();
    if (st.empty()) ImGui::TextDisabled("%s", tr("None: warnings are kept from the NAVTEX messages decoded, until cancelled or expired."));
    for (char c : st) {
        if (!ImGui::TreeNodeEx((void*)(intptr_t)c, ImGuiTreeNodeFlags_DefaultOpen, "%s %c", tr("Station"), c)) continue;
        for (const auto& w : n.inForce(c)) {
            ImGui::BulletText("%s  %s", w.header.c_str(), w.title.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", w.text.c_str());
            if (w.expires) { ImGui::SameLine(); ImGui::TextDisabled(tr("until %s UTC"), utc((double)w.expires, true).c_str()); }
        }
        ImGui::TreePop();
    }
}

void iotTab(in::Hub& h, double now) {
    auto& f = h.field();
    ImGui::SeparatorText(tr("Mesh census (Meshtastic, MeshCore, LoRa APRS, MeshCom)"));
    auto& m = f.meshCensus();
    if (!m.packets()) ImGui::TextDisabled("%s", tr("No mesh packet yet."));
    else {
        ImGui::Text(tr("%zu nodes (%zu in the last hour), %llu packets, channel use %.1f %% in the last hour"), m.nodes().size(), m.activeNodes(now, 3600),
                    (unsigned long long)m.packets(), m.utilisation(now));
        std::string r; for (const auto& [k, v] : m.roles()) r += (r.empty() ? "" : ", ") + k + " " + std::to_string(v);
        ImGui::TextWrapped(tr("Roles: %s"), r.c_str());
        std::string hp; const auto& hops = m.hops(); for (int i = 0; i < 8; i++) if (hops[i]) hp += (hp.empty() ? "" : ", ") + std::to_string(i) + ": " + std::to_string(hops[i]);
        ImGui::TextWrapped(tr("Hops (packets): %s"), hp.c_str());
        std::string bh; for (const auto& [hr, c] : m.busiestHours(3)) { char b[32]; snprintf(b, sizeof b, "%s%02d:00 (%llu)", bh.empty() ? "" : ", ", hr, (unsigned long long)c); bh += b; }
        ImGui::Text(tr("Busiest hours (UTC): %s"), bh.c_str());
        if (ImGui::BeginTable("##census", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 160 * gUi))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(tr("Node")); ImGui::TableSetupColumn(tr("Role")); ImGui::TableSetupColumn(tr("Fewest hops"));
            ImGui::TableSetupColumn(tr("Packets")); ImGui::TableSetupColumn(tr("Airtime"));
            ImGui::TableHeadersRow();
            for (const auto& x : m.nodes()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(x.id.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(x.role.empty() ? "-" : x.role.c_str());
                ImGui::TableNextColumn(); if (x.minHops >= 0) ImGui::Text("%d", x.minHops); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::Text("%u", x.packets);
                ImGui::TableNextColumn(); ImGui::Text("%.1f s", x.airtimeSec);
            }
            ImGui::EndTable();
        }
    }
    ImGui::SeparatorText(tr("LoRaWAN health"));
    auto& lw = f.lorawanHealth();
    const auto hrs = lw.hours();
    if (hrs.empty()) ImGui::TextDisabled("%s", tr("No LoRaWAN frame yet (LoRaWAN mode, or the mesh mode's LoRaWAN channels)."));
    else {
        const auto& last = hrs.back();
        ImGui::Text(tr("This hour: %u join requests, %u join accepts, %u uplinks, %u downlinks"), last.joinReq, last.joinAcc, last.up, last.down);
        const auto mix = lw.sfMix(now, 24);
        std::string sm; for (int i = 0; i < 13; i++) if (mix[i] > 0.005) { char b[24]; snprintf(b, sizeof b, "%s%s%d %.0f %%", sm.empty() ? "" : ", ", i ? "SF" : "FSK", i ? i : 0, mix[i] * 100); sm += b; }
        ImGui::TextWrapped(tr("Spreading factors, last 24 h: %s"), sm.c_str());
        int shown = 0;
        for (const auto& d : lw.devices()) {
            if (d.frames + d.lost < 10 || shown >= 10) continue;
            shown++;
            ImGui::BulletText(tr("%s: %.0f %% lost (%u of %u), %u repeats, %u restarts"), d.id.c_str(), d.lossPct(), d.lost, d.frames + d.lost, d.repeats, d.restarts);
        }
    }
    ImGui::SeparatorText(tr("My water meters (wireless M-Bus)"));
    if (f.leaks().empty()) ImGui::TextDisabled("%s", tr("Mark your own meter in the Wireless M-Bus mode (Meters, select it) to check it for leaks."));
    for (auto& [k, w] : f.leaks()) {
        const int64_t day = (int64_t)std::floor(((double)now + w.tzOffsetSec) / 86400.0);
        const double base = w.baselineLph(day + 1), lastNight = w.nights().empty() ? -1 : w.nights().rbegin()->second;
        char ln[32] = "-", us[32] = "-";
        if (lastNight >= 0) snprintf(ln, sizeof ln, "%.1f L/h", lastNight);
        if (base >= 0) snprintf(us, sizeof us, "%.1f L/h", base);
        ImGui::BulletText(tr("%s: %.0f L on the register; last night %s, usual %s; %.0f h since the water last stood still"), k.c_str(), w.lastLitres(), ln, us,
                          w.hoursSinceQuiet((int64_t)now));
    }
}

}  // namespace

// the FT8 / FT4 / JS8 view hands its decodes over (hfdig_ftx_ui.cpp); only the ones newer than the last seen
void insightFeedFtx(double slotUtc, double dialHz, const std::string& msg, float snr) {
    if (!S.on || slotUtc < S.lastFtx - 300) return;   // the list is handed over every frame: older ones were seen before
    if (!S.ftxSeen.insert({slotUtc, msg}).second) return;
    if (slotUtc > S.lastFtx) {
        S.lastFtx = slotUtc;
        while (!S.ftxSeen.empty() && S.ftxSeen.begin()->first < S.lastFtx - 600) S.ftxSeen.erase(S.ftxSeen.begin());
    }
    in::live().onFtx(slotUtc, in::hamBand(dialHz), msg, snr);
}

// the pager view's "Hide automatic pages": the capcode sends automatic or test pages (the hub's key is "<protocol> <address>"); cached a few seconds
bool insightPagerAutomatic(const char* protocol, uint32_t address) {
    static std::map<std::string, bool> cache;
    static double at = 0;
    if (!S.on) return false;
    const double now = ImGui::GetTime();
    if (now - at > 3) { cache.clear(); at = now; }
    const std::string key = std::string(protocol) + " " + std::to_string(address);
    auto it = cache.find(key);
    if (it == cache.end()) {
        bool v = false;
        in::live().with([&](in::Hub& h) { v = h.pagerPatterns().automatic(key); });
        it = cache.emplace(key, v).first;
    }
    return it->second;
}

// the FT8 / FT4 / JS8 view: "" or the call this one is probably a busted copy of (one character from a call heard often); cached a few seconds
std::string insightBustedHint(const std::string& call, std::string* why) {
    static std::map<std::string, std::pair<std::string, std::string>> cache;
    static double at = 0;
    if (!S.on || call.empty() || call[0] == '<') return {};
    const double now = ImGui::GetTime();
    if (now - at > 3) { cache.clear(); at = now; }
    auto it = cache.find(call);
    if (it == cache.end()) {
        in::BustVerdict v;
        in::live().with([&](in::Hub& h) { v = h.bustedCalls().check(call); });
        it = cache.emplace(call, std::make_pair(v.likely, v.why)).first;   // only a likely call counts: FT8 messages hold non-call words too
    }
    if (why) *why = it->second.second;
    return it->second.first;
}

void insightWindow(App& a) {
    tick(a);
    d40Tick(a);   // 4.0 features wired into the mode screens (app/d40, docs/WIRING-4.0.md)
    smart5Tick(a);   // the round-5 engines of broadcast, TV, satellites, GNSS and science (app/smart5), whatever screen is open
    if (!a.insightOpen) return;
    firstWindowSize(760 * gUi, 460 * gUi);
    if (!ImGui::Begin(trw("Insights"), &a.insightOpen)) { ImGui::End(); return; }
    const double now = (double)time(nullptr);
    if (ImGui::BeginTabBar("##instabs")) {
        if (ImGui::BeginTabItem(trw("Findings"))) { findingsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Ham"))) { in::live().with([](in::Hub& h) { hamTab(h); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Voice"))) { in::live().with([now](in::Hub& h) { voiceTab(h, now); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Pagers"))) { in::live().with([](in::Hub& h) { pagerTab(h); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("HF utility"))) { in::live().with([](in::Hub& h) { hfTab(h); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Aviation"))) { in::live().with([now](in::Hub& h) { aviationTab(h, now); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Marine"))) { in::live().with([now](in::Hub& h) { marineTab(h, now); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("IoT"))) { in::live().with([now](in::Hub& h) { iotTab(h, now); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Broadcast and TV"))) { smart5InsightBroadcast(a); smart5InsightTv(a); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Satellites and science"))) { smart5InsightSat(a); smart5InsightSci(a); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    if (ImGui::CollapsingHeader(trw("Advanced"))) {
        if (ImGui::Checkbox(tr("Analyse decodes"), &S.on)) { plat::prefs().setB("insight.on", S.on); in::live().enable(S.on); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Who hears me, antenna pattern, APRS health, talkgroup activity, trunk load, automatic pages, overdue broadcasts, aircraft manoeuvres, thermals, HFDL propagation, AIS ducting, NAVTEX warnings, mesh census, LoRaWAN health, my water meter. All of it stays on this computer."));
        ImGui::SameLine();
        if (ImGui::Button(tr("Clear findings"))) S.rows.clear();
    }
    ImGui::End();
}

bool wmbusIsMine(const std::string& meterKey) { return mineSet().count(meterKey) > 0; }
void wmbusSetMine(const std::string& meterKey, bool mine) {
    auto m = mineSet();
    if (mine) m.insert(meterKey); else m.erase(meterKey);
    std::string t; for (const auto& k : m) t += k + "\n";
    plat::prefs().setS("wmbusMine", t);
    plat::prefs().flush();
}
