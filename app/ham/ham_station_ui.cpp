// The 4.0 station layer of the Ham view: see ham_station_ui.h.
#include "ham_station_ui.h"
#include "ham_ops_ui.h"
#include "../platform/platform.h"
#include "dect2/geo/maidenhead.h"
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_safety.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

using namespace dect2;

namespace {
std::string readAll(const std::string& p, bool& ok) {
    std::ifstream f(p, std::ios::binary);
    ok = (bool)f;
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
bool writeAll(const std::string& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
    return (bool)f;
}
double nowUtc() { return (double)time(nullptr); }
std::string hhmm(double t) {
    const long long d = ((long long)t % 86400 + 86400) % 86400;
    char b[8];
    snprintf(b, sizeof b, "%02d%02d", (int)(d / 3600), (int)(d % 3600 / 60));
    return b;
}
std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> v;
    std::istringstream is(s);
    std::string l;
    while (std::getline(is, l)) if (!l.empty()) v.push_back(l);
    return v;
}
void saveLogNow() {
    HamOps& o = hamOps();
    std::string err;
    if (!o.log.save(o.logPath, err)) hamToast("Log not saved: " + err);
    o.workedDirty = true;
    hamStation().needDirty = true;
    hamLogChanged();
}
ImVec4 needColour(int level) {
    switch (level) {
    case 6: return ImVec4(1, 0.5f, 1, 1);
    case 5: return ImVec4(1, 0.65f, 0.9f, 1);
    case 4: case 3: return ImVec4(1, 0.78f, 0.4f, 1);
    case 2: return ImVec4(0.5f, 0.8f, 1, 1);
    case 1: return ImVec4(0.85f, 0.85f, 0.85f, 1);
    default: return ImVec4(0.55f, 0.55f, 0.55f, 1);
    }
}
}  // namespace

HamStation::~HamStation() {
    feeds.stop();
    if (cbWorker.joinable()) cbWorker.join();
    if (tqslWorker.joinable()) tqslWorker.join();
}
HamStation& hamStation() { static HamStation s; return s; }

void hamStationLoad() {
    HamStation& s = hamStation();
    if (s.loaded) return;
    s.loaded = true;
    plat::Prefs& p = plat::basePrefs();
    s.country = p.getS("ham.country", "");
    s.antennas = lines(p.getS("ham.antennas", ""));
    if (s.antennas.empty() && !hamOps().antenna.empty()) s.antennas.push_back(hamOps().antenna);
    s.mem.fromText(p.getS("ham.freqMem", ""));
    s.cqMod = p.getS("ham.cqMod", "");
    s.cwMatch = p.getB("ham.cwMatch", false);
    // spots: every feed off unless the user switched it on
    ham::SpotFeedConfig& f = s.feedCfg;
    f.cluster = p.getB("ham.feed.cluster", false); f.clusterHost = p.getS("ham.feed.clusterHost", ""); f.clusterPort = (int)p.getI("ham.feed.clusterPort", 7300);
    f.rbn = p.getB("ham.feed.rbn", false); f.rbnDigital = p.getB("ham.feed.rbnDigital", false);
    f.pota = p.getB("ham.feed.pota", false); f.sota = p.getB("ham.feed.sota", false); f.pskr = p.getB("ham.feed.pskr", false);
    f.myCall = p.getS("ham.call", "");
    s.feeds.setConfig(f);
    // callsign lookup: off unless the user enters an account
    s.cbCfg.service = (ham::CallbookService)std::max(0, std::min(2, (int)p.getI("ham.cb.service", 0)));
    s.cbCfg.user = p.getS("ham.cb.user", ""); s.cbCfg.password = p.getS("ham.cb.password", "");
    s.cb.setConfig(s.cbCfg);
    s.cb.loadCache(plat::dataDir() + "/onair_ham_callbook.tsv");
    // alerts
    s.alertsOn = p.getB("ham.alert.on", true);
    ham::AlertRules& r = s.alerter.rules;
    r.newDxcc = p.getB("ham.alert.dxcc", true); r.newDxccBand = p.getB("ham.alert.dxccBand", true); r.newDxccClass = p.getB("ham.alert.dxccMode", false);
    r.newZone = p.getB("ham.alert.zone", false); r.newState = p.getB("ham.alert.state", false); r.newGrid = p.getB("ham.alert.grid", false);
    r.newPrefix = p.getB("ham.alert.prefix", false);
    r.watch = lines(p.getS("ham.alert.watch", ""));
    // activation
    s.act.prog = (ham::Program)std::max(0, std::min(4, (int)p.getI("ham.act.prog", 0)));
    {
        std::string refs = p.getS("ham.act.refs", ""), cur;
        for (char c : refs + ",") { if (c == ',') { if (!cur.empty()) s.act.refs.push_back(ham::refNormal(cur)); cur.clear(); } else cur += c; }
    }
    s.act.startUtc = p.getD("ham.act.start", 0);
    // band hopping
    s.hopOn = p.getB("ham.hop.on", false);
    s.hopText = p.getS("ham.hop.text", "0000-0600 40m\n0600-1600 20m\n1600-2000 auto\n2000-2400 40m\n");
    std::string err;
    ham::HopSchedule::parse(s.hopText, s.hop, err);
    s.hopper.setBands({"160m", "80m", "40m", "30m", "20m", "17m", "15m", "12m", "10m"});
    s.tqslPath = p.getS("ham.tqsl.path", "tqsl");
    s.tqslLocation = p.getS("ham.tqsl.location", "");
}

