// Search everything heard (View > Search everything heard): one box for a call sign, ICAO address, MMSI or frequency, over the history of every mode.
// The search and the grouping are core/src/station/heard.cpp; live positions come from the map screens' tracks.
#include "../app.h"
#include "dect2/common/time_fmt.h"
#include "dect2/msgdb/msgdb.h"
#include "dect2/station/fun.h"
#include "dect2/station/heard.h"
#include "dect2/track/track_hub.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>

namespace st = dect2::station;
namespace trk = dect2::track;

namespace {

struct State {
    char q[96] = "";
    std::string last;
    std::vector<st::Heard> rows;
    bool dossierOpen = false;
    st::Dossier dossier;
    // rare catches: the newest records are looked at every few seconds
    st::RareWatcher rare;
    bool rareStarted = false, rareLoaded = false, rareOn = true;
    int64_t rareLastId = 0;
    double rareNext = 0;
    std::vector<st::RareCatch> alerts;   // shown until dismissed
} S;

const char* kindText(st::QueryKind k) {
    switch (k) {
    case st::QueryKind::Icao: return tr("aircraft");
    case st::QueryKind::Mmsi: return tr("ship");
    case st::QueryKind::Callsign: return tr("call sign");
    default: return "";
    }
}

bool liveOf(const std::string& ident, double& lat, double& lon) {
    const st::QueryKind k = st::identKind(ident);
    if (k == st::QueryKind::Icao) return trk::liveFix(trk::keyFromIcao((uint32_t)std::strtoul(ident.c_str(), nullptr, 16)), lat, lon);
    return trk::liveFix(trk::keyFromText(ident), lat, lon);
}

std::string timeText(double t) {
    char b[40] = "";
    const std::time_t tt = (std::time_t)t;
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
    return b;
}

void rareTick() {
    if (!S.rareLoaded) { S.rareLoaded = true; S.rareOn = plat::prefs().getB("rareCatchAlerts", true); }
    dect2::msgdb::Db& db = dect2::msgdb::global();
    const double now = ImGui::GetTime();
    if (!S.rareOn || !db.isOpen() || now < S.rareNext) return;
    S.rareNext = now + 3;
    if (!S.rareStarted) {   // what is already in the history does not alert
        S.rareStarted = true;
        S.rare.setKnownModes(db.modes());
        dect2::msgdb::Filter f; f.limit = 1;
        const std::vector<dect2::msgdb::Record> last = db.query(f);
        S.rareLastId = last.empty() ? 0 : last[0].id;
        return;
    }
    dect2::msgdb::Filter f; f.limit = 500;
    std::vector<dect2::msgdb::Record> rows = db.query(f);
    std::sort(rows.begin(), rows.end(), [](const dect2::msgdb::Record& x, const dect2::msgdb::Record& y) { return x.id < y.id; });
    for (const dect2::msgdb::Record& r : rows) {
        if (r.id <= S.rareLastId) continue;
        S.rareLastId = r.id;
        st::RareCatch c;
        if (S.rare.check(r, c) && S.alerts.size() < 5) S.alerts.push_back(c);
    }
}

void rareBanner() {
    if (S.alerts.empty()) return;
    const ImGuiViewport* v = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(v->WorkPos.x + v->WorkSize.x - 12 * gUi, v->WorkPos.y + 40 * gUi), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("##rarecatch", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ImGui::TextColored(ImVec4(1, .8f, .3f, 1), "%s", tr("Rare catch"));
        int drop = -1;
        for (int i = 0; i < (int)S.alerts.size(); i++) {
            ImGui::PushID(i);
            ImGui::TextUnformatted(S.alerts[(size_t)i].text.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Dismiss"))) drop = i;
            ImGui::PopID();
        }
        if (drop >= 0) S.alerts.erase(S.alerts.begin() + drop);
    }
    ImGui::End();
}

void dossierWindow(App& a) {
    if (!S.dossierOpen) return;
    firstWindowSize(520 * gUi, 420 * gUi);
    if (!ImGui::Begin(trw("Dossier"), &S.dossierOpen)) { ImGui::End(); return; }
    const st::Dossier& d = S.dossier;
    ImGui::TextUnformatted(d.ident.c_str());
    if (!d.what.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr(d.what.c_str())); }
    if (!d.country.empty()) ImGui::Text("%s: %s", tr("Country"), d.country.c_str());
    if (!d.registration.empty()) ImGui::Text("%s: %s", tr("Registration"), d.registration.c_str());
    ImGui::Text("%s: %lld", tr("Messages"), (long long)d.messages);
    if (d.messages) ImGui::Text("%s %s   %s %s (UTC)", tr("First heard"), timeText(d.first).c_str(), tr("Last heard"), timeText(d.last).c_str());
    std::string list;
    for (const std::string& m : d.modes) { if (!list.empty()) list += ", "; list += m; }
    if (!list.empty()) ImGui::TextWrapped("%s: %s", tr("Modes"), list.c_str());
    list.clear();
    for (double f : d.freqsHz) { char b[32]; snprintf(b, sizeof b, "%.4f", f / 1e6); if (!list.empty()) list += ", "; list += b; }
    if (!list.empty()) ImGui::TextWrapped("%s (MHz): %s", tr("Frequencies"), list.c_str());
    if (d.hasPos) ImGui::Text("%s: %.3f, %.3f", tr("Last position"), d.lat, d.lon);
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Latest messages"));
    for (const dect2::msgdb::Record& r : d.recent) {
        ImGui::TextDisabled("%s  %s", timeText(r.time).c_str(), r.mode.c_str());
        if (!r.text.empty()) ImGui::TextWrapped("%s", r.text.c_str());
        const std::string plain = st::plainEnglish(r);
        if (!plain.empty()) ImGui::TextColored(ImVec4(.55f, .85f, .6f, 1), "%s", tr(plain.c_str()));
    }
    (void)a;
    ImGui::End();
}

}  // namespace

