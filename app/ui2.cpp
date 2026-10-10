// The new interface, in a plain developer-tool style: flat, dense and monochrome. A text list of modes with keyboard shortcuts on the left,
// square bordered panes and hairlines instead of cards, one muted accent. It reuses every panel of the classic interface (main.cpp).
#include "app.h"

namespace {
// set the waterfall range from the spectrum: the noise floor is the 20th percentile, the top is the strongest signal (at most 45 dB above the floor)
void autoRange(App& a) {
    if (a.smooth.size() < 64) return;
    std::vector<float> v(a.smooth.begin(), a.smooth.end());
    const size_t k = v.size() / 5;
    std::nth_element(v.begin(), v.begin() + (long)k, v.end());
    const float floorDb = v[k];
    const float top = *std::max_element(a.smooth.begin(), a.smooth.end());
    a.wf.minDb = std::floor(floorDb - 6.f);
    a.wf.maxDb = std::ceil(std::min(top + 2.f, floorDb + 45.f));
    if (a.wf.maxDb < a.wf.minDb + 12.f) a.wf.maxDb = a.wf.minDb + 12.f;
}
// Palettes. 0: "terminal" (warm greys, amber, lower-case labels, key hints, log line); 1: "instrument" (mid greys, yellow trace, plain labels);
// 2: "mono" (neutral greys, white trace, no accent at all, plain labels).
struct UiTheme {
    ImVec4 bg, rail, edge, text, dim, bright, accent, frame, button, plotBg, series[8];
    ImU32 tone[5];      // what the classic interface's blues become: bright fill, dark fill, mid fill, soft icon, bright icon
    bool flair;         // lower-case labels, key hints and a log line
};
UiTheme T;
#define kBg T.bg
#define kRail T.rail
#define kEdge T.edge
#define kText T.text
#define kDim T.dim
#define kBright T.bright
#define kAccent T.accent

void loadTheme(int n) {
    T = UiTheme();
    if (n == 1) {
        T.bg = ImVec4(0.165f, 0.168f, 0.172f, 1); T.rail = ImVec4(0.135f, 0.138f, 0.142f, 1); T.edge = ImVec4(0.33f, 0.335f, 0.34f, 1);
        T.text = ImVec4(0.86f, 0.87f, 0.88f, 1); T.dim = ImVec4(0.60f, 0.61f, 0.62f, 1); T.bright = ImVec4(1, 1, 1, 1); T.accent = ImVec4(0.90f, 0.91f, 0.92f, 1);
        T.frame = ImVec4(0.095f, 0.098f, 0.102f, 1); T.button = ImVec4(0.235f, 0.240f, 0.247f, 1); T.plotBg = ImVec4(0.015f, 0.016f, 0.018f, 1);
        const ImVec4 s[8] = {ImVec4(0.90f, 0.91f, 0.92f, 1), ImVec4(0.55f, 0.80f, 0.62f, 1), ImVec4(0.88f, 0.68f, 0.42f, 1), ImVec4(0.62f, 0.72f, 0.92f, 1), ImVec4(0.80f, 0.58f, 0.78f, 1), ImVec4(0.55f, 0.56f, 0.58f, 1), ImVec4(0.88f, 0.84f, 0.56f, 1), ImVec4(0.35f, 0.36f, 0.38f, 1)};
        for (int k = 0; k < 8; k++) T.series[k] = s[k];
        const ImU32 t[5] = {IM_COL32(120, 128, 138, 255), IM_COL32(58, 62, 68, 255), IM_COL32(84, 90, 98, 255), IM_COL32(170, 176, 184, 255), IM_COL32(225, 228, 232, 255)};
        for (int k = 0; k < 5; k++) T.tone[k] = t[k];
        T.flair = false;
    } else if (n == 2) {
        T.bg = ImVec4(0.075f, 0.075f, 0.075f, 1); T.rail = ImVec4(0.055f, 0.055f, 0.055f, 1); T.edge = ImVec4(0.22f, 0.22f, 0.22f, 1);
        T.text = ImVec4(0.84f, 0.84f, 0.84f, 1); T.dim = ImVec4(0.50f, 0.50f, 0.50f, 1); T.bright = ImVec4(1, 1, 1, 1); T.accent = ImVec4(0.92f, 0.92f, 0.92f, 1);
        T.frame = ImVec4(0.045f, 0.045f, 0.045f, 1); T.button = ImVec4(0.115f, 0.115f, 0.115f, 1); T.plotBg = ImVec4(0.03f, 0.03f, 0.03f, 1);
        const ImVec4 s[8] = {ImVec4(0.93f, 0.93f, 0.93f, 1), ImVec4(0.62f, 0.62f, 0.62f, 1), ImVec4(0.42f, 0.42f, 0.42f, 1), ImVec4(0.80f, 0.80f, 0.80f, 1), ImVec4(0.55f, 0.55f, 0.55f, 1), ImVec4(0.35f, 0.35f, 0.35f, 1), ImVec4(0.72f, 0.72f, 0.72f, 1), ImVec4(0.28f, 0.28f, 0.28f, 1)};
        for (int k = 0; k < 8; k++) T.series[k] = s[k];
        const ImU32 t[5] = {IM_COL32(150, 150, 150, 255), IM_COL32(70, 70, 70, 255), IM_COL32(100, 100, 100, 255), IM_COL32(170, 170, 170, 255), IM_COL32(235, 235, 235, 255)};
        for (int k = 0; k < 5; k++) T.tone[k] = t[k];
        T.flair = false;
    } else {
        T.bg = ImVec4(0.045f, 0.045f, 0.044f, 1); T.rail = ImVec4(0.037f, 0.037f, 0.036f, 1); T.edge = ImVec4(0.175f, 0.172f, 0.165f, 1);
        T.text = ImVec4(0.82f, 0.81f, 0.78f, 1); T.dim = ImVec4(0.46f, 0.45f, 0.42f, 1); T.bright = ImVec4(0.96f, 0.95f, 0.92f, 1); T.accent = ImVec4(0.86f, 0.65f, 0.30f, 1);
        T.frame = ImVec4(0.030f, 0.030f, 0.030f, 1); T.button = ImVec4(0.045f, 0.045f, 0.044f, 1); T.plotBg = ImVec4(0.030f, 0.030f, 0.030f, 1);
        const ImVec4 s[8] = {ImVec4(0.86f, 0.65f, 0.30f, 1), ImVec4(0.82f, 0.81f, 0.77f, 1), ImVec4(0.56f, 0.55f, 0.51f, 1), ImVec4(0.74f, 0.50f, 0.30f, 1), ImVec4(0.62f, 0.68f, 0.52f, 1), ImVec4(0.45f, 0.44f, 0.41f, 1), ImVec4(0.86f, 0.74f, 0.50f, 1), ImVec4(0.35f, 0.35f, 0.33f, 1)};
        for (int k = 0; k < 8; k++) T.series[k] = s[k];
        const ImU32 t[5] = {IM_COL32(166, 122, 46, 255), IM_COL32(86, 68, 36, 255), IM_COL32(110, 84, 36, 255), IM_COL32(168, 146, 104, 255), IM_COL32(232, 190, 112, 255)};
        for (int k = 0; k < 5; k++) T.tone[k] = t[k];
        T.flair = true;
    }
    for (int k = 0; k < 5; k++) pal::tones()[k] = T.tone[k];
}

ImU32 u32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }

// A bordered pane with square corners. autoH: grow with the contents.
// A pane with autoH is as tall as its contents. ImGui sizes it from the last frame, so a status line whose values change width (and so
// wrap onto one line more or less) made it flick between heights, a line cut off every other frame: the height it needs is kept here
// and it shrinks only after a second without the extra line.
struct PaneH { ImGuiID key; bool autoH; };
std::vector<PaneH> gPanes;
constexpr int kPaneHold = 60;   // frames
void beginPane(const char* id, ImVec2 size, bool autoH = false) {
    ImGui::PushStyleColor(ImGuiCol_Border, kEdge);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7 * gUi, 4 * gUi));
    const ImGuiID key = ImGui::GetID(id);
    const float held = autoH ? ImGui::GetStateStorage()->GetFloat(key, 0.f) : 0.f;
    if (held > 0) size.y = held;
    gPanes.push_back({key, autoH});
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | (autoH && held <= 0 ? ImGuiChildFlags_AutoResizeY : 0));
}
void endPane() {
    const PaneH p = gPanes.back();
    gPanes.pop_back();
    // the height of what was drawn: down to the last line, and the padding below it
    const float need = p.autoH ? ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + ImGui::GetStyle().WindowPadding.y : 0.f;
    ImGui::EndChild();
    if (p.autoH) {
        ImGuiStorage* st = ImGui::GetStateStorage();
        const ImGuiID ageKey = p.key + 1;
        const float held = st->GetFloat(p.key, 0.f);
        const int age = st->GetInt(ageKey, 0);
        if (need > held + 0.5f || held <= 0) { st->SetFloat(p.key, need); st->SetInt(ageKey, 0); }   // grows at once
        else if (need < held - 0.5f) {                                                             // shrinks after a while
            if (age >= kPaneHold) { st->SetFloat(p.key, need); st->SetInt(ageKey, 0); } else st->SetInt(ageKey, age + 1);
        } else st->SetInt(ageKey, 0);
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

// A flat button with a 1 px outline; returns true when clicked.
bool flatButton(const char* label, float w, ImU32 textCol, ImU32 edgeCol) {
    const ImVec2 sz(w, ImGui::GetFrameHeight() + 4 * gUi);
    const bool clicked = ImGui::InvisibleButton(label, sz);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 14));
    dl->AddRect(p0, p1, hov ? textCol : edgeCol);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p0.x + (sz.x - ts.x) * 0.5f, p0.y + (sz.y - ts.y) * 0.5f), textCol, label);
    return clicked;
}

