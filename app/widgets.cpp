// small reusable widgets: pill buttons, lamps, gauges, scatter and history plots, formatting helpers
#include "app.h"

bool gTightTabs = false;
bool tabItem(const char* name, Ic icon) {
    ImGuiTabItemFlags fl = 0;
    if (!gForceTab.empty() && gForceTab == name) { fl = ImGuiTabItemFlags_SetSelected; gForceTab.clear(); }
    // the label is padded with spaces to leave room for the icon, which is drawn over that gap
    const float gap = pal::dev() ? 0.f : iconSize() + 5.f;
    const int nSp = (int)std::ceil(gap / ImGui::CalcTextSize(" ").x);
    const std::string label = std::string(nSp, ' ') + name + "###" + name;
    // a narrow pane (gTightTabs, set by mainTabs() for its tab bar): less padding, so that every tab keeps its whole name in view
    if (gTightTabs) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3 * gUi, ImGui::GetStyle().FramePadding.y));
    const bool open = ImGui::BeginTabItem(label.c_str(), nullptr, fl);
    if (gTightTabs) ImGui::PopStyleVar();
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    if (!pal::dev()) icons::draw(icon, ImVec2(r0.x + ImGui::GetStyle().FramePadding.x + iconSize() * 0.5f + 1.f, (r0.y + r1.y) * 0.5f), iconSize() * 0.92f,
                open ? IM_COL32(255, 255, 255, 255) : IM_COL32(140, 154, 170, 255));
    return open;
}

void toggleFullscreen() {
    if (!gWindow) return;
    if (glfwGetWindowMonitor(gWindow)) { glfwSetWindowMonitor(gWindow, nullptr, gWinX, gWinY, gWinW, gWinH, 0); return; }
    glfwGetWindowPos(gWindow, &gWinX, &gWinY); glfwGetWindowSize(gWindow, &gWinW, &gWinH);
    GLFWmonitor* m = glfwGetPrimaryMonitor();
    const GLFWvidmode* vm = glfwGetVideoMode(m);
    glfwSetWindowMonitor(gWindow, m, 0, 0, vm->width, vm->height, vm->refreshRate);
}

bool pillButton(const char* label, bool selected, float padX) {
    ImGui::PushID(label);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 sz(ts.x + padX * 2, ImGui::GetFrameHeight() - 3);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##pill", sz);
    const bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 bg = selected ? pal::remap(IM_COL32(52, 92, 108, 255)) : hov ? IM_COL32(46, 56, 68, 255) : IM_COL32(30, 35, 42, 255);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, pal::rnd(3.f));
    dl->AddText(ImVec2(p.x + padX, p.y + (sz.y - ts.y) * 0.5f), selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(176, 184, 194, 255), label);
    ImGui::PopID();
    return clicked;
}

// A row of pills that switches between sub-views of a tab. Returns the selected index.
int subNav(const char* id, int& cur, std::initializer_list<const char*> names) {
    ImGui::PushID(id);
    int i = 0;
    for (const char* n : names) { if (i) sameLineIf(ImGui::CalcTextSize(n).x + 22, 6 * gUi); if (pillButton(n, cur == i)) cur = i; i++; }   // wraps in a narrow tab
    ImGui::PopID();
    ImGui::Spacing();
    return cur;
}

