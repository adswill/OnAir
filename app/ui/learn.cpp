// The hands-on lessons (a spotlight on the real control and a card that checks the step), the Lessons list, the concepts glossary with
// small pictures, and the checkup. The lesson table, the checks, the progress and the checkup rules are in learn_logic.h (tested).
// Nothing here blocks the window: the dimming is drawn on top and takes no clicks, so the person uses the real controls.
#include "../app.h"
#include "learn_logic.h"
#include "tour_logic.h"
#include "autogain_iface.h"
#include "dect2/audio/audio_rec.h"
#include "dect2/engine/mode_tuning.h"
#include <cmath>

namespace {

struct Run {
    bool on = false;
    std::vector<int> plan;     // lesson indexes
    int pos = 0;               // in plan
    learn::Observed start;     // when the lesson opened
    bool passed = false;
    bool autogainRan = false;
    double t0 = 0;             // when the lesson opened
    std::string doneMsg;
};
Run R;
learn::Progress gProg;
bool gProgLoaded = false;
char gQuery[64] = "";

learn::Progress& progress() {
    if (!gProgLoaded) { gProg = learn::decode(plat::prefs().getS(learn::prefProgress(), "")); gProgLoaded = true; }
    return gProg;
}
void setState(const char* id, int st) {
    progress()[id] = st;
    plat::prefs().setS(learn::prefProgress(), learn::encode(progress()));
    plat::prefs().flush();
}

const learn::Region& region() { return learn::regions(nullptr)[learn::clampRegion(plat::prefs().getI("setupRegion", 0))]; }

std::string modeIdOf(const App& a) {
    int n = 0;
    const SubMode* t = subModes(n);
    if (a.sub >= 1 && a.sub <= n && t[a.sub - 1].stdMode == a.family + 2) return t[a.sub - 1].id;
    switch (a.family) {
    case 5: return "fm";
    case 2: return "dab";
    case 0: case 1: case 3: case 4: case 7: return "tv";
    default: break;
    }
    if (const ModeTuning* mt = modeTuning(a.family + 2)) return mt->id;
    return std::to_string(a.family);
}

learn::Observed observe(const App& a) {
    learn::Observed o;
    o.freqMhz = a.freqMhz;
    o.modeId = modeIdOf(a);
    o.agcOn = a.agcOn;
    o.autogainRan = R.autogainRan;
    o.gainDb = a.tune.gainDb + 1000.0 * a.tune.lnaDb + 100.0 * a.tune.vgaDb + (a.tune.ampOn ? 0.5 : 0.0);
    o.volume = a.volume;
    o.muted = a.muted;
    o.running = a.engine.running();
    o.recording = dect2::AudioRecorder::instance().stats().active || a.engine.recordingStats().active;
    auto& p = plat::prefs();
    o.squelch = (p.getB("genSqOn", true) ? 1000.0 : 0.0) + (p.getB("genSqAuto", true) ? 500.0 : 0.0) + p.getD("genSqDb", -60);
    char k[24];
    snprintf(k, sizeof k, "genWidth%ld", p.getI("genDemod", 0));
    o.bandwidthHz = p.getD(k, 0);
    o.windows = (a.mapOpen ? learn::WinMap : 0) | (a.passesOpen ? learn::WinPasses : 0) | (a.sweepOpen ? learn::WinSweep : 0) | (a.glossaryOpen ? learn::WinGlossary : 0);
    return o;
}

const learn::Lesson* current() {
    if (!R.on || R.pos < 0 || R.pos >= (int)R.plan.size()) return nullptr;
    return &learn::lessons(nullptr)[R.plan[R.pos]];
}

void enter(App& a, int pos) {
    R.pos = pos;
    R.passed = false;
    R.autogainRan = false;
    R.doneMsg.clear();
    R.start = observe(a);
    R.t0 = ImGui::GetTime();
    if (const learn::Lesson* l = current()) if (learn::stateOf(progress(), l->id) == learn::Done) R.passed = true;   // done before: shown as done
}

void advance(App& a) {
    for (int k = R.pos + 1; k < (int)R.plan.size(); k++) { enter(a, k); return; }
    R.on = false;
    a.checkupOpen = true;   // the end of the lessons: what works and what to improve
}

void doHelp(App& a, const learn::Lesson& l) {
    switch (l.help) {
    case learn::Help::StartRadio: if (!a.engine.running()) startReceiver(a); break;
    case learn::Help::SetMode: if (l.modeId) selectModeById(a, l.modeId); if (l.check == learn::Check::TuneTo && l.target > 0) tuneFreq(a, l.target); break;
    case learn::Help::TuneTarget: tuneFreq(a, learn::lessonTarget(l, region().fmMhz)); break;
    case learn::Help::AutoGain: R.autogainRan = autogainRun(a, a.devIdx, modeIdOf(a)); break;
    case learn::Help::OpenMap: a.mapOpen = true; break;
    case learn::Help::OpenPasses: a.passesOpen = true; break;
    case learn::Help::OpenSweep: a.sweepOpen = true; break;
    case learn::Help::None: break;
    }
}

// the window dimmed except `lo..hi` and the card `c0..c1`
void dimAround(ImDrawList* fg, ImVec2 disp, ImVec2 lo, ImVec2 hi, ImVec2 c0, ImVec2 c1) {
    const ImU32 dim = IM_COL32(6, 8, 10, 120);
    auto band = [&](float x0, float y0, float x1, float y1) {
        if (x1 <= x0 || y1 <= y0) return;
        const float ix0 = std::max(x0, c0.x), iy0 = std::max(y0, c0.y), ix1 = std::min(x1, c1.x), iy1 = std::min(y1, c1.y);
        if (ix0 >= ix1 || iy0 >= iy1) { fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), dim); return; }
        fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, iy0), dim);
        fg->AddRectFilled(ImVec2(x0, iy1), ImVec2(x1, y1), dim);
        fg->AddRectFilled(ImVec2(x0, iy0), ImVec2(ix0, iy1), dim);
        fg->AddRectFilled(ImVec2(ix1, iy0), ImVec2(x1, iy1), dim);
    };
    band(0, 0, disp.x, lo.y);
    band(0, hi.y, disp.x, disp.y);
    band(0, lo.y, lo.x, hi.y);
    band(hi.x, lo.y, disp.x, hi.y);
}

