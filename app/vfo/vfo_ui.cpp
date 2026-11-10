// Multi-VFO screens (docs/MULTI-VFO.md): bands on the spectrum and waterfall, the Add channel click, the channel list window and the per-frame
// hand-over of the rows to the engine. All the DSP is in core/src/engine/vfo.cpp; nothing here runs when there are no rows.
#include "app.h"
#include "gfx/plot.h"
#include "dect2/engine/vfo.h"
#include "i18n/i18n.h"
#include "gfx/drag_tune.h"
#include "ui/touch.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using dect2::VfoMode;

namespace {
int gDrag = -1;   // row whose centre is held
bool gGrabTune = false; double gGrabOff = 0;   // drag to tune: grabbed anywhere in its band, by this offset from its centre (MHz)
const ImVec4 kCols[6] = {{0.95f, 0.75f, 0.25f, 1}, {0.35f, 0.80f, 0.95f, 1}, {0.55f, 0.90f, 0.40f, 1}, {0.95f, 0.45f, 0.70f, 1}, {0.75f, 0.60f, 0.95f, 1}, {0.95f, 0.55f, 0.30f, 1}};
ImVec4 colFor(const VfoRow& r, float a) { ImVec4 c = kCols[(size_t)(r.id % 6)]; c.w = a; return c; }
std::vector<dect2::VfoConfig> gPushed;
bool gPushedValid = false;
double gPushedCentre = -1;

int nextId(const App& a) { int m = 0; for (const VfoRow& r : a.vfoRows) m = std::max(m, r.id); return m + 1; }

std::vector<dect2::VfoConfig> configs(const App& a, double centreHz) {
    std::vector<dect2::VfoConfig> v;
    for (const VfoRow& r : a.vfoRows) {
        if (!r.on) continue;
        dect2::VfoConfig c;
        c.id = r.id; c.offsetHz = r.freqMhz * 1e6 - centreHz; c.bandwidthHz = r.bwKhz * 1e3; c.mode = (VfoMode)r.mode;
        c.squelchDb = r.sqDb; c.volume = r.vol; c.pan = r.pan; c.mute = r.mute;
        v.push_back(c);
    }
    return v;
}

bool sameCfg(const std::vector<dect2::VfoConfig>& x, const std::vector<dect2::VfoConfig>& y) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); i++)
        if (x[i].id != y[i].id || x[i].offsetHz != y[i].offsetHz || x[i].bandwidthHz != y[i].bandwidthHz || x[i].mode != y[i].mode || x[i].squelchDb != y[i].squelchDb ||
            x[i].volume != y[i].volume || x[i].pan != y[i].pan || x[i].mute != y[i].mute) return false;
    return true;
}
} // namespace

void vfoAddAt(App& a, double mhz) {
    VfoRow r;
    r.id = nextId(a);
    r.freqMhz = std::round(mhz * 1e4) / 1e4;   // 100 Hz
    r.mode = (int)vfoDefaultMode(mhz, engineStd(a));
    r.sqDb = dect2::vfoIsDecoder((VfoMode)r.mode) ? -150 : -50;
    a.vfoRows.push_back(r);
    a.vfoPanel = true; a.vfoArm = false;
    savePrefs(a);
}

void vfoSync(App& a) {
    static bool wasSilent = false;
    if (a.vfoRows.empty() && !gPushedValid) return;   // nothing, never pushed: the engine is not touched at all
    if (!a.engine.running()) return;
    const double centre = a.engine.captureCentreHz();
    std::vector<dect2::VfoConfig> c = configs(a, centre);
    if (!gPushedValid || gPushedCentre != centre || !sameCfg(c, gPushed)) {
        a.engine.setVfoChannels(c);
        gPushed = c; gPushedValid = !c.empty(); gPushedCentre = centre;
    }
    if (wasSilent != a.muted) { a.engine.setVfoSound(!a.muted); wasSilent = a.muted; }
}