// Rows that wrap. The groups of a row are counted per window and frame; each one's width is kept in the window's storage for the next frame.
namespace {
struct FlowKeys { ImGuiID frame, n, x, y, same; };
FlowKeys flowKeys() { return {ImGui::GetID("##flowF"), ImGui::GetID("##flowN"), ImGui::GetID("##flowX"), ImGui::GetID("##flowY"), ImGui::GetID("##flowS")}; }
ImGuiID flowWidthKey(int k) { ImGui::PushID(k); const ImGuiID id = ImGui::GetID("##flowW"); ImGui::PopID(); return id; }
// keeps the width of the group that has just ended; returns its number
int flowMeasure(ImGuiStorage* st, const FlowKeys& k) {
    const int frame = ImGui::GetFrameCount();
    if (st->GetInt(k.frame, -1) != frame) { st->SetInt(k.frame, frame); st->SetInt(k.n, 0); st->SetFloat(k.y, -FLT_MAX); }
    const ImVec2 i0 = ImGui::GetItemRectMin(), i1 = ImGui::GetItemRectMax();
    const int n = st->GetInt(k.n, 0);
    const float gy = st->GetFloat(k.y, -FLT_MAX);
    const float gx = gy <= i0.y + 1 && gy >= i0.y - ImGui::GetFrameHeight() ? st->GetFloat(k.x, i0.x) : ImGui::GetCursorScreenPos().x;   // else a new row: from the line start
    st->SetFloat(flowWidthKey(n), i1.x - gx);
    return n;
}
bool flowFresh(ImGuiStorage* st, const FlowKeys& k) {   // nothing drawn since the last flowNext()
    return st->GetInt(k.frame, -1) == ImGui::GetFrameCount() && st->GetFloat(k.x, 0) == ImGui::GetCursorScreenPos().x && st->GetFloat(k.y, 0) == ImGui::GetCursorScreenPos().y;
}
}

bool flowNext(float spacing) {
    const float sp = spacing < 0 ? ImGui::GetStyle().ItemSpacing.x : spacing;
    ImGuiStorage* st = ImGui::GetStateStorage();
    const FlowKeys k = flowKeys();
    const int n = flowMeasure(st, k);
    const float need = st->GetFloat(flowWidthKey(n + 1), 0.f);
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    // a few pixels to spare: the width is last frame's, and a live value can grow by a digit since (the FM status line ran 9 px over)
    const float slack = 0.6f * ImGui::GetFontSize();
    const bool same = ImGui::GetItemRectMax().x + sp + need + slack <= right + 0.5f || need <= 0.f;
    if (same) ImGui::SameLine(0, sp);
    st->SetInt(k.n, n + 1);
    st->SetFloat(k.x, ImGui::GetCursorScreenPos().x);
    st->SetFloat(k.y, ImGui::GetCursorScreenPos().y);
    st->SetBool(k.same, same);
    return same;
}

void flowEnd() {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const FlowKeys k = flowKeys();
    if (st->GetInt(k.frame, -1) != ImGui::GetFrameCount() || flowFresh(st, k)) return;
    // the next row in this window numbers its groups on from here: else its first group took this one's number and width
    // (the DVB-S lamps: "FEC" stood in for "Framing", which then did not wrap and ran past the edge)
    st->SetInt(k.n, flowMeasure(st, k) + 1);
}

void flowBreak() {
    ImGuiStorage* st = ImGui::GetStateStorage();
    const FlowKeys k = flowKeys();
    if (!flowFresh(st, k)) { flowEnd(); ImGui::NewLine(); }
    else if (st->GetBool(k.same, false)) ImGui::NewLine();
}

bool sameLineIf(float w, float spacing) {
    const float sp = spacing < 0 ? ImGui::GetStyle().ItemSpacing.x : spacing;
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    if (ImGui::GetItemRectMax().x + sp + w > right + 0.5f) return false;
    ImGui::SameLine(0, sp);
    return true;
}

StatusPanel::StatusPanel() {
    p = ImGui::GetCursorScreenPos();
    id = ImGui::GetID("##statuspanel");
    const float h = std::max(ImGui::GetFrameHeight() * 2.f + ImGui::GetStyle().ItemSpacing.y * 2.f, ImGui::GetStateStorage()->GetFloat(id, 0.f));
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + ImGui::GetContentRegionAvail().x + 4, p.y + h), IM_COL32(22, 23, 25, 255), 3.f);
}
StatusPanel::~StatusPanel() { ImGui::GetStateStorage()->SetFloat(id, ImGui::GetItemRectMax().y - p.y + ImGui::GetStyle().ItemSpacing.y); }

