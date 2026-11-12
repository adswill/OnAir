// ISM: line code classifier for the signals no protocol accepted (PWM / PPM / Manchester / PCM, lengths, rows, rtl_433 flex line). docs/WIRING-4.0.md
#include "d40_ui.h"
#include "dect2/ism/ism_tel.h"
#include "dect2/ism/ism_classify.h"
#include <cstdio>

void d40IsmClassifyLine(const dect2::IsmRaw& r) {
    std::vector<dect2::IsmTimedPulse> p;
    p.reserve(r.pulses.size());
    for (const auto& x : r.pulses) p.push_back({(double)x.pulseUs, (double)x.gapUs});
    const auto c = dect2::ismClassify(p);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Line code worked out from the pulse and gap lengths of this burst: how the bits are coded, the short and long length, the gap that separates repeated rows and the bits per row. The flex line can be pasted into rtl_433. Noise bursts are not claimed; the confidence tells how well the lengths fit."));
    ImGui::SameLine();
    if (c.coding == "unknown") { ImGui::TextDisabled("%s", tr("Classifier: no regular line code found.")); return; }
    ImGui::Text("%s: %s, %.0f / %.0f us, %d %s, %d %s (%.0f %%)", tr("Classifier"), c.coding.c_str(), c.shortUs, c.longUs, c.rows, tr("rows"), c.bitsPerRow, tr("bits per row"), c.confidence * 100);
    if (!c.flex.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy flex##d40ismflex")) ImGui::SetClipboardText(c.flex.c_str());
    }
}