void hamStationSave() {
    HamStation& s = hamStation();
    plat::Prefs& p = plat::basePrefs();
    p.setS("ham.country", s.country);
    std::string a;
    for (const auto& x : s.antennas) a += x + "\n";
    p.setS("ham.antennas", a);
    p.setS("ham.freqMem", s.mem.toText());
    p.setS("ham.cqMod", s.cqMod);
    p.setB("ham.cwMatch", s.cwMatch);
    const ham::SpotFeedConfig& f = s.feedCfg;
    p.setB("ham.feed.cluster", f.cluster); p.setS("ham.feed.clusterHost", f.clusterHost); p.setI("ham.feed.clusterPort", f.clusterPort);
    p.setB("ham.feed.rbn", f.rbn); p.setB("ham.feed.rbnDigital", f.rbnDigital);
    p.setB("ham.feed.pota", f.pota); p.setB("ham.feed.sota", f.sota); p.setB("ham.feed.pskr", f.pskr);
    p.setI("ham.cb.service", (int)s.cbCfg.service); p.setS("ham.cb.user", s.cbCfg.user); p.setS("ham.cb.password", s.cbCfg.password);
    const ham::AlertRules& r = s.alerter.rules;
    p.setB("ham.alert.on", s.alertsOn);
    p.setB("ham.alert.dxcc", r.newDxcc); p.setB("ham.alert.dxccBand", r.newDxccBand); p.setB("ham.alert.dxccMode", r.newDxccClass);
    p.setB("ham.alert.zone", r.newZone); p.setB("ham.alert.state", r.newState); p.setB("ham.alert.grid", r.newGrid); p.setB("ham.alert.prefix", r.newPrefix);
    std::string w;
    for (const auto& x : r.watch) w += x + "\n";
    p.setS("ham.alert.watch", w);
    p.setI("ham.act.prog", (int)s.act.prog);
    std::string refs;
    for (const auto& x : s.act.refs) refs += (refs.empty() ? "" : ",") + x;
    p.setS("ham.act.refs", refs);
    p.setD("ham.act.start", s.act.startUtc);
    p.setB("ham.hop.on", s.hopOn); p.setS("ham.hop.text", s.hopText);
    p.setS("ham.tqsl.path", s.tqslPath); p.setS("ham.tqsl.location", s.tqslLocation);
    p.flush();
}