void checkMark(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x - s * 0.3f, c.y + s * 0.7f), col, 2.f);
    dl->AddLine(ImVec2(c.x - s * 0.3f, c.y + s * 0.7f), ImVec2(c.x + s, c.y - s * 0.7f), col, 2.f);
}

void lessonCard(App& a, ImVec2 disp) {
    const learn::Lesson* lp = current();
    if (!lp) { R.on = false; return; }
    const learn::Lesson& l = *lp;
    // the parts reported through the older rectangle table
    auto legacy = [&](const char* id, int tg) { ImVec2 x0, x1; if (!tourAnchor(id, x0, x1) && a.tgMax[tg].x > a.tgMin[tg].x) tourMark(id, a.tgMin[tg], a.tgMax[tg]); };
    legacy("modes", TgSwitch); legacy("spectrum", TgMain); legacy("panel", TgRight);

    const learn::Observed now = observe(a);
    if (!R.passed && ImGui::GetTime() - R.t0 > 0.25 && learn::verify(l, R.start, now, region().fmMhz)) {
        R.passed = true;
        setState(l.id, learn::Done);
        char b[256];
        snprintf(b, sizeof b, tr(l.done), now.freqMhz);   // only the texts with a %.3f use it
        R.doneMsg = b;
    }

    const float u = gUi;
    ImVec2 lo(0, 0), hi(0, 0);
    const bool haveAnchor = l.anchor && *l.anchor && !a.touchUi && tourAnchor(l.anchor, lo, hi);
    if (haveAnchor) { const float pad = 3 * u; lo = ImVec2(std::max(0.f, lo.x - pad), std::max(0.f, lo.y - pad)); hi = ImVec2(std::min(disp.x, hi.x + pad), std::min(disp.y, hi.y + pad)); }
    static float hMeasured = 200.f;
    const float margin = 12 * u, gap = 12 * u;
    const float W = std::min(380.f * u, disp.x - 2 * margin), H = hMeasured;
    ImVec2 p((disp.x - W) * 0.5f, disp.y - H - 3 * margin);   // no part to point at: near the bottom, in the middle
    if (haveAnchor) {
        if (hi.x + gap + W <= disp.x - margin) p = ImVec2(hi.x + gap, lo.y);
        else if (lo.x - gap - W >= margin) p = ImVec2(lo.x - gap - W, lo.y);
        else if (hi.y + gap + H <= disp.y - margin) p = ImVec2((lo.x + hi.x - W) * 0.5f, hi.y + gap);
        else if (lo.y - gap - H >= margin) p = ImVec2((lo.x + hi.x - W) * 0.5f, lo.y - gap - H);
        else p = ImVec2(hi.x - W - gap, hi.y - H - gap);
    }
    p.x = std::max(margin, std::min(p.x, disp.x - W - margin));
    p.y = std::max(margin, std::min(p.y, disp.y - H - margin));
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (haveAnchor) {
        dimAround(fg, disp, lo, hi, p, ImVec2(p.x + W, p.y + H));
        const float pulse = 0.55f + 0.45f * std::sin((float)ImGui::GetTime() * 4.f);
        fg->AddRect(lo, hi, ImGui::ColorConvertFloat4ToU32(R.passed ? pal::okGreen() : pal::accent(pulse)), 3.f, 0, 2.f);
    }

    ImGui::SetNextWindowPos(p);
    ImGui::SetNextWindowSize(ImVec2(W, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.085f, 0.092f, 0.10f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, R.passed ? pal::okGreen() : pal::accent(0.8f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * u, 14 * u));
    ImGui::Begin("##lesson", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    char idx[48];
    snprintf(idx, sizeof idx, tr("Lesson %d of %d"), R.pos + 1, (int)R.plan.size());
    ImGui::TextColored(pal::accent(), "%s", idx);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr(l.goalMask ? "for your goal" : "basics"));
    // a thin progress bar of the plan
    {
        const ImVec2 b0 = ImGui::GetCursorScreenPos();
        const float bw = ImGui::GetContentRegionAvail().x, f = R.plan.empty() ? 0.f : (float)learn::doneCount(R.plan, progress()) / R.plan.size();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + 3 * u), IM_COL32(255, 255, 255, 30), 2.f);
        dl->AddRectFilled(b0, ImVec2(b0.x + bw * f, b0.y + 3 * u), ImGui::ColorConvertFloat4ToU32(pal::okGreen()), 2.f);
        ImGui::Dummy(ImVec2(bw, 6 * u));
    }
    ImGui::TextUnformatted(tr(l.title));
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 16 * u);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.76f, 0.80f, 0.84f, 1.f));
    ImGui::TextUnformatted(tr(l.text));
    ImGui::PopStyleColor();
    if (l.check == learn::Check::TuneTo) ImGui::TextColored(pal::accent(), tr("Target: %.3f MHz (now %.3f MHz)"), learn::lessonTarget(l, region().fmMhz), now.freqMhz);
    else if (l.check == learn::Check::TuneInBand) ImGui::TextColored(pal::accent(), tr("Target: %.1f to %.1f MHz (now %.3f MHz)"), l.lo, l.hi, now.freqMhz);
    if (!haveAnchor && l.anchor && *l.anchor && !a.touchUi) ImGui::TextDisabled("%s", tr("That part is not in this layout: View > Layout > Sidebar shows it."));
    ImGui::Spacing();
    if (R.passed) {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        checkMark(ImGui::GetWindowDrawList(), ImVec2(c.x + 7 * u, c.y + ImGui::GetTextLineHeight() * 0.5f), 5 * u, ImGui::ColorConvertFloat4ToU32(pal::okGreen()));
        ImGui::Dummy(ImVec2(18 * u, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextColored(pal::okGreen(), "%s", R.doneMsg.empty() ? tr("Done.") : R.doneMsg.c_str());
    } else if (l.check != learn::Check::Manual) {
        ImGui::TextDisabled("%s", tr("Waiting for you to do it..."));
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    const float bh = a.touchUi ? 40.f * u : 0.f;
    bool goNext = false, goBack = false, skip = false, close = false;
    if (!R.passed && l.help != learn::Help::None && l.helpLabel) {
        if (ImGui::Button(tr(l.helpLabel), ImVec2(0, bh))) doHelp(a, l);
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(R.pos == 0);
    if (ImGui::Button(tr("Back"), ImVec2(0, bh))) goBack = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(R.passed || l.check == learn::Check::Manual ? 0.55f : 0.2f));
    const bool last = R.pos + 1 >= (int)R.plan.size();
    if (R.passed) { if (ImGui::Button(tr(last ? "Finish" : "Next"), ImVec2(0, bh))) goNext = true; }
    else if (l.check == learn::Check::Manual) { if (ImGui::Button(tr("Got it"), ImVec2(0, bh))) { setState(l.id, learn::Done); goNext = true; } }
    else if (ImGui::Button(tr("Skip"), ImVec2(0, bh))) skip = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 12 * u);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(tr("Pause"), ImVec2(0, bh))) close = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Close the lessons. Help > Setup and lessons > Continue the lessons comes back here."));
    ImGui::PopStyleColor();
    hMeasured = ImGui::GetWindowHeight();
    ImGui::End();

    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) && (R.passed || l.check == learn::Check::Manual)) { if (!R.passed) setState(l.id, learn::Done); goNext = true; }
    }
    if (close) { R.on = false; return; }
    if (skip) { setState(l.id, learn::Skipped); advance(a); return; }
    if (goBack && R.pos > 0) enter(a, R.pos - 1);
    else if (goNext) advance(a);
}