// a text link: dim, brighter on hover, underlined on hover
bool textLink(const char* label) {
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const bool clicked = ImGui::InvisibleButton(label, ts);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(p, u32(hov ? kBright : kDim), label);
    if (hov) dl->AddLine(ImVec2(p.x, p.y + ts.y), ImVec2(p.x + ts.x, p.y + ts.y), u32(kBright));
    return clicked;
}

// ---- the SDR++ style frequency readout: digits you can click (upper half up, lower half down) or scroll. With the mouse over it, typing a
// number sets the digit under the mouse and moves on to the next one, the arrow keys step the digit (up/down) or pick another (left/right),
// and a double click opens a box to type the whole frequency.
bool gFreqHovered = false;   // last frame: the mouse was over the digits (the 1-9 mode keys stay off then)
float freqDigits(App& a, float fontSize = 30.f) {
    static int kbIdx = -1;               // the digit the keyboard works on after typing or left/right (-1: the one under the mouse)
    static ImVec2 kbMouse;               // where the mouse was then: moving it gives the digit back to the mouse
    static double preClickMhz = 0;       // the frequency before the first click of a double click, which the double click undoes
    static char typed[32] = "";
    const bool fm = a.fmMode, dab = a.dabMode;
    const ModeTuning* mt = a.family >= 6 ? modeTuning(a.family + 2) : nullptr;
    const double lo = mt ? mt->minMhz : fm ? 87.5 : dab ? 174.0 : 1.0, hi = mt ? mt->maxMhz : fm ? 108.0 : dab ? 240.0 : 6000.0;
    long long hz = (long long)std::llround(a.freqMhz * 1e6);
    char buf[16];
    snprintf(buf, sizeof buf, "%010lld", hz);   // ten digits: L-band (DVB-S, ADS-B) is above 1 GHz
    ImGui::PushFont(a.mono, fontSize);
    const ImVec2 cs = ImGui::CalcTextSize("0");
    const float cw = cs.x + 1 * gUi, ch = cs.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    float x = o.x;
    long long delta = 0;
    bool leading = true, dbl = false;
    int hovIdx = -1;
    float digitX[10];
    for (int i = 0; i < 10; i++) {
        digitX[i] = x;
        const long long place = (long long)std::llround(std::pow(10.0, 9 - i));
        ImGui::SetCursorScreenPos(ImVec2(x, o.y));
        ImGui::PushID(i);
        ImGui::InvisibleButton("##d", ImVec2(cw, ch));
        ImGui::PopID();
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const bool upper = ImGui::GetIO().MousePos.y < p0.y + ch * 0.5f;
        if (hov) hovIdx = i;
        if (hov && ImGui::IsMouseDoubleClicked(0)) dbl = true;
        else if (ImGui::IsItemClicked(0)) { preClickMhz = a.freqMhz; delta = upper ? place : -place; }
        if (hov && ImGui::GetIO().MouseWheel != 0) { delta = ImGui::GetIO().MouseWheel > 0 ? place : -place; ImGui::GetIO().MouseWheel = 0; }
        if (hov) {
            dl->AddRectFilled(p0, ImVec2(p0.x + cw, p0.y + ch), IM_COL32(255, 255, 255, 18));
            const float my = upper ? p0.y + 2 * gUi : p0.y + ch - 2 * gUi;
            dl->AddTriangleFilled(ImVec2(p0.x + cw * 0.5f - 3 * gUi, upper ? my + 4 * gUi : my - 4 * gUi), ImVec2(p0.x + cw * 0.5f + 3 * gUi, upper ? my + 4 * gUi : my - 4 * gUi), ImVec2(p0.x + cw * 0.5f, my), u32(T.dim));
        }
        if (buf[i] != '0') leading = false;
        const bool dim = leading && i < 7;   // leading zeros are shown dim, like SDR++
        char c[2] = {buf[i], 0};
        dl->AddText(ImVec2(x + (cw - cs.x) * 0.5f, o.y), u32(dim ? ImVec4(T.dim.x * 0.8f, T.dim.y * 0.8f, T.dim.z * 0.8f, 1) : T.bright), c);
        x += cw;
        if (i == 0 || i == 3 || i == 6) { dl->AddText(ImVec2(x - cs.x * 0.275f, o.y), u32(T.dim), "."); x += cs.x * 0.45f; }   // the dot centred in the gap
    }
    ImGui::SetCursorScreenPos(ImVec2(x + 8 * gUi, o.y + ch * 0.5f - ImGui::GetTextLineHeight() * 0.35f));
    ImGui::PopFont();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Hz");
    const float endX = ImGui::GetItemRectMax().x;
    ImGuiIO& io = ImGui::GetIO();
    gFreqHovered = hovIdx >= 0 || (kbIdx >= 0 && io.MousePos.x == kbMouse.x && io.MousePos.y == kbMouse.y);
    if (kbIdx >= 0 && (io.MousePos.x != kbMouse.x || io.MousePos.y != kbMouse.y)) kbIdx = -1;
    const int cur = kbIdx >= 0 ? kbIdx : hovIdx;
    if (cur >= 0 && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
        const long long place = (long long)std::llround(std::pow(10.0, 9 - cur));
        for (int k = 0; k < 10; k++)
            if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_0 + k), false) || ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_Keypad0 + k), false)) {
                const int old = (int)((hz / place) % 10);
                delta = (long long)(k - old) * place;
                kbIdx = std::min(9, cur + 1); kbMouse = io.MousePos;
            }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) delta = place;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) delta = -place;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { kbIdx = std::max(0, cur - 1); kbMouse = io.MousePos; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { kbIdx = std::min(9, cur + 1); kbMouse = io.MousePos; }
        if (kbIdx >= 0 && kbIdx != hovIdx) {   // show where the keyboard is when it is not under the mouse
            const float kx = digitX[kbIdx];
            dl->AddRect(ImVec2(kx, o.y), ImVec2(kx + cw, o.y + ch), u32(T.dim), 2.f);
        }
    }
    if (dbl) {
        if (std::fabs(preClickMhz - a.freqMhz) > 1e-9 && preClickMhz > 0) tuneFreq(a, preClickMhz);   // undo the first click's step
        snprintf(typed, sizeof typed, "%.6f", preClickMhz > 0 ? preClickMhz : a.freqMhz);
        ImGui::OpenPopup("##freqtype");
    }
    if (ImGui::BeginPopup("##freqtype")) {
        ImGui::TextDisabled("Frequency in MHz (%.3f to %.3f)", lo, hi);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(170 * gUi);
        const bool enter = ImGui::InputText("##ftext", typed, sizeof typed, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::SameLine();
        if (enter || ImGui::Button("Tune")) {
            char* e = nullptr;
            const double v = strtod(typed, &e);
            if (e != typed && v > 0) tuneFreq(a, std::min(hi, std::max(lo, v)));
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (delta) {
        const double mhz = std::min(hi, std::max(lo, (double)(hz + delta) / 1e6));
        if (std::fabs(mhz - a.freqMhz) > 1e-9) tuneFreq(a, mhz);
    }
    return endX;
}

// two lines about what is being received, in the top bar: the station or ensemble, the state and the main numbers
void summaryText(App& a, std::string& l1, std::string& l2) {
    const bool run = a.engine.running();
    const ModeDef* cur = &kModes[0];
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) cur = &kModes[i];
    l1 = cur->name; l2 = run ? "starting" : "stopped";
    char b[96];
    if (run) {
        const RxTelemetry& rx = a.rx;
        const ModeUi* mu = modeUi(a.family);
        if (mu && mu->summary) {
            mu->summary(a, l1, l2);
        } else if (a.fmMode && rx.standard == 6) {
            const FmTelemetry& f = rx.fm;
            std::string ps = f.psName;
            while (!ps.empty() && (ps.back() == ' ' || ps.back() == '-')) ps.pop_back();
            while (!ps.empty() && (ps.front() == ' ' || ps.front() == '-')) ps.erase(ps.begin());
            l1 = f.carrier ? (ps.empty() ? "FM" : ps) : "FM: no station";
            snprintf(b, sizeof b, "%s  %s  SNR %.0f dB", f.state == 2 ? "locked" : f.state == 1 ? "weak" : "searching", f.stereo ? "stereo" : "mono", f.snrDb);
            l2 = f.carrier ? b : "searching";
        } else if (a.dabMode && rx.standard == 3) {
            l1 = rx.dab.ensembleLabel.empty() ? "DAB" : rx.dab.ensembleLabel;
            snprintf(b, sizeof b, "%s  SNR %.0f dB  %d services", rx.dab.state == 2 ? "locked" : "searching", rx.dab.snrDb, rx.dab.services);
            l2 = b;
        } else {
            snprintf(b, sizeof b, "%s  SNR %.1f dB  CFO %+.0f Hz", rx.state == 2 ? "locked" : "searching", rx.dataSnrDb, rx.cfoHz);
            l2 = b;
        }
    }
}
float summaryWidth(App& a, float scale = 1.f) {
    std::string l1, l2;
    summaryText(a, l1, l2);
    return std::max(ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * scale, FLT_MAX, 0, l1.c_str()).x,
                    ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * (scale > 1 ? 1.15f : 1.f), FLT_MAX, 0, l2.c_str()).x);
}
// maxW: the lines are cut short with "..." rather than run into what is to their right
void summary(App& a, float x, float y, float scale = 1.f, float maxW = FLT_MAX) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string l1, l2;
    summaryText(a, l1, l2);
    const float s1 = ImGui::GetFontSize() * scale, s2 = ImGui::GetFontSize() * (scale > 1 ? 1.15f : 1.f);
    dl->AddText(ImGui::GetFont(), s1, ImVec2(x, y), u32(T.bright), ellipsize(l1, maxW, s1).c_str());
    dl->AddText(ImGui::GetFont(), s2, ImVec2(x, y + s1 + 3 * gUi), u32(T.dim), ellipsize(l2, maxW, s2).c_str());
}

