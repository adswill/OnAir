// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "station/station_loc.h"
#include "ease/ease_ui.h"
// View > Data feeds: send what the decoders find to other programs (tar1090 / readsb / VRS, OpenCPN, APRS-IS, acarshub).
// Every feed is off until the user turns it on; the network work is in core (feeds.cpp).
#include "../app.h"
#include "../ui/typography.h"
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <ctime>

void sondeNmeaControls();   // sonde_nmea_ui.cpp

namespace {
const char* kKeys[dect2::kNumFeeds] = {"feedSbs", "feedBeast", "feedAis", "feedAprsIs", "feedKiss", "feedAcars", "feedVdl2", "feedGnss", "feedBle", "feedLora"};

void apply(App& a) {
    dect2::Feeds::get().configure(a.feeds);
    savePrefs(a);
}

// a port field; true when the edit is finished
bool portField(const char* label, int& port) {
    ImGui::SetNextItemWidth(90 * gUi);
    int p = port;
    ImGui::InputInt(tr(label), &p, 0, 0);
    port = std::max(1, std::min(65535, p));
    return ImGui::IsItemDeactivatedAfterEdit();
}

bool textField(const char* label, std::string& s, ImGuiInputTextFlags fl = 0, float w = 200) {
    char b[128];
    snprintf(b, sizeof b, "%s", s.c_str());
    ImGui::SetNextItemWidth(w * gUi);
    if (ImGui::InputText(tr(label), b, sizeof b, fl)) s = b;
    return ImGui::IsItemDeactivatedAfterEdit();
}

void statusLine(int id, bool server) {
    const dect2::FeedStatus s = dect2::Feeds::get().status(id);
    ImGui::Indent();
    if (s.state != "off") {
        const ImVec4 col = !s.lastError.empty() ? pal::warnAmber() : s.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        if (server) ImGui::TextColored(col, tr("%s, %d client%s, %llu sent, %llu dropped"), s.state.c_str(), s.clients, s.clients == 1 ? "" : "s", (unsigned long long)s.sent, (unsigned long long)s.dropped);
        else ImGui::TextColored(col, tr("%s, %llu sent, %llu dropped"), s.state.c_str(), (unsigned long long)s.sent, (unsigned long long)s.dropped);
        if (!s.detail.empty()) ImGui::TextDisabled("%s", s.detail.c_str());
        if (!s.lastError.empty()) { ImGui::TextColored(pal::badRed(), "%s", s.lastError.c_str()); }
    }
    ImGui::Unindent();
}

// ---- the Data feeds window: everything that SENDS data, in five places (tabs of a side list) ----
enum Lvl { kOff, kWarn, kOk, kBad };

// a small status chip: grey off, amber waiting, green working, red failed
void chip(Lvl l, const char* text) {
    const ImVec4 col = l == kOk ? pal::okGreen() : l == kWarn ? pal::warnAmber() : l == kBad ? pal::badRed() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const ImVec2 ts = ImGui::CalcTextSize(text), pad(6 * gUi, 1 * gUi);
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.f, ImGui::GetStyle().FramePadding.y - pad.y));
    const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + ts.x + 2 * pad.x, p.y + ts.y + 2 * pad.y);
    ImGui::Dummy(ImVec2(q.x - p.x, q.y - p.y));
    ImVec4 bg = col; bg.w = 0.22f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, ImGui::GetColorU32(bg), 8 * gUi);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::GetColorU32(col), text);
}

Lvl levelOf(bool on, bool working, bool hasErr) {
    if (!on) return kOff;
    if (hasErr) return working ? kWarn : kBad;
    return working ? kOk : kWarn;
}
const char* chipText(bool on, bool hasErr, bool working, const std::string& state) {
    if (!on) return tr("off");
    if (hasErr && !working) return tr("error");
    return state.empty() ? tr("on") : state.c_str();
}

// "settings" expander of a row: draws `body` indented when open
template <class F> void settingsExpander(const char* id, F body) {
    ImGui::Indent();
    ImGui::PushID(id);
    if (ImGui::TreeNode(trw("settings##x"))) { body(); ImGui::TreePop(); }
    ImGui::PopID();
    ImGui::Unindent();
}

// one line of counters / last result under a row
void resultLine(const std::string& text, const std::string& err) {
    ImGui::Indent();
    if (!text.empty()) ImGui::TextDisabled("%s", text.c_str());
    if (!err.empty()) ImGui::TextColored(pal::badRed(), "%s", err.c_str());
    ImGui::Unindent();
}
std::string fmtS(const char* fmt, const std::string& st, unsigned long long a, unsigned long long b = 0, unsigned long long c = 0, unsigned long long d = 0) {
    char buf[256]; snprintf(buf, sizeof buf, fmt, st.c_str(), a, b, c, d); return buf;
}
std::string lastSendText(int64_t t) {
    char b[32], o[80]; snprintf(b, sizeof b, "%s", dect2::timefmt::clock(t).c_str()); snprintf(o, sizeof o, tr("last send %s"), b); return o;
}
std::string fmtU(const char* fmt, unsigned long long a, unsigned long long b = 0, unsigned long long c = 0, unsigned long long d = 0) {
    char buf[256]; snprintf(buf, sizeof buf, fmt, a, b, c, d); return buf;
}

