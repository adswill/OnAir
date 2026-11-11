// CP16 screens: the picture of the 16 carriers (as a waterfall would show it, wrapped in strips), the characters read from it, and under Advanced the fixed
// parameters and the font.
#include "app.h"
#include "../ui/cjk_fonts.h"
#include "dect2/cp16/cp16_proto.h"
#include "dect2/cp16/cp16_tel.h"
#include "platform/platform.h"
#include <algorithm>
#include <cstdio>

namespace {

struct State {
    bool fontInit = false;
    char fontPath[512] = "";
    std::string fontMsg;
    bool manual = false;
    float manBase = 1000, manSpacing = 15, manChar = 2;
    int manSide = 0;
    bool read = true;
    int scale = 3;
};
State S;

bool live(const App& a) { return a.rx.standard == 791; }

// the font the characters are drawn and read with: the user's file, else the bundled Chinese font (SIL OFL 1.1)
void ensureFont() {
    if (S.fontInit) return;
    S.fontInit = true;
    std::string err;
    if (!dect2::cp16::setDefaultFontData(cjk::regularFontData(), "Noto Sans CJK SC", err)) S.fontMsg = tr(err.c_str());
    snprintf(S.fontPath, sizeof S.fontPath, "%s", plat::prefs().getS("cp16Font", "").c_str());
    if (S.fontPath[0] && !dect2::cp16::loadFontFile(S.fontPath, err)) S.fontMsg = tr(err.c_str());
}

void picture(const dect2::Cp16Telemetry& t) {
    constexpr int kStripRows = 17 * 8;   // eight characters a strip
    const int px = S.scale;
    const float stripW = (float)(16 * px + 6);
    const float availW = ImGui::GetContentRegionAvail().x;
    const int strips = std::max(1, (int)(availW / stripW));
    const float h = (float)(kStripRows * px);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(availW, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + availW, p0.y + h), IM_COL32(8, 10, 20, 255));
    if (t.rows.empty()) return;
    // the newest strip on the right; a strip holds rows of whole strips of the row serial, so the picture does not move while a strip fills
    const uint64_t lastRow = t.firstRow + t.rows.size() - 1;
    const uint64_t lastStrip = lastRow / kStripRows;
    for (int s = 0; s < strips; s++) {
        if ((uint64_t)s > lastStrip) break;
        const uint64_t strip = lastStrip - (uint64_t)s;
        const float x0 = p0.x + availW - (float)(s + 1) * stripW;
        for (int r = 0; r < kStripRows; r++) {
            const uint64_t serial = strip * kStripRows + (uint64_t)r;
            if (serial < t.firstRow || serial > lastRow) continue;
            const auto& row = t.rows[(size_t)(serial - t.firstRow)];
            const float y = p0.y + (float)(r * px);
            for (int c = 0; c < 16; c++) {
                const int v = row[(size_t)c];
                if (v < 24) continue;
                dl->AddRectFilled(ImVec2(x0 + (float)(c * px), y), ImVec2(x0 + (float)((c + 1) * px), y + (float)px), IM_COL32(v, v, std::min(255, v + 30), 255));
            }
        }
    }
}

void tab(App& a) {
    ensureFont();
    const dect2::Cp16Telemetry& t = a.rx.cp16;
    if (!live(a)) ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("Start the receiver."));
    else if (t.state < 2) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Listening for 16 equally spaced carriers. CP16 is read by eye on a waterfall: the picture appears here once the carriers and their rows are found (about 10 seconds of signal)."));
        ImGui::PopTextWrapPos();
    }
    picture(t);
    ImGui::SliderInt(tr("Dot size"), &S.scale, 2, 6);
    ImGui::Separator();
    ImGui::TextUnformatted(tr("Characters read"));
    ImGui::PushTextWrapPos(0);
    if (t.text.empty()) ImGui::TextDisabled("%s", tr("None yet."));
    else ImGui::TextUnformatted(t.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::TextDisabled("%s", tr("Read by matching the dots against the font. The picture above is what was received; trust it over the characters when they differ."));
}