void heardWindow(App& a) {
    rareTick();
    rareBanner();
    dossierWindow(a);
    if (!a.heardOpen) return;
    firstWindowSize(820 * gUi, 480 * gUi);
    if (!ImGui::Begin(trw("Search everything heard"), &a.heardOpen)) { ImGui::End(); return; }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##heardq", tr("Call sign, ICAO address, MMSI, frequency (162.025) or a word"), S.q, sizeof S.q);
    dect2::msgdb::Db& db = dect2::msgdb::global();
    if (!db.isOpen()) (void)msgdbQuickSearch("", 1);   // starts the store when Messages has not been opened yet
    if (!db.isOpen()) { ImGui::TextDisabled("%s", tr("The message history could not be opened.")); ImGui::End(); return; }
    if (S.last != S.q) { S.last = S.q; S.rows = st::searchHeard(db, S.last, liveOf); }
    if (S.rows.empty() && S.q[0]) ImGui::TextDisabled("%s", tr("Not heard yet."));
    if (!S.rows.empty() && ImGui::BeginTable("##heard", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Who")); ImGui::TableSetupColumn(tr("What")); ImGui::TableSetupColumn(tr("Modes"));
        ImGui::TableSetupColumn(tr("Messages")); ImGui::TableSetupColumn(tr("First heard (UTC)")); ImGui::TableSetupColumn(tr("Last heard (UTC)")); ImGui::TableSetupColumn(tr("Position"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < S.rows.size(); i++) {
            const st::Heard& h = S.rows[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string who = h.ident.empty() ? std::string(tr("(no identity)")) : h.ident;
            if (ImGui::Selectable(who.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) msgdbSearchFor(a, h.ident.empty() ? S.last : h.ident);
            if (ImGui::IsItemHovered() && !h.lastText.empty()) ImGui::SetTooltip("%s", h.lastText.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(kindText(h.kind));
            ImGui::TableNextColumn();
            std::string modes;
            for (const std::string& m : h.modes) { if (!modes.empty()) modes += ", "; modes += m; }
            ImGui::TextUnformatted(modes.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%lld", (long long)h.count);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(timeText(h.first).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(timeText(h.last).c_str());
            ImGui::TableNextColumn();
            if (h.hasPos) ImGui::Text("%.3f, %.3f%s", h.lat, h.lon, h.live ? "  (live)" : "");
            if (!h.ident.empty()) { ImGui::SameLine(); if (ImGui::SmallButton(tr("Dossier"))) { S.dossier = st::dossier(db, h.ident); S.dossierOpen = true; } }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader(tr("Advanced"))) {
        if (ImGui::Checkbox(tr("Rare catch alerts"), &S.rareOn)) plat::prefs().setB("rareCatchAlerts", S.rareOn);
    }
    ImGui::End();
}