// a start / stop button drawn as a play triangle or a stop square
bool runButton(bool running, float size) {
    const bool clicked = ImGui::InvisibleButton("##run", ImVec2(size, size));
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 16));
    dl->AddRect(p0, p1, u32(running ? pal::badRed() : T.edge));
    const float m = size * 0.28f;
    if (running) dl->AddRectFilled(ImVec2(p0.x + m, p0.y + m), ImVec2(p1.x - m, p1.y - m), u32(pal::badRed()));
    else dl->AddTriangleFilled(ImVec2(p0.x + m + 2 * gUi, p0.y + m), ImVec2(p0.x + m + 2 * gUi, p1.y - m), ImVec2(p1.x - m + 1 * gUi, (p0.y + p1.y) * 0.5f), u32(pal::okGreen()));
    if (hov) ImGui::SetTooltip("%s (space)", running ? "Stop the receiver" : "Start the receiver");
    return clicked;
}

// a collapsible sidebar section; returns whether it is open
bool section(const char* title) {
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1, 1, 1, 0.045f));
    const bool open = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor();
    return open;
}

// the modes as a plain radio list, grouped like the menu
void modeList(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool running = a.engine.running();
    const ImVec2 first = ImGui::GetCursorScreenPos();
    for (int g = 0; g < kNumGroups; g++) {
        ImGui::TextDisabled("%s", kGroupNames[g]);
        for (int i = 0; i < kNumModes; i++) {
            const ModeDef& m = kModes[i];
            if (m.group != g) continue;
            const float h = ImGui::GetFrameHeight() - 2 * gUi, w = ImGui::GetContentRegionAvail().x;
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##mode", ImVec2(w, h));
            ImGui::PopID();
            const bool hov = ImGui::IsItemHovered();
            const ImVec2 p0 = ImGui::GetItemRectMin();
            const bool sel = modeSelected(a, m);
            if (hov) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(255, 255, 255, 12));
            const ImVec2 c(p0.x + 9 * gUi, p0.y + h * 0.5f);
            dl->AddCircle(c, 5 * gUi, u32(sel ? T.bright : T.dim), 0, 1.f);
            if (sel) dl->AddCircleFilled(c, 2.6f * gUi, u32(T.accent));
            dl->AddText(ImVec2(p0.x + 24 * gUi, p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f), u32(sel ? T.bright : (running ? T.dim : T.text)), m.name);
            const ImVec2 ss = ImGui::CalcTextSize(m.sub);
            if (p0.x + 24 * gUi + ImGui::CalcTextSize(m.name).x + 10 * gUi < p0.x + w - ss.x - 6 * gUi)   // no room: the name alone, not both on top of each other
                dl->AddText(ImVec2(p0.x + w - ss.x - 6 * gUi, p0.y + (h - ss.y) * 0.5f), u32(T.dim), m.sub);
            if (clicked) selectMode(a, m.family, m.preset);
            if (hov && running && !sel) ImGui::SetTooltip("Stop the receiver to switch mode");
        }
    }
    a.tgMin[TgSwitch] = first; a.tgMax[TgSwitch] = ImVec2(first.x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y);
}

void sidebar(App& a) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * gUi, 4 * gUi));
    if (section("Source")) {
        ImGui::Indent(6 * gUi);
        toolbarParts(a, TbSource, true);
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    if (section("Mode")) { ImGui::Indent(6 * gUi); modeList(a); ImGui::Unindent(6 * gUi); ImGui::Spacing(); }
    if (section(a.fmMode ? "Tuner (FM)" : a.dabMode ? "Tuner (DAB)" : "Tuner")) {
        ImGui::Indent(6 * gUi);
        toolbarParts(a, TbTuner, true);
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    if (section("Gain")) {
        ImGui::Indent(6 * gUi);
        toolbarParts(a, TbGain, true);
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    if (section("Display")) {
        ImGui::Indent(6 * gUi);
        ImGui::Checkbox("Peak hold", &a.peakHold);
        const float col = ImGui::GetCursorPosX() + ImGui::CalcTextSize("Waterfall").x + 8 * gUi;   // the controls line up after the longer label
        const float autoW = ImGui::CalcTextSize("Auto").x + ImGui::GetStyle().FramePadding.x * 2 + 4 * gUi;
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Spectrum"); ImGui::SameLine(col); ImGui::SetNextItemWidth(std::min(150.f * gUi, ImGui::GetContentRegionAvail().x));
        ImGui::DragFloatRange2("##specdb", &a.yMin, &a.yMax, 1, -160, 20, "%.0f", "%.0f");
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Waterfall"); ImGui::SameLine(col);
        ImGui::SetNextItemWidth(std::min(104.f * gUi, ImGui::GetContentRegionAvail().x - autoW));
        ImGui::DragFloatRange2("##wfdb", &a.wf.minDb, &a.wf.maxDb, 1, -160, 20, "%.0f", "%.0f");
        ImGui::SameLine(0, 4 * gUi);
        if (ImGui::SmallButton("Auto")) autoRange(a);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set the waterfall range from the noise floor and the strongest signal");
        ImGui::Unindent(6 * gUi);
        ImGui::Spacing();
    }
    if ((a.family == 0 || a.family == 3 || (modeUi(a.family) && modeUi(a.family)->decoder)) && section("Decoder")) {
        ImGui::Indent(6 * gUi);
        toolbarParts(a, TbDecoder, true);
        ImGui::Unindent(6 * gUi);
    }
    if (section("Log")) {
        ImGui::BeginChild("##logc", ImVec2(0, std::max(40.f * gUi, ImGui::GetContentRegionAvail().y - 4 * gUi)), ImGuiChildFlags_Borders);
        size_t n = 0;
        const std::vector<std::string> lines = a.engine.logSnapshot(n);
        for (const std::string& l : lines) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", l.c_str()); ImGui::PopTextWrapPos(); }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
    }
    ImGui::PopStyleVar();
}

// WSJT-X style status bar: fields separated by rules
void statusLine(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool run = a.engine.running();
    const ModeDef* cur = &kModes[0];
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) cur = &kModes[i];
    const float lh = ImGui::GetTextLineHeight();
    auto field = [&](const char* text, bool first = false, ImVec4 col = ImVec4(-1, 0, 0, 0), bool fit = false) {
        if (!first) {
            if (!sameLineIf(16 * gUi + (fit ? 40 * gUi : ImGui::CalcTextSize(text).x), 8 * gUi)) return;   // no room left on the bar
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(p.x, p.y + 1), ImVec2(p.x, p.y + lh - 1), u32(T.dim));
            ImGui::SameLine(0, 8 * gUi);
        }
        const std::string t = fit ? ellipsize(text, ImGui::GetContentRegionAvail().x, ImGui::GetFontSize()) : text;   // the log line: as much as fits
        if (col.x < 0) ImGui::TextUnformatted(t.c_str()); else ImGui::TextColored(col, "%s", t.c_str());
    };
    char b[96];
    ImGui::AlignTextToFramePadding();
    field(run ? "Receiving" : "Stopped", true, run ? pal::okGreen() : pal::grey());
    field(cur->name);
    snprintf(b, sizeof b, "%.3f MHz", a.freqMhz); field(b);
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); field(b); }
    if (run) { snprintf(b, sizeof b, "level %.1f dBFS", a.spec.stats.rmsDbfs); field(b); }
    if (run) {
        const SampleLoss l = a.engine.sampleLoss();
        if (l.live) { snprintf(b, sizeof b, "load %.0f%%", l.loadPct); field(b, false, l.loadPct > 90 ? pal::warnAmber() : ImVec4(-1, 0, 0, 0)); lossTooltip(l); }
        if (l.radioEvents || l.cpuEvents) { field(("dropped " + lossText(l)).c_str(), false, lossColour(l)); lossTooltip(l); }
    }
    {
        size_t n = 0;
        const std::vector<std::string> lines = a.engine.logSnapshot(n);
        if (!lines.empty()) field(lines.back().c_str(), false, ImVec4(-1, 0, 0, 0), true);
    }
}
} // namespace