void receiver(App& a) {
    ensureFont();
    const dect2::Cp16Telemetry& t = a.rx.cp16;
    if (live(a) && t.state >= 1) {
        ImGui::Text("%s %.1f Hz  %s %.2f Hz", tr("Lowest carrier"), t.baseHz, tr("spacing"), t.spacingHz);
        if (t.state == 2) ImGui::Text("%s %.1f ms  (%.2f s %s)", tr("Row"), t.rowSec * 1000, t.charSec, tr("a character"));
        ImGui::Text("%s  %s %.0f dB", t.mirrored ? tr("Lower sideband") : tr("Upper sideband"), tr("contrast"), t.snrDb);
    }
    if (ImGui::TreeNode(trw("Advanced"))) {
        bool changed = ImGui::Checkbox(tr("Fixed parameters (instead of measuring them)"), &S.manual);
        if (S.manual) {
            changed |= ImGui::InputFloat(tr("Lowest carrier (Hz)"), &S.manBase, 1, 50, "%.1f");
            changed |= ImGui::InputFloat(tr("Spacing (Hz)"), &S.manSpacing, 0.1f, 1, "%.2f");
            changed |= ImGui::InputFloat(tr("Seconds a character"), &S.manChar, 0.1f, 0.5f, "%.2f");
            const char* sides[] = {tr("Automatic"), tr("Upper sideband"), tr("Lower sideband")};
            changed |= ImGui::Combo(tr("Sideband"), &S.manSide, sides, 3);
        }
        if (changed) a.engine.cp16().setManual(S.manual ? S.manBase : 0, S.manual ? S.manSpacing : 0, S.manual ? S.manChar : 0, S.manual ? S.manSide : 0);
        if (ImGui::Checkbox(tr("Read characters"), &S.read)) a.engine.cp16().setRead(S.read);
        ImGui::Separator();
        ImGui::TextWrapped("%s", tr("Font: CP16 stations send 16 x 16 dot characters, usually from an HZK16 font. The characters are read best with the sender's font: choose an HZK16 file or a .ttf / .otf here. Empty: the bundled Noto Sans CJK SC, drawn at 16 dots."));
        ImGui::SetNextItemWidth(320 * gUi);
        ImGui::InputText(tr("Font file"), S.fontPath, sizeof S.fontPath);
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse…##cp16font"))) { const std::string p = openFileDialog(); if (!p.empty()) snprintf(S.fontPath, sizeof S.fontPath, "%s", p.c_str()); }
        if (ImGui::Button(tr("Use this font##cp16"))) {
            std::string err;
            if (dect2::cp16::loadFontFile(S.fontPath, err)) { plat::prefs().setS("cp16Font", S.fontPath); S.fontMsg = S.fontPath[0] ? tr("Font loaded.") : ""; }
            else S.fontMsg = tr(err.c_str());
        }
        if (!S.fontMsg.empty()) ImGui::TextDisabled("%s", S.fontMsg.c_str());
        ImGui::TreePop();
    }
    if (resultsClearButton("cp16")) a.engine.cp16().clearResults();
}

void list(App&) {}
void status(App& a) { if (live(a)) ImGui::TextUnformatted(dect2::cp16Summary(a.rx.cp16).c_str()); }
void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "CP16";
    l2 = live(a) ? dect2::cp16Summary(a.rx.cp16) : "";
}
void tuner(App&, bool&) {}
void synth(App&, bool&) {}
void tick(App&) { ensureFont(); }
void meters(const App& a, std::vector<ModeMeter>& out) {
    const auto& t = a.rx.cp16;
    out.push_back({"CHARS", "%.0f", (double)t.blocksOk, 0, 200, t.blocksOk ? 1 : 0});
    out.push_back({"CONTRAST", "%.0f", (double)t.snrDb, 0, 40, t.dataValid ? 1 : 0});
}

} // namespace

extern const ModeUi kCp16Ui;
const ModeUi kCp16Ui = {
    .sideTitle = "CP16",
    .tabName = "Picture",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