// ---- glossary pictures: small sketches in a w x h box ----
void picture(learn::Pic pic, ImVec2 o, float w, float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 frame = IM_COL32(255, 255, 255, 30), ink = ImGui::ColorConvertFloat4ToU32(pal::accent()), dimc = IM_COL32(170, 180, 190, 160),
                warm = ImGui::ColorConvertFloat4ToU32(pal::warnAmber()), good = ImGui::ColorConvertFloat4ToU32(pal::okGreen());
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(0, 0, 0, 70), 3.f);
    dl->AddRect(o, ImVec2(o.x + w, o.y + h), frame, 3.f);
    const float floorY = o.y + h * 0.78f;
    auto noise = [&](float x0, float x1, float y, float amp, ImU32 c) {   // a deterministic grainy line
        ImVec2 prev(x0, y);
        for (float x = x0 + 2; x <= x1; x += 2) {
            const float v = std::sin(x * 1.7f) * std::sin(x * 0.37f + 1.f) * amp;
            const ImVec2 q(x, y + v);
            dl->AddLine(prev, q, c);
            prev = q;
        }
    };
    auto bump = [&](float cx, float half, float top, ImU32 c) {   // a signal: a smooth hill above the floor
        ImVec2 prev(cx - half * 1.6f, floorY);
        for (int i = 0; i <= 24; i++) {
            const float t = -1.6f + 3.2f * i / 24.f;
            const ImVec2 q(cx + t * half, floorY - (floorY - top) * std::exp(-t * t * 2.f));
            dl->AddLine(prev, q, c, 1.5f);
            prev = q;
        }
    };
    const float u = gUi;
    switch (pic) {
    case learn::Pic::Snr:
        noise(o.x + 4, o.x + w - 4, floorY, 2.f * u, dimc);
        bump(o.x + w * 0.55f, w * 0.06f, o.y + h * 0.2f, ink);
        dl->AddLine(ImVec2(o.x + w * 0.78f, o.y + h * 0.2f), ImVec2(o.x + w * 0.78f, floorY), warm, 1.5f);
        dl->AddLine(ImVec2(o.x + w * 0.62f, o.y + h * 0.2f), ImVec2(o.x + w * 0.82f, o.y + h * 0.2f), warm);
        dl->AddText(ImVec2(o.x + w * 0.80f, o.y + h * 0.4f), warm, "SNR");
        break;
    case learn::Pic::Gain: {
        const float y = o.y + h * 0.5f, x0 = o.x + 6, x1 = o.x + w - 6;
        dl->AddLine(ImVec2(x0, o.y + h * 0.15f), ImVec2(x1, o.y + h * 0.15f), IM_COL32(230, 90, 80, 200));   // the limit of the radio
        dl->AddLine(ImVec2(x0, o.y + h * 0.85f), ImVec2(x1, o.y + h * 0.85f), IM_COL32(230, 90, 80, 200));
        ImVec2 prev(x0, y);
        for (float x = x0; x <= x1; x += 1.5f) {
            const float k = (x - x0) / (x1 - x0);   // the gain grows to the right: small, good, clipped
            float v = std::sin(x * 0.25f) * h * (0.08f + 0.6f * k);
            v = std::max(-h * 0.35f, std::min(h * 0.35f, v));
            const ImVec2 q(x, y - v);
            dl->AddLine(prev, q, k < 0.25f ? dimc : k < 0.6f ? good : warm, 1.3f);
            prev = q;
        }
        break;
    }
    case learn::Pic::SampleRate:
        noise(o.x + 4, o.x + w - 4, floorY, 1.5f * u, dimc);
        bump(o.x + w * 0.3f, w * 0.03f, o.y + h * 0.4f, ink);
        bump(o.x + w * 0.65f, w * 0.05f, o.y + h * 0.3f, ink);
        dl->AddLine(ImVec2(o.x + w * 0.15f, o.y + h * 0.12f), ImVec2(o.x + w * 0.85f, o.y + h * 0.12f), warm, 1.5f);
        dl->AddLine(ImVec2(o.x + w * 0.15f, o.y + h * 0.06f), ImVec2(o.x + w * 0.15f, o.y + h * 0.9f), warm);
        dl->AddLine(ImVec2(o.x + w * 0.85f, o.y + h * 0.06f), ImVec2(o.x + w * 0.85f, o.y + h * 0.9f), warm);
        break;
    case learn::Pic::Bandwidth:
        noise(o.x + 4, o.x + w - 4, floorY, 1.5f * u, dimc);
        bump(o.x + w * 0.5f, w * 0.07f, o.y + h * 0.25f, ink);
        dl->AddRectFilled(ImVec2(o.x + w * 0.38f, o.y + h * 0.12f), ImVec2(o.x + w * 0.62f, o.y + h * 0.9f), IM_COL32(90, 200, 130, 40));
        dl->AddRect(ImVec2(o.x + w * 0.38f, o.y + h * 0.12f), ImVec2(o.x + w * 0.62f, o.y + h * 0.9f), good);
        break;
    case learn::Pic::Offset:
        noise(o.x + 4, o.x + w - 4, floorY, 1.5f * u, dimc);
        for (float y = o.y + 4; y < o.y + h - 4; y += 6) dl->AddLine(ImVec2(o.x + w * 0.4f, y), ImVec2(o.x + w * 0.4f, y + 3), warm);
        bump(o.x + w * 0.66f, w * 0.05f, o.y + h * 0.3f, ink);
        dl->AddLine(ImVec2(o.x + w * 0.4f, o.y + h * 0.15f), ImVec2(o.x + w * 0.66f, o.y + h * 0.15f), warm, 1.5f);
        dl->AddTriangleFilled(ImVec2(o.x + w * 0.66f, o.y + h * 0.15f), ImVec2(o.x + w * 0.63f, o.y + h * 0.10f), ImVec2(o.x + w * 0.63f, o.y + h * 0.20f), warm);
        break;
    case learn::Pic::Ppm:
        noise(o.x + 4, o.x + w - 4, floorY, 1.5f * u, dimc);
        for (float y = o.y + 4; y < o.y + h - 4; y += 6) dl->AddLine(ImVec2(o.x + w * 0.45f, y), ImVec2(o.x + w * 0.45f, y + 3), dimc);
        bump(o.x + w * 0.56f, w * 0.05f, o.y + h * 0.3f, warm);
        dl->AddText(ImVec2(o.x + w * 0.70f, o.y + h * 0.2f), warm, "+20 ppm");
        break;
    case learn::Pic::Squelch: {
        const float thr = o.y + h * 0.5f;
        noise(o.x + 4, o.x + w * 0.35f, floorY, 2.f * u, dimc);
        for (int i = 0; i < 12; i++) dl->AddLine(ImVec2(o.x + w * 0.35f + i * w * 0.02f, floorY), ImVec2(o.x + w * 0.35f + i * w * 0.02f, o.y + h * 0.22f + (i % 3) * 3 * u), ink, 1.5f);
        noise(o.x + w * 0.6f, o.x + w - 4, floorY, 2.f * u, dimc);
        dl->AddLine(ImVec2(o.x + 4, thr), ImVec2(o.x + w - 4, thr), warm, 1.5f);
        dl->AddText(ImVec2(o.x + w * 0.62f, thr - ImGui::GetTextLineHeight() - 2), warm, "squelch");
        break;
    }
    case learn::Pic::Modulation: {
        const float yA = o.y + h * 0.28f, yF = o.y + h * 0.72f, x0 = o.x + 26 * u, x1 = o.x + w - 6;
        dl->AddText(ImVec2(o.x + 4, yA - ImGui::GetTextLineHeight() * 0.5f), dimc, "AM");
        dl->AddText(ImVec2(o.x + 4, yF - ImGui::GetTextLineHeight() * 0.5f), dimc, "FM");
        ImVec2 pa(x0, yA), pf(x0, yF);
        float ph = 0;
        for (float x = x0; x <= x1; x += 1.f) {
            const float k = (x - x0) / (x1 - x0) * 6.28f;
            const float env = 0.5f + 0.4f * std::sin(k);
            const ImVec2 qa(x, yA - std::sin(x * 0.9f) * h * 0.18f * env);
            ph += 0.5f + 0.4f * std::sin(k);
            const ImVec2 qf(x, yF - std::sin(ph) * h * 0.12f);
            dl->AddLine(pa, qa, ink); dl->AddLine(pf, qf, good);
            pa = qa; pf = qf;
        }
        break;
    }
    case learn::Pic::Decoder: {
        const float y = o.y + h * 0.5f;
        ImVec2 prev(o.x + 6, y);
        for (float x = o.x + 6; x < o.x + w * 0.32f; x += 1.f) { const ImVec2 q(x, y - std::sin(x * 0.7f) * h * 0.2f); dl->AddLine(prev, q, ink); prev = q; }
        dl->AddRect(ImVec2(o.x + w * 0.38f, o.y + h * 0.25f), ImVec2(o.x + w * 0.62f, o.y + h * 0.75f), warm, 3.f, 0, 1.5f);
        dl->AddLine(ImVec2(o.x + w * 0.32f, y), ImVec2(o.x + w * 0.38f, y), dimc);
        dl->AddLine(ImVec2(o.x + w * 0.62f, y), ImVec2(o.x + w * 0.68f, y), dimc);
        for (int i = 0; i < 3; i++) dl->AddLine(ImVec2(o.x + w * 0.72f, o.y + h * (0.32f + 0.16f * i)), ImVec2(o.x + w * (0.92f - 0.06f * i), o.y + h * (0.32f + 0.16f * i)), good, 2.f);
        break;
    }
    case learn::Pic::Waterfall: {
        const int rows = 10;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < 24; c++) {
                const float x0 = o.x + 4 + c * (w - 8) / 24.f, y0 = o.y + 4 + r * (h - 8) / rows;
                const float n = 0.15f + 0.1f * std::fabs(std::sin(r * 3.1f + c * 1.7f));
                float v = n;
                if (c == 7) v = 0.95f;                         // a steady broadcast
                if (c >= 15 && c <= 16 && r % 4 == 1) v = 0.8f;   // short bursts
                const ImVec4 col(0.1f + 0.9f * v, 0.2f + 0.6f * v, 0.5f - 0.3f * v, 1.f);
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (w - 8) / 24.f, y0 + (h - 8) / rows), ImGui::ColorConvertFloat4ToU32(col));
            }
        }
        break;
    }
    case learn::Pic::None: break;
    }
}

