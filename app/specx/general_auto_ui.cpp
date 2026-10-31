// General / Auto, the App side (core: dect2/engine/general_auto.h, docs/GENERAL-AUTO.md). In the General receiver the identifier runs in the
// background at a low rate on the engine's own capture thread; every signal it recognises gets a label on the spectrum and the waterfall
// ("DVB-T / T2  8 MHz  90 %") with a Decode button. Decode tunes and sets up the matching mode, and the app stays in General: tune away from the
// signal and it goes back to the General receiver (a plain spectrum); tune back and the label is offered again. A compact list of the signals
// is behind the "Signals" button in the corner of the spectrum, with Auto-decode (off by default) and the per-label Ignore.
#include "app.h"
#include "memory/memory_options.h"
#include "state/listen_freq.h"
#include "ui/theme.h"
#include "dect2/engine/general_auto.h"
#include "dect2/specx/specx_measure.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
namespace ga = dect2::genauto;
namespace si = dect2::sigid;

struct Chip { int id; ImVec2 a, b, decA, decB, xA, xB; bool canDecode; };

struct State {
    ga::GeneralAuto g;
    bool waiting = false;
    uint64_t seq = 0;
    double scanCentre = 0, scanRate = 0, lastCentre = 0, lastRate = 0, lastScanDone = -1e9;
    dect2::Memory back;            // the General receiver as it was before a Decode
    int launchFamily = -1;
    std::string note;
    double noteUntil = 0;
    std::vector<Chip> chips[2];
    bool hovered = false;
    int wideDecodeId = 0;
};
State S;

void say(const std::string& s) { S.note = s; S.noteUntil = ImGui::GetTime() + 6; }

std::string bwText(double hz) {
    char b[32];
    if (hz >= 1e6) snprintf(b, sizeof b, "%.3g MHz", hz / 1e6);
    else if (hz >= 1e3) snprintf(b, sizeof b, "%.3g kHz", hz / 1e3);
    else snprintf(b, sizeof b, "%.0f Hz", hz);
    return b;
}

bool canOpenMode(const std::string& mode) { return !mode.empty() && memoryModeIndex(mode) >= 0; }
bool canOpen(const ga::Detection& d) { return canOpenMode(d.mem.mode); }

void startDecode(App& a, int id) {
    const ga::Label* l = S.g.find(id);
    if (!l) { say(tr("That signal is gone.")); return; }
    if (!canOpen(l->d)) { say(std::string(tr("This build has no decoder for ")) + l->d.name + "."); return; }
    const dect2::Memory back = memoryCapture(a);
    const ga::Step st = S.g.decode(ImGui::GetTime(), id);
    if (st.act != ga::Action::Decode) { say(st.note); return; }
    std::string n;
    if (memoryRecallApp(a, st.mem, true, n)) {
        S.back = back; S.launchFamily = a.family;
        say(std::string(tr("Decoding ")) + l->d.name + (n.empty() ? "" : "  (" + n + ")"));
    } else {
        S.g.abandon();
        say(n.empty() ? std::string(tr("Could not open this mode.")) : n);
    }
}

void backToGeneral(App& a, const std::string& why) {
    dect2::Memory m = S.back;
    if (m.mode.empty()) m.mode = "general";
    m.freqHz = (uint64_t)std::llround(a.freqMhz * 1e6);
    m.name.clear();
    std::string n;
    memoryRecallApp(a, m, true, n);
    S.launchFamily = -1;
    say(why);
}

void handle(App& a, const ga::Step& st) {
    if (st.act == ga::Action::ReturnToGeneral) backToGeneral(a, st.note.empty() ? std::string(tr("Back to General")) : st.note);
}

} // namespace

bool generalAutoHovered() { return S.hovered; }
bool generalAutoDecoding() { return S.g.state() == ga::State::Decoding; }

