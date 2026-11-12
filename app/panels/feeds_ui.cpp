#include "ease/ease_ui.h"
// View > Data feeds: send what the decoders find to other programs (tar1090 / readsb / VRS, OpenCPN, APRS-IS, acarshub).
// Every feed is off until the user turns it on; the network work is in core (feeds.cpp).
#include "../app.h"
#include "../ui/typography.h"
#include <cstdio>
#include <ctime>

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
    if (s.state == "off") ImGui::TextDisabled("%s", tr("off"));
    else {
        const ImVec4 col = !s.lastError.empty() ? pal::warnAmber() : s.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        if (server) ImGui::TextColored(col, tr("%s, %d client%s, %llu sent, %llu dropped"), s.state.c_str(), s.clients, s.clients == 1 ? "" : "s", (unsigned long long)s.sent, (unsigned long long)s.dropped);
        else ImGui::TextColored(col, tr("%s, %llu sent, %llu dropped"), s.state.c_str(), (unsigned long long)s.sent, (unsigned long long)s.dropped);
        if (!s.detail.empty()) ImGui::TextDisabled("%s", s.detail.c_str());
        if (!s.lastError.empty()) { ImGui::PushTextWrapPos(460 * gUi); ImGui::TextColored(pal::badRed(), "%s", s.lastError.c_str()); ImGui::PopTextWrapPos(); }
    }
    ImGui::Unindent();
}