std::string ellipsize(std::string s, float w, float size) {
    ImFont* f = ImGui::GetFont();
    if (size <= 0) size = ImGui::GetFontSize();
    if (f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x <= w) return s;
    while (!s.empty() && f->CalcTextSizeA(size, FLT_MAX, 0, (s + "...").c_str()).x > w) {
        s.pop_back();
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();   // not in the middle of a UTF-8 character
        if (!s.empty() && (unsigned char)s.back() >= 0xC0) s.pop_back();
    }
    return s.empty() ? s : s + "...";
}

std::string fitCaption(const std::string& s, float w) {
    if (ImGui::CalcTextSize(s.c_str()).x <= w) return s;
    const size_t k = s.find(" (");
    if (k != std::string::npos && ImGui::CalcTextSize(s.substr(0, k).c_str()).x <= w) return s.substr(0, k);
    return ellipsize(s, w);
}

void captionFit(float w, const char* fmt, ...) {
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    const std::string c = fitCaption(b, w);
    ImGui::TextDisabled("%s", c.c_str());
    if (c != b && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", b);
}

void kvColumn(float col) {
    // in screen space: after the key the cursor is at the start of the next line (the left of the window, the indent or the group)
    const float lineStart = ImGui::GetCursorScreenPos().x, keyEnd = ImGui::GetItemRectMax().x;
    const float want = lineStart + col - ImGui::GetCursorStartPos().x;   // where SameLine(col) put it
    const float x = std::max(keyEnd + 8 * gUi, std::min(want, lineStart + ImGui::GetContentRegionAvail().x * 0.5f));
    ImGui::SameLine(0, x - keyEnd);
}

// Small rounded label drawn at an absolute position; returns its width.
float tagAt(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float w = ts.x + 10, h = ts.y + 2;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bg, pal::rnd(3.f));
    dl->AddText(ImVec2(pos.x + 5, pos.y + 1), fg, text);
    return w;
}

void gaugePill(float width, float frac, ImU32 fill, const char* text) {
    width = std::max(width, ImGui::CalcTextSize(text).x + 10 * gUi);   // never narrower than its text (a large display scale)
    const float h = ImGui::GetFrameHeight() - 2;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += 1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), pal::dev() ? IM_COL32(8, 8, 8, 255) : IM_COL32(14, 16, 20, 255), pal::rnd(3.f));
    frac = std::min(1.f, std::max(0.f, frac));
    if (frac > 0.02f) dl->AddRectFilled(p, ImVec2(p.x + std::max(pal::dev() ? 1.f : h, width * frac), p.y + h), pal::remap(fill), pal::rnd(3.f));
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), pal::dev() ? IM_COL32(44, 44, 42, 255) : IM_COL32(52, 58, 66, 255), pal::rnd(3.f));
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(240, 244, 248, 255), text);
    ImGui::Dummy(ImVec2(width, h));
}