void generalAutoFrame(App& a) {
    const double now = ImGui::GetTime();
    S.g.cfg.autoDecode = a.genAutoDecode;
    S.g.cfg.releaseHz = a.genAutoReleaseKhz * 1e3;
    if (!a.genAutoOn || !a.engine.running()) {
        if (S.waiting) { S.waiting = false; S.g.scanAborted(); }
        if (!a.genAutoOn && (S.g.state() == ga::State::Decoding || !S.g.labels().empty())) { S.g.abandon(); S.g.reset(); }
        return;
    }
    if (S.g.state() == ga::State::Decoding && a.family != S.launchFamily) S.g.abandon();   // the user chose another mode by hand: that is theirs now
    if (S.g.state() == ga::State::General && a.family != 55) {   // not in General: nothing runs
        if (S.waiting) { S.waiting = false; S.g.scanAborted(); }
        if (!S.g.labels().empty()) S.g.reset();
        return;
    }
    const double tuned = (S.g.state() == ga::State::Decoding ? a.freqMhz : listenMhz(a)) * 1e6;
    {   // Auto-decode calls decode() inside tick(): remember the General receiver before the radio moves
        const dect2::Memory before = memoryCapture(a);
        const bool wasGeneral = S.g.state() == ga::State::General;
        const ga::Step st = S.g.tick(now, tuned);
        if (st.act == ga::Action::Decode) {
            std::string n;
            if (canOpenMode(st.mem.mode) && memoryRecallApp(a, st.mem, true, n)) {
                if (wasGeneral) S.back = before;
                S.launchFamily = a.family;
                say(st.note);
            } else { S.g.abandon(); S.g.ignore(st.labelId); say(n.empty() ? std::string(tr("Could not open this mode.")) : n); }
        } else handle(a, st);
    }
    if (S.g.state() != ga::State::General || a.family != 55) return;
    const double centre = radioCenterMhz(a) * 1e6, rate = a.engine.sampleRate();
    if (std::fabs(centre - S.lastCentre) > 1 || rate != S.lastRate) {
        S.lastCentre = centre; S.lastRate = rate;
        S.g.retuned(now);
        if (S.waiting) { S.waiting = false; S.g.scanAborted(); }
    }
    if (identifyBusy()) { if (S.waiting) { S.waiting = false; S.g.scanAborted(); } return; }   // the Identify window owns the capture
    si::Capture& cap = a.engine.sigid();
    if (S.waiting) {
        std::vector<si::Capture::Found> f;
        if (cap.resultAll(f, S.seq)) {
            S.waiting = false;
            std::vector<ga::Detection> v;
            for (const auto& x : f) { ga::Detection d; if (ga::fromFound(x, S.scanCentre, d)) v.push_back(d); }
            S.g.scanDone(now, S.scanCentre - S.scanRate / 2, S.scanCentre + S.scanRate / 2, v);
            S.lastScanDone = now;
        }
        return;
    }
    if (rate > 0 && S.g.wantScan(now) && cap.state() != 1 && cap.state() != 2) {
        std::vector<si::Capture::Found> tmp; uint64_t s = S.seq;
        if (cap.resultAll(tmp, s)) S.seq = s;   // a finished capture of the Identify window: not ours
        cap.startAll(rate, centre, si::captureSeconds(rate), 12);
        S.scanCentre = centre; S.scanRate = rate; S.waiting = true;
        S.g.scanStarted(now);
    }
}