void glossaryWindow(App& a) {
    if (!a.glossaryOpen) return;
    const float u = gUi;
    firstWindowSize(560 * u, 520 * u);
    if (!ImGui::Begin(trw("Concepts"), &a.glossaryOpen)) { ImGui::End(); return; }
    ImGui::TextWrapped("%s", tr("The words you meet on a radio, in plain language. Type to search."));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##gq", tr("Search (for example gain)"), gQuery, sizeof gQuery);
    ImGui::Separator();
    ImGui::BeginChild("##gl");
    int n = 0;
    const learn::Term* t = learn::glossary(&n);
    int shown = 0;
    const float pw = 150 * u, ph = 64 * u;
    for (int i = 0; i < n; i++) {
        const char* word = tr(t[i].word);
        const char* text = tr(t[i].text);
        if (!learn::termMatches(word, text, gQuery) && !learn::termMatches(t[i].word, t[i].text, gQuery)) continue;
        shown++;
        ImGui::PushID(i);
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const bool pic = t[i].pic != learn::Pic::None;
        if (pic) { picture(t[i].pic, o, pw, ph); ImGui::Dummy(ImVec2(pw, ph)); ImGui::SameLine(0, 12 * u); }
        ImGui::BeginGroup();
        ImGui::TextColored(pal::accent(), "%s", word);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PopID();
    }
    if (!shown) ImGui::TextDisabled(tr("Nothing matches \"%s\""), gQuery);
    ImGui::EndChild();
    ImGui::End();
}