void vfoBands(const App& a, bool fill) {
    if (a.vfoRows.empty()) return;
    const plt::Rect lim = plt::GetPlotLimits();
    for (const VfoRow& r : a.vfoRows) {
        const double h = vfoHalfWidthMhz(r), f = r.freqMhz;
        if (f + h < lim.X.Min || f - h > lim.X.Max) continue;
        const float al = r.on ? 1.f : 0.35f;
        char id[24];
        if (fill) { plt::Spec b; b.FillColor = colFor(r, 0.16f * al); snprintf(id, sizeof id, "vfob%d", r.id); plt::PlotVBand(id, f - h, f + h, b); }
        const double c[1] = {f};
        plt::Spec cs; cs.LineColor = colFor(r, 0.9f * al); cs.LineWeight = gDrag == r.id ? 2.4f : 1.4f;
        snprintf(id, sizeof id, "vfoc%d", r.id);
        plt::PlotInfLines(id, c, 1, cs);
        if (fill) { char lb[24]; snprintf(lb, sizeof lb, "%d %s", r.id, dect2::vfoModeName((VfoMode)r.mode)); plt::PlotText(lb, f, lim.Y.Max, ImVec2(4 * gUi, 18 * gUi)); }
    }
}

bool vfoPlotMouse(App& a, int plotId) {
    (void)plotId;
    const bool hov = plt::IsPlotHovered();
    if (gDrag >= 0) {   // a centre is held
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        for (VfoRow& r : a.vfoRows) if (r.id == gDrag) {
            if (ImGui::IsMouseDown(0)) {
                if (gGrabTune) {   // drag to tune: the point it was grabbed by stays under the pointer, snapped to the zoom (Shift / Ctrl: fine)
                    const plt::Rect lim = plt::GetPlotLimits();
                    const double hzPerPx = (lim.X.Max - lim.X.Min) * 1e6 / std::max(1.f, plt::GetPlotSize().x);
                    const ImGuiIO& io = ImGui::GetIO();
                    r.freqMhz = dragtune::vfoDragCentre(plt::GetPlotMousePos().x, gGrabOff, dragtune::dragStepHz(0, 100, hzPerPx, io.KeyShift || io.KeyCtrl));
                } else r.freqMhz = std::round(plt::GetPlotMousePos().x * 1e4) / 1e4;
                ImGui::SetTooltip("channel %d: %.4f MHz", r.id, r.freqMhz);
            }
            else { gDrag = -1; savePrefs(a); }
            return true;
        }
        gDrag = -1;
        return true;
    }
    if (!hov) return false;
    if (a.vfoArm || ImGui::GetIO().KeyAlt) {   // Add channel: the next click puts one here
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const double mhz = plt::GetPlotMousePos().x;
        ImGui::SetTooltip("%s %.4f MHz", tr("Click to add a channel at"), mhz);
        if (ImGui::IsMouseReleased(0)) { vfoAddAt(a, mhz); }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) a.vfoArm = false;
        return true;
    }
    if (a.vfoRows.empty()) return false;
    const plt::Rect lim = plt::GetPlotLimits();
    const ImVec2 pp = plt::GetPlotPos(), ps = plt::GetPlotSize();
    const double span = std::max(1e-9, lim.X.Max - lim.X.Min);
    const float mx = ImGui::GetIO().MousePos.x;
    // drag to tune (Settings > Radio > Drag on spectrum): a press anywhere in a channel's band grabs that channel, as SDR++ grabs a VFO
    const bool grabBand = specDragTunes(a) && !touch::on();
    int grab = -1;
    if (grabBand) {
        std::vector<double> c, h;
        for (const VfoRow& r : a.vfoRows) { c.push_back(r.freqMhz); h.push_back(vfoHalfWidthMhz(r)); }
        grab = dragtune::grabVfo(c.data(), h.data(), (int)c.size(), plt::GetPlotMousePos().x, span / std::max(1.f, ps.x), 6 * gUi);
    }
    for (size_t i = 0; i < a.vfoRows.size(); i++) {
        const VfoRow& r = a.vfoRows[i];
        const float px = pp.x + (float)((r.freqMhz - lim.X.Min) / span * ps.x);
        if (grabBand ? grab == (int)i && !plt::PressOnScale() : std::fabs(mx - px) < 6 * gUi) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::SetTooltip("%d %s  %.4f MHz  (%s)", r.id, dect2::vfoModeName((VfoMode)r.mode), r.freqMhz, tr("drag to move, double click to open the list"));
            if (ImGui::IsMouseDoubleClicked(0)) a.vfoPanel = true;
            else if (ImGui::IsMouseClicked(0)) { gDrag = r.id; gGrabTune = grabBand; gGrabOff = plt::GetPlotMousePos().x - r.freqMhz; }
            return true;
        }
    }
    return false;
}

