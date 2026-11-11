// CDMA2000 / IS-95 cells: the base stations heard, by pilot PN offset, with what their Sync Channel Message says.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/cdma/cdma_rx.h"
#include <cstdio>
#include <string>

namespace {
bool live(const App& a) { return a.rx.standard == 661; }   // the engine reports its standard code minus one

void tab(App& a) {
    ImGui::TextDisabled("%s", tr("Base stations heard in this 1.25 MHz channel by their pilot, with the system identity and time from their Sync Channel."));
    char cl[40]; snprintf(cl, sizeof cl, "%s##cdma", tr("Clear"));
    ImGui::SameLine(); if (ImGui::Button(cl)) a.engine.cdma().clearResults();
    if (!live(a)) { ImGui::TextDisabled("%s", tr("Start the receiver with this mode selected.")); return; }
    const CdmaTelemetry& t = a.rx.cdma;
    if (t.bandClass >= 0) ImGui::Text(tr("Band class %d"), t.bandClass);
    if (ImGui::BeginTable("##cdmacells", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("PN"); ImGui::TableSetupColumn("SID"); ImGui::TableSetupColumn("NID"); ImGui::TableSetupColumn("Ec/Io");
        ImGui::TableSetupColumn(tr("Carrier offset")); ImGui::TableSetupColumn(tr("Paging channel")); ImGui::TableSetupColumn(tr("Protocol"));
        ImGui::TableSetupColumn(tr("System time")); ImGui::TableSetupColumn(tr("Messages"));
        ImGui::TableHeadersRow();
        for (auto& c : t.cells) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (c.hasMsg) ImGui::Text("%d", c.pilotPn); else ImGui::TextDisabled("+%d", c.relOffset);
            ImGui::TableNextColumn(); if (c.hasMsg) ImGui::Text("%d", c.msg.sid);
            ImGui::TableNextColumn(); if (c.hasMsg) ImGui::Text("%d", c.msg.nid);
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", c.ecIoDb);
            ImGui::TableNextColumn(); ImGui::Text("%+.0f Hz", c.cfoHz);
            ImGui::TableNextColumn();
            if (c.hasMsg) {
                const double hz = t.bandClass >= 0 ? cdma::cdmaChannelHz(t.bandClass, c.msg.cdmaFreq) : 0;
                if (hz > 0) ImGui::Text("%s, ch %d (%.3f MHz)", cdma::pratText(c.msg.prat), c.msg.cdmaFreq, hz / 1e6);
                else ImGui::Text("%s, ch %d", cdma::pratText(c.msg.prat), c.msg.cdmaFreq);
                if (c.msg.extCdmaFreq != c.msg.cdmaFreq && ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Extended paging channel: ch %d"), c.msg.extCdmaFreq);
            }
            ImGui::TableNextColumn(); if (c.hasMsg) ImGui::Text("P_REV %d (min %d)", c.msg.pRev, c.msg.minPRev);
            ImGui::TableNextColumn();
            if (c.hasMsg) {
                ImGui::TextUnformatted(cdma::sysTimeText(c.msg.sysTime, c.msg.lpSec, c.msg.ltmOff, false).c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(tr("Local time %s (offset %+.1f h%s), %d leap seconds"), cdma::sysTimeText(c.msg.sysTime, c.msg.lpSec, c.msg.ltmOff, true).c_str(), c.msg.ltmOff * 0.5,
                                      c.msg.daylt ? tr(", daylight saving") : "", c.msg.lpSec);
            }
            ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)c.msgs, (unsigned long long)(c.msgs + c.crcFails));
        }
        ImGui::EndTable();
    }
}
void status(App& a) { if (!live(a)) return; ImGui::Text("%zu %s, %llu %s", a.rx.cdma.cells.size(), tr("base stations"), (unsigned long long)a.rx.cdma.scans, tr("scans")); }
void summary(const App& a, std::string& l1, std::string& l2) { l1 = cdmaSummary(a.rx.cdma); l2.clear(); }
}  // namespace

extern const ModeUi kCdmaUi;
const ModeUi kCdmaUi = { .sideTitle = "CELLS", .tabName = "Cells", .tabIcon = Ic::Radio, .tab = tab, .status = status, .summary = summary };