void lessonsWindow(App& a) {
    if (!a.lessonsOpen) return;
    const float u = gUi;
    firstWindowSize(520 * u, 500 * u);
    if (!ImGui::Begin(trw("Lessons"), &a.lessonsOpen)) { ImGui::End(); return; }
    const unsigned goals = (unsigned)plat::prefs().getI("setupGoals", 0);
    const std::vector<int> plan = learn::lessonPlan(goals);
    ImGui::TextWrapped("%s", tr("Short hands-on lessons on the real controls. Each one lights up the part to use and notices when you have done it. Skip any you know already."));
    ImGui::Text(tr("%d of %d done"), learn::doneCount(plan, progress()), (int)plan.size());
    ImGui::SameLine();
    if (ImGui::Button(tr(learn::resumeAt(plan, progress()) > 0 ? "Continue" : "Start"))) { a.lessonsOpen = false; lessonsStart(a); }
    ImGui::SameLine();
    if (ImGui::Button(tr("Change my goals"))) setupWizardOpen(a);
    ImGui::SameLine();
    if (ImGui::Button(tr("Forget my progress"))) { progress().clear(); plat::prefs().setS(learn::prefProgress(), ""); plat::prefs().flush(); }
    ImGui::Separator();
    int n = 0;
    const learn::Lesson* L = learn::lessons(&n);
    auto row = [&](int li, int planPos) {
        ImGui::PushID(li);
        const int st = learn::stateOf(progress(), L[li].id);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const float lh = ImGui::GetFrameHeight();
        if (st == learn::Done) checkMark(ImGui::GetWindowDrawList(), ImVec2(c.x + 8 * u, c.y + lh * 0.5f), 5 * u, ImGui::ColorConvertFloat4ToU32(pal::okGreen()));
        else ImGui::GetWindowDrawList()->AddCircle(ImVec2(c.x + 8 * u, c.y + lh * 0.5f), 5 * u, st == learn::Skipped ? IM_COL32(150, 150, 150, 160) : ImGui::ColorConvertFloat4ToU32(pal::accent()), 0, 1.5f);
        ImGui::Dummy(ImVec2(20 * u, lh));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr(L[li].title));
        if (st == learn::Skipped) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("(skipped)")); }
        ImGui::SameLine(ImGui::GetWindowWidth() - 90 * u);
        if (ImGui::SmallButton(tr(st == learn::Done ? "Again" : "Open"))) {
            a.lessonsOpen = false;
            if (planPos >= 0) lessonsStart(a, planPos);
            else { R.plan = {li}; R.on = true; enter(a, 0); }
        }
        ImGui::PopID();
    };
    ImGui::TextColored(pal::accent(), "%s", tr("Basics"));
    for (int k = 0; k < (int)plan.size(); k++) if (!L[plan[k]].goalMask) row(plan[k], k);
    bool header = false;
    for (int k = 0; k < (int)plan.size(); k++) if (L[plan[k]].goalMask) { if (!header) { ImGui::Spacing(); ImGui::TextColored(pal::accent(), "%s", tr("For your goals")); header = true; } row(plan[k], k); }
    header = false;
    for (int i = 0; i < n; i++) {
        if (!L[i].goalMask || std::find(plan.begin(), plan.end(), i) != plan.end()) continue;
        if (!header) { ImGui::Spacing(); ImGui::TextColored(pal::accent(), "%s", tr("More lessons")); header = true; }
        row(i, -1);
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(tr("Concepts (glossary)"))) a.glossaryOpen = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("Checkup"))) a.checkupOpen = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("Window tour"))) { a.lessonsOpen = false; tourStart(a); }
    ImGui::End();
}