void lamp(const char* label, int state /*0 grey 1 green 2 amber 3 red*/, int icon) {
    ImVec4 c = state == 1 ? pal::okGreen() : state == 2 ? pal::warnAmber()
             : state == 3 ? pal::badRed() : ImVec4(0.26f, 0.29f, 0.33f, 1);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const ImU32 cu = ImGui::ColorConvertFloat4ToU32(c);
    (void)icon;
    const float sq = 7.f, cy = p.y + ImGui::GetTextLineHeight() * 0.5f;
    if (pal::panel()) {   // a round LED in a dark bezel, with a small highlight when lit
        const ImVec2 c(p.x + 6.f * gUi, cy);
        dl->AddCircleFilled(c, 6.f * gUi, IM_COL32(8, 8, 8, 255));
        dl->AddCircle(c, 6.f * gUi, IM_COL32(70, 70, 68, 255), 0, 1.f);
        if (state) {
            dl->AddCircleFilled(c, 4.4f * gUi, cu);
            dl->AddCircleFilled(ImVec2(c.x - 1.4f * gUi, c.y - 1.6f * gUi), 1.5f * gUi, IM_COL32(255, 255, 255, 120));
        } else dl->AddCircleFilled(c, 4.4f * gUi, IM_COL32(30, 31, 31, 255));
        ImGui::Dummy(ImVec2(14.f * gUi, ImGui::GetTextLineHeight()));
    } else {
    if (state) dl->AddRectFilled(ImVec2(p.x + 1, cy - sq * 0.5f), ImVec2(p.x + 1 + sq, cy + sq * 0.5f), cu);
    else dl->AddRect(ImVec2(p.x + 1, cy - sq * 0.5f), ImVec2(p.x + 1 + sq, cy + sq * 0.5f), cu);
    ImGui::Dummy(ImVec2(sq + 5, ImGui::GetTextLineHeight()));
    }
    ImGui::SameLine(0, 0);
    ImGui::TextDisabled("%s", label);
    static const struct { const char* k; const char* tip; } kTips[] = {
        {"IQ", "Sample stream. Green: the ADC level is in a healthy range. Amber: too low or high. Red: clipping."},
        {"P1", "DVB-T2: the P1 preamble symbol (frame start and mode) is being found."},
        {"GI", "Guard interval and symbol timing are locked."},
        {"L1-pre", "DVB-T2: the L1 pre-signalling block (frame structure) decodes with a good CRC."},
        {"L1-post", "DVB-T2: the L1 post-signalling block (PLP list, modulation) decodes with a good CRC."},
        {"Frame", "A full T2 frame is being received and equalised."},
        {"LDPC", "Forward error correction, inner code: share of blocks that decode."},
        {"BCH", "Forward error correction, outer code: share of blocks that decode."},
        {"TS", "Transport stream: services found, continuity errors counted."},
        {"Video", "The player is decoding and showing pictures."},
        {"Audio", "The player has audio buffered and playing."},
        {"Sync", "DVB-T: OFDM symbol sync from the cyclic prefix."},
        {"TPS", "DVB-T: the transmission parameter signalling bits are decoded."},
        {"Chan", "Channel estimate from the pilots is valid."},
        {"Viterbi", "DVB-T: the convolutional decoder found the packet sync."},
        {"RS", "Reed-Solomon outer code: share of packets that are clean."},
        {"Pilot", "ATSC: the 8-VSB pilot carrier is locked."},
        {"Seg", "ATSC: data segment sync found."},
        {"Field", "ATSC: field sync found."},
        {"Eq", "ATSC: the equaliser is trained."},
        {"Trellis", "ATSC: the trellis decoder produces a valid transport stream."}};
    {
        const ImVec2 mx = ImGui::GetItemRectMax();
        if (ImGui::IsMouseHoveringRect(p, ImVec2(mx.x, p.y + ImGui::GetTextLineHeight()))) {
            for (auto& e : kTips) if (!strcmp(e.k, label)) { ImGui::SetTooltip("%s", e.tip); break; }
        }
    }
}

void scatter(const char* id, const std::vector<cf32>& pts, ImVec2 size, double lim, ImVec4 col) {
    if (plt::BeginPlot(id, size, plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_Equal)) {
        plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, plt::AxisFlags_NoTickLabels);
        plt::SetupAxisLimits(plt::X1, -lim, lim, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -lim, lim, plt::Cond_Always);
        if (!pts.empty()) {
            plt::Spec sp;
            sp.Marker = plt::Marker_Circle; sp.MarkerSize = 1.6f; sp.Stride = sizeof(cf32);
            sp.MarkerFillColor = col; sp.MarkerLineColor = col; sp.LineColor = col;
            const float* d = reinterpret_cast<const float*>(pts.data());
            plt::PlotScatter("pts", d, d + 1, (int)pts.size(), sp);
        }
        plt::EndPlot();
    }
}