// ---- the header: frequency, S-meter, band plan hint ----
void hamStationHeader(App& a, double txAudioHz, const std::string& modeClass, double rxDb) {
    HamOps& o = hamOps();
    const double hz = a.freqMhz * 1e6;
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.8f);
    ImGui::Text("%.4f", a.freqMhz);
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The dial frequency (MHz). The band bar below goes to the frequency you last used on that band in this mode."));
    ImGui::SameLine();
    ImGui::TextDisabled("MHz  %s", ham::rxBandNameFor(hz).c_str());
    // the meter: the receive level; S units when the user calibrated it (dBm = dBFS + offset)
    ImGui::SameLine(0, 16 * gUi);
    static float offset = (float)plat::basePrefs().getD("ham.smeterOffset", 0);
    const bool on = rxDb > -150;
    const float w = 180 * gUi, h = ImGui::GetFrameHeight() * 0.6f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p0.x, p0.y + h * 0.3f), ImVec2(p0.x + w, p0.y + h * 1.3f), IM_COL32(26, 30, 36, 255), 3.f);
    if (on) {
        const float f = (float)std::max(0.0, std::min(1.0, (rxDb + 120.0) / 120.0));
        dl->AddRectFilled(ImVec2(p0.x, p0.y + h * 0.3f), ImVec2(p0.x + w * f, p0.y + h * 1.3f), f > 0.92f ? IM_COL32(220, 80, 60, 255) : IM_COL32(90, 200, 120, 255), 3.f);
    }
    ImGui::Dummy(ImVec2(w, h * 1.4f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Receive level. Right click to calibrate it to S units (enter what a known signal reads in dBm)."));
    if (ImGui::BeginPopupContextItem("##smcal")) {
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::InputFloat(tr("dBm = dBFS +"), &offset, 1, 10, "%.0f")) { plat::basePrefs().setD("ham.smeterOffset", offset); plat::basePrefs().flush(); }
        ImGui::TextDisabled("%s", tr("0 = not calibrated. S9 is -73 dBm below 30 MHz and -93 dBm above."));
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (!on) ImGui::TextDisabled("%s", tr("no signal"));
    else if (offset != 0) ImGui::Text("%s  (%.0f dBm)", ham::sMeterText(rxDb + offset, hz).c_str(), rxDb + offset);
    else ImGui::Text("%.0f dBFS", rxDb);
    // the band plan of the region: a hint, never a lock
    const std::string warn = o.region > 0 && !modeClass.empty() ? ham::segmentWarning(o.region, hz + txAudioHz, modeClass) : "";
    if (!warn.empty()) { ImGui::SameLine(0, 16 * gUi); ImGui::TextColored(ImVec4(1, 0.65f, 0.25f, 1), "%s", warn.c_str()); }
    else if (o.region > 0) {
        if (const ham::BandSegment* sg = ham::segmentAt(o.region, hz + txAudioHz)) { ImGui::SameLine(0, 16 * gUi); ImGui::TextDisabled("%s", sg->label); }
    }
}

// ---- the band bar with the memory ----
void hamRememberFreq(App& a, const std::string& modeId) {
    HamStation& s = hamStation();
    const std::string b = ham::rxBandNameFor(a.freqMhz * 1e6);
    if (b.empty() || modeId.empty()) return;
    if (s.mem.recall(b, modeId, 0) == a.freqMhz * 1e6) return;
    s.mem.remember(b, modeId, a.freqMhz * 1e6);
    hamStationSave();
}
bool hamBandBar(App& a, const std::string& modeId, int slotMode) {
    HamStation& s = hamStation();
    HamOps& o = hamOps();
    const std::string cur = ham::rxBandNameFor(a.freqMhz * 1e6);
    bool tuned = false;
    std::vector<std::pair<std::string, double>> bands;   // band, the usual dial (Hz) of this mode there
    for (const auto& b : ham::bandDials()) {
        double mhz = slotMode >= 0 ? ham::dialFor(b.band, slotMode) : ham::usualDialMhz(b.band, modeId, o.region);
        if (mhz <= 0 && slotMode < 0) mhz = ham::dialFor(b.band, 0);   // no usual frequency for this mode: the band's FT8 dial as a start
        if (mhz <= 0 && !s.mem.has(b.band, modeId)) continue;
        bands.push_back({b.band, mhz * 1e6});
    }
    for (size_t i = 0; i < bands.size(); i++) {
        const std::string& b = bands[i].first;
        const bool on = cur == b;
        if (i) ImGui::SameLine(0, 3 * gUi);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool click = ImGui::SmallButton(b.c_str());
        if (on) ImGui::PopStyleColor();
        const double target = s.mem.recall(b, modeId, bands[i].second);
        if (ImGui::IsItemHovered()) {
            const char* where = s.mem.has(b, modeId) ? ", where you were last" : ", the usual frequency";
            if (b == "11m") ImGui::SetTooltip("%.4f MHz%s. CB: not an amateur band, you are responsible for being allowed to transmit here", target / 1e6, where);
            else ImGui::SetTooltip("%.4f MHz%s (Alt+Up / Alt+Down)", target / 1e6, where);
        }
        if (click && target > 0) {
            if (!cur.empty()) s.mem.remember(cur, modeId, a.freqMhz * 1e6);
            tuneFreq(a, target / 1e6);
            hamStationSave();
            tuned = true;
        }
    }
    if (!bands.empty()) {
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton(tr("Usual"))) {
            const double u = slotMode >= 0 ? ham::dialFor(cur, slotMode) : ham::usualDialMhz(cur, modeId, o.region);
            if (u > 0) { s.mem.forget(cur, modeId); tuneFreq(a, u); hamStationSave(); tuned = true; }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Back to the usual frequency of this mode on this band (forgets the remembered one)."));
    }
    return tuned;
}

// ---- the modes with their own screens ----
void hamMoreModesCombo(App& a) {
    ImGui::SetNextItemWidth(150 * gUi);
    if (!ImGui::BeginCombo("##hammore", tr("More modes"))) {
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Every other amateur mode OnAir decodes (JS8Call, SSTV, digital voice, packet, SSB / AM / FM): opens its own screen. Your log, callbook and spots stay in the Ham view."));
        return;
    }
    std::string group;
    for (const auto& m : ham::hamModeCatalog()) {
        if (m.run != ham::HamRun::Screen || !m.screen) continue;
        if (group != m.group) { group = m.group; ImGui::SeparatorText(tr(m.group)); }
        if (ImGui::Selectable(m.name)) {
            for (const auto& d : allModes())
                if (d.name && !strcmp(d.name, m.screen)) {
                    selectMode(a, d.family, d.subId);
                    hamViewFlag() = false;
                    hamToast(std::string(m.name) + ": opened its own screen. The Ham view keeps your log and spots.");
                    break;
                }
        }
    }
    ImGui::EndCombo();
}

// ---- needed and alerts ----
ham::NeedResult hamNeedOf(const std::string& call, const std::string& grid, const std::string& band, const std::string& mode, bool alert) {
    HamStation& s = hamStation();
    HamOps& o = hamOps();
    static size_t lastN = (size_t)-1;
    static double lastT = 0;
    const double now = nowUtc();
    if (s.needDirty || lastN != o.log.size() || now - lastT > 10) { s.need.build(o.log.qsos()); s.needDirty = false; lastN = o.log.size(); lastT = now; }
    ham::NeedQuery q;
    q.call = call; q.grid = grid; q.band = band; q.mode = mode;
    const ham::DxccHit h = o.dxcc.lookup(call);
    q.dxccKey = h.key(); q.cqz = h.cqz; q.ituz = h.ituz;
    const ham::NeedResult r = s.need.check(q);
    std::string why;
    if (alert && s.alertsOn && s.alerter.check(q, r, now, why)) { s.lastAlert = hhmm(now) + " " + why; hamToast(why); }
    return r;
}

// ---- the Cluster tab ----
void hamClusterTab(App& a, const std::function<void(const std::string& call, const std::string& grid)>& pick) {
    HamStation& s = hamStation();
    HamOps& o = hamOps();
    const double now = nowUtc();
    s.book.addAll(s.feeds.take());
    s.book.expire(now);
    // the sources: each off until switched on
    ham::SpotFeedConfig& f = s.feedCfg;
    const bool anyOn = f.cluster || f.rbn || f.rbnDigital || f.pota || f.sota || f.pskr;
    if (ImGui::CollapsingHeader(trw("Spot sources (off by default)"), anyOn ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("%s", tr("Privacy: a DX cluster and the Reverse Beacon Network are telnet servers that ask for your callsign; OnAir sends only that. POTA and SOTA spots are read from their public pages; nothing about you is sent. PSK Reporter is asked which stations heard your callsign (the callsign is in the request). Nothing runs while it is off."));
        bool ch = false;
        ch |= ImGui::Checkbox(tr("DX cluster"), &f.cluster);
        ImGui::SameLine(); ImGui::SetNextItemWidth(200 * gUi);
        static char host[96] = {};
        static bool hostInit = false;
        if (!hostInit) { hostInit = true; snprintf(host, sizeof host, "%s", f.clusterHost.c_str()); }
        if (ImGui::InputTextWithHint("##clhost", tr("cluster host"), host, sizeof host, ImGuiInputTextFlags_CharsNoBlank)) { f.clusterHost = host; ch = true; }
        ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
        if (ImGui::InputInt(tr("Port"), &f.clusterPort, 0, 0)) { f.clusterPort = std::max(1, std::min(65535, f.clusterPort)); ch = true; }
        ch |= ImGui::Checkbox(tr("RBN (CW, RTTY)"), &f.rbn);
        ImGui::SameLine(); ch |= ImGui::Checkbox(tr("RBN digital (FT8, FT4)"), &f.rbnDigital);
        ImGui::SameLine(); ch |= ImGui::Checkbox("POTA", &f.pota);
        ImGui::SameLine(); ch |= ImGui::Checkbox("SOTA", &f.sota);
        ImGui::SameLine(); ch |= ImGui::Checkbox(tr("PSK Reporter: who hears me"), &f.pskr);
        f.myCall = plat::basePrefs().getS("ham.call", "");
        if ((f.cluster || f.rbn || f.rbnDigital || f.pskr) && f.myCall.empty()) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", tr("Enter your callsign first: the cluster, RBN and PSK Reporter need it."));
        if (ch) { s.feeds.setConfig(f); hamStationSave(); }
        for (int i = 1; i < ham::kSpotSrcs; i++) {
            const std::string st = s.feeds.status((ham::SpotSrc)i);
            if (!st.empty()) ImGui::TextDisabled("%s: %s", ham::spotSrcName((ham::SpotSrc)i), st.c_str());
        }
        // alerts
        ImGui::SeparatorText(tr("Alerts"));
        bool ac = ImGui::Checkbox(tr("Alert on needed stations"), &s.alertsOn);
        ham::AlertRules& r = s.alerter.rules;
        ImGui::SameLine(); ac |= ImGui::Checkbox(tr("new DXCC"), &r.newDxcc);
        ImGui::SameLine(); ac |= ImGui::Checkbox(tr("on a new band"), &r.newDxccBand);
        ImGui::SameLine(); ac |= ImGui::Checkbox(tr("in a new mode"), &r.newDxccClass);
        ac |= ImGui::Checkbox(tr("new zone"), &r.newZone);
        ImGui::SameLine(); ac |= ImGui::Checkbox(tr("new state"), &r.newState);
        ImGui::SameLine(); ac |= ImGui::Checkbox(tr("new grid"), &r.newGrid);
        ImGui::SameLine(); ac |= ImGui::Checkbox(tr("new prefix"), &r.newPrefix);
        static char watch[512] = {};
        static bool wInit = false;
        if (!wInit) { wInit = true; std::string t; for (const auto& x : r.watch) t += x + " "; snprintf(watch, sizeof watch, "%s", t.c_str()); }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##watch", tr("watch list: calls or patterns, e.g. 3Y0J VP8* */MM"), watch, sizeof watch, ImGuiInputTextFlags_CharsUppercase)) {
            r.watch.clear();
            std::istringstream is(watch);
            std::string t;
            while (is >> t) r.watch.push_back(t);
            ac = true;
        }
        if (ac) hamStationSave();
        if (!s.lastAlert.empty()) ImGui::TextDisabled("%s %s", tr("Last alert:"), s.lastAlert.c_str());
    }
    // the filters
    const std::string band = ham::rxBandNameFor(a.freqMhz * 1e6);
    ImGui::Checkbox(tr("This band only"), &s.bandOnly);
    ImGui::SameLine(); ImGui::Checkbox(tr("Needed only"), &s.neededOnly);
    ImGui::PushID("##srcfilter");
    for (int i = 0; i < ham::kSpotSrcs; i++) {
        ImGui::SameLine();
        bool b = (s.srcMask >> i & 1u) != 0;
        if (ImGui::Checkbox(ham::spotSrcName((ham::SpotSrc)i), &b)) s.srcMask = b ? s.srcMask | (1u << i) : s.srcMask & ~(1u << i);
    }
    ImGui::PopID();
    // own decodes go in as "own" spots
    for (const auto& sp : o.spots.spots()) {
        if (now - sp.lastUtc > 60) break;   // newest first: only the fresh ones each frame
        ham::ExtSpot e;
        e.src = ham::SpotSrc::Own; e.call = sp.call; e.grid = sp.grid; e.mode = sp.mode; e.utc = sp.lastUtc; e.snr = sp.snr;
        e.freqHz = sp.dialHz + sp.hz; e.spotter = "me"; e.comment = sp.lastMsg;
        s.book.add(e);
    }
    const auto rows = s.book.filter(s.bandOnly ? band : "", "", s.srcMask);
    if (s.splitTxHz > 0 && !s.splitFor.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 1, 1), "%s %s: %s %.3f MHz", s.splitFor.c_str(), tr("works split"), tr("transmit on"), s.splitTxHz / 1e6);
        ImGui::SameLine(); if (ImGui::SmallButton(tr("Forget the split"))) { s.splitTxHz = 0; s.splitFor.clear(); }
    }
    const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("##hamcluster", 9, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn(tr("Call")); ImGui::TableSetupColumn(tr("Mode"));
    ImGui::TableSetupColumn(tr("Needed")); ImGui::TableSetupColumn(tr("Ref")); ImGui::TableSetupColumn(tr("Source")); ImGui::TableSetupColumn(tr("Spotter"));
    ImGui::TableSetupColumn(tr("Comment"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int shown = 0;
    for (const auto* rw : rows) {
        if (shown >= 400) break;
        const ham::ExtSpot& e = rw->s;
        const ham::NeedResult nr = e.src == ham::SpotSrc::PskReporter ? ham::NeedResult() : hamNeedOf(e.call, e.grid, e.band(), e.mode, e.src != ham::SpotSrc::Own);
        if (s.neededOnly && nr.level < 2) continue;
        shown++;
        ImGui::TableNextRow();
        ImGui::PushID(shown);
        ImGui::TableNextColumn();
        if (ImGui::Selectable(hhmm(e.utc).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
            const ham::HamModeInfo* mi = nullptr;
            for (const auto& m : ham::hamModeCatalog()) if (m.run == ham::HamRun::Slot && !strcmp(m.name, e.mode.c_str())) mi = &m;
            double tune = e.freqHz;
            if (mi && e.src != ham::SpotSrc::PskReporter) {   // a slot mode: the band's dial for it (the spot is dial + audio)
                const double d = hamStation().mem.recall(e.band(), mi->id, ham::dialFor(e.band(), mi->slotMode) * 1e6);
                if (d > 0) tune = d;
            }
            if (e.src != ham::SpotSrc::PskReporter && tune > 0) tuneFreq(a, tune / 1e6);
            if (e.src == ham::SpotSrc::Pota || e.src == ham::SpotSrc::Sota) s.hunted[e.call] = {e.src == ham::SpotSrc::Pota ? ham::Program::Pota : ham::Program::Sota, e.ref};
            double tx = 0;
            if (ham::parseSplit(e.comment, e.freqHz, tx)) { s.splitTxHz = tx; s.splitFor = e.call; } else { s.splitTxHz = 0; s.splitFor.clear(); }
            if (e.src != ham::SpotSrc::PskReporter && pick) pick(e.call, e.grid);
        }
        ImGui::TableNextColumn(); ImGui::Text("%.3f", e.freqHz / 1e6);
        ImGui::TableNextColumn();
        if (e.src == ham::SpotSrc::PskReporter) ImGui::TextDisabled("%s", e.call.c_str());
        else ImGui::TextColored(needColour(nr.level), "%s", e.call.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.mode.c_str());
        ImGui::TableNextColumn();
        if (!nr.label.empty() && nr.level > 0) ImGui::TextColored(needColour(nr.level), "%s", nr.label.c_str());
        else if (!nr.label.empty()) ImGui::TextDisabled("%s", nr.label.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.ref.c_str());
        if (!e.refName.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.refName.c_str());
        ImGui::TableNextColumn();
        if (e.snr > -99) ImGui::Text("%s %+d dB%s", ham::spotSrcName(e.src), e.snr, rw->spotters > 1 ? "+" : "");
        else ImGui::Text("%s%s", ham::spotSrcName(e.src), rw->spotters > 1 ? (" x" + std::to_string(rw->spotters)).c_str() : "");
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s %s", e.spotter.c_str(), e.spotterGrid.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.comment.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ---- activation ----
void hamActivateTab(App& a) {
    (void)a;
    HamStation& s = hamStation();
    HamOps& o = hamOps();
    const double now = nowUtc();
    ImGui::TextWrapped("%s", tr("Parks on the Air, Summits on the Air, WWFF and IOTA. While an activation runs, every logged QSO carries your reference (MY_SIG, MY_POTA_REF ...). A station picked from a POTA or SOTA spot gets its reference in the log too."));
    int prog = (int)s.act.prog;
    ImGui::SetNextItemWidth(120 * gUi);
    bool ch = ImGui::Combo(tr("Program"), &prog, "none\0POTA\0SOTA\0WWFF\0IOTA\0");
    if (ch) s.act.prog = (ham::Program)prog;
    static char refs[128] = {};
    static bool init = false;
    if (!init) { init = true; std::string t; for (const auto& x : s.act.refs) t += (t.empty() ? "" : ",") + x; snprintf(refs, sizeof refs, "%s", t.c_str()); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::InputTextWithHint(tr("My reference"), "US-0001,US-0002  /  W7W/LC-001", refs, sizeof refs, ImGuiInputTextFlags_CharsUppercase)) {
        s.act.refs.clear();
        std::string cur;
        for (char c : std::string(refs) + ",") { if (c == ',') { if (!ham::refNormal(cur).empty()) s.act.refs.push_back(ham::refNormal(cur)); cur.clear(); } else cur += c; }
        ch = true;
    }
    bool bad = false;
    for (const auto& r : s.act.refs) if (!ham::refValid(s.act.prog, r)) bad = true;
    if (s.act.prog != ham::Program::None && bad) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", tr("does not look like a reference of this program")); }
    if (ImGui::Button(s.act.startUtc > 0 && s.act.active() ? tr("Restart the activation now") : tr("Start the activation"))) { s.act.startUtc = now; ch = true; }
    ImGui::SameLine();
    if (ImGui::Button(tr("Stop"))) { s.act.prog = ham::Program::None; s.act.startUtc = 0; ch = true; }
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    {
        static const char* kMods[] = {"CQ", "CQ DX", "CQ POTA", "CQ SOTA", "CQ WWFF", "CQ TEST"};
        static const char* kIds[] = {"", "DX", "POTA", "SOTA", "WWFF", "TEST"};
        int cur = 0;
        for (int i = 0; i < 6; i++) if (s.cqMod == kIds[i]) cur = i;
        if (ImGui::Combo(tr("FT8 CQ"), &cur, kMods, 6)) { s.cqMod = kIds[cur]; ch = true; }
    }
    if (ch) hamStationSave();
    if (s.act.active()) {
        const ham::ActivationProgress p = ham::activationProgress(o.log.qsos(), s.act, now);
        char b[96];
        if (p.needed > 0) {
            snprintf(b, sizeof b, "%d / %d", p.qsos, p.needed);
            ImGui::ProgressBar(std::min(1.f, (float)p.qsos / (float)p.needed), ImVec2(260 * gUi, 0), b);
            ImGui::SameLine();
            ImGui::TextColored(p.done ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(1, 0.8f, 0.4f, 1), "%s", p.done ? tr("activated") : tr("QSOs still needed today"));
        } else ImGui::Text("%d %s", p.qsos, tr("QSOs"));
        std::string pb;
        for (const auto& kv : p.perBand) pb += kv.first + " " + std::to_string(kv.second) + "   ";
        if (!pb.empty()) ImGui::TextDisabled("%s", pb.c_str());
        if (s.act.prog == ham::Program::Pota) {
            for (const auto& r : s.act.refs) {
                ImGui::PushID(r.c_str());
                if (ImGui::Button((std::string(tr("Export the POTA file for ")) + r).c_str())) {
                    const std::string call = plat::basePrefs().getS("ham.call", "");
                    const std::string p2 = plat::saveFileDialog(ham::potaFileName(call, r, now).c_str());
                    if (!p2.empty()) hamToast(writeAll(p2, o.log.exportAdif(ham::activationQsos(o.log.qsos(), ham::Program::Pota, r, now))) ? "Wrote " + p2 : "Cannot write " + p2);
                }
                ImGui::PopID();
            }
            ImGui::TextDisabled("%s", tr("Upload the file at pota.app yourself; OnAir does not send it."));
        }
        if (s.act.prog == ham::Program::Sota && !s.act.refs.empty() && ImGui::Button(tr("Export the SOTA CSV"))) {
            const std::string p2 = plat::saveFileDialog("sota.csv");
            if (!p2.empty()) hamToast(writeAll(p2, ham::sotaCsv(o.log.qsos(), s.act.refs.front(), plat::basePrefs().getS("ham.call", ""), s.act.startUtc)) ? "Wrote " + p2 : "Cannot write " + p2);
        }
    }
    if (!s.hunted.empty()) {
        ImGui::SeparatorText(tr("Hunting"));
        ImGui::TextDisabled("%s", tr("Stations picked from POTA / SOTA spots; their reference goes into the log with the QSO."));
        for (const auto& kv : s.hunted) ImGui::Text("%s  %s %s", kv.first.c_str(), ham::programName(kv.second.first), kv.second.second.c_str());
        if (ImGui::SmallButton(tr("Clear the list"))) s.hunted.clear();
    }
}

void hamStationOnLog(ham::LogQso& q) {
    HamStation& s = hamStation();
    HamOps& o = hamOps();
    if (s.act.active()) ham::applyActivation(q, s.act);
    auto it = s.hunted.find(q.call);
    if (it == s.hunted.end()) it = s.hunted.find(ham::baseCall(q.call));
    if (it != s.hunted.end()) ham::applyHunted(q, it->second.first, it->second.second);
    const ham::CallbookEntry e = s.cb.local(q.call, &o.log.qsos());
    if (!e.empty()) ham::callbookFill(q, e);
    s.needDirty = true;
}

// ---- callsign lookup ----
void hamCallbookSettings() {
    HamStation& s = hamStation();
    ImGui::TextWrapped("%s", tr("Optional. With an account at QRZ.com (XML subscription) or HamQTH, OnAir looks up the name, QTH and grid of the station you work. Your user name and password stay in OnAir's settings on this computer and are sent only to that service, over HTTPS, in the request body. Off: only the cache and your own log are used."));
    int sv = (int)s.cbCfg.service;
    ImGui::SetNextItemWidth(140 * gUi);
    bool ch = ImGui::Combo(tr("Online lookup"), &sv, "off\0QRZ.com\0HamQTH\0");
    static char user[64] = {}, pw[64] = {};
    static bool init = false;
    if (!init) { init = true; snprintf(user, sizeof user, "%s", s.cbCfg.user.c_str()); snprintf(pw, sizeof pw, "%s", s.cbCfg.password.c_str()); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
    ch |= ImGui::InputText(tr("User"), user, sizeof user, ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
    ch |= ImGui::InputText(tr("Password"), pw, sizeof pw, ImGuiInputTextFlags_Password);
    if (ch) {
        s.cbCfg.service = (ham::CallbookService)sv; s.cbCfg.user = user; s.cbCfg.password = pw;
        s.cb.setConfig(s.cbCfg);
        hamStationSave();
    }
    ImGui::TextDisabled("%s %d", tr("Calls in the cache:"), (int)s.cb.cacheSize());
}
void hamCallbookPanel(const std::string& call) {
    HamStation& s = hamStation();
    HamOps& o = hamOps();
    if (call.empty()) return;
    if (call != s.cbCall && !s.cbBusy) {
        if (s.cbWorker.joinable()) s.cbWorker.join();
        s.cbCall = call;
        s.cbErr.clear();
        s.cbEntry = s.cb.local(call, &o.log.qsos());
        if (s.cbCfg.ready() && (s.cbEntry.empty() || s.cbEntry.source == "log")) {
            s.cbBusy = true;
            const double now = nowUtc();
            s.cbWorker = std::thread([&s, call, now] {
                ham::CallbookEntry e;
                std::string err;
                const bool ok = s.cb.lookup(call, now, e, err);
                std::lock_guard<std::mutex> l(s.cbMu);
                if (s.cbCall == call) {
                    if (ok) { ham::CallbookEntry m = e; ham::callbookMerge(m, s.cbEntry); s.cbEntry = m; }
                    else s.cbErr = err;
                }
                s.cbBusy = false;
            });
        }
    }
    std::lock_guard<std::mutex> l(s.cbMu);
    const ham::CallbookEntry& e = s.cbEntry;
    if (!e.empty()) {
        std::string t = e.name;
        if (!e.qth.empty()) t += (t.empty() ? "" : ", ") + e.qth;
        if (!e.state.empty()) t += " " + e.state;
        ImGui::TextUnformatted(t.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s: %s", tr("from"), e.source.c_str());
            if (!e.county.empty()) ImGui::Text("%s: %s", tr("county"), e.county.c_str());
            if (!e.country.empty()) ImGui::Text("%s", e.country.c_str());
            if (!e.qslVia.empty()) ImGui::Text("QSL: %s", e.qslVia.c_str());
            if (e.lotw) ImGui::TextUnformatted(tr("uses LoTW"));
            if (e.eqsl) ImGui::TextUnformatted(tr("uses eQSL"));
            ImGui::EndTooltip();
        }
        if (!e.grid.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", e.grid.c_str()); }
    } else if (s.cbBusy) ImGui::TextDisabled("%s", tr("looking up..."));
    else if (!s.cbErr.empty()) ImGui::TextDisabled("%s: %s", tr("lookup"), s.cbErr.c_str());
}

void hamStationWizardExtras(char* grid, size_t gridN) {
    HamStation& s = hamStation();
    HamOps& o = hamOps();
    static char cc[8] = {};
    static bool init = false;
    if (!init) { init = true; snprintf(cc, sizeof cc, "%s", s.country.c_str()); }
    ImGui::SetNextItemWidth(60 * gUi);
    if (ImGui::InputText(tr("Country (ISO code)"), cc, sizeof cc, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank)) {
        s.country = cc;
        if (const int r = ham::regionForCountry(s.country)) o.region = r;
        hamStationSave();
        hamSaveStation();
    }
    ImGui::SameLine();
    const std::string plan = ham::hamCountryPlanName(s.country);
    if (!plan.empty()) ImGui::TextDisabled("%s, IARU Region %d", plan.c_str(), ham::regionForCountry(s.country));
    else if (cc[0]) ImGui::TextDisabled("%s", tr("no built-in band plan for it: the region is used"));
    if (receiverLocationSet() && !ham::hamGridValid(grid)) {   // the station step has the same button; here only while the grid is still missing
        double la = 0, lo = 0;
        receiverLocation(la, lo);
        const std::string g = toMaidenhead(la, lo, 6);
        if (ImGui::SmallButton((std::string(tr("Use my position: ")) + g).c_str())) snprintf(grid, gridN, "%s", g.c_str());
    }
    ImGui::SeparatorText(tr("Antennas"));
    static char ant[64] = {};
    for (size_t i = 0; i < s.antennas.size(); i++) {
        ImGui::PushID((int)i);
        if (ImGui::SmallButton("x")) { s.antennas.erase(s.antennas.begin() + (long)i); hamStationSave(); ImGui::PopID(); break; }
        ImGui::SameLine();
        const bool cur = o.antenna == s.antennas[i];
        if (ImGui::RadioButton(s.antennas[i].c_str(), cur)) { o.antenna = s.antennas[i]; hamSaveStation(); }
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputTextWithHint("##addant", tr("e.g. dipole 40 m, 3 el Yagi"), ant, sizeof ant);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Add")) && ant[0]) { s.antennas.push_back(ant); o.antenna = ant; ant[0] = 0; hamStationSave(); hamSaveStation(); }
}

// ---- band hopping ----
std::string hamHopBand(double slotUtc, double slotSec, const std::string& curBand) {
    HamStation& s = hamStation();
    if (!s.hopOn) return "";
    const ham::HopEntry* e = s.hop.at(slotUtc);
    if (!e) return "";
    if (e->band == "auto") {
        s.hopper.slotS = slotSec;
        const std::string b = s.hopper.next(slotUtc);
        return b != curBand ? b : "";
    }
    return e->band != curBand ? e->band : "";
}
void hamHopSpot(const std::string& band, double utc) { if (!band.empty()) hamStation().hopper.spot(band, utc); }
void hamHopUi() {
    HamStation& s = hamStation();
    bool ch = ImGui::Checkbox(tr("Band hopping"), &s.hopOn);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("While no QSO runs, the band follows the schedule (UTC). \"auto\" picks the band with the most decodes in the last hour and visits every band now and then."));
    static char text[512] = {};
    static bool init = false;
    if (!init) { init = true; snprintf(text, sizeof text, "%s", s.hopText.c_str()); }
    static std::string err;
    if (ImGui::InputTextMultiline("##hop", text, sizeof text, ImVec2(260 * gUi, ImGui::GetTextLineHeight() * 5))) {
        ham::HopSchedule h;
        if (ham::HopSchedule::parse(text, h, err)) { s.hop = h; s.hopText = text; err.clear(); ch = true; }
    }
    if (!err.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", err.c_str());
    if (const ham::HopEntry* e = s.hop.at(nowUtc())) ImGui::TextDisabled("%s %s", tr("now:"), e->band.c_str());
    if (ch) hamStationSave();
}

// ---- logbook tools ----
void hamLogToolsUi() {
    HamStation& s = hamStation();
    HamOps& o = hamOps();
    const std::string bkDir = plat::dataDir() + "/ham-backups";
    if (!s.backupChecked) {   // once a day, when the log is first shown
        s.backupChecked = true;
        if (!o.logPath.empty() && o.log.size() > 0 && ham::logBackupDue(bkDir, nowUtc())) { std::string err; ham::logBackup(o.logPath, bkDir, nowUtc(), 30, err); }
    }
    if (s.tqslState == 2 || s.tqslState == 3) {
        if (s.tqslWorker.joinable()) s.tqslWorker.join();
        if (s.tqslState == 2) {   // mark what was signed
            for (const auto& q : o.log.qsos()) {
                const auto it = q.extra.find("LOTW_QSL_SENT");
                if (it != q.extra.end() && it->second == "Y") continue;
                ham::LogQso c = q;
                c.extra["LOTW_QSL_SENT"] = "Y";
                o.log.update(c);
            }
            saveLogNow();
        }
        hamToast(s.tqslMsg);
        s.tqslState = 0;
    }
    if (!ImGui::CollapsingHeader(trw("Logbook tools: Cabrillo, QSL confirmations, backups, LoTW"))) return;
    if (ImGui::Button(tr("Import Cabrillo"))) {
        const std::string p = plat::openFileDialog();
        bool ok = false;
        const std::string t = p.empty() ? "" : readAll(p, ok);
        if (ok) {
            const ham::CabrilloLog L = ham::cabrilloParse(t);
            int added = 0, dupes = 0;
            for (const auto& q : L.qsos) {
                if (o.log.findDupe(q.call, q.band, q.mode, true, true, q.startUtc, 60) >= 0) { dupes++; continue; }
                o.log.add(q);
                added++;
            }
            saveLogNow();
            char b[160]; snprintf(b, sizeof b, "Cabrillo: %d QSOs added, %d already in the log, %d lines unreadable", added, dupes, L.bad);
            hamToast(b);
        } else if (!p.empty()) hamToast("Cannot read " + p);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("QSL confirmations from a downloaded ADIF file:"));
    static const char* kSrc[] = {"LoTW", "eQSL", "QSL cards"};
    for (int i = 0; i < 3; i++) {
        ImGui::SameLine();
        if (ImGui::Button(kSrc[i])) {
            const std::string p = plat::openFileDialog();
            bool ok = false;
            const std::string t = p.empty() ? "" : readAll(p, ok);
            if (ok) {
                const ham::MergeResult m = ham::mergeConfirmations(o.log, t, i == 0 ? ham::QslSource::Lotw : i == 1 ? ham::QslSource::Eqsl : ham::QslSource::Card);
                saveLogNow();
                char b[160]; snprintf(b, sizeof b, "%s: %d matched, %d newly confirmed, %d not in the log", kSrc[i], m.matched, m.newlyConfirmed, m.unmatched);
                hamToast(b);
            }
        }
    }
    const ham::QslSummary q = ham::qslSummary(o.log.qsos());
    ImGui::TextDisabled("%d QSOs: %d confirmed (LoTW %d, cards %d), eQSL %d, QSL sent %d and still waiting %d", q.qsos, q.confirmed, q.lotw, q.rcvd, q.eqsl, q.sent, q.pending);
    // backups
    if (ImGui::Button(tr("Back up the log now"))) {
        std::string err;
        const std::string p = ham::logBackup(o.logPath, bkDir, nowUtc(), 30, err);
        hamToast(p.empty() ? "Backup failed: " + err : "Saved " + p);
    }
    ImGui::SameLine();
    const auto bks = ham::logBackups(bkDir);
    ImGui::TextDisabled("%d %s %s", (int)bks.size(), tr("backups (one a day, the last 30 kept) in"), bkDir.c_str());
    // LoTW through TQSL
    ImGui::SeparatorText("LoTW (TQSL)");
    ImGui::TextWrapped("%s", tr("OnAir does not hold your LoTW certificate. With TQSL installed, OnAir writes the QSOs not yet sent to a file and asks TQSL to sign and upload it; TQSL uses your own certificate and station location."));
    static char tp[256] = {}, loc[96] = {};
    static bool init = false;
    if (!init) { init = true; snprintf(tp, sizeof tp, "%s", s.tqslPath.c_str()); snprintf(loc, sizeof loc, "%s", s.tqslLocation.c_str()); }
    ImGui::SetNextItemWidth(220 * gUi);
    bool ch = ImGui::InputText(tr("TQSL program"), tp, sizeof tp);
    ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi);
    ch |= ImGui::InputText(tr("Station location"), loc, sizeof loc);
    if (ch) { s.tqslPath = tp; s.tqslLocation = loc; hamStationSave(); }
    std::vector<size_t> todo;
    for (size_t i = 0; i < o.log.qsos().size(); i++) {
        const auto it = o.log.qsos()[i].extra.find("LOTW_QSL_SENT");
        if (it == o.log.qsos()[i].extra.end() || it->second != "Y") todo.push_back(i);
    }
    ImGui::BeginDisabled(todo.empty() || s.tqslState != 0);
    if (ImGui::Button((std::string(tr("Sign and upload with TQSL")) + " (" + std::to_string(todo.size()) + ")").c_str())) {
        const std::string file = plat::dataDir() + "/onair_lotw_upload.adi";
        if (!writeAll(file, o.log.exportAdif(todo))) hamToast("Cannot write " + file);
        else {
            std::string cmd = ham::tqslCommand(s.tqslPath, s.tqslLocation, file);
#ifdef _WIN32
            cmd = "\"" + cmd + "\"";   // cmd.exe drops the outer quotes
#endif
            s.tqslState = 1;
            s.tqslWorker = std::thread([&s, cmd] {
                int rc = std::system(cmd.c_str());
#ifndef _WIN32
                if (rc != -1 && (rc & 0x7f) == 0) rc = (rc >> 8) & 0xff;   // the exit status
#endif
                s.tqslMsg = ham::tqslOk(rc) ? "TQSL signed and uploaded the QSOs" : "TQSL failed (exit " + std::to_string(rc) + "): check the program and the station location";
                s.tqslState = ham::tqslOk(rc) ? 2 : 3;
            });
        }
    }
    ImGui::EndDisabled();
    if (s.tqslState == 1) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("TQSL is running...")); }
}