bool gRadioWorked = false;

void checkupWindow(App& a) {
    if (a.engine.running() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio() && !gRadioWorked) {
        gRadioWorked = true;
        plat::prefs().setB("radioWorked", true);
    }
    if (!a.checkupOpen) return;
    const float u = gUi;
    ImGui::SetNextWindowSize(ImVec2(520 * u, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.45f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin(trw("Checkup"), &a.checkupOpen, ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    learn::CheckupIn in;
    const bool okDev = a.devIdx >= 0 && a.devIdx < (int)a.devices.size();
    in.radio = okDev && a.devices[a.devIdx].isRadio();
    in.networkRadio = okDev && a.devices[a.devIdx].isNetwork();
    in.radioOpened = gRadioWorked || plat::prefs().getB("radioWorked", false);
    in.startError = a.startErr;
    in.antenna = (int)plat::prefs().getI(learn::prefAntenna(), learn::AntWhip);
    in.goals = (unsigned)plat::prefs().getI("setupGoals", 0);
    in.location = !plat::prefs().getS("setupLoc", "").empty() || plat::prefs().getD("adsbLat", 0) != 0;
    in.agcOn = a.agcOn;
    const std::vector<int> plan = learn::lessonPlan(in.goals);
    in.lessonsDone = learn::doneCount(plan, progress());
    in.lessonsTotal = (int)plan.size();
    in.fmMhz = region().fmMhz;
    ImGui::TextWrapped("%s", tr("What works, and what would make reception better."));
    ImGui::Separator();
    ImGui::PushTextWrapPos(500 * u);
    for (const learn::CheckItem& it : learn::checkup(in)) {
        const ImVec4 col = it.level == learn::Ok ? pal::okGreen() : it.level == learn::Tip ? pal::accent() : pal::warnAmber();
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const float lh = ImGui::GetTextLineHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (it.level == learn::Ok) checkMark(dl, ImVec2(c.x + 7 * u, c.y + lh * 0.5f), 4.5f * u, ImGui::ColorConvertFloat4ToU32(col));
        else if (it.level == learn::Tip) dl->AddCircleFilled(ImVec2(c.x + 7 * u, c.y + lh * 0.5f), 3.5f * u, ImGui::ColorConvertFloat4ToU32(col));
        else dl->AddTriangleFilled(ImVec2(c.x + 7 * u, c.y + 1), ImVec2(c.x + 1, c.y + lh - 1), ImVec2(c.x + 13 * u, c.y + lh - 1), ImGui::ColorConvertFloat4ToU32(col));
        ImGui::Dummy(ImVec2(18 * u, lh));
        ImGui::SameLine();
        ImGui::TextUnformatted(tr(it.text));
    }
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (ImGui::Button(tr("Run the setup again"))) { a.checkupOpen = false; setupWizardOpen(a); }
    ImGui::SameLine();
    if (ImGui::Button(tr("Lessons"))) a.lessonsOpen = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("Close"))) a.checkupOpen = false;
    ImGui::End();
}

}  // namespace