// Classic or new colours and shapes. The classic style is captured once and restored when switching back.
void applyUiTheme(App& a) {
    pal::panel() = a.newUi && a.uiVariant == 7;   // round LEDs only in the Panel layout
    static ImGuiStyle classic;
    static plt::Style classicPlot;
    static ImVec4 classicAccent;
    static bool have = false;
    static int key = -1;
    static float scale = 0;
    if (!have) { classic = ImGui::GetStyle(); classicPlot = plt::GetStyle(); classicAccent = pal::accentRef(); have = true; scale = gUi; }
    // the window went to a monitor with another display scale: main.cpp has just reset the style to the classic one at that scale
    // (taken again as the classic style), and the chosen theme is applied again with its sizes at the new scale
    if (gUi != scale) { classic = ImGui::GetStyle(); classicPlot.Scale = gUi; scale = gUi; key = -1; }
    const int want = a.newUi ? 1 + a.uiTheme : 0;
    if (want == key) return;
    key = want;
    ImGui::GetStyle() = classic;
    plt::GetStyle() = classicPlot;
    pal::accentRef() = classicAccent;
    pal::dev() = a.newUi;
    if (a.newUi) loadTheme(a.uiTheme);
    pal::wfMode() = !a.newUi ? 0 : a.uiTheme == 2 ? 1 : 2;
    a.wf.palette();
    // the colour map needs a tighter range to show the noise floor as dark; the classic range comes back when switching back
    if (a.newUi && a.wf.minDb == -100 && a.wf.maxDb == -32) { a.wf.minDb = -88; a.wf.maxDb = -36; }
    else if (!a.newUi && a.wf.minDb == -88 && a.wf.maxDb == -36) { a.wf.minDb = -100; a.wf.maxDb = -32; }
    if (!a.newUi) return;
    pal::accentRef() = kAccent;

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0; s.FrameRounding = 1; s.ChildRounding = 0; s.PopupRounding = 0; s.TabRounding = 0; s.GrabRounding = 0; s.ScrollbarRounding = 0;
    s.FramePadding = ImVec2(5 * gUi, 1.5f * gUi); s.ItemSpacing = ImVec2(6 * gUi, 2.5f * gUi); s.CellPadding = ImVec2(4 * gUi, 1 * gUi);
    s.FrameBorderSize = 1; s.ChildBorderSize = 1; s.PopupBorderSize = 1; s.TabBarBorderSize = 1; s.TabBarOverlineSize = 2 * gUi;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = kBg; c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_PopupBg] = ImVec4(0.062f, 0.062f, 0.060f, 1);
    c[ImGuiCol_Border] = kEdge; c[ImGuiCol_Separator] = kEdge;
    c[ImGuiCol_FrameBg] = T.frame; c[ImGuiCol_FrameBgHovered] = ImVec4(T.frame.x + 0.05f, T.frame.y + 0.05f, T.frame.z + 0.05f, 1); c[ImGuiCol_FrameBgActive] = ImVec4(T.frame.x + 0.08f, T.frame.y + 0.08f, T.frame.z + 0.08f, 1);
    c[ImGuiCol_Button] = T.button; c[ImGuiCol_ButtonHovered] = ImVec4(T.button.x + 0.06f, T.button.y + 0.06f, T.button.z + 0.06f, 1); c[ImGuiCol_ButtonActive] = ImVec4(T.accent.x * 0.45f, T.accent.y * 0.45f, T.accent.z * 0.45f, 1);
    c[ImGuiCol_Header] = ImVec4(1, 0.95f, 0.85f, 0.06f); c[ImGuiCol_HeaderHovered] = ImVec4(1, 0.95f, 0.85f, 0.10f); c[ImGuiCol_HeaderActive] = ImVec4(0.86f, 0.65f, 0.30f, 0.30f);
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TabHovered] = ImVec4(1, 0.95f, 0.85f, 0.06f); c[ImGuiCol_TabSelected] = ImVec4(1, 0.95f, 0.85f, 0.05f);
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TabDimmedSelected] = ImVec4(1, 0.95f, 0.85f, 0.04f); c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_SliderGrab] = ImVec4(0.58f, 0.56f, 0.52f, 1); c[ImGuiCol_SliderGrabActive] = kAccent; c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_Text] = kText; c[ImGuiCol_TextDisabled] = kDim; c[ImGuiCol_MenuBarBg] = kRail;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 0.95f, 0.85f, 0.02f); c[ImGuiCol_TableHeaderBg] = ImVec4(0, 0, 0, 0); c[ImGuiCol_TableBorderLight] = kEdge; c[ImGuiCol_TableBorderStrong] = kEdge;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.86f, 0.65f, 0.30f, 0.30f);
    plt::Style& p = plt::GetStyle();
    for (int i = 0; i < 8; i++) p.AutoColors[i] = T.series[i];
    p.PlotBg = T.plotBg; p.Border = kEdge; p.Grid = ImVec4(0.72f, 0.72f, 0.70f, 0.10f); p.Text = ImVec4(T.dim.x + 0.1f, T.dim.y + 0.1f, T.dim.z + 0.1f, 1);
}