// Aggregator targets for one kind of data (ADS-B or AIS): each row off until its own box is ticked
void targetRows(dect2::FeedConfig& c, bool ais, bool& changed) {
    using dect2::FeedKind;
    ImGui::PushID(ais ? "tgtAis" : "tgtAdsb");
    for (size_t i = 0; i < c.targets.size(); i++) {
        dect2::FeedTarget& t = c.targets[i];
        if ((t.kind == FeedKind::AisNmea) != ais) continue;
        ImGui::PushID((int)i);
        const dect2::FeedTargetStatus s = dect2::FeedTargets::get().status(i);
        const bool err = !s.lastError.empty();
        changed |= ImGui::Checkbox(t.name.c_str(), &t.enabled);
        chip(levelOf(t.enabled, s.connected, err), chipText(t.enabled, err, s.connected, s.state));
        if (t.enabled) resultLine(fmtS(tr("%s, %llu sent, %llu dropped, %llu reconnects"), s.state, s.sent, s.dropped, s.reconnects), s.lastError);
        bool removed = false;
        settingsExpander("tgtset", [&] {
            changed |= textField(N_("Host"), t.host, 0, 160);
            changed |= portField(N_("Port"), t.port);
            if (t.proto == dect2::FeedProto::Tcp) changed |= textField(N_("Key / id line (optional)"), t.key, ImGuiInputTextFlags_Password, 160);
            if (ImGui::SmallButton(tr("Remove"))) { c.targets.erase(c.targets.begin() + (long)i); changed = true; removed = true; }
        });
        ImGui::PopID();
        if (removed) break;
    }
    if (ImGui::BeginCombo("##add", tr("Add a target..."))) {
        for (const dect2::FeedPreset& p : dect2::feedPresets()) {
            if ((p.kind == FeedKind::AisNmea) != ais) continue;
            if (ImGui::Selectable(p.name)) {
                dect2::FeedTarget t; t.name = p.name; t.kind = p.kind; t.proto = p.proto; t.host = p.host; t.port = p.port;
                c.targets.push_back(t); changed = true;
            }
            if (ImGui::IsItemHovered() && p.note[0]) ImGui::SetTooltip("%s", p.note);
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
}

// SondeHub (tracker.sondehub.org): the fixed first entry of the radiosonde upload list, off until ticked. Not APRS-IS: HTTPS uploads of
// the SondeHub telemetry format (core: dect2/net/sondehub.h, docs/SONDEHUB-4.0.md). The panel shows what was sent last.
std::string firstObject(const std::string& arr) {   // the first object of the uploaded array, and how many more
    if (arr.size() < 2) return {};
    const size_t e = arr.find("},{\"software_name\"");
    if (e == std::string::npos) return arr.substr(1, arr.size() - 2);
    size_t n = 0; for (size_t p = e; (p = arr.find("},{\"software_name\"", p)) != std::string::npos; p++) n++;
    return arr.substr(1, e) + "   (+" + std::to_string(n) + " " + tr("more frames like it") + ")";
}
void sondeHubRow(dect2::FeedConfig& c, bool& changed) {
    dect2::SondeHubConfig& h = c.sondeHub;
    if (!dect2::sondehub::allowed()) {   // 4.2: switched off until SondeHub has validated the decoders (issue #526); code kept, developers use ONAIR_SONDEHUB=1
        h.enabled = false;
        ImGui::BeginDisabled();
        bool no = false;
        ImGui::Checkbox(tr("SondeHub (tracker.sondehub.org)"), &no);
        ImGui::EndDisabled();
        chip(kOff, tr("off by default, see note"));
        ImGui::Indent();
        ImGui::TextDisabled("%s", tr("SondeHub upload is switched off until SondeHub has validated OnAir's decoders (see issue #526)"));
        ImGui::Unindent();
        return;
    }
    ImGui::PushID("sondehub");
    const dect2::SondeHubStatus s = dect2::SondeHub::get().status();
    const bool err = !s.lastError.empty();
    if (ImGui::Checkbox(tr("SondeHub (tracker.sondehub.org)"), &h.enabled)) {
        changed = true;
        if (h.enabled && h.call.empty()) { h.call = c.aprsCall; if (h.call.empty() && !c.sondeTargets.empty()) h.call = c.sondeTargets.front().call; }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The SondeHub database and map. Not APRS-IS: OnAir sends the frames over HTTPS in SondeHub's own format, every 15 s."));
    chip(levelOf(h.enabled, s.connected, err), chipText(h.enabled, err, s.connected, s.state));
    if (h.enabled) {
        resultLine(fmtS(tr("%s, %llu frames uploaded, %llu station uploads, %llu dropped, %llu retries"), s.state, s.sent, s.beacons, s.dropped, s.reconnects), s.lastError);
        ImGui::Indent();
        if (s.skipped) ImGui::TextDisabled(tr("%llu frames kept back (last: %s)"), (unsigned long long)s.skipped, s.skipWhy.c_str());
        if (!s.lastLine.empty()) ImGui::TextDisabled("%s %s", tr("last upload:"), s.lastLine.c_str());
        ImGui::Unindent();
    }
    settingsExpander("shset", [&] {
        changed |= textField(N_("Uploader callsign"), h.call, 0, 140);
        changed |= textField(N_("Antenna (optional)"), h.antenna, 0, 180);
        changed |= ImGui::Checkbox(tr("Send my station position (shown on the SondeHub map)"), &h.sharePos);
        ImGui::BeginDisabled(!h.sharePos);
        changed |= ImGui::Checkbox(tr("Chase car: send my moving GPS position every 30 s"), &h.mobile);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Needs a live GPS position (Radiosonde map > My position > GPS). Without one the chase position is not sent."));
        ImGui::EndDisabled();
        if (!h.call.empty() && !dect2::sondehub::validUploader(h.call)) ImGui::TextColored(pal::warnAmber(), "%s", tr("The uploader callsign needs 3 to 64 characters."));
        if (h.enabled && ImGui::TreeNode(trw("What is sent##sondehubjson"))) {
            const std::string lj = dect2::SondeHub::get().lastListenerJson(), tj = dect2::SondeHub::get().lastJson();
            ImGui::TextDisabled("%s", tr("Station record (PUT /listeners):"));
            ImGui::TextUnformatted(lj.empty() ? tr("(not sent yet)") : lj.c_str());
            if (!s.lastListenerLine.empty()) ImGui::TextDisabled("%s", s.lastListenerLine.c_str());
            ImGui::TextDisabled("%s", tr("Last telemetry upload (PUT /sondes/telemetry, gzip-compressed):"));
            const std::string fo = firstObject(tj);
            ImGui::TextUnformatted(fo.empty() ? tr("(not sent yet)") : fo.c_str());
            if (!s.lastLine.empty()) ImGui::TextDisabled("%s", s.lastLine.c_str());
            ImGui::TextDisabled("%s", tr("SondeHub's last answer:"));
            ImGui::TextUnformatted(s.lastReply.empty() ? tr("(none yet)") : s.lastReply.c_str());
            ImGui::TreePop();
        }
        if (ImGui::TreeNode(trw("Advanced##sondehubadv"))) {
            ImGui::SetNextItemWidth(120 * gUi);
            changed |= ImGui::SliderInt(tr("upload every (s)##sondehub"), &h.uploadS, 10, 60);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("SondeHub's own uploader uses 15 s. While a DFM or Meisei sonde is heard the wait is 1.5 times longer (at most 30 s) so SondeHub's checks get enough points."));
            changed |= textField(N_("Contact e-mail (optional, only the SondeHub admins see it)"), h.email, 0, 180);
            changed |= textField(N_("Server##sondehubsrv"), h.server, 0, 220);
            ImGui::TreePop();
        }
        ImGui::TextDisabled("%s", tr("Privacy: SondeHub publishes your uploader callsign and, with the box ticked, your station position on tracker.sondehub.org; "
                                     "the sonde data become part of its open database (CC BY-SA). Only frames with a GPS fix and the sonde's own time are sent; iMet-1/4 are not."));
    });
    ImGui::PopID();
}

// Radiosonde upload to APRS-IS aggregators: a list of targets, each off until its own box is ticked
void sondeUploadRows(dect2::FeedConfig& c, bool& changed) {
    ImGui::PushID("sondeup");
    ImGui::TextDisabled("%s", tr("Sends every decoded radiosonde (position, speed, altitude, climb rate, temperature, humidity, pressure, frequency, type) to SondeHub and to APRS-IS sites such as wettersonde.net "
                                 "and radiosondy.info, after a station beacon with your location. Each target is off until you tick it."));
    sondeHubRow(c, changed);
    for (size_t i = 0; i < c.sondeTargets.size(); i++) {
        dect2::SondeUploadTarget& t = c.sondeTargets[i];
        ImGui::PushID((int)i);
        const dect2::SondeUploadStatus s = dect2::SondeUpload::get().status(i);
        const bool err = !s.lastError.empty();
        changed |= ImGui::Checkbox(t.name.c_str(), &t.enabled);
        chip(levelOf(t.enabled, s.connected, err), chipText(t.enabled, err, s.connected, s.state));
        if (t.enabled) {
            resultLine(fmtS(tr("%s, %llu packets forwarded, %llu beacons, %llu dropped, %llu reconnects"), s.state, s.sent, s.beacons, s.dropped, s.reconnects), s.lastError);
            if (!s.lastLine.empty()) { ImGui::Indent(); ImGui::TextDisabled("%s %s", tr("last line:"), s.lastLine.c_str()); ImGui::Unindent(); }
        }
        bool removed = false;
        settingsExpander("supset", [&] {
            changed |= textField(N_("Host"), t.host, 0, 160);
            changed |= portField(N_("Port"), t.port);
            changed |= textField(N_("Callsign-SSID"), t.call, ImGuiInputTextFlags_CharsUppercase, 120);
            changed |= textField(N_("Passcode"), t.pass, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsDecimal, 120);
            ImGui::SetNextItemWidth(90 * gUi);
            changed |= ImGui::SliderInt(tr("beacon every (min)"), &t.beaconMin, 1, 120);
            if (!t.call.empty() && !dect2::aprsValidCallsign(t.call)) ImGui::TextColored(pal::warnAmber(), "%s", tr("Not a valid callsign (like DG9BFC-10)."));
            if (ImGui::SmallButton(tr("Remove"))) { c.sondeTargets.erase(c.sondeTargets.begin() + (long)i); changed = true; removed = true; }
        });
        ImGui::PopID();
        if (removed) break;
    }
    if (ImGui::BeginCombo("##addsonde", tr("Add a target..."))) {
        for (const dect2::SondeUploadPreset& p : dect2::sondeUploadPresets()) {
            if (ImGui::Selectable(p.name)) {
                dect2::SondeUploadTarget t; t.name = p.name; t.host = p.host; t.port = p.port;
                if (!c.aprsCall.empty()) t.call = c.aprsCall;
                c.sondeTargets.push_back(t); changed = true;
            }
            if (ImGui::IsItemHovered() && p.note[0]) ImGui::SetTooltip("%s", p.note);
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("%s", tr("Privacy: the sites publish your station position and callsign. Set your location in View > Map first. Your passcode stays in your settings only. "
                                 "M10 and M20 sondes are not sent (no agreed APRS name for them)."));
    ImGui::PopID();
}

// a local feed row: [x] name (chip), then host and port, then the counters
void programRow(dect2::FeedConfig& c, const char* id, const char* label, int feed, bool server, bool& changed, std::string* host, int& port) {
    ImGui::PushID(id);
    const dect2::FeedStatus s = dect2::Feeds::get().status(feed);
    const bool err = !s.lastError.empty();
    changed |= ImGui::Checkbox(label, &c.on[feed]);
    chip(levelOf(c.on[feed], s.active, err), chipText(c.on[feed], err, s.active, s.state));
    ImGui::Indent();
    if (host) { changed |= textField(N_("Host"), *host, 0, 120); ImGui::SameLine(); }
    changed |= portField(N_("Port"), port);
    ImGui::Unindent();
    statusLine(feed, server);
    ImGui::PopID();
}

// ---- the five tabs ----
int gTab = kFeedsTabUpload;
bool gTabForce = false;

void tabUpload(App& a, dect2::FeedConfig& c, bool& changed) {
    ImGui::TextDisabled("%s", tr("Everything that sends what you receive to a website or service. Each one is off until you tick it. Your callsign and location are in My station."));
    typo::separatorText(tr("APRS-IS"));
    {
        ImGui::PushID("aprsis");
        const dect2::FeedStatus s = dect2::Feeds::get().status(dect2::kFeedAprsIs);
        const bool err = !s.lastError.empty();
        changed |= ImGui::Checkbox(tr("APRS-IS (receive-only igate)"), &c.on[dect2::kFeedAprsIs]);
        chip(levelOf(c.on[dect2::kFeedAprsIs], s.active, err), chipText(c.on[dect2::kFeedAprsIs], err, s.active, s.state));
        statusLine(dect2::kFeedAprsIs, false);
        settingsExpander("aprsset", [&] {
            changed |= textField(N_("Callsign"), c.aprsCall, ImGuiInputTextFlags_CharsUppercase, 120);
            changed |= textField(N_("Passcode"), c.aprsPass, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsDecimal, 120);
            changed |= textField("##server", c.aprsServer, 0, 160);
            ImGui::SameLine(); changed |= portField(N_("Server"), c.aprsServerPort);
            ImGui::TextDisabled("%s", tr("Receive-only: packets heard on the air go to APRS-IS, nothing from the Internet is ever sent out on the air. Your passcode stays in your settings only."));
            if (!c.aprsCall.empty() && !dect2::aprsValidCallsign(c.aprsCall)) ImGui::TextColored(pal::warnAmber(), "%s", tr("Not a valid callsign (like LY1ABC or LY1ABC-10)."));
            if (ImGui::TreeNode(trw("Advanced##aprsgate"))) {
                changed |= ImGui::SliderInt(tr("packets per minute, all"), &c.aprsGate.maxPerMin, 0, 600, c.aprsGate.maxPerMin ? "%d" : "no limit");
                changed |= ImGui::SliderInt(tr("packets per minute, per station"), &c.aprsGate.perStationPerMin, 0, 60, c.aprsGate.perStationPerMin ? "%d" : "no limit");
                changed |= ImGui::SliderInt(tr("same packet again after (s)"), &c.aprsGate.dedupSec, 0, 300, c.aprsGate.dedupSec ? "%d" : "off");
                ImGui::TreePop();
            }
        });
        ImGui::PopID();
    }

    typo::separatorText(tr("PSK Reporter"));
    {
        dect2::PskReporterConfig& r = a.pskr;
        bool pc = easeui::stationFillOnce(a, "stationFilled.pskr", r.call, r.locator);
        const dect2::PskReporterStatus s = dect2::PskReporter::get().status();
        const bool err = !s.lastError.empty();
        pc |= ImGui::Checkbox(tr("Send spots to PSK Reporter (pskreporter.info)"), &r.on);
        chip(levelOf(r.on, s.active, err), chipText(r.on, err, s.active, s.state));
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(380 * gUi);
            ImGui::TextUnformatted(tr("Sends the FT8, FT4, FT2, WSPR, JS8Call, PSK31 and CW stations you decode to pskreporter.info, so they show on its public map and statistics. "
                "A report is: the sender's callsign, frequency, SNR, mode, its locator when the message has one, and when it was heard; with your callsign, locator, antenna text and 'OnAir'. "
                "Reports are batched and sent every 5 minutes; the same station on the same band is sent once per 10 minutes. Nothing is sent until you tick the box. "
                "Spots are public: use your own valid amateur radio callsign. CW and PSK31 are only reported when the text says 'DE callsign'."));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (r.on) {
            const std::string last = s.lastSend ? "  " + lastSendText((int64_t)s.lastSend) : std::string();
            resultLine(fmtS(tr("%s, %llu spots sent in %llu packets, %llu duplicates skipped"), s.state, s.spotsSent, s.packets, s.duplicates) + last, s.lastError);
        }
        ImGui::PushID("pskr");
        settingsExpander("pskrset", [&] {
            pc |= textField(N_("Your callsign"), r.call, ImGuiInputTextFlags_CharsUppercase, 120);
            pc |= stationloc::locatorRow(N_("Your locator"), r.locator, "pskrLocatorOwn", 90);
            pc |= textField(N_("Antenna"), r.antenna, 0, 200);
            ImGui::TextDisabled("%s", tr("Spots are public. A valid amateur radio callsign is required; the dial frequency is the tuned frequency."));
        });
        ImGui::PopID();
        if (pc) { dect2::PskReporter::get().configure(r); savePrefs(a); }
    }

    typo::separatorText(tr("WSPRnet"));
    {
        dect2::WsprNetConfig& wn = a.wsprNet;
        bool wc = easeui::stationFillOnce(a, "stationFilled.wspr", wn.call, wn.locator);
        const dect2::WsprNetStatus ws = dect2::WsprNet::get().status();
        const bool err = !ws.lastError.empty();
        wc |= ImGui::Checkbox(tr("Send WSPR spots to WSPRnet (wsprnet.org)"), &wn.on);
        chip(levelOf(wn.on, ws.active, err), chipText(wn.on, err, ws.active, ws.state));
        ImGui::SameLine(); ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(380 * gUi);
            ImGui::TextUnformatted(tr("Sends WSPR stations you decode to wsprnet.org (HTTP), so they appear on the WSPR map and database. "
                "Each spot includes the sender's callsign, frequency, SNR, grid and power; with your callsign and locator. "
                "Uploads are queued with a short pause between posts; the same station in the same two-minute slot is sent once. "
                "Nothing is sent until you tick the box. Spots are public: use your own valid amateur radio callsign."));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (wn.on) {
            const std::string last = ws.lastSend ? "  " + lastSendText((int64_t)ws.lastSend) : std::string();
            resultLine(fmtS(tr("%s, %llu spots sent, %llu duplicates skipped, %llu failed"), ws.state, ws.spotsSent, ws.duplicates, ws.failures) + last, ws.lastError);
        }
        ImGui::PushID("wspr");
        settingsExpander("wsprset", [&] {
            wc |= textField(N_("Your callsign"), wn.call, ImGuiInputTextFlags_CharsUppercase, 120);
            wc |= stationloc::locatorRow(N_("Your locator"), wn.locator, "wsprNetLocatorOwn", 90);
            ImGui::TextDisabled("%s", tr("WSPR decodes only. Spots are public. A valid amateur radio callsign and locator are required."));
        });
        ImGui::PopID();
        if (wc) { dect2::WsprNet::get().configure(wn); savePrefs(a); }
    }

    typo::separatorText(tr("Radiosonde upload (SondeHub, APRS-IS)"));
    sondeUploadRows(c, changed);

    typo::separatorText(tr("ADS-B aggregators"));
    ImGui::TextDisabled("%s", tr("Send to aggregators (each target is off until you tick it, with your own account details)"));
    targetRows(c, false, changed);
    typo::separatorText(tr("AIS aggregators"));
    targetRows(c, true, changed);
    ImGui::TextDisabled("%s", tr("Privacy: aggregators publish what you send, and they know your station's position from your account there. Beast and NMEA data carry no key of yours; "
                                 "enter only the address and key your own account gave you. Nothing is sent until you tick a target."));
}

void tabPrograms(dect2::FeedConfig& c, bool& changed) {
    ImGui::TextDisabled("%s", tr("Sends decoded data to other programs while the mode runs. Every feed is off until you turn it on."));
    changed |= ImGui::Checkbox(tr("Let other computers connect to the TCP feeds (LAN)"), &c.lan);
    typo::separatorText(tr("ADS-B"));
    programRow(c, "sbs", tr("SBS-1 BaseStation (TCP server)"), dect2::kFeedSbs, true, changed, nullptr, c.sbsPort);
    programRow(c, "beast", tr("Beast binary (TCP server)"), dect2::kFeedBeast, true, changed, nullptr, c.beastPort);
    typo::separatorText(tr("AIS"));
    programRow(c, "ais", tr("NMEA !AIVDM (UDP)"), dect2::kFeedAis, false, changed, &c.aisHost, c.aisPort);
    typo::separatorText(tr("ACARS and VDL2 (JSON)"));
    programRow(c, "acars", tr("ACARS, acarsdec format (UDP)"), dect2::kFeedAcars, false, changed, &c.acarsHost, c.acarsPort);
    programRow(c, "vdl2", tr("VDL2, dumpvdl2 format (UDP)"), dect2::kFeedVdl2, false, changed, &c.vdl2Host, c.vdl2Port);
    changed |= textField(N_("Station ID"), c.stationId, 0, 120);
    typo::separatorText(tr("GNSS, Bluetooth LE, LoRa (UDP JSON / NMEA)"));
    programRow(c, "gnss", tr("GNSS NMEA RMC/GGA (UDP)"), dect2::kFeedGnss, false, changed, &c.gnssHost, c.gnssPort);
    programRow(c, "ble", tr("Bluetooth LE advertisements, JSON (UDP)"), dect2::kFeedBle, false, changed, &c.bleHost, c.blePort);
    programRow(c, "lora", tr("LoRa / LoRaWAN / Meshtastic, JSON (UDP)"), dect2::kFeedLora, false, changed, &c.loraHost, c.loraPort);
    ImGui::TextDisabled("%s", tr("JSON lines (one object a line) unless GNSS, which is NMEA. Off until you turn a feed on. Only modes that already have structured messages are sent."));
    typo::separatorText(tr("KISS (packet radio)"));
    programRow(c, "kiss", tr("KISS (TCP server)"), dect2::kFeedKiss, true, changed, nullptr, c.kissPort);
}

void tabShare(App& a) {
    typo::separatorText(tr("Share my radio (rtl_tcp server)"));
    dect2::ShareConfig& sc = a.share;
    bool sch = false;
    const dect2::ShareStatus ss = dect2::ShareServer::get().status();
    sch |= ImGui::Checkbox(tr("Share the radio's samples as an rtl_tcp server"), &sc.on);
    chip(!sc.on ? kOff : !ss.listening ? (ss.lastError.empty() ? kWarn : kBad) : kOk, !sc.on ? tr("off") : ss.listening ? tr("listening") : ss.lastError.empty() ? tr("starting") : tr("error"));
    ImGui::Indent();
    sch |= portField(N_("Port"), sc.port);
    sch |= ImGui::Checkbox(tr("Let other computers connect (LAN)"), &sc.lan);
    sch |= ImGui::Checkbox(tr("Allow remote control (frequency and gain)"), &sc.allowControl);
    if (sc.on && !ss.listening && !ss.lastError.empty()) ImGui::TextColored(pal::badRed(), "%s", ss.lastError.c_str());
    else if (sc.on && ss.listening) {
        ImGui::TextColored(pal::okGreen(), tr("listening on %s port %d, %d client(s), %.1f MB sent, %.1f MB dropped"), sc.lan ? "all addresses" : "127.0.0.1", sc.port,
                           (int)ss.clients.size(), (double)ss.sentBytes / 1e6, (double)ss.droppedBytes / 1e6);
        for (const std::string& cl : ss.clients) ImGui::BulletText("%s", cl.c_str());
    }
    ImGui::TextDisabled("%s", tr("A second OnAir (Add network radio, rtl_tcp), SDR# or SDR++ connects to this computer's address and the port. Samples are sent as 8-bit IQ at the rate the radio runs at. "
                                 "Without 'allow remote control' the clients cannot change the frequency or gain."));
    ImGui::Unindent();
    if (sch) { dect2::ShareServer::get().configure(sc); savePrefs(a); }
    remoteApiSection(a);
    mcpSection(a);
}

void tabStation(App& a, dect2::FeedConfig& c, bool& changed) {
    ImGui::TextDisabled("%s", tr("Your callsign, location and antenna height in one place. The uploads, the maps and the sonde look angles use them."));
    typo::separatorText(tr("Callsign"));
    changed |= textField(N_("My callsign"), c.aprsCall, ImGuiInputTextFlags_CharsUppercase, 140);
    if (ImGui::SmallButton(tr("Use it for all upload targets"))) {
        const std::string full = c.aprsCall;
        const size_t d = full.find('-');
        const std::string base = d == std::string::npos ? full : full.substr(0, d);
        a.pskr.call = base; a.wsprNet.call = base; c.sondeHub.call = full;
        for (dect2::SondeUploadTarget& t : c.sondeTargets) t.call = full;
        dect2::PskReporter::get().configure(a.pskr); dect2::WsprNet::get().configure(a.wsprNet);
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Copies this callsign into PSK Reporter, WSPRnet (without the -SSID), SondeHub and the radiosonde APRS-IS targets. The APRS-IS passcodes stay as they are."));
    if (!c.aprsCall.empty() && !dect2::aprsValidCallsign(c.aprsCall)) ImGui::TextColored(pal::warnAmber(), "%s", tr("Not a valid callsign (like LY1ABC or LY1ABC-10)."));
    typo::separatorText(tr("Location"));
    if (receiverLocationSet()) {
        double la = 0, lo = 0; receiverLocation(la, lo);
        ImGui::Text("%s  (%.5f, %.5f)", stationloc::locator().c_str(), la, lo);
    } else ImGui::TextDisabled("%s", tr("No location set yet."));
    {
        static char buf[64] = {};
        ImGui::SetNextItemWidth(200 * gUi);
        if (ImGui::InputText(tr("locator or latitude, longitude##stnloc"), buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) { if (stationloc::setFromText(a, buf)) buf[0] = 0; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A Maidenhead locator (JN55wi) or coordinates (54.68, 25.28). Press Enter."));
    }
    if (ImGui::Button(tr("Set location from the map"))) a.mapOpen = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("Location settings..."))) settingsOpenPage("Location");
    typo::separatorText(tr("Antenna height"));
    {   // the antenna height: the station height of the SondeHub and APRS uploads and of the sonde look angles (shared with the ADS-B position)
        double h = receiverAltitudeM();
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputDouble(tr("Own height (m)##sondeown"), &h, 0, 0, "%.0f m", ImGuiInputTextFlags_EnterReturnsTrue)) setReceiverAltitudeM(a, h);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Set your antenna height above sea level, in metres. It goes into the station position of the uploads and is used for the line-of-sight angle to a sonde. Press Enter."));
        if (h == 0 && receiverLocationSet()) ImGui::TextDisabled("%s", tr("Height is 0 m: set your antenna height above sea level."));
    }
}

void tabNmea() {
    ImGui::TextDisabled("%s", tr("For a map or navigation program that takes a GPS: the sonde chosen in the radiosonde list is sent as GPS sentences."));
    sondeNmeaControls();
}

const char* tabName(int i) {
    switch (i) {
        case kFeedsTabUpload: return N_("Upload to websites");
        case kFeedsTabPrograms: return N_("Send to programs");
        case kFeedsTabShare: return N_("Share my radio");
        case kFeedsTabStation: return N_("My station");
        default: return N_("Serial / NMEA output");
    }
}
} // namespace

void feedsOpenTab(App& a, int tab) {
    a.feedsOpen = true;
    gTab = std::max(0, std::min(kFeedsTabCount - 1, tab));
    gTabForce = true;
}

void loadFeedPrefs(App& a) {
    plat::Prefs& d = plat::prefs();
    dect2::FeedConfig& c = a.feeds;
    for (int i = 0; i < dect2::kNumFeeds; i++) c.on[i] = d.getB(kKeys[i], false);
    c.sbsPort = (int)d.getI("feedSbsPort", c.sbsPort);
    c.lan = d.getI("feedLan", 0) != 0;
    c.beastPort = (int)d.getI("feedBeastPort", c.beastPort);
    c.kissPort = (int)d.getI("feedKissPort", c.kissPort);
    c.aisHost = d.getS("feedAisHost", c.aisHost); c.aisPort = (int)d.getI("feedAisPort", c.aisPort);
    c.acarsHost = d.getS("feedAcarsHost", c.acarsHost); c.acarsPort = (int)d.getI("feedAcarsPort", c.acarsPort);
    c.vdl2Host = d.getS("feedVdl2Host", c.vdl2Host); c.vdl2Port = (int)d.getI("feedVdl2Port", c.vdl2Port);
    c.gnssHost = d.getS("feedGnssHost", c.gnssHost); c.gnssPort = (int)d.getI("feedGnssPort", c.gnssPort);
    c.bleHost = d.getS("feedBleHost", c.bleHost); c.blePort = (int)d.getI("feedBlePort", c.blePort);
    c.loraHost = d.getS("feedLoraHost", c.loraHost); c.loraPort = (int)d.getI("feedLoraPort", c.loraPort);
    c.aprsCall = d.getS("feedAprsCall", ""); c.aprsPass = d.getS("feedAprsPass", "");
    c.aprsServer = d.getS("feedAprsServer", c.aprsServer); c.aprsServerPort = (int)d.getI("feedAprsServerPort", c.aprsServerPort);
    c.aprsGate.maxPerMin = (int)d.getI("feedAprsMaxMin", c.aprsGate.maxPerMin); c.aprsGate.perStationPerMin = (int)d.getI("feedAprsStationMin", c.aprsGate.perStationPerMin);
    c.aprsGate.dedupSec = (int)d.getI("feedAprsDedup", c.aprsGate.dedupSec);
    c.stationId = d.getS("feedStationId", c.stationId);
    dect2::PskReporterConfig& r = a.pskr;
    r.on = d.getB("pskrOn", false); r.call = d.getS("pskrCall", ""); r.locator = d.getS("pskrLocator", ""); r.antenna = d.getS("pskrAntenna", "");
    if (r.on) dect2::PskReporter::get().configure(r);
    dect2::WsprNetConfig& w = a.wsprNet;
    w.on = d.getB("wsprNetOn", false); w.call = d.getS("wsprNetCall", ""); w.locator = d.getS("wsprNetLocator", "");
    if (w.on) dect2::WsprNet::get().configure(w);
    const int nt = (int)d.getI("feedTgtCount", 0);
    c.targets.clear();
    for (int i = 0; i < nt && i < 16; i++) {
        const std::string k = "feedTgt" + std::to_string(i);
        dect2::FeedTarget t;
        t.name = d.getS((k + "Name").c_str(), ""); t.kind = (dect2::FeedKind)(int)d.getI((k + "Kind").c_str(), 0); t.proto = (dect2::FeedProto)(int)d.getI((k + "Proto").c_str(), 0);
        t.host = d.getS((k + "Host").c_str(), ""); t.port = (int)d.getI((k + "Port").c_str(), 0); t.key = d.getS((k + "Key").c_str(), ""); t.enabled = d.getB((k + "On").c_str(), false);
        c.targets.push_back(t);
    }
    const int ns = (int)d.getI("sondeUpCount", 0);
    c.sondeTargets.clear();
    for (int i = 0; i < ns && i < 8; i++) {
        const std::string k = "sondeUp" + std::to_string(i);
        dect2::SondeUploadTarget t;
        t.name = d.getS((k + "Name").c_str(), ""); t.host = d.getS((k + "Host").c_str(), ""); t.port = (int)d.getI((k + "Port").c_str(), 14580);
        t.call = d.getS((k + "Call").c_str(), ""); t.pass = d.getS((k + "AprsPass").c_str(), ""); t.enabled = d.getB((k + "On").c_str(), false);
        t.beaconMin = (int)d.getI((k + "Beacon").c_str(), 10);
        c.sondeTargets.push_back(t);
    }
    {
        dect2::SondeHubConfig& h = c.sondeHub;
        h.enabled = d.getB("sondeHubOn", false); h.call = d.getS("sondeHubCall", ""); h.antenna = d.getS("sondeHubAnt", ""); h.email = d.getS("sondeHubMail", "");
        h.sharePos = d.getB("sondeHubPos", true); h.mobile = d.getB("sondeHubMobile", false);
        h.uploadS = std::max(10, std::min(60, (int)d.getI("sondeHubEvery", 15))); h.server = d.getS("sondeHubServer", h.server);
        if (h.server.empty()) h.server = dect2::SondeHubConfig().server;
        if (!dect2::sondehub::allowed()) h.enabled = false;   // a saved "on" does not upload after the 4.2 update (issue #526)
    }
    bool any = c.sondeHub.enabled;
    for (bool o : c.on) any = any || o;
    for (const dect2::FeedTarget& t : c.targets) any = any || t.enabled;
    for (const dect2::SondeUploadTarget& t : c.sondeTargets) any = any || t.enabled;
    if (any) dect2::Feeds::get().configure(c);
    dect2::ShareConfig& sc = a.share;
    sc.on = d.getB("shareOn", false); sc.port = (int)d.getI("sharePort", sc.port); sc.lan = d.getI("shareLan", 0) != 0; sc.allowControl = d.getI("shareControl", 0) != 0;
    if (sc.on) dect2::ShareServer::get().configure(sc);
    remoteApiLoadPrefs(a);
    rotatorLoadPrefs();
}

// Remote control from the clients of "Share my radio": their frequency and gain commands (only arrive when the box is ticked)
void shareTick(App& a) {
    if (a.rxId == 1) {   // the station beacon position (APRS-IS) and the SondeHub station: the chase GPS while it has a fix, else the set location
        double la = 0, lo = 0; receiverLocation(la, lo); dect2::SondeUpload::get().setStation(la, lo, receiverLocationSet(), receiverAltitudeM());
        dect2::SondeHubStation st;
        if (!sondeChasePosition(st.lat, st.lon, st.altM)) { st.lat = la; st.lon = lo; st.altM = receiverAltitudeM(); st.set = receiverLocationSet(); }
        else { st.set = true; st.live = true; }
        dect2::SondeHub::get().setStation(st);
    }
    if (a.rxId != 1 || !a.share.on || !a.share.allowControl) return;
    bool retune = false;
    for (const dect2::ShareCommand& m : dect2::ShareServer::get().takeCommands()) {
        if (m.cmd == 0x01 && m.value > 0) { a.freqMhz = m.value / 1e6; a.tune.centerHz = a.freqMhz * 1e6; retune = true; }
        else if (m.cmd == 0x04 && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isGeneric()) { a.tune.gainDb = m.value / 10.0; retune = true; }
        else if (m.cmd == 0x0e) { a.tune.biasTee = m.value != 0; retune = true; }
    }
    if (retune && a.engine.running()) { a.engine.retune(a.tune); a.peak.clear(); }
}

void saveFeedPrefs(const App& a) {
    plat::Prefs& d = plat::prefs();
    const dect2::FeedConfig& c = a.feeds;
    for (int i = 0; i < dect2::kNumFeeds; i++) d.setB(kKeys[i], c.on[i]);
    d.setI("feedLan", c.lan ? 1 : 0); d.setI("feedSbsPort", c.sbsPort); d.setI("feedBeastPort", c.beastPort); d.setI("feedKissPort", c.kissPort);
    d.setS("feedAisHost", c.aisHost); d.setI("feedAisPort", c.aisPort);
    d.setS("feedAcarsHost", c.acarsHost); d.setI("feedAcarsPort", c.acarsPort);
    d.setS("feedVdl2Host", c.vdl2Host); d.setI("feedVdl2Port", c.vdl2Port);
    d.setS("feedGnssHost", c.gnssHost); d.setI("feedGnssPort", c.gnssPort);
    d.setS("feedBleHost", c.bleHost); d.setI("feedBlePort", c.blePort);
    d.setS("feedLoraHost", c.loraHost); d.setI("feedLoraPort", c.loraPort);
    d.setS("feedAprsCall", c.aprsCall); d.setS("feedAprsPass", c.aprsPass);
    d.setS("feedAprsServer", c.aprsServer); d.setI("feedAprsServerPort", c.aprsServerPort);
    d.setI("feedAprsMaxMin", c.aprsGate.maxPerMin); d.setI("feedAprsStationMin", c.aprsGate.perStationPerMin); d.setI("feedAprsDedup", c.aprsGate.dedupSec);
    d.setS("feedStationId", c.stationId);
    d.setI("feedTgtCount", (long)c.targets.size());
    for (size_t i = 0; i < c.targets.size(); i++) {
        const std::string k = "feedTgt" + std::to_string(i);
        const dect2::FeedTarget& t = c.targets[i];
        d.setS((k + "Name").c_str(), t.name); d.setI((k + "Kind").c_str(), (int)t.kind); d.setI((k + "Proto").c_str(), (int)t.proto);
        d.setS((k + "Host").c_str(), t.host); d.setI((k + "Port").c_str(), t.port); d.setS((k + "Key").c_str(), t.key); d.setB((k + "On").c_str(), t.enabled);
    }
    d.setI("sondeUpCount", (long)c.sondeTargets.size());
    for (size_t i = 0; i < c.sondeTargets.size(); i++) {
        const std::string k = "sondeUp" + std::to_string(i);
        const dect2::SondeUploadTarget& t = c.sondeTargets[i];
        d.setS((k + "Name").c_str(), t.name); d.setS((k + "Host").c_str(), t.host); d.setI((k + "Port").c_str(), t.port);
        d.setS((k + "Call").c_str(), t.call); d.setS((k + "AprsPass").c_str(), t.pass); d.setB((k + "On").c_str(), t.enabled); d.setI((k + "Beacon").c_str(), t.beaconMin);
    }
    {
        const dect2::SondeHubConfig& h = c.sondeHub;
        d.setB("sondeHubOn", h.enabled); d.setS("sondeHubCall", h.call); d.setS("sondeHubAnt", h.antenna); d.setS("sondeHubMail", h.email);
        d.setB("sondeHubPos", h.sharePos); d.setB("sondeHubMobile", h.mobile); d.setI("sondeHubEvery", h.uploadS); d.setS("sondeHubServer", h.server);
    }
    d.setB("shareOn", a.share.on); d.setI("sharePort", a.share.port); d.setI("shareLan", a.share.lan ? 1 : 0); d.setI("shareControl", a.share.allowControl ? 1 : 0);
    remoteApiSavePrefs(a);
    rotatorSavePrefs();
    d.setB("pskrOn", a.pskr.on); d.setS("pskrCall", a.pskr.call); d.setS("pskrLocator", a.pskr.locator); d.setS("pskrAntenna", a.pskr.antenna);
    d.setB("wsprNetOn", a.wsprNet.on); d.setS("wsprNetCall", a.wsprNet.call); d.setS("wsprNetLocator", a.wsprNet.locator);
}

void feedsWindow(App& a) {
    if (a.rxId != 1) {   // the feeds and PSK Reporter are shared by all receivers (they send what every receiver decodes): one window, RX 1's settings
        if (App* f = receiverById(1)) { f->feedsOpen = a.feedsOpen; feedsWindow(*f); a.feedsOpen = f->feedsOpen; }
        return;
    }
    {   // dev/test only: DECT2_FEEDS_TAB=0..4 opens the window on that tab (hidden --shot runs)
        static const char* env = getenv("DECT2_FEEDS_TAB");
        static bool done = false;
        if (env && !done) { done = true; feedsOpenTab(a, atoi(env)); }
    }
    if (!a.feedsOpen) return;
    const ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::min(740 * gUi, vp.x * 0.92f), std::min(600 * gUi, vp.y * 0.9f)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(280 * gUi, 240 * gUi), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin(trw("Data feeds"), &a.feedsOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    dect2::FeedConfig& c = a.feeds;
    bool changed = false;
    const bool narrow = ImGui::GetContentRegionAvail().x < 540 * gUi;
    if (narrow) {   // small window: the places in a drop-down on top, the content below
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##feedstab", tr(tabName(gTab)))) {
            for (int i = 0; i < kFeedsTabCount; i++) if (ImGui::Selectable(tr(tabName(i)), gTab == i)) gTab = i;
            ImGui::EndCombo();
        }
    } else {
        ImGui::BeginChild("##feedsnav", ImVec2(190 * gUi, 0), ImGuiChildFlags_Borders);
        for (int i = 0; i < kFeedsTabCount; i++) if (ImGui::Selectable(tr(tabName(i)), gTab == i)) gTab = i;
        ImGui::EndChild();
        ImGui::SameLine();
    }
    gTabForce = false;
    ImGui::BeginChild("##feedsbody", ImVec2(0, 0), ImGuiChildFlags_None);
    ImGui::PushTextWrapPos(0.0f);   // text wraps at the window edge: no sideways scrolling
    switch (gTab) {
        case kFeedsTabUpload: tabUpload(a, c, changed); break;
        case kFeedsTabPrograms: tabPrograms(c, changed); break;
        case kFeedsTabShare: tabShare(a); break;
        case kFeedsTabStation: tabStation(a, c, changed); break;
        default: tabNmea(); break;
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    if (changed) apply(a);
    ImGui::End();
}