void selectModeById(App& a, const std::string& id) {
    const learn::Region& r = region();
    if (id == "fm") { selectMode(a, 5, nullptr); tuneFreq(a, r.fmMhz); return; }
    if (id == "tv") {
        if (r.tvId) { if (const ModeTuning* mt = modeTuningById(r.tvId)) { selectMode(a, mt->stdMode - 2, nullptr); return; } }
        selectMode(a, r.tvFamily, nullptr);
        if (r.tvFamily == 0) a.stdMode = r.tvStd;
        return;
    }
    int n = 0;
    const SubMode* t = subModes(n);
    for (int i = 0; i < n; i++) if (id == t[i].id) { selectMode(a, t[i].stdMode - 2, t[i].id); return; }
    if (const ModeTuning* mt = modeTuningById(id)) selectMode(a, mt->stdMode - 2, nullptr);
}

void lessonsStart(App& a, int planPos) {
    R.plan = learn::lessonPlan((unsigned)plat::prefs().getI("setupGoals", 0));
    if (R.plan.empty()) return;
    int at = planPos;
    if (at < 0) at = learn::resumeAt(R.plan, progress());
    if (at < 0) at = 0;   // everything done: start again from the top (shown as done)
    at = std::min(at, (int)R.plan.size() - 1);
    a.wizOpen = false;    // one guide at a time
    R.on = true;
    enter(a, at);
}

void lessonsUi(App& a, ImVec2 disp) {
    if (tour::guideInFront(a.setupOpen, a.wizOpen, listenWizardShown(), R.on) == tour::Guide::Lesson) lessonCard(a, disp);   // one guide at a time (#491)
    lessonsWindow(a);
    glossaryWindow(a);
    checkupWindow(a);
}