namespace {
const char* kVariantNames[8] = {"Sidebar", "Scope", "Tiles", "Faceplate", "Scope: Cinema", "Scope: Meters", "Scope: Dock", "Panel"};

float menuBar(App& a) {
    float menuH = 0;
    if (ImGui::BeginMainMenuBar()) {
        menuH = ImGui::GetWindowHeight();
        const bool running = a.engine.running();
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem(running ? "Stop receiver" : "Start receiver", "Space")) { if (running) a.engine.stop(); else startReceiver(a); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Mode")) {
            int key = 0;
            for (int g = 0; g < kNumGroups; g++) {
                if (g) ImGui::Separator();
                for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g) {
                    char k[4]; snprintf(k, sizeof k, "%d", ++key);
                    if (ImGui::MenuItem(kModes[i].name, key <= 9 ? k : nullptr, modeSelected(a, kModes[i]))) selectMode(a, kModes[i].family, kModes[i].preset);   // only 1-9 are keys
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            for (int v = 0; v < 8; v++) if (ImGui::MenuItem(kVariantNames[v], nullptr, a.uiVariant == v)) { a.uiVariant = v; savePrefs(a); }
            ImGui::Separator();
            static const char* names[3] = {"Terminal palette", "Instrument palette", "Mono palette"};
            for (int t = 0; t < 3; t++) if (ImGui::MenuItem(names[t], nullptr, a.uiTheme == t)) { a.uiTheme = t; savePrefs(a); }
            if (ImGui::MenuItem("Light (dark on white)", nullptr, &a.lightUi)) savePrefs(a);
            ImGui::Separator();
            if (ImGui::MenuItem("Classic interface")) { a.newUi = false; savePrefs(a); }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Guided tour")) { a.wizOpen = true; a.wizX = -1; a.wizStep = 0; a.wizStepT = ImGui::GetTime(); }
            ImGui::EndMenu();
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - 150 * gUi);
        updateButton(a);
        ImGui::EndMainMenuBar();
    }
    return menuH;
}

// the ADC level meter: segments, the last two mean overload
void levelMeter(App& a, float x, float y, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool running = a.engine.running();
    const float db = running ? a.spec.stats.rmsDbfs : -120.f;
    const int n = 26, lit = (int)std::lround(std::min(1.f, std::max(0.f, (db + 60.f) / 60.f)) * n);
    const float sw = (w - (n - 1) * 2 * gUi) / n, sy = y + 20 * gUi, sh = 12 * gUi;
    char lv[32]; snprintf(lv, sizeof lv, running ? "%.1f dBFS" : "-", db);
    const float lw = ImGui::CalcTextSize(lv).x, room = w - lw - 8 * gUi;   // a narrow meter: a shorter caption, or none
    const char* cap = ImGui::CalcTextSize("ADC level").x <= room ? "ADC level" : ImGui::CalcTextSize("ADC").x <= room ? "ADC" : "";
    dl->AddText(ImVec2(x, y), u32(T.dim), cap);
    dl->AddText(ImVec2(x + w - lw, y), u32(T.text), lv);
    for (int k = 0; k < n; k++) {
        const bool on = k < lit, hot = k >= n - 2;
        const ImU32 col = on ? (hot ? u32(pal::badRed()) : k >= n - 5 ? u32(pal::warnAmber()) : u32(ImVec4(0.62f, 0.70f, 0.64f, 1))) : u32(ImVec4(0.25f, 0.26f, 0.27f, 1));
        dl->AddRectFilled(ImVec2(x + k * (sw + 2 * gUi), sy), ImVec2(x + k * (sw + 2 * gUi) + sw, sy + sh), col);
    }
}

void volumeControl(App& a, float w) {
    if (iconFlat(a.muted ? Ic::Mute : Ic::Speaker, a.muted ? "Unmute" : "Mute")) {
        a.muted = !a.muted;
        a.engine.player().setMuted(a.muted); a.engine.dabAudio().setMuted(a.muted);
    }
    ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(w - 34 * gUi);
    float vol = a.volume * 100.f;
    if (ImGui::SliderFloat("##vol", &vol, 0, 100, "%.0f %%")) { a.volume = vol / 100.f; a.engine.player().setVolume(a.volume); a.engine.dabAudio().setVolume(a.volume); }
    if (ImGui::IsItemHovered() && a.family == 9) ImGui::SetTooltip("DMR voice audio is not decoded: AMBE+2 is proprietary, so there is no sound.");
}

void topBar(App& a, ImVec2 disp) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kRail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 4 * gUi));
    ImGui::BeginChild("##top", ImVec2(0, 50 * gUi), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    a.tgMin[TgToolbar] = ImGui::GetWindowPos(); a.tgMax[TgToolbar] = ImVec2(a.tgMin[TgToolbar].x + ImGui::GetWindowSize().x, a.tgMin[TgToolbar].y + ImGui::GetWindowSize().y);
    {
        const bool running = a.engine.running();
        const ImVec2 o = ImGui::GetCursorScreenPos();
        if (runButton(running, 40 * gUi) || (!running && a.wizStart)) { if (running) a.engine.stop(); else startReceiver(a); a.wizStart = false; }
        const float W = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorScreenPos(ImVec2(o.x + 58 * gUi, o.y));
        const float digitsEnd = freqDigits(a);
        // a narrow window: the level meter and the volume give up width (the meter goes last), then the summary is cut short
        const float sumX = digitsEnd + 34 * gUi;
        float vw = 230 * gUi, mw = 260 * gUi;
        const float spare = o.x + W - sumX - summaryWidth(a) - 40 * gUi;
        if (spare < vw + mw) {
            const float k = std::max(0.f, spare) / (vw + mw);
            vw = std::max(150.f * gUi, vw * k); mw = std::max(130.f * gUi, mw * k);
            if (o.x + W - vw - mw - 20 * gUi < sumX + 120 * gUi) mw = 0;
        }
        const float meterX = o.x + W - vw - mw - 20 * gUi;
        summary(a, sumX, o.y + 6 * gUi, 1.f, (mw > 0 ? meterX : o.x + W - vw) - 16 * gUi - sumX);
        if (mw > 0) levelMeter(a, meterX, o.y, mw);
        ImGui::SetCursorScreenPos(ImVec2(o.x + W - vw + 4 * gUi, o.y + 8 * gUi));
        volumeControl(a, vw);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetWindowPos().x, ImGui::GetCursorScreenPos().y), ImVec2(ImGui::GetWindowPos().x + disp.x, ImGui::GetCursorScreenPos().y), u32(kEdge));
}

void bottomBar(App& a, ImVec2 disp) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kRail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * gUi, 4 * gUi));
    ImGui::BeginChild("##statusbar", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::GetWindowDrawList()->AddLine(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + disp.x, ImGui::GetWindowPos().y), u32(kEdge));
    statusLine(a);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void tgRect(App& a, int id) { a.tgMin[id] = ImGui::GetWindowPos(); a.tgMax[id] = ImVec2(a.tgMin[id].x + ImGui::GetWindowSize().x, a.tgMin[id].y + ImGui::GetWindowSize().y); }

void logConsole(App& a, float h) {
    ImGui::BeginChild("##logc", ImVec2(0, h), ImGuiChildFlags_Borders);
    size_t n = 0;
    const std::vector<std::string> lines = a.engine.logSnapshot(n);
    for (const std::string& l : lines) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", l.c_str()); ImGui::PopTextWrapPos(); }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

void modeCombo(App& a, float w) {
    const ModeDef* cur = &kModes[0];
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) cur = &kModes[i];
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##modec", cur->name)) {
        const char* q = modeSearchBox();
        for (int g = 0; g < kNumGroups; g++) {
            bool any = false;
            for (int i = 0; i < kNumModes; i++) any |= kModes[i].group == g && modeMatches(kModes[i], q);
            if (!any) continue;
            ImGui::Separator();
            ImGui::TextDisabled("%s", kGroupNames[g]);
            for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g && modeMatches(kModes[i], q)) {
                ImGui::PushID(i);
                if (ImGui::Selectable(kModes[i].name, modeSelected(a, kModes[i]))) selectMode(a, kModes[i].family, kModes[i].preset);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kModes[i].tip);
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
}