namespace {

ImU32 confCol(double c, int alpha) { return c >= 0.8 ? IM_COL32(110, 215, 130, alpha) : c >= 0.6 ? IM_COL32(235, 195, 90, alpha) : IM_COL32(170, 175, 185, alpha); }

bool inRect(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y; }

void tooltipFor(const ga::Label& l) {
    ImGui::BeginTooltip();
    ImGui::Text("%s", l.d.name.c_str());
    ImGui::TextDisabled("%s  -  %s wide  -  SNR %.0f dB", dect2::specx::freqText(l.d.rfHz).c_str(), bwText(l.d.bwHz).c_str(), l.d.snrDb);
    ImGui::TextDisabled("%.0f %% sure%s", l.confidence * 100, l.d.verify == si::VerifyConfirmed ? tr(", confirmed by the mode's own detector") : l.d.verify == si::VerifyFailed ? tr(", the mode's own detector did not confirm it") : "");
    ImGui::PushTextWrapPos(320 * gUi);
    ImGui::TextDisabled("%s", tr("Decode tunes and sets up the mode. You stay in General: tune away from the signal and it goes back to the plain spectrum."));
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

} // namespace

void generalAutoPlot(App& a, int plotId, ImVec2 plotMin, ImVec2 plotSize, double loMhz, double hiMhz) {
    if (plotId == 0) S.hovered = false;
    S.chips[plotId ? 1 : 0].clear();
    if (!a.genAutoOn || !a.engine.running() || plotSize.x < 120 * gUi || plotSize.y < 70 * gUi || !(hiMhz > loMhz)) return;
    const bool decoding = S.g.state() == ga::State::Decoding;
    if (!decoding && a.family != 55) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * 0.9f, padX = 5 * gUi, rowH = fs + 6 * gUi, gap = 4 * gUi;
    const ImVec2 plotMax(plotMin.x + plotSize.x, plotMin.y + plotSize.y);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto xOf = [&](double mhz) { return plotMin.x + (float)((mhz - loMhz) / (hiMhz - loMhz)) * plotSize.x; };
    dl->PushClipRect(plotMin, plotMax, true);
    const bool released = ImGui::IsMouseReleased(0), plotHover = ImGui::IsMouseHoveringRect(plotMin, plotMax) && !ImGui::IsAnyItemActive();
    const ImVec2 drag = ImGui::GetMouseDragDelta(0, 0);
    const bool click = released && std::fabs(drag.x) < 4 && std::fabs(drag.y) < 4;
    if (decoding) {
        const ga::Label* l = S.g.active();
        if (l && plotId == 0) {   // the banner: what runs, how far the tuning may drift, and the way back
            char t[200];
            snprintf(t, sizeof t, tr("Decoding %s   (stays on within +-%s)"), l->d.name.c_str(), bwText(S.g.activeReleaseHz()).c_str());
            const char* btn = tr("Back to General");
            const float tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x, bw = font->CalcTextSizeA(fs, FLT_MAX, 0, btn).x + 2 * padX;
            const float w = tw + bw + 3 * padX + gap, x0 = plotMin.x + (plotSize.x - w) / 2, y0 = plotMin.y + ImGui::GetTextLineHeight() + 10 * gUi;
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + rowH), IM_COL32(14, 16, 20, 225), 4 * gUi);
            dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + rowH), IM_COL32(110, 215, 130, 200), 4 * gUi);
            dl->AddText(font, fs, ImVec2(x0 + padX, y0 + (rowH - fs) / 2), IM_COL32(236, 238, 240, 255), t);
            const ImVec2 b0(x0 + 2 * padX + tw, y0 + 2 * gUi), b1(b0.x + bw, y0 + rowH - 2 * gUi);
            const bool h = plotHover && inRect(mouse, b0, b1);
            dl->AddRectFilled(b0, b1, ImGui::GetColorU32(h ? pal::accent(0.95f) : pal::accent(0.7f)), 3 * gUi);
            dl->AddText(font, fs, ImVec2(b0.x + padX, y0 + (rowH - fs) / 2), IM_COL32(255, 255, 255, 255), btn);
            if (plotHover && inRect(mouse, ImVec2(x0, y0), ImVec2(x0 + w, y0 + rowH))) S.hovered = true;
            if (h && click) handle(a, S.g.stop(ImGui::GetTime()));
        }
        dl->PopClipRect();
        return;
    }
    const std::vector<const ga::Label*> vis = S.g.visible();
    const float top = ImGui::GetTextLineHeight() + 10 * gUi, rightKeep = plotId ? 100 * gUi : 0;
    struct Placed { float x0, x1; int row; };
    std::vector<Placed> placed;
    const int maxRows = std::max(1, std::min(4, (int)((plotSize.y * 0.45f - top) / (rowH + gap))));
    for (const ga::Label* l : vis) {
        const double mhz = l->d.rfHz / 1e6;
        const float cx = xOf(mhz);
        if (cx < plotMin.x - 40 * gUi || cx > plotMax.x + 40 * gUi) continue;
        const float bx0 = xOf(mhz - l->d.bwHz / 2e6), bx1 = xOf(mhz + l->d.bwHz / 2e6);
        dl->AddRectFilled(ImVec2(bx0, plotMin.y), ImVec2(std::max(bx1, bx0 + 2.f), plotMax.y), confCol(l->confidence, plotId ? 22 : 26));
        char t[160];
        snprintf(t, sizeof t, "%s  %s  %.0f%%", l->d.name.c_str(), bwText(l->d.bwHz).c_str(), l->confidence * 100);
        const bool cd = canOpen(l->d);
        const char* dec = tr("Decode");
        const float tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x, dw = cd ? font->CalcTextSizeA(fs, FLT_MAX, 0, dec).x + 2 * padX : 0, xw = font->CalcTextSizeA(fs, FLT_MAX, 0, "x").x + 2 * padX;
        const float w = padX + tw + padX + (cd ? dw + gap : 0) + xw + gap;
        float x0 = std::min(std::max(cx - w / 2, plotMin.x + 2 * gUi), plotMax.x - rightKeep - w);
        int row = -1;
        for (int r = 0; r < maxRows && row < 0; r++) {
            bool free = true;
            for (const Placed& p : placed) if (p.row == r && x0 < p.x1 + gap && x0 + w > p.x0 - gap) { free = false; break; }
            if (free) row = r;
        }
        if (row < 0) continue;   // no room: it is in the list
        placed.push_back({x0, x0 + w, row});
        const float y0 = plotId ? plotMin.y + top + row * (rowH + gap) : plotMax.y - 10 * gUi - (row + 1) * (rowH + gap);
        const bool hov = plotHover && inRect(mouse, ImVec2(x0, y0), ImVec2(x0 + w, y0 + rowH));
        if (hov) S.hovered = true;
        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + rowH), hov ? IM_COL32(36, 42, 50, 240) : IM_COL32(14, 16, 20, 215), 4 * gUi);
        dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + rowH), confCol(l->confidence, 220), 4 * gUi);
        dl->AddLine(ImVec2(cx, plotId ? y0 + rowH : y0), ImVec2(cx, plotId ? y0 + rowH + 8 * gUi : y0 - 8 * gUi), confCol(l->confidence, 220), 1.5f);
        dl->AddText(font, fs, ImVec2(x0 + padX, y0 + (rowH - fs) / 2), IM_COL32(236, 238, 240, 255), t);
        Chip c{l->id, ImVec2(x0, y0), ImVec2(x0 + w, y0 + rowH), ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), ImVec2(0, 0), cd};
        float bx = x0 + padX + tw + padX;
        if (cd) {
            c.decA = ImVec2(bx, y0 + 2 * gUi); c.decB = ImVec2(bx + dw, y0 + rowH - 2 * gUi);
            const bool h = plotHover && inRect(mouse, c.decA, c.decB);
            dl->AddRectFilled(c.decA, c.decB, ImGui::GetColorU32(h ? pal::accent(0.95f) : pal::accent(0.7f)), 3 * gUi);
            dl->AddText(font, fs, ImVec2(bx + padX, y0 + (rowH - fs) / 2), IM_COL32(255, 255, 255, 255), dec);
            bx += dw + gap;
        }
        c.xA = ImVec2(bx, y0 + 2 * gUi); c.xB = ImVec2(bx + xw, y0 + rowH - 2 * gUi);
        const bool hx = plotHover && inRect(mouse, c.xA, c.xB);
        dl->AddRectFilled(c.xA, c.xB, hx ? IM_COL32(150, 60, 60, 230) : IM_COL32(60, 64, 72, 200), 3 * gUi);
        dl->AddText(font, fs, ImVec2(bx + padX, y0 + (rowH - fs) / 2), IM_COL32(236, 238, 240, 255), "x");
        S.chips[plotId ? 1 : 0].push_back(c);
    }
    dl->PopClipRect();
    // clicks and tooltips after the drawing (they may change the labels)
    int act = 0, actId = 0;
    for (const Chip& c : S.chips[plotId ? 1 : 0]) {
        if (!plotHover || !inRect(mouse, c.a, c.b)) continue;
        if (c.canDecode && inRect(mouse, c.decA, c.decB)) { if (click) { act = 1; actId = c.id; } else ImGui::SetTooltip("%s", tr("Decode this signal: tune to it and open its mode. Stays in General.")); }
        else if (inRect(mouse, c.xA, c.xB)) { if (click) { act = 2; actId = c.id; } else ImGui::SetTooltip("%s", tr("Ignore this label")); }
        else if (const ga::Label* l = S.g.find(c.id)) tooltipFor(*l);
        break;
    }
    if (act == 1) startDecode(a, actId);
    else if (act == 2) S.g.ignore(actId);
    if (S.noteUntil > ImGui::GetTime() && plotId == 0 && !S.note.empty()) {
        const ImVec2 ts = ImGui::CalcTextSize(S.note.c_str());
        const ImVec2 q(plotMin.x + 8 * gUi, plotMin.y + ImGui::GetTextLineHeight() + 12 * gUi);
        ImDrawList* d2 = ImGui::GetWindowDrawList();
        d2->AddRectFilled(ImVec2(q.x - 5 * gUi, q.y - 2 * gUi), ImVec2(q.x + ts.x + 5 * gUi, q.y + ts.y + 2 * gUi), IM_COL32(20, 22, 26, 220), 3 * gUi);
        d2->AddText(q, IM_COL32(150, 215, 160, 255), S.note.c_str());
    }
}