// Aggregator targets for one kind of data (ADS-B or AIS): a list, each off until its own box is ticked
void targetsSection(dect2::FeedConfig& c, bool ais, bool& changed) {
    using dect2::FeedKind;
    ImGui::PushID(ais ? "tgtAis" : "tgtAdsb");
    ImGui::TextDisabled("%s", tr("Send to aggregators (each target is off until you tick it, with your own account details)"));
    for (size_t i = 0; i < c.targets.size(); i++) {
        dect2::FeedTarget& t = c.targets[i];
        if ((t.kind == FeedKind::AisNmea) != ais) continue;
        ImGui::PushID((int)i);
        changed |= ImGui::Checkbox(t.name.c_str(), &t.enabled);
        ImGui::SameLine(); changed |= textField("##host", t.host, 0, 140);
        ImGui::SameLine(); changed |= portField(N_("Port"), t.port);
        ImGui::SameLine(); if (ImGui::SmallButton(tr("Remove"))) { c.targets.erase(c.targets.begin() + (long)i); changed = true; ImGui::PopID(); break; }
        if (t.proto == dect2::FeedProto::Tcp) { ImGui::Indent(); changed |= textField(N_("Key / id line (optional)"), t.key, ImGuiInputTextFlags_Password, 160); ImGui::Unindent(); }
        const dect2::FeedTargetStatus s = dect2::FeedTargets::get().status(i);
        ImGui::Indent();
        if (!t.enabled) ImGui::TextDisabled("%s", tr("off"));
        else {
            ImGui::TextColored(s.connected ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), tr("%s, %llu sent, %llu dropped, %llu reconnects"), s.state.c_str(), (unsigned long long)s.sent, (unsigned long long)s.dropped, (unsigned long long)s.reconnects);
            if (!s.lastError.empty()) { ImGui::PushTextWrapPos(460 * gUi); ImGui::TextColored(pal::badRed(), "%s", s.lastError.c_str()); ImGui::PopTextWrapPos(); }
        }
        ImGui::Unindent();
        ImGui::PopID();
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
    ImGui::PushTextWrapPos(460 * gUi);
    ImGui::TextDisabled("%s", tr("Privacy: aggregators publish what you send, and they know your station's position from your account there. Beast and NMEA data carry no key of yours; "
                                 "enter only the address and key your own account gave you. Nothing is sent until you tick a target."));
    ImGui::PopTextWrapPos();
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
void sondeHubEntry(dect2::FeedConfig& c, bool& changed) {
    dect2::SondeHubConfig& h = c.sondeHub;
    ImGui::PushID("sondehub");
    if (ImGui::Checkbox(tr("SondeHub (tracker.sondehub.org)"), &h.enabled)) {
        changed = true;
        if (h.enabled && h.call.empty()) { h.call = c.aprsCall; if (h.call.empty() && !c.sondeTargets.empty()) h.call = c.sondeTargets.front().call; }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The SondeHub database and map. Not APRS-IS: OnAir sends the frames over HTTPS in SondeHub's own format, every 15 s."));
    ImGui::Indent();
    changed |= textField(N_("Uploader callsign"), h.call, 0, 140);
    changed |= textField(N_("Antenna (optional)"), h.antenna, 0, 180);
    changed |= ImGui::Checkbox(tr("Send my station position (shown on the SondeHub map)"), &h.sharePos);
    ImGui::BeginDisabled(!h.sharePos);
    changed |= ImGui::Checkbox(tr("Chase car: send my moving GPS position every 30 s"), &h.mobile);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Needs a live GPS position (Radiosonde map > My position > GPS). Without one the chase position is not sent."));
    ImGui::EndDisabled();
    if (!h.call.empty() && !dect2::sondehub::validUploader(h.call)) ImGui::TextColored(pal::warnAmber(), "%s", tr("The uploader callsign needs 3 to 64 characters."));
    const dect2::SondeHubStatus s = dect2::SondeHub::get().status();
    if (!h.enabled) ImGui::TextDisabled("%s", tr("off"));
    else {
        ImGui::TextColored(s.connected ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), tr("%s, %llu frames uploaded, %llu station uploads, %llu dropped, %llu retries"),
                           s.state.c_str(), (unsigned long long)s.sent, (unsigned long long)s.beacons, (unsigned long long)s.dropped, (unsigned long long)s.reconnects);
        ImGui::PushTextWrapPos(460 * gUi);
        if (s.skipped) ImGui::TextDisabled(tr("%llu frames kept back (last: %s)"), (unsigned long long)s.skipped, s.skipWhy.c_str());
        if (!s.lastError.empty()) ImGui::TextColored(pal::badRed(), "%s", s.lastError.c_str());
        if (!s.lastLine.empty()) ImGui::TextDisabled("%s %s", tr("last upload:"), s.lastLine.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::TreeNode(trw("What is sent##sondehubjson"))) {
            ImGui::PushTextWrapPos(460 * gUi);
            const std::string lj = dect2::SondeHub::get().lastListenerJson(), tj = dect2::SondeHub::get().lastJson();
            ImGui::TextDisabled("%s", tr("Station record (PUT /listeners):"));
            ImGui::TextUnformatted(lj.empty() ? tr("(not sent yet)") : lj.c_str());
            ImGui::TextDisabled("%s", tr("Last telemetry upload (PUT /sondes/telemetry, gzip-compressed):"));
            const std::string fo = firstObject(tj);
            ImGui::TextUnformatted(fo.empty() ? tr("(not sent yet)") : fo.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TreePop();
        }
    }
    if (ImGui::TreeNode(trw("Advanced##sondehubadv"))) {
        ImGui::SetNextItemWidth(120 * gUi);
        changed |= ImGui::SliderInt(tr("upload every (s)##sondehub"), &h.uploadS, 10, 60);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("SondeHub's own uploader uses 15 s. While a DFM or Meisei sonde is heard the wait is 1.5 times longer (at most 30 s) so SondeHub's checks get enough points."));
        changed |= textField(N_("Contact e-mail (optional, only the SondeHub admins see it)"), h.email, 0, 180);
        changed |= textField(N_("Server##sondehubsrv"), h.server, 0, 220);
        ImGui::TreePop();
    }
    ImGui::PushTextWrapPos(460 * gUi);
    ImGui::TextDisabled("%s", tr("Privacy: SondeHub publishes your uploader callsign and, with the box ticked, your station position on tracker.sondehub.org; "
                                 "the sonde data become part of its open database (CC BY-SA). Only frames with a GPS fix and the sonde's own time are sent; iMet-1/4 are not."));
    ImGui::PopTextWrapPos();
    ImGui::Unindent();
    ImGui::PopID();
}

// Radiosonde upload to APRS-IS aggregators: a list of targets, each off until its own box is ticked
void sondeUploadSection(dect2::FeedConfig& c, bool& changed) {
    ImGui::PushID("sondeup");
    ImGui::PushTextWrapPos(460 * gUi);
    ImGui::TextDisabled("%s", tr("Sends every decoded radiosonde (position, speed, altitude, climb rate, temperature, humidity, pressure, frequency, type) to SondeHub and to APRS-IS sites such as wettersonde.net "
                                 "and radiosondy.info, after a station beacon with your location. Each target is off until you tick it."));
    ImGui::PopTextWrapPos();
    sondeHubEntry(c, changed);
    for (size_t i = 0; i < c.sondeTargets.size(); i++) {
        dect2::SondeUploadTarget& t = c.sondeTargets[i];
        ImGui::PushID((int)i);
        changed |= ImGui::Checkbox(t.name.c_str(), &t.enabled);
        ImGui::SameLine(); changed |= textField("##host", t.host, 0, 140);
        ImGui::SameLine(); changed |= portField(N_("Port"), t.port);
        ImGui::SameLine(); if (ImGui::SmallButton(tr("Remove"))) { c.sondeTargets.erase(c.sondeTargets.begin() + (long)i); changed = true; ImGui::PopID(); break; }
        ImGui::Indent();
        changed |= textField(N_("Callsign-SSID"), t.call, ImGuiInputTextFlags_CharsUppercase, 120);
        changed |= textField(N_("Passcode"), t.pass, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsDecimal, 120);
        ImGui::SetNextItemWidth(90 * gUi);
        changed |= ImGui::SliderInt(tr("beacon every (min)"), &t.beaconMin, 1, 120);
        if (!t.call.empty() && !dect2::aprsValidCallsign(t.call)) ImGui::TextColored(pal::warnAmber(), "%s", tr("Not a valid callsign (like DG9BFC-10)."));
        const dect2::SondeUploadStatus s = dect2::SondeUpload::get().status(i);
        if (!t.enabled) ImGui::TextDisabled("%s", tr("off"));
        else {
            ImGui::TextColored(s.connected ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), tr("%s, %llu packets forwarded, %llu beacons, %llu dropped, %llu reconnects"),
                               s.state.c_str(), (unsigned long long)s.sent, (unsigned long long)s.beacons, (unsigned long long)s.dropped, (unsigned long long)s.reconnects);
            if (!s.lastError.empty()) { ImGui::PushTextWrapPos(460 * gUi); ImGui::TextColored(pal::badRed(), "%s", s.lastError.c_str()); ImGui::PopTextWrapPos(); }
            if (!s.lastLine.empty()) { ImGui::PushTextWrapPos(460 * gUi); ImGui::TextDisabled("%s %s", tr("last line:"), s.lastLine.c_str()); ImGui::PopTextWrapPos(); }
        }
        ImGui::Unindent();
        ImGui::PopID();
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
    ImGui::PushTextWrapPos(460 * gUi);
    ImGui::TextDisabled("%s", tr("Privacy: the sites publish your station position and callsign. Set your location in View > Map first. Your passcode stays in your settings only. "
                                 "M10 and M20 sondes are not sent (no agreed APRS name for them)."));
    ImGui::PopTextWrapPos();
    ImGui::PopID();
}
} // namespace

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
        double la = 0, lo = 0; receiverLocation(la, lo); dect2::SondeUpload::get().setStation(la, lo, receiverLocationSet());
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
    if (!a.feedsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(480 * gUi, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Data feeds"), &a.feedsOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    dect2::FeedConfig& c = a.feeds;
    bool changed = false;
    ImGui::PushTextWrapPos(460 * gUi);
    ImGui::TextDisabled("%s", tr("Sends decoded data to other programs while the mode runs. Every feed is off until you turn it on."));
    ImGui::PopTextWrapPos();

    changed |= ImGui::Checkbox(tr("Let other computers connect to the TCP feeds (LAN)"), &c.lan);
    typo::separatorText(tr("ADS-B"));
    ImGui::PushID("sbs");
    changed |= ImGui::Checkbox(tr("SBS-1 BaseStation (TCP server)"), &c.on[dect2::kFeedSbs]);
    ImGui::SameLine(); changed |= portField(N_("Port"), c.sbsPort);
    statusLine(dect2::kFeedSbs, true);
    ImGui::PopID();
    ImGui::PushID("beast");
    changed |= ImGui::Checkbox(tr("Beast binary (TCP server)"), &c.on[dect2::kFeedBeast]);
    ImGui::SameLine(); changed |= portField(N_("Port"), c.beastPort);
    statusLine(dect2::kFeedBeast, true);
    ImGui::PopID();
    targetsSection(c, false, changed);

    typo::separatorText(tr("AIS"));
    ImGui::PushID("ais");
    changed |= ImGui::Checkbox(tr("NMEA !AIVDM (UDP)"), &c.on[dect2::kFeedAis]);
    ImGui::SameLine(); changed |= textField("##host", c.aisHost, 0, 120);
    ImGui::SameLine(); changed |= portField(N_("Port"), c.aisPort);
    statusLine(dect2::kFeedAis, false);
    ImGui::PopID();
    targetsSection(c, true, changed);

    typo::separatorText(tr("APRS"));
    ImGui::PushID("aprsis");
    changed |= ImGui::Checkbox(tr("APRS-IS (receive-only igate)"), &c.on[dect2::kFeedAprsIs]);
    ImGui::Indent();
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
    ImGui::Unindent();
    statusLine(dect2::kFeedAprsIs, false);
    ImGui::PopID();
    typo::separatorText(tr("Radiosonde upload (SondeHub, APRS-IS)"));
    sondeUploadSection(c, changed);
    ImGui::PushID("kiss");
    changed |= ImGui::Checkbox(tr("KISS (TCP server)"), &c.on[dect2::kFeedKiss]);
    ImGui::SameLine(); changed |= portField(N_("Port"), c.kissPort);
    statusLine(dect2::kFeedKiss, true);
    ImGui::PopID();

    typo::separatorText(tr("ACARS and VDL2 (JSON)"));
    ImGui::PushID("acars");
    changed |= ImGui::Checkbox(tr("ACARS, acarsdec format (UDP)"), &c.on[dect2::kFeedAcars]);
    ImGui::SameLine(); changed |= textField("##host", c.acarsHost, 0, 120);
    ImGui::SameLine(); changed |= portField(N_("Port"), c.acarsPort);
    statusLine(dect2::kFeedAcars, false);
    ImGui::PopID();
    ImGui::PushID("vdl2");
    changed |= ImGui::Checkbox(tr("VDL2, dumpvdl2 format (UDP)"), &c.on[dect2::kFeedVdl2]);
    ImGui::SameLine(); changed |= textField("##host", c.vdl2Host, 0, 120);
    ImGui::SameLine(); changed |= portField(N_("Port"), c.vdl2Port);
    statusLine(dect2::kFeedVdl2, false);
    ImGui::PopID();
    changed |= textField(N_("Station ID"), c.stationId, 0, 120);

    typo::separatorText(tr("GNSS, Bluetooth LE, LoRa (UDP JSON / NMEA)"));
    ImGui::PushID("gnss");
    changed |= ImGui::Checkbox(tr("GNSS NMEA RMC/GGA (UDP)"), &c.on[dect2::kFeedGnss]);
    ImGui::SameLine(); changed |= textField("##host", c.gnssHost, 0, 120);
    ImGui::SameLine(); changed |= portField(N_("Port"), c.gnssPort);
    statusLine(dect2::kFeedGnss, false);
    ImGui::PopID();
    ImGui::PushID("ble");
    changed |= ImGui::Checkbox(tr("Bluetooth LE advertisements, JSON (UDP)"), &c.on[dect2::kFeedBle]);
    ImGui::SameLine(); changed |= textField("##host", c.bleHost, 0, 120);
    ImGui::SameLine(); changed |= portField(N_("Port"), c.blePort);
    statusLine(dect2::kFeedBle, false);
    ImGui::PopID();
    ImGui::PushID("lora");
    changed |= ImGui::Checkbox(tr("LoRa / LoRaWAN / Meshtastic, JSON (UDP)"), &c.on[dect2::kFeedLora]);
    ImGui::SameLine(); changed |= textField("##host", c.loraHost, 0, 120);
    ImGui::SameLine(); changed |= portField(N_("Port"), c.loraPort);
    statusLine(dect2::kFeedLora, false);
    ImGui::PopID();
    ImGui::PushTextWrapPos(460 * gUi);
    ImGui::TextDisabled("%s", tr("JSON lines (one object a line) unless GNSS, which is NMEA. Off until you turn a feed on. Only modes that already have structured messages are sent."));
    ImGui::PopTextWrapPos();

    typo::separatorText(tr("PSK Reporter"));
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
    dect2::PskReporterConfig& r = a.pskr;
    bool pc = easeui::stationFillOnce(a, "stationFilled.pskr", r.call, r.locator);
    pc |= ImGui::Checkbox(tr("Send spots to PSK Reporter (pskreporter.info)"), &r.on);
    ImGui::Indent();
    pc |= textField(N_("Your callsign"), r.call, ImGuiInputTextFlags_CharsUppercase, 120);
    pc |= textField(N_("Your locator"), r.locator, ImGuiInputTextFlags_CharsUppercase, 90);
    pc |= textField(N_("Antenna"), r.antenna, 0, 200);
    ImGui::Unindent();
    {
        const dect2::PskReporterStatus s = dect2::PskReporter::get().status();
        ImGui::Indent();
        if (!r.on) ImGui::TextDisabled("%s", tr("off"));
        else {
            ImGui::TextColored(s.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), tr("%s, %llu spots sent in %llu packets, %llu duplicates skipped"), s.state.c_str(), (unsigned long long)s.spotsSent, (unsigned long long)s.packets, (unsigned long long)s.duplicates);
            if (s.lastSend) { char b[32]; const time_t t = (time_t)s.lastSend; snprintf(b, sizeof b, "%s", dect2::timefmt::clock((int64_t)t).c_str()); ImGui::Text(tr("last send %s"), b); }
            if (!s.lastError.empty()) ImGui::TextColored(pal::badRed(), "%s", s.lastError.c_str());
        }
        ImGui::PushTextWrapPos(460 * gUi);
        ImGui::TextDisabled("%s", tr("Spots are public. A valid amateur radio callsign is required; the dial frequency is the tuned frequency."));
        ImGui::PopTextWrapPos();
        ImGui::Unindent();
    }
    if (pc) { dect2::PskReporter::get().configure(r); savePrefs(a); }

    typo::separatorText(tr("WSPRnet"));
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
    dect2::WsprNetConfig& wn = a.wsprNet;
    bool wc = easeui::stationFillOnce(a, "stationFilled.wspr", wn.call, wn.locator);
    wc |= ImGui::Checkbox(tr("Send WSPR spots to WSPRnet (wsprnet.org)"), &wn.on);
    ImGui::Indent();
    wc |= textField(N_("Your callsign"), wn.call, ImGuiInputTextFlags_CharsUppercase, 120);
    wc |= textField(N_("Your locator"), wn.locator, ImGuiInputTextFlags_CharsUppercase, 90);
    ImGui::Unindent();
    {
        const dect2::WsprNetStatus ws = dect2::WsprNet::get().status();
        ImGui::Indent();
        if (!wn.on) ImGui::TextDisabled("%s", tr("off"));
        else {
            ImGui::TextColored(ws.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), tr("%s, %llu spots sent, %llu duplicates skipped, %llu failed"), ws.state.c_str(), (unsigned long long)ws.spotsSent, (unsigned long long)ws.duplicates, (unsigned long long)ws.failures);
            if (ws.lastSend) { char b[32]; const time_t t = (time_t)ws.lastSend; snprintf(b, sizeof b, "%s", dect2::timefmt::clock((int64_t)t).c_str()); ImGui::Text(tr("last send %s"), b); }
            if (!ws.lastError.empty()) ImGui::TextColored(pal::badRed(), "%s", ws.lastError.c_str());
        }
        ImGui::PushTextWrapPos(460 * gUi);
        ImGui::TextDisabled("%s", tr("WSPR decodes only. Spots are public. A valid amateur radio callsign and locator are required."));
        ImGui::PopTextWrapPos();
        ImGui::Unindent();
    }
    if (wc) { dect2::WsprNet::get().configure(wn); savePrefs(a); }

    typo::separatorText(tr("Share my radio (rtl_tcp server)"));
    {
        dect2::ShareConfig& sc = a.share;
        bool sch = false;
        sch |= ImGui::Checkbox(tr("Share the radio's samples as an rtl_tcp server"), &sc.on);
        ImGui::SameLine(); sch |= portField(N_("Port"), sc.port);
        ImGui::Indent();
        sch |= ImGui::Checkbox(tr("Let other computers connect (LAN)"), &sc.lan);
        sch |= ImGui::Checkbox(tr("Allow remote control (frequency and gain)"), &sc.allowControl);
        const dect2::ShareStatus ss = dect2::ShareServer::get().status();
        if (!sc.on) ImGui::TextDisabled("%s", tr("off"));
        else if (!ss.listening) ImGui::TextColored(pal::badRed(), "%s", ss.lastError.empty() ? tr("starting") : ss.lastError.c_str());
        else {
            ImGui::TextColored(pal::okGreen(), tr("listening on %s port %d, %d client(s), %.1f MB sent, %.1f MB dropped"), sc.lan ? "all addresses" : "127.0.0.1", sc.port,
                               (int)ss.clients.size(), (double)ss.sentBytes / 1e6, (double)ss.droppedBytes / 1e6);
            for (const std::string& c : ss.clients) ImGui::BulletText("%s", c.c_str());
        }
        ImGui::PushTextWrapPos(460 * gUi);
        ImGui::TextDisabled("%s", tr("A second OnAir (Add network radio, rtl_tcp), SDR# or SDR++ connects to this computer's address and the port. Samples are sent as 8-bit IQ at the rate the radio runs at. "
                                     "Without 'allow remote control' the clients cannot change the frequency or gain."));
        ImGui::PopTextWrapPos();
        ImGui::Unindent();
        if (sch) { dect2::ShareServer::get().configure(sc); savePrefs(a); }
    }
    remoteApiSection(a);
    mcpSection(a);

    if (changed) apply(a);
    ImGui::End();
}