void displayControls(App& a, bool wide) {
    ImGui::Checkbox("Peak hold", &a.peakHold);
    ImGui::SameLine(0, 12 * gUi); ImGui::TextDisabled("Spectrum"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth((wide ? 130 : 104) * gUi);
    ImGui::DragFloatRange2("##specdb", &a.yMin, &a.yMax, 1, -160, 20, "%.0f", "%.0f");
    ImGui::SameLine(0, 12 * gUi); ImGui::TextDisabled("Waterfall"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth((wide ? 130 : 104) * gUi);
    ImGui::DragFloatRange2("##wfdb", &a.wf.minDb, &a.wf.maxDb, 1, -160, 20, "%.0f", "%.0f");
    ImGui::SameLine(0, 4 * gUi);
    if (ImGui::SmallButton("Auto")) autoRange(a);
}

// ---------------------------------------------------------------- variant 0: sidebar (the parent)
void bodyParent(App& a, float bodyH, ImVec2 disp) {
    // a narrow window (a laptop at 125 or 150 %): the side panel and the list on the right give up width before the spectrum does
    const float sideW = std::floor(std::max(244.f * gUi, std::min(296.f * gUi, disp.x * 0.24f)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kRail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 6 * gUi));
    ImGui::BeginChild("##side", ImVec2(sideW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    sidebar(a);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    {
        const ImVec2 p = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - bodyH), ImVec2(p.x, p.y), u32(kEdge));
    }
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    // scrolls only when even the smallest panes do not fit (a 1366 x 768 screen at 150 %)
    ImGui::BeginChild("##content", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    if (a.devices[a.devIdx].kind == DeviceInfo::Synthetic || (a.devices[a.devIdx].kind == DeviceInfo::File && !a.engine.running())) {
        beginPane("##srcopt", ImVec2(0, 0), true);
        sourceOptions(a);
        endPane();
    }
    beginPane("##status", ImVec2(0, 0), true);
    statusBar(a);
    endPane();
    // the analysis row gives up height (down to 150) before the tabs above it go below 240; the list on the right narrows with the window
    const float availH = ImGui::GetContentRegionAvail().y;
    const float logH = a.atsc3Mode ? 0 : std::max(144.f * gUi, std::min(215.f * gUi, availH - gap - 240.f * gUi));
    const float rightW = std::floor(std::max(236.f * gUi, std::min(330.f * gUi, ImGui::GetContentRegionAvail().x * 0.30f)));
    const float mainH = std::max(186.f * gUi, availH - (logH > 0 ? logH + gap : 0));
    const float mainW = ImGui::GetContentRegionAvail().x - rightW - gap;
    beginPane("##main2", ImVec2(mainW, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    ImGui::SameLine();
    beginPane("##right2", ImVec2(0, mainH));
    tgRect(a, TgRight);
    rightPanel(a);
    endPane();
    if (!a.atsc3Mode) {
        beginPane("##const2", ImVec2(0, std::max(logH, ImGui::GetContentRegionAvail().y)));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

void ribbonPane(App& a) {
    beginPane("##ribbon", ImVec2(0, 0), true);
    a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("MODE"); ImGui::SameLine(0, 6 * gUi); modeCombo(a, 130 * gUi);
    a.tgMax[TgSwitch] = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    ImGui::SameLine(0, 18 * gUi);
    displayControls(a, false);
    toolbarParts(a, TbSource | TbTuner | TbGain | TbDecoder, false);
    if (a.devices[a.devIdx].isRadio() && !a.devices[a.devIdx].settings.empty()) { ImGui::SameLine(0, 14 * gUi); radioSettingsUi(a, false); }
    if (a.devices[a.devIdx].kind == DeviceInfo::Synthetic || (a.devices[a.devIdx].kind == DeviceInfo::File && !a.engine.running())) sourceOptions(a);
    endPane();
}

// ---------------------------------------------------------------- variant 1: scope (the picture first: a control ribbon, a huge spectrum and waterfall)
void bodyScope(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##scope", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ribbonPane(a);
    beginPane("##status", ImVec2(0, 0), true);
    statusBar(a);
    endPane();
    const float botH = (a.atsc3Mode ? 0 : 150) * gUi, dockW = 310 * gUi;
    const float mainH = ImGui::GetContentRegionAvail().y - (botH > 0 ? botH + gap : 0);
    beginPane("##main2", ImVec2(ImGui::GetContentRegionAvail().x - dockW - gap, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    ImGui::SameLine();
    beginPane("##right2", ImVec2(0, mainH));
    tgRect(a, TgRight);
    rightPanel(a);
    endPane();
    if (!a.atsc3Mode) {
        beginPane("##const2", ImVec2(0, 0));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- variant 2: tiles (no tabs in the way: everything on screen at once, in titled tiles)
void beginTile(const char* id, const char* title, ImVec2 size) {
    beginPane(id, size);
    ImGui::TextDisabled("%s", title);
    ImGui::Separator();
}

void bodyTiles(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##tiles", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    if (a.devices[a.devIdx].kind == DeviceInfo::Synthetic || (a.devices[a.devIdx].kind == DeviceInfo::File && !a.engine.running())) {
        beginPane("##srcopt", ImVec2(0, 0), true);
        sourceOptions(a);
        endPane();
    }
    beginPane("##status", ImVec2(0, 0), true);
    statusBar(a);
    endPane();
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float row1 = std::floor(H * 0.60f) - gap;
    const float wA = std::floor(W * 0.46f), wB = std::floor(W * 0.24f), wC = W - wA - wB - 2 * gap;
    beginTile("##t_scope", "SPECTRUM / WATERFALL", ImVec2(wA, row1));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    ImGui::SameLine();
    beginTile("##t_list", listTitle(a, true), ImVec2(wB, row1));
    tgRect(a, TgRight);
    rightPanel(a);
    endPane();
    ImGui::SameLine();
    beginTile("##t_ctl", "CONTROLS", ImVec2(wC, row1));
    tgRect(a, TgToolbar);
    ImGui::TextDisabled("source"); toolbarParts(a, TbSource, true);
    ImGui::Spacing();
    a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
    ImGui::TextDisabled("mode"); modeList(a);
    ImGui::Spacing();
    ImGui::TextDisabled("tuner"); toolbarParts(a, TbTuner, true);
    ImGui::Spacing();
    ImGui::TextDisabled("gain"); toolbarParts(a, TbGain, true);
    if (a.family == 0 || a.family == 3 || (modeUi(a.family) && modeUi(a.family)->decoder)) { ImGui::Spacing(); ImGui::TextDisabled("decoder"); toolbarParts(a, TbDecoder, true); }
    endPane();
    const float row2 = ImGui::GetContentRegionAvail().y;
    const float wD = std::floor(W * 0.70f);
    if (!a.atsc3Mode) {
        beginTile("##t_plots", "ANALYSIS", ImVec2(wD, row2));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
        ImGui::SameLine();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    beginTile("##t_log", "LOG", ImVec2(0, row2));
    ImGui::BeginChild("##logc", ImVec2(0, 0));
    size_t n = 0;
    const std::vector<std::string> lines = a.engine.logSnapshot(n);
    for (const std::string& l : lines) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", l.c_str()); ImGui::PopTextWrapPos(); }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    endPane();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- variant 3: faceplate (a hardware-like front: a light chassis with dark display modules and printed labels)
const ImVec4 kChassis(0.705f, 0.712f, 0.720f, 1), kSilk(0.20f, 0.21f, 0.22f, 1);

void beginModule(const char* id, const char* label, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddText(ImVec2(p.x + 2 * gUi, p.y), u32(kSilk), label);
    dl->AddLine(ImVec2(p.x + ImGui::CalcTextSize(label).x + 8 * gUi, p.y + ImGui::GetTextLineHeight() * 0.5f), ImVec2(p.x + (size.x > 0 ? size.x : ImGui::GetContentRegionAvail().x), p.y + ImGui::GetTextLineHeight() * 0.5f), u32(ImVec4(0.45f, 0.46f, 0.47f, 1)));
    ImGui::Dummy(ImVec2(1, ImGui::GetTextLineHeight() + 1 * gUi));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBg);
    beginPane(id, ImVec2(size.x, size.y > 0 ? size.y - ImGui::GetTextLineHeight() - 3 * gUi : 0));
}
void endModule() {
    endPane();
    const ImVec2 a0 = ImGui::GetItemRectMin(), a1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(a0.x, a1.y + 1), ImVec2(a1.x + 1, a1.y + 1), IM_COL32(255, 255, 255, 110));   // inset: light edge below and right
    dl->AddLine(ImVec2(a1.x + 1, a0.y), ImVec2(a1.x + 1, a1.y + 1), IM_COL32(255, 255, 255, 110));
    ImGui::PopStyleColor();
}

void bodyFace(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 6 * gUi));
    ImGui::BeginChild("##face", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 8 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 3 * gUi));
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float anaH = (a.atsc3Mode ? 0 : 190) * gUi;
    const float topH = H - (anaH > 0 ? anaH + gap : 0);
    const float leftW = std::floor(W * 0.66f), rightW = W - leftW - gap;
    ImGui::BeginGroup();
    {   // the display: run, frequency, what is being received
        const float dispH = 142 * gUi;
        beginModule("##f_disp", "DISPLAY", ImVec2(leftW, dispH));
        tgRect(a, TgToolbar);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const bool running = a.engine.running();
        if (runButton(running, 54 * gUi) || (!running && a.wizStart)) { if (running) a.engine.stop(); else startReceiver(a); a.wizStart = false; }
        ImGui::SetCursorScreenPos(ImVec2(o.x + 72 * gUi, o.y - 2 * gUi));
        const float end = freqDigits(a, 52.f);
        (void)end;
        summary(a, o.x + 74 * gUi, o.y + 66 * gUi, 1.5f, ImGui::GetContentRegionAvail().x - 74 * gUi);
        const float mw = 250 * gUi;
        levelMeter(a, o.x + ImGui::GetContentRegionAvail().x - mw, o.y, mw);
        ImGui::SetCursorScreenPos(ImVec2(o.x + ImGui::GetContentRegionAvail().x - mw, o.y + 44 * gUi));
        volumeControl(a, mw);
        endModule();
    }
    {
        beginModule("##f_scope", "SPECTRUM", ImVec2(leftW, topH - 142 * gUi - 3 * gUi));
        tgRect(a, TgMain);
        mainTabs(a);
        endModule();
    }
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    {
        const float cH = 188 * gUi;
        beginModule("##f_src", "SOURCE / GAIN", ImVec2(rightW, cH + 38 * gUi));
        toolbarParts(a, TbSource, true);
        toolbarParts(a, TbGain, true);
        endModule();
        const float modeModH = (kNumModes * 22 + 3 * 20 + 50) * gUi;   // twelve modes in three groups, then the tuner
        beginModule("##f_mode", "MODE / TUNER", ImVec2(rightW, modeModH));
        a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
        modeList(a);
        toolbarParts(a, TbTuner, true);
        if (a.family == 0 || a.family == 3 || (modeUi(a.family) && modeUi(a.family)->decoder)) toolbarParts(a, TbDecoder, true);
        endModule();
        const float rest = topH - (cH + 38 * gUi) - modeModH - 9 * gUi;          // what the first two modules leave
        const float listH = std::max(150.f * gUi, rest * 0.64f), logH = std::max(60.f * gUi, rest - listH);
        beginModule("##f_list", listTitle(a, true), ImVec2(rightW, listH));
        tgRect(a, TgRight);
        rightPanel(a);
        endModule();
        beginModule("##f_log", "LOG", ImVec2(rightW, logH));
        logConsole(a, 0);
        endModule();
    }
    ImGui::EndGroup();
    if (anaH > 0) {
        beginModule("##f_ana", "ANALYSIS", ImVec2(W, anaH));
        tgRect(a, TgConst);
        constellationsTab(a);
        endModule();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- variant 4: scope / cinema (the biggest canvas: controls fold away, stations are bookmark chips)
void chip(const char* label, bool sel, bool& clicked) {
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 sz(ts.x + 14 * gUi, ImGui::GetFrameHeight());
    clicked = ImGui::InvisibleButton(label, sz);
    const bool hov = ImGui::IsItemHovered();
    const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (sel) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 30));
    else if (hov) dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 14));
    dl->AddRect(p0, p1, u32(sel ? T.bright : T.edge));
    dl->AddText(ImVec2(p0.x + 7 * gUi, p0.y + (sz.y - ts.y) * 0.5f), u32(sel ? T.bright : T.text), label);
}

void bodyCinema(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    static bool open = false;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##cinema", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    beginPane("##cin_head", ImVec2(0, 0), true);
    a.tgMin[TgSwitch] = ImGui::GetCursorScreenPos();
    modeCombo(a, 120 * gUi);
    a.tgMax[TgSwitch] = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    ImGui::SameLine(0, 10 * gUi);
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - 96 * gUi;
    bool any = false;
    auto drawChip = [&](const char* label, double mhz, bool fm) {
        const float w = ImGui::CalcTextSize(label).x + 14 * gUi;
        if (ImGui::GetCursorScreenPos().x + w > right) return;
        bool clicked = false;
        chip(label, std::fabs(mhz - a.freqMhz) < 0.05, clicked);
        ImGui::SameLine(0, 4 * gUi);
        any = true;
        if (clicked && !a.fmScan.running) { if (fm) fmTune(a, mhz); else tuneFreq(a, mhz); }
    };
    if (a.fmMode) {
        for (const auto& r : a.fmScan.results) if (r.found) { char b[64]; snprintf(b, sizeof b, "%.1f %s", r.mhz, r.name.c_str()); drawChip(b, r.mhz, true); }
    } else if (!a.dabMode && a.family < 6) {
        for (const auto& c : a.channels) { char b[64]; snprintf(b, sizeof b, "%.0f %s", c.freqMhz, c.name.empty() ? "mux" : c.name.c_str()); drawChip(b, c.freqMhz, false); }
    }
    if (!any) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled(a.fmMode ? "no stations yet: run the scan" : a.dabMode || a.family >= 6 ? "" : "no saved channels yet: run the scan"); ImGui::SameLine(); }
    ImGui::SetCursorScreenPos(ImVec2(right + 8 * gUi, ImGui::GetItemRectMin().y));
    if (ImGui::SmallButton(open ? "Controls  \xe2\x96\xb4" : "Controls  \xe2\x96\xbe")) open = !open;
    endPane();
    if (open) ribbonPane(a);
    const float botH = (a.atsc3Mode ? 0 : 118) * gUi;
    const float mainH = ImGui::GetContentRegionAvail().y - (botH > 0 ? botH + gap : 0);
    beginPane("##cin_main", ImVec2(0, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    a.tgMin[TgRight] = a.tgMax[TgRight] = ImVec2(0, 0);
    if (!a.atsc3Mode) {
        beginPane("##cin_ana", ImVec2(0, 0));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- variant 5: scope / meters (the dock is a bank of big instrument meters)
struct Meter { std::string label, value; float frac; int level; };   // level: 0 neutral, 1 good, 2 warn, 3 bad

void drawMeters(const std::vector<Meter>& ms, float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const Meter& m : ms) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float bh = 9 * gUi;
        dl->AddText(p, u32(T.dim), m.label.c_str());
        ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * 1.45f);
        const ImVec2 ts = ImGui::CalcTextSize(m.value.c_str());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + w - ts.x, p.y - 4 * gUi), u32(T.bright), m.value.c_str());
        ImGui::PopFont();
        const float y = p.y + ImGui::GetTextLineHeight() + 6 * gUi;
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w, y + bh), u32(ImVec4(0.07f, 0.075f, 0.08f, 1)));
        const ImVec4 col = m.level == 3 ? pal::badRed() : m.level == 2 ? pal::warnAmber() : m.level == 1 ? ImVec4(0.62f, 0.78f, 0.66f, 1) : ImVec4(0.72f, 0.74f, 0.77f, 1);
        dl->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + w * std::min(1.f, std::max(0.f, m.frac)), y + bh), u32(col));
        for (int k = 1; k < 10; k++) dl->AddLine(ImVec2(p.x + w * k / 10.f, y), ImVec2(p.x + w * k / 10.f, y + bh * (k == 5 ? 1.f : 0.45f)), u32(ImVec4(0.04f, 0.04f, 0.045f, 1)));
        dl->AddRect(ImVec2(p.x, y), ImVec2(p.x + w, y + bh), u32(T.edge));
        ImGui::Dummy(ImVec2(w, ImGui::GetTextLineHeight() + bh + 12 * gUi));
    }
}