void historyPlot(const char* id, const char* ylabel, const std::deque<float>& h, ImVec2 size) {
    if (plt::BeginPlot(id, size, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("samples (~30/s)", ylabel, 0, plt::AxisFlags_AutoFit);
        plt::SetupAxisLimits(plt::X1, 0, 600, plt::Cond_Always);
        if (!h.empty()) {
            std::vector<float> v(h.begin(), h.end());
            plt::PlotLine("h", v.data(), (int)v.size());
        }
        plt::EndPlot();
    }
}

std::string fmtLocal(int64_t utc, const char* f) {
    time_t t = (time_t)utc; struct tm m; dect2::localTime(t, &m);
    char b[48]; strftime(b, sizeof b, f, &m);
    return b;
}

const char* genreName(int g) {
    static const char* n[] = {"", "Movie / drama", "News / current affairs", "Show / game show", "Sports", "Children's / youth", "Music / ballet / dance", "Arts / culture", "Social / political / economics", "Education / science", "Leisure / hobbies"};
    return g >= 1 && g <= 10 ? n[g] : "";
}

const char* fmtKbps(char* b, size_t n, double k) { if (k >= 1000) snprintf(b, n, "%.2f Mbit/s", k / 1000); else snprintf(b, n, "%.0f kbit/s", k); return b; }

void qualityBar(App& a, float width) {
    QualityReport a3q;
    const bool a3 = a.atsc3Mode && atsc3Quality(a, a3q);
    const QualityReport& q = a3 ? a3q : a.quality.report();
    const bool run = a.engine.running();
    const float pct = run && q.valid ? (float)q.percent : 0.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), pal::dev() ? IM_COL32(8, 8, 8, 255) : IM_COL32(120, 28, 28, 255), pal::rnd(3.f)); // red = nothing yet
    const float t = pct / 100.f;
    const ImVec4 lo(0.95f, 0.55f, 0.15f, 1), hi(0.25f, 0.85f, 0.35f, 1);
    const ImVec4 col = pal::dev() ? ImVec4(0.65f, 0.48f, 0.18f, 1) : ImVec4(lo.x + (hi.x - lo.x) * t, lo.y + (hi.y - lo.y) * t, lo.z + (hi.z - lo.z) * t, 1);   // the dev palette has no red-to-green blend
    if (pct > 0) dl->AddRectFilled(p, ImVec2(p.x + width * t, p.y + h), ImGui::ColorConvertFloat4ToU32(col), pal::rnd(3.f));
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), pal::dev() ? IM_COL32(44, 44, 42, 255) : IM_COL32(70, 76, 84, 255), pal::rnd(3.f));
    char txt[96];
    if (!run || !q.valid) snprintf(txt, sizeof txt, "signal quality: %s", run ? "no lock" : "-");
    else snprintf(txt, sizeof txt, "signal quality %.0f%%  %s", q.percent, q.label.c_str());
    ImVec2 ts = ImGui::CalcTextSize(txt);
    dl->AddText(ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(235, 238, 242, 255), txt);
    ImGui::Dummy(ImVec2(width, h));
    if (ImGui::IsItemHovered() && run && q.valid) {
        ImGui::BeginTooltip();
        if (a3) {
            ImGui::Text("FEC blocks decoded in the last frame: %.1f%%", q.fecOk * 100);
            ImGui::TextDisabled("ATSC 3.0 does not report a signal-to-noise ratio yet; this bar shows how much of the data decodes.");
        } else if (q.requiredDb <= 0) {   // the modes with no table of required SNR
            ImGui::Text("data SNR %.1f dB, data decoded %.1f%% (last frames)", q.snrDb, q.fecOk * 100);
            ImGui::TextDisabled("This bar shows how much of the recent data decodes.");
        } else {
            ImGui::Text("data SNR %.1f dB, needed for this modulation/code rate about %.1f dB", q.snrDb, q.requiredDb);
            ImGui::Text("margin %+.1f dB, FEC blocks decoded %.1f%% (last frames)", q.marginDb, q.fecOk * 100);
            ImGui::TextDisabled("0 dB margin is the edge of reception (25%%); +6 dB or more is comfortable.");
        }
        ImGui::EndTooltip();
    }
}

