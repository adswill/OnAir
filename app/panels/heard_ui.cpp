// Search everything heard (View > Search everything heard): one box for a call sign, ICAO address, MMSI or frequency, over the history of every mode.
// The search and the grouping are core/src/station/heard.cpp; live positions come from the map screens' tracks.
#include "../app.h"
#include "dect2/common/time_fmt.h"
#include "dect2/msgdb/msgdb.h"
#include "dect2/station/heard.h"
#include "dect2/track/track_hub.h"
#include <cstdlib>
#include <ctime>

namespace st = dect2::station;
namespace trk = dect2::track;

namespace {

struct State {
    char q[96] = "";
    std::string last;
    std::vector<st::Heard> rows;
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

}  // namespace

void heardWindow(App& a) {
    if (!a.heardOpen) return;
    firstWindowSize(820 * gUi, 480 * gUi);
    if (!ImGui::Begin(trw("Search everything heard"), &a.heardOpen)) { ImGui::End(); return; }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##heardq", tr("Call sign, ICAO address, MMSI, frequency (162.025) or a word"), S.q, sizeof S.q);
    dect2::msgdb::Db& db = dect2::msgdb::global();
    if (!db.isOpen()) { ImGui::TextDisabled("%s", tr("The message history is not open yet. Open View > Messages once.")); ImGui::End(); return; }
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
            if (ImGui::Selectable(who.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) msgdbSearchFor(a, h.ident.empty() ? S.last : h.ident);
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
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
