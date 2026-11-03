// GUI half of the shared export row (see export.h).
#include "export.h"
#include "../../app.h"

namespace kit {

void exportBar(const resexp::Table& t, const char* kind, std::string& note, const char* id) {
    ImGui::PushID(id);
    resexp::saveButton(t, kind, note, "sv");
    sameLineIf(ImGui::CalcTextSize(tr("Copy CSV")).x + 12 * gUi, -1.f);
    if (ImGui::SmallButton(tr("Copy CSV"))) {
        ImGui::SetClipboardText(clipboardCsv(t).c_str());
        note = std::string(tr("copied")) + " " + std::to_string(t.rows.size()) + " " + tr("rows");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", hasPositions(t) ? tr("Copy the table as CSV to the clipboard (positions included)") : tr("Copy the table as CSV to the clipboard"));
    if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    ImGui::PopID();
}

}  // namespace kit