void vfoPopupSection(App& a) {
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("Channels (several at once)"));
    if (ImGui::Button(a.vfoArm ? tr("Click on the waterfall...") : tr("Add channel"))) a.vfoArm = !a.vfoArm;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Puts one more channel on this capture: its own frequency, mode (FM, AM, SSB, pagers, APRS), squelch and volume. Alt+click on the spectrum does the same."));
    ImGui::SameLine();
    if (ImGui::Button(a.vfoPanel ? tr("Hide list") : tr("Channel list"))) a.vfoPanel = !a.vfoPanel;
}

void vfoPanelWindow(App& a) {
    if (!a.vfoPanel) return;
    firstWindowSize(560 * gUi, 260 * gUi);
    bool open = true;
    if (ImGui::Begin(trw("Channels"), &open)) {
        if (ImGui::Button(a.vfoArm ? tr("Click on the waterfall...") : tr("Add channel"))) a.vfoArm = !a.vfoArm;
        ImGui::SameLine();
        if (ImGui::Button(tr("Add here")) ) vfoAddAt(a, a.freqMhz);
        ImGui::SameLine();
        const double load = a.engine.vfoLoad();
        if (!a.engine.running()) ImGui::TextDisabled("%s", tr("Start the radio: the channels share its capture."));
        else if (load > 0.85) ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.3f, 1), "%s %.0f%%", tr("The CPU cannot keep up with the channels:"), load * 100);
        else if (!a.vfoRows.empty()) ImGui::TextDisabled("CPU %.0f%%", load * 100);
        const std::vector<dect2::VfoStatus> st = a.engine.vfoStatus();
        int del = -1;
        if (ImGui::BeginTable("##vfot", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX)) {
            const char* hd[] = {"", "MHz", "Mode", "BW kHz", "Squelch dB", "Volume / pan", "dB", ""};
            for (const char* h : hd) ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < a.vfoRows.size(); i++) {
                VfoRow& r = a.vfoRows[i];
                ImGui::PushID(r.id);
                ImGui::TableNextRow();
                bool ch = false;
                ImGui::TableNextColumn(); ch |= ImGui::Checkbox("##on", &r.on);
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(95 * gUi); ch |= ImGui::InputDouble("##f", &r.freqMhz, 0, 0, "%.4f");
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(70 * gUi);
                if (ImGui::BeginCombo("##m", dect2::vfoModeName((VfoMode)r.mode))) {
                    for (int m = 0; m < (int)VfoMode::Count; m++) if (ImGui::Selectable(dect2::vfoModeName((VfoMode)m), m == r.mode)) { r.mode = m; ch = true; }
                    ImGui::EndCombo();
                }
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(60 * gUi); ch |= ImGui::InputDouble("##bw", &r.bwKhz, 0, 0, r.bwKhz > 0 ? "%.1f" : "auto");
                ImGui::TableNextColumn();
                if (dect2::vfoIsDecoder((VfoMode)r.mode)) ImGui::TextDisabled("-");
                else { ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputDouble("##sq", &r.sqDb, 0, 0, r.sqDb <= -150 ? "open" : "%.0f"); }
                ImGui::TableNextColumn();
                if (dect2::vfoIsDecoder((VfoMode)r.mode)) {
                    const auto it = std::find_if(st.begin(), st.end(), [&](const dect2::VfoStatus& s) { return s.id == r.id; });
                    ImGui::TextDisabled("%s", it != st.end() && !it->info.empty() ? it->info.c_str() : tr("decoding"));
                } else {
                    ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::SliderFloat("##v", &r.vol, 0.f, 2.f, "%.2f");
                    ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ch |= ImGui::SliderFloat("##p", &r.pan, -1.f, 1.f, "%.1f");
                    ImGui::SameLine(); ch |= ImGui::Checkbox("M", &r.mute);
                }
                ImGui::TableNextColumn();
                const auto it = std::find_if(st.begin(), st.end(), [&](const dect2::VfoStatus& s) { return s.id == r.id; });
                if (it != st.end()) ImGui::TextColored(it->open ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "%.0f", it->levelDb);
                else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("x")) del = (int)i;
                const std::string w = a.engine.vfoWarning(r.id);
                if (!w.empty() && r.on) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.3f, 1), "!"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", w.c_str()); }
                if (ch) savePrefsSoon(a);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (del >= 0) { a.vfoRows.erase(a.vfoRows.begin() + del); savePrefs(a); }
        if (a.vfoRows.empty()) ImGui::TextWrapped("%s", tr("No extra channels. Add channel, then click on the waterfall: each channel decodes or plays on its own and the sound of the open ones is mixed."));
    }
    ImGui::End();
    if (!open) a.vfoPanel = false;
}