namespace {

void listBody(App& a) {
    bool save = false;
    if (ImGui::Checkbox(tr("Spot signals automatically"), &a.genAutoOn)) save = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("OnAir looks at the spectrum in the background every few seconds, spots signals it knows and labels them. Nothing is decoded until you press Decode."));
    ImGui::BeginDisabled(!a.genAutoOn);
    if (ImGui::Checkbox(tr("Auto-decode when I tune onto a sure signal"), &a.genAutoDecode)) save = true;
    ImGui::Separator();
    const bool decoding = S.g.state() == ga::State::Decoding;
    if (decoding) {
        if (const ga::Label* l = S.g.active()) {
            ImGui::Text(tr("Decoding %s at %s"), l->d.name.c_str(), dect2::specx::freqText(l->d.rfHz).c_str());
            if (ImGui::Button(tr("Back to General"))) handle(a, S.g.stop(ImGui::GetTime()));
        }
    } else {
        std::vector<const ga::Label*> rows;
        for (const ga::Label& l : S.g.labels()) if (l.shown && !l.ignored) rows.push_back(&l);
        std::sort(rows.begin(), rows.end(), [](const ga::Label* x, const ga::Label* y) { return x->inView != y->inView ? x->inView : x->d.rfHz < y->d.rfHz; });
        if (rows.empty()) ImGui::TextDisabled("%s", S.waiting ? tr("Looking...") : tr("No signal spotted yet. Tune around: labels appear on the spectrum."));
        else if (ImGui::BeginTable("##gasig", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            int decodeId = 0, ignoreId = 0, tuneId = 0;
            for (const ga::Label* l : rows) {
                ImGui::PushID(l->id);
                ImGui::TableNextRow();
                if (!l->inView) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::specx::freqText(l->d.rfHz).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s  %s", l->d.name.c_str(), bwText(l->d.bwHz).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.0f%%", l->confidence * 100);
                if (!l->inView) ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                if (canOpen(l->d)) { if (ImGui::SmallButton(tr("Decode"))) decodeId = l->id; ImGui::SameLine(); }
                if (ImGui::SmallButton(tr("Ignore"))) ignoreId = l->id;
                if (!l->inView && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Seen earlier, outside the current view"));
                ImGui::PopID();
            }
            ImGui::EndTable();
            (void)tuneId;
            if (decodeId) startDecode(a, decodeId);
            if (ignoreId) S.g.ignore(ignoreId);
        }
    }
    if (ImGui::TreeNode(tr("Advanced"))) {
        int k = a.genAutoReleaseKhz;
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt(tr("Keep decoding within (kHz)"), &k, 0, 0)) { a.genAutoReleaseKhz = std::max(0, k); save = true; }
        ImGui::TextDisabled("%s", tr("0: automatic, a quarter of the signal's width (at least 25 kHz). Tune further away and OnAir goes back to General."));
        if (S.g.ignoredCount() > 0) { char b[64]; snprintf(b, sizeof b, "%s (%zu)", tr("Show ignored signals again"), S.g.ignoredCount()); if (ImGui::SmallButton(b)) S.g.clearIgnored(); }
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
    if (save) savePrefs(a);
}

} // namespace

float generalAutoButton(App& a, ImVec2 plotMin, ImVec2 plotSize, float rightUsed) {
    if (a.family != 55 && S.g.state() != ga::State::Decoding) return 0;
    char lb[96];
    if (S.g.state() == ga::State::Decoding && S.g.active()) snprintf(lb, sizeof lb, "%s: %s###gabtn", tr("Auto"), S.g.active()->d.name.c_str());
    else if (!a.genAutoOn) snprintf(lb, sizeof lb, "%s###gabtn", tr("Detect: off"));
    else snprintf(lb, sizeof lb, "%s (%zu)###gabtn", tr("Signals"), S.g.visible().size());
    return specCornerPopup(a, lb, "##gapop", plotMin, plotSize, rightUsed, 60 * gUi, 0, a.genAutoOn && !S.g.visible().empty(),
                           tr("Signals OnAir has spotted: Decode one, ignore one, switch Auto-decode on"), listBody);
}
