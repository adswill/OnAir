// ADS-B arrivals and departures of one airport: the airport (nearest larger one by default, or a code), the board, every second.
// Core: dect2/adsb/adsb_airport_board.h; airports from dect2/airports/airports.h.
#include "adsb_internal.h"
#include "dect2/adsb/adsb_airport_board.h"
#include "dect2/airports/airports.h"
#include "i18n/i18n.h"
#include <cstdio>
#include <ctime>

namespace adsbui {

namespace {
struct Ap {
    bool loaded = false;
    std::string code;              // ICAO code; empty: the nearest large / medium airport
    const dect2::Airport* ap = nullptr;
    dect2::AirportBoard board;
    char buf[16] = {};
    double last = 0;
    double pickedLat = 1e9, pickedLon = 1e9;
};
Ap A;

void pick() {
    const dect2::AirportDb& db = dect2::AirportDb::builtin();
    A.ap = nullptr;
    if (!A.code.empty()) A.ap = db.find(A.code);
    if (!A.ap && receiverLocationSet()) {
        for (const auto& h : db.nearby(S.refLat, S.refLon, 60, 50, false)) { const dect2::Airport& x = db.all()[h.index]; if (x.kind == 'L' || x.kind == 'M') { A.ap = &x; break; } }
    }
    A.pickedLat = S.refLat; A.pickedLon = S.refLon;
}
}  // namespace

void airportTick(App& a) {
    if (!A.loaded) { A.loaded = true; A.code = plat::prefs().getS("adsbBoardAirport", ""); std::snprintf(A.buf, sizeof A.buf, "%s", A.code.c_str()); pick(); }
    if (A.code.empty() && (A.pickedLat != S.refLat || A.pickedLon != S.refLon)) pick();
    if (!live(a) || !A.ap || ImGui::GetTime() - A.last < 1.0) return;
    A.last = ImGui::GetTime();
    dect2::AirportBoardConfig c;
    c.lat = A.ap->latDeg; c.lon = A.ap->lonDeg; c.elevFt = A.ap->elevFt;
    A.board.update(a.rx.adsb.aircraft, c, (int64_t)time(nullptr), [](uint32_t icao) { return std::make_pair(std::string(), registrationOf(icao)); });
}

void airportView(App& a) {
    (void)a;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Airport")); ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::InputTextWithHint("##adsbap", tr("nearest"), A.buf, sizeof A.buf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase)) {
        A.code = A.buf; plat::prefs().setS("adsbBoardAirport", A.code); pick();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("ICAO or IATA code, Enter. Empty: the nearest large or medium airport to the receiver."));
    ImGui::SameLine();
    if (A.ap) ImGui::TextDisabled("%s %s, %d ft, %.4f %.4f", A.ap->icao.c_str(), A.ap->name.c_str(), A.ap->elevFt, A.ap->latDeg, A.ap->lonDeg);
    else { ImGui::TextColored(kWarn, "%s", tr(A.code.empty() ? "Set the receiver position, or type an airport code." : "Airport not found.")); return; }
    ImGui::TextDisabled("%s", tr("Landed: came down near the field and reported the ground, or was lost low on the approach. Took off: from the ground (or first heard low) climbing out. Within 8 km, below 2000 ft above the field."));
    const auto& v = A.board.all();
    if (!ImGui::BeginTable("##adsbapt", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(""); ImGui::TableSetupColumn(tr("Callsign")); ImGui::TableSetupColumn(tr("Reg"));
    ImGui::TableSetupColumn(tr("Runway")); ImGui::TableSetupColumn("ICAO", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); { const time_t t = (time_t)it->utc; struct tm l = *localtime(&t); char b[16]; std::strftime(b, sizeof b, "%H:%M", &l); ImGui::TextUnformatted(b); }
        ImGui::TableNextColumn(); ImGui::TextColored(it->landing ? kGood : kWarn, "%s", it->landing ? tr("landed") : tr("took off"));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(it->callsign.empty() ? "-" : it->callsign.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(it->registration.empty() ? "-" : it->registration.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(it->runway.empty() ? "-" : it->runway.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(icaoText(it->icao).c_str());
    }
    ImGui::EndTable();
    if (v.empty()) ImGui::TextDisabled("%s", tr("No movements yet."));
}

}  // namespace adsbui