void meterBank(App& a, float w) {
    std::vector<Meter> ms;
    auto add = [&](const char* label, const char* fmt, double v, double lo, double hi, int level) {
        char b[32]; snprintf(b, sizeof b, fmt, v);
        ms.push_back({label, b, (float)((v - lo) / (hi - lo)), level});
    };
    const RxTelemetry& rx = a.rx;
    const bool run = a.engine.running();
    if (const ModeUi* mu = modeUi(a.family); run && mu && mu->meters) {
        std::vector<ModeMeter> mm;
        mu->meters(a, mm);
        for (const ModeMeter& m : mm) add(m.label, m.fmt, m.v, m.lo, m.hi, m.level);
    } else if (run && a.fmMode && rx.standard == 6) {
        const FmTelemetry& f = rx.fm;
        add("AUDIO SNR  dB", "%.1f", f.snrDb, 0, 50, f.snrDb >= 30 ? 1 : f.snrDb >= 18 ? 2 : 3);
        add("DEVIATION  kHz", "%.0f", f.devKhz, 0, 100, f.devKhz > 90 ? 3 : f.devKhz > 75 ? 2 : 1);
        add("PILOT  %", "%.1f", f.pilotPct, 0, 12, f.stereo ? 1 : 0);
        add("RDS BLOCKS OK  %", "%.0f", f.rdsBlockOkPct, 0, 100, f.rdsBlockOkPct >= 70 ? 1 : f.rdsSync ? 2 : 0);
    } else if (run && a.dabMode && rx.standard == 3) {
        add("SNR  dB", "%.1f", rx.dab.snrDb, 0, 30, rx.dab.snrDb >= 12 ? 1 : 2);
        add("FIC BLOCKS OK  /12", "%.0f", rx.dab.ficRecentOk, 0, 12, rx.dab.ficRecentOk >= 11 ? 1 : rx.dab.ficRecentOk > 0 ? 2 : 3);
    } else if (run) {
        add("SNR  dB", "%.1f", rx.dataValid ? rx.dataSnrDb : rx.cpSnrDb, 0, 40, 0);
        if (rx.plpMerDb > 0) add("MER  dB", "%.1f", rx.plpMerDb, 0, 40, 0);
        const double tot = (double)(rx.blocksOk + rx.blocksBad);
        if (tot > 0) add("FEC BLOCKS OK  %", "%.0f", 100.0 * (double)rx.blocksOk / tot, 0, 100, 0);
    }
    const double db = run ? a.spec.stats.rmsDbfs : -60.0;
    add("ADC LEVEL  dBFS", "%.1f", db, -60, 0, !run ? 0 : db > -3 ? 3 : db > -12 ? 2 : db < -45 ? 2 : 1);
    drawMeters(ms, w);
}

void bodyMeters(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##meters", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ribbonPane(a);
    const float botH = (a.atsc3Mode ? 0 : 150) * gUi, dockW = 320 * gUi;
    const float mainH = ImGui::GetContentRegionAvail().y - (botH > 0 ? botH + gap : 0);
    beginPane("##main2", ImVec2(ImGui::GetContentRegionAvail().x - dockW - gap, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    ImGui::SameLine();
    beginPane("##right2", ImVec2(0, mainH));
    tgRect(a, TgRight);
    ImGui::TextDisabled("INSTRUMENTS");
    ImGui::Separator();
    meterBank(a, ImGui::GetContentRegionAvail().x - 4 * gUi);
    ImGui::Separator();
    rightPanel(a);
    endPane();
    if (!a.atsc3Mode) {
        beginPane("##const2", ImVec2(0, 0));
        tgRect(a, TgConst);
        constellationsTab(a);
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- variant 6: scope / dock (an IDE-style tabbed panel along the bottom)
void bodyDock(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4 * gUi, 4 * gUi));
    ImGui::BeginChild("##dockbody", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    ribbonPane(a);
    const float dockH = 230 * gUi;
    const float mainH = ImGui::GetContentRegionAvail().y - dockH - gap;
    beginPane("##main2", ImVec2(0, mainH));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    a.tgMin[TgRight] = a.tgMax[TgRight] = ImVec2(0, 0);
    beginPane("##dock", ImVec2(0, 0));
    tgRect(a, TgConst);
    if (ImGui::BeginTabBar("##docktabs", ImGuiTabBarFlags_DrawSelectedOverline)) {
        if (!a.atsc3Mode && ImGui::BeginTabItem("Analysis")) { constellationsTab(a); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(listTitle(a, false))) { tgRect(a, TgRight); rightPanel(a); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Receiver")) { statusBar(a); ImGui::Spacing(); receiverGlance(a, ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Log")) { logConsole(a, 0); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    endPane();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- variant 7: panel (the mode keys on the right; SDR settings, spectrum and waterfall, LEDs and constellations down the left)
const ImVec4 kPanelChassis(0.735f, 0.728f, 0.706f, 1);

// the mode keys: grouped, each with a lamp that lights when the mode is selected
void modeKeys(App& a) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool running = a.engine.running();
    const ImVec2 first = ImGui::GetCursorScreenPos();
    for (int g = 0; g < kNumGroups; g++) {
        ImGui::TextDisabled("%s", kGroupNames[g]);
        for (int i = 0; i < kNumModes; i++) {
            const ModeDef& m = kModes[i];
            if (m.group != g) continue;
            const float h = 26 * gUi, w = ImGui::GetContentRegionAvail().x - 2 * gUi;   // twelve keys have to fit: the name on the left, the short description on the right
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##key", ImVec2(w, h));
            ImGui::PopID();
            const bool hov = ImGui::IsItemHovered();
            const bool sel = modeSelected(a, m);
            const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
            const ImU32 plate = sel ? IM_COL32(18, 19, 20, 255) : hov ? IM_COL32(58, 59, 60, 255) : IM_COL32(48, 49, 50, 255);
            dl->AddRectFilled(p0, p1, plate, 2.f);
            if (sel) {   // pressed: a dark inset
                dl->AddRect(p0, p1, IM_COL32(8, 8, 8, 255), 2.f);
                dl->AddLine(ImVec2(p0.x + 1, p1.y), ImVec2(p1.x, p1.y), IM_COL32(95, 95, 92, 255));
            } else {     // raised: a light top edge, a dark bottom edge
                dl->AddLine(ImVec2(p0.x + 1, p0.y + 1), ImVec2(p1.x - 1, p0.y + 1), IM_COL32(110, 111, 112, 255));
                dl->AddLine(ImVec2(p0.x + 1, p1.y - 1), ImVec2(p1.x - 1, p1.y - 1), IM_COL32(14, 14, 15, 255));
                dl->AddRect(p0, p1, IM_COL32(20, 20, 21, 255), 2.f);
            }
            const ImVec2 c(p0.x + 15 * gUi, (p0.y + p1.y) * 0.5f);
            dl->AddCircleFilled(c, 6.f * gUi, IM_COL32(8, 8, 8, 255));
            dl->AddCircle(c, 6.f * gUi, IM_COL32(70, 70, 68, 255), 0, 1.f);
            if (sel) {
                dl->AddCircleFilled(c, 4.4f * gUi, u32(m.accent));
                dl->AddCircleFilled(ImVec2(c.x - 1.4f * gUi, c.y - 1.6f * gUi), 1.5f * gUi, IM_COL32(255, 255, 255, 120));
            } else dl->AddCircleFilled(c, 4.4f * gUi, IM_COL32(30, 31, 31, 255));
            const float fs = ImGui::GetFontSize();
            const ImVec2 ns = ImGui::CalcTextSize(m.name), ss = ImGui::CalcTextSize(m.sub);
            dl->AddText(ImVec2(p0.x + 30 * gUi, p0.y + (h - ns.y) * 0.5f), u32(sel ? T.bright : (running ? T.dim : T.text)), m.name);
            const float sw = ss.x * 0.84f;
            if (p0.x + 30 * gUi + ns.x + 14 * gUi < p1.x - sw - 8 * gUi) dl->AddText(ImGui::GetFont(), fs * 0.84f, ImVec2(p1.x - sw - 8 * gUi, p0.y + (h - ss.y * 0.84f) * 0.5f), u32(T.dim), m.sub);
            if (clicked) selectMode(a, m.family, m.preset);
            if (hov) ImGui::SetTooltip("%s", running && !sel ? "Stop the receiver to switch mode" : m.tip);
        }
        ImGui::Dummy(ImVec2(1, 2 * gUi));
    }
    a.tgMin[TgSwitch] = first; a.tgMax[TgSwitch] = ImVec2(first.x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y);
}

void bodyPanel(App& a, float bodyH, ImVec2 disp) {
    (void)disp;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 6 * gUi));
    ImGui::BeginChild("##panel", ImVec2(0, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const float gap = 8 * gUi, vgap = 4 * gUi;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, vgap));
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float rightW = 236 * gUi, leftW = W - rightW - gap;
    const bool synth = a.devices[a.devIdx].kind == DeviceInfo::Synthetic || (a.devices[a.devIdx].kind == DeviceInfo::File && !a.engine.running());
    const float sdrH = (synth ? 196 : 148) * gUi + (modeUi(a.family) && modeUi(a.family)->tuner ? 70 * gUi : 0), ledH = 80 * gUi, anaH = (a.atsc3Mode ? 0 : 224) * gUi;
    const float wfH = H - sdrH - ledH - anaH - (anaH > 0 ? 3 : 2) * vgap;
    ImGui::BeginGroup();
    {   // SDR settings: run, frequency, what is received, level and volume; then the source, tuner and gain controls
        beginModule("##p_sdr", "SDR", ImVec2(leftW, sdrH));
        tgRect(a, TgToolbar);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const bool running = a.engine.running();
        if (runButton(running, 44 * gUi) || (!running && a.wizStart)) { if (running) a.engine.stop(); else startReceiver(a); a.wizStart = false; }
        ImGui::SetCursorScreenPos(ImVec2(o.x + 62 * gUi, o.y - 1 * gUi));
        const float digitsEnd = freqDigits(a, 38.f);
        const float vw = 210 * gUi, mw = 250 * gUi;
        summary(a, digitsEnd + 30 * gUi, o.y + 5 * gUi, 1.f, o.x + ImGui::GetContentRegionAvail().x - vw - mw - 36 * gUi - (digitsEnd + 30 * gUi));
        levelMeter(a, o.x + ImGui::GetContentRegionAvail().x - vw - mw - 20 * gUi, o.y, mw);
        ImGui::SetCursorScreenPos(ImVec2(o.x + ImGui::GetContentRegionAvail().x - vw + 4 * gUi, o.y + 8 * gUi));
        volumeControl(a, vw);
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + 52 * gUi));
        toolbarParts(a, TbSource | TbTuner | TbGain | TbDecoder, false);
        if (a.devices[a.devIdx].isRadio() && !a.devices[a.devIdx].settings.empty()) { ImGui::SameLine(0, 14 * gUi); radioSettingsUi(a, false); }
        if (synth) sourceOptions(a);
        endModule();
    }
    {   // the picture
        beginModule("##p_wf", "SPECTRUM / WATERFALL", ImVec2(leftW, wfH));
        tgRect(a, TgMain);
        mainTabs(a);
        endModule();
    }
    {   // the LEDs
        beginModule("##p_led", "STATUS", ImVec2(leftW, ledH));
        statusBar(a);
        endModule();
    }
    if (anaH > 0) {
        beginModule("##p_ana", "CONSTELLATIONS", ImVec2(leftW, anaH));
        tgRect(a, TgConst);
        constellationsTab(a);
        endModule();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::BeginGroup();
    {
        const float modeH = (kNumModes * 30 + 3 * 26 + 14) * gUi;
        beginModule("##p_mode", "MODE", ImVec2(rightW, modeH));
        modeKeys(a);
        endModule();
        beginModule("##p_list", listTitle(a, true), ImVec2(rightW, H - modeH - vgap));
        tgRect(a, TgRight);
        rightPanel(a);
        endModule();
    }
    ImGui::EndGroup();
    ImGui::PopStyleVar();
    ImGui::EndChild();
}
} // namespace

void drawShell2(App& a, ImVec2 disp) {
    {   // keyboard: 1-9 pick a mode, space starts and stops (not while typing)
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && !ImGui::IsAnyItemActive() && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
            if (!gFreqHovered) for (int k = 0; k < 9; k++) if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + k), false)) {
                int n = 0;
                for (int g = 0; g < kNumGroups; g++) for (int i = 0; i < kNumModes; i++) if (kModes[i].group == g && n++ == k) selectMode(a, kModes[i].family, kModes[i].preset);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) { if (a.engine.running()) a.engine.stop(); else startReceiver(a); }
        }
    }
    const float menuH = menuBar(a);
    {   // the waterfall range is set automatically once, two seconds after the receiver starts
        static bool wasRunning = false, done = false;
        static double t0 = 0;
        const bool run = a.engine.running();
        if (run && !wasRunning) { t0 = ImGui::GetTime(); done = false; }
        wasRunning = run;
        if (run && !done && ImGui::GetTime() - t0 > 2.5) { autoRange(a); done = true; }
    }
    pal::panel() = a.uiVariant == 7;
    const bool face = a.uiVariant == 3 || a.uiVariant == 7;
    ImGui::SetNextWindowPos(ImVec2(0, menuH));
    ImGui::SetNextWindowSize(ImVec2(disp.x, disp.y - menuH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (face) ImGui::PushStyleColor(ImGuiCol_WindowBg, a.uiVariant == 7 ? kPanelChassis : kChassis);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    if (face) ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    const float sbH = ImGui::GetFrameHeight() + 8 * gUi;
    if (!face) topBar(a, disp);
    const float bodyH = ImGui::GetContentRegionAvail().y - sbH;
    switch (a.uiVariant) {
    case 1: bodyScope(a, bodyH, disp); break;
    case 2: bodyTiles(a, bodyH, disp); break;
    case 3: bodyFace(a, bodyH, disp); break;
    case 4: bodyCinema(a, bodyH, disp); break;
    case 5: bodyMeters(a, bodyH, disp); break;
    case 6: bodyDock(a, bodyH, disp); break;
    case 7: bodyPanel(a, bodyH, disp); break;
    default: bodyParent(a, bodyH, disp); break;
    }
    bottomBar(a, disp);
    ImGui::End();
}
