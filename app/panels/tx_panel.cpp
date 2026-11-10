// The transmit panel, in the receiver's own style: a top bar with the run button and the big frequency digits (the same widget as the receiver's), then three
// panes: the signal list, the Basic block (state, radio, level, what to send, with Advanced folded below it) and the preview. A narrow window (or the pop-out
// window) stacks them in one scrolling column. Space starts / stops and Esc stops when no text box has the keyboard. Nothing starts by itself.
#include "tx_state.h"
#include "../ui/ui2_internal.h"
#include "../ui/icons.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace txui;

namespace txui {

// An inline message: a bar in the colour of its kind on the left and wrapped text (never a popup)
void banner(Note kind, const char* text) {
    const ImVec4 col = kind == Note::Error ? kRed : kind == Note::Warn ? pal::warnAmber() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const float pad = 6 * gUi, bar = 3 * gUi;
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, std::max(40.f, w - 2 * pad - bar));
    const float h = ts.y + 2 * pad;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = col; bg.w = kind == Note::Info ? 0.07f : 0.14f;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), ImGui::GetColorU32(bg));
    dl->AddRectFilled(p0, ImVec2(p0.x + bar, p0.y + h), ImGui::GetColorU32(col));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + bar + pad, p0.y + pad));
    ImGui::PushTextWrapPos(p0.x + w - pad - ImGui::GetWindowPos().x + ImGui::GetScrollX());
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(w, h));   // an item the size of the box: the window grows to hold it (a bare cursor move would not)
}

} // namespace txui

namespace {

enum class State { Idle, Preparing, Transmitting, Test, Carrier };

State stateOf(const TxUi& s) {
    if (s.pendingStart || s.pendingCarrier) return State::Preparing;
    if (!s.session.running()) return State::Idle;
    const TxSession::Stats st = s.session.stats();
    if (st.samples == 0) return State::Preparing;
    if (s.sessionDry) return State::Test;
    return st.carrierTune ? State::Carrier : State::Transmitting;
}

// the elapsed time as mm:ss (h:mm:ss from an hour)
std::string elapsedText(double sec) {
    const int t = (int)std::max(0.0, sec);
    char b[24];
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(b, sizeof b, "%02d:%02d", t / 60, t % 60);
    return b;
}

// the big button: accent when it starts, red when it stops
bool bigButton(const char* lab, bool stop, bool enabled, float w) {
    const float h = ImGui::GetFrameHeight() * 1.7f;
    ImGui::BeginDisabled(!enabled);
    if (stop) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.10f, 0.10f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.15f, 0.15f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal::accent(1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.07f, 0.09f, 1));
    }
    const bool hit = ImGui::Button(lab, ImVec2(w, h));
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    return hit;
}

// Space / Esc: only when no text box wants the keyboard
void shortcuts(App& a, TxUi& s, const DeviceInfo* radio, int conflict, const TxModeSpec* mode) {
    ImGuiIO& io = ImGui::GetIO();
    gSpaceClaimFrame = ImGui::GetFrameCount();   // Space is the transmitter's here, not the receiver's
    if (io.WantTextInput || ImGui::IsPopupOpen("##txnotice") || ImGui::IsPopupOpen("##freqtype")) return;
    const bool running = s.session.running();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && (running || s.pendingStart || s.schedArmed)) stopTx(a, s, "(Esc)");
    else if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && !s.pttHold) {
        if (running || s.schedArmed) stopTx(a, s, "(Space)");
        else if (canStart(a, s, radio, conflict, mode)) s.pendingStart = true;
    }
}

int gBarFrame = -1;   // the frame the TX top bar was drawn in (the new interface); other interfaces get a one-row bar
struct Ctx { const TxModeSpec* mode; const DeviceInfo* radio; int conflict; };
Ctx frameCtx(App& a, TxUi& s) {
    ensureTxMode(a, s);
    loadPayloadPrefs(s);
    Ctx c{txModeById(s.modeId), nullptr, 0};
    c.radio = bindRadio(a, s, c.conflict);
    if (c.mode && s.pvMode != s.modeId) paramsEnter(s, *c.mode);
    if (s.linkRx && !s.session.running()) s.freqMhz = a.freqMhz;
    return c;
}

// the panel body: state and big button, messages, then the panes. `inWindow`: drawn in the pop-out window
void body(App& a, bool inWindow) {
    TxUi& s = ui();
    const Ctx c = frameCtx(a, s);
    const bool running = s.session.running();
    // Start was pressed last frame: the state showed "Preparing", now the radio is opened (it can take a moment)
    if (s.pendingStart && !running) {
        s.pendingStart = false;
        if (c.mode && canStart(a, s, c.radio, c.conflict, c.mode)) {
            if (scheduleConfigured(s)) scheduleArm(s);   // a start time or a repeat: the schedule starts it (txTick); the press is the operator's decision
            else startTx(a, s, c.radio, c.mode, false);
        }
    }
    if (s.pendingCarrier && !running) { s.pendingCarrier = false; if (c.mode && canStart(a, s, c.radio, c.conflict, c.mode)) startTx(a, s, c.radio, c.mode, true); }
    if (s.devAutoStart && c.mode && !running) { s.devAutoStart = false; s.dryRun = true; s.pendingStart = true; }   // dev flag --txdry (no RF)
    shortcuts(a, s, c.radio, c.conflict, c.mode);

    const float availW = ImGui::GetContentRegionAvail().x;
    const bool wide = availW > 900 * gUi && !inWindow;
    const float gap = 4 * gUi;
    auto basic = [&] {
        drawStatusHeader(a, s, c.radio, c.conflict, c.mode);
        if (!s.error.empty()) banner(Note::Error, s.error.c_str());
        if (!s.info.empty()) banner(Note::Info, s.info.c_str());
        if (const std::string st = s.session.stats().error; running == false && s.session.stats().failed && !st.empty() && st != s.error) banner(Note::Error, st.c_str());
        stepSettings(a, s, c.radio, c.conflict, c.mode, running);
    };
    if (wide) {
        const float h = ImGui::GetContentRegionAvail().y;
        const float sigW = std::max(230.f * gUi, std::min(300.f * gUi, availW * 0.22f));
        const float prevW = std::max(340.f * gUi, std::min(560.f * gUi, availW * 0.36f));
        ui2::beginPane("##txsig", ImVec2(sigW, h));
        stepSignal(a, s, running, 0);
        ui2::endPane();
        ImGui::SameLine(0, gap);
        ui2::beginPane("##txbasic", ImVec2(availW - sigW - prevW - 2 * gap, h));
        basic();
        ui2::endPane();
        ImGui::SameLine(0, gap);
        ui2::beginPane("##txprev", ImVec2(0, h));
        stepSend(a, s, c.radio, c.conflict, c.mode, running);
        ui2::endPane();
    } else {
        ImGui::BeginChild("##txcol", ImVec2(0, 0), false);
        basic();
        ImGui::Spacing();
        std::string sig = std::string(tr("Signal")) + ": " + (c.mode ? c.mode->name : tr("none chosen")) + "###txsigsec";
        if (ImGui::CollapsingHeader(sig.c_str(), c.mode ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) stepSignal(a, s, running, 280 * gUi);
        ImGui::Spacing();
        if (ImGui::CollapsingHeader(trw("Preview"), ImGuiTreeNodeFlags_DefaultOpen)) stepSend(a, s, c.radio, c.conflict, c.mode, running);
        ImGui::EndChild();
    }
    txNoticePopup(s);
}

} // namespace

namespace txui {

// State word and the big button. In a voice signal "Push to talk" turns the button into one that transmits only while it is held.
void drawStatusHeader(App& a, TxUi& s, const DeviceInfo* radio, int conflict, const TxModeSpec* mode) {
    const State st = stateOf(s);
    const bool running = s.session.running();
    const float w = ImGui::GetContentRegionAvail().x;
    const char* word = st == State::Idle ? tr("Idle") : st == State::Preparing ? tr("Preparing") : st == State::Test ? (s.sessionFile ? "Writing IQ file" : tr("Test (no RF)")) : st == State::Carrier ? tr("Carrier tune") : tr("Transmitting");
    const ImVec4 col = st == State::Idle ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : st == State::Preparing ? pal::warnAmber() : st == State::Test ? pal::accent() : kRed;
    const bool live = st == State::Transmitting || st == State::Carrier;
    const float pulse = live ? 0.5f + 0.5f * (float)std::sin(ImGui::GetTime() * 5.0) : 1.f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float fh = ImGui::GetFrameHeight(), r = fh * 0.28f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 dc(p0.x + r + 2 * gUi, p0.y + fh * 0.5f);
    if (live) dl->AddCircleFilled(dc, r * (1.5f + 0.5f * pulse), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.25f * pulse)));
    dl->AddCircleFilled(dc, r, ImGui::GetColorU32(col));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 2 * r + 12 * gUi, p0.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col, "%s", word);
    if (running) { ImGui::SameLine(0, 12 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text("%s", elapsedText(ImGui::GetTime() - s.startedAt).c_str()); ImGui::PopFont(); }
    if (mode) { ImGui::SameLine(0, 12 * gUi); ImGui::TextDisabled("%s  %.6f MHz", mode->name, running ? s.freqSent / 1e6 : s.freqMhz); }

    const bool can = canStart(a, s, radio, conflict, mode);
    const bool voice = mode && (isVoiceId(mode->id ? mode->id : "") || s.analogVoice);
    if (voice && s.pttHold) {   // push to talk: down starts, up stops (one button id throughout, so the hold survives the start)
        const bool on = running || s.pendingStart;
        ImGui::BeginDisabled(!can && !on);
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.10f, 0.10f, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.10f, 0.10f, 1));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, pal::accent(1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.07f, 0.09f, 1));
        }
        std::string lab = std::string(on ? tr("TRANSMITTING (release to stop)") : tr("HOLD TO TRANSMIT")) + "##ptt";
        ImGui::Button(lab.c_str(), ImVec2(w, ImGui::GetFrameHeight() * 1.7f));
        ImGui::PopStyleColor(3);
        const bool down = ImGui::IsItemActive();
        ImGui::EndDisabled();
        if (down && !s.pttDown && !running) s.pendingStart = true;
        if (!down && s.pttDown) stopTx(a, s, "(push to talk released)");
        s.pttDown = down;
    } else if (bigButton(running || s.pendingStart || s.schedArmed ? tr("STOP") : tr("START TRANSMITTING"), running || s.pendingStart || s.schedArmed, can || running || s.pendingStart || s.schedArmed, w)) {
        if (running || s.pendingStart || s.schedArmed) stopTx(a, s, "by the user");
        else s.pendingStart = true;
    }
    if (!running && !can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", mode ? tr("Choose a radio below, or switch on the test (no RF).") : tr("Choose a signal in the list."));
    if (voice) {
        ImGui::BeginDisabled(running);
        ImGui::Checkbox(tr("Push to talk (hold the button)"), &s.pttHold);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("The signal goes out only while you hold the button down."));
    }
    ImGui::TextDisabled("%s", s.pttHold && voice ? tr("Esc = stop") : tr("Space = start / stop     Esc = stop"));
    ImGui::Spacing();
}

} // namespace txui

// The top bar of the TX view: the same bar as the receiver's. The run button, the frequency digits (the receiver's widget), whether the transmit frequency
// is linked to the receiver's, the band presets, the signal and the radio, and on the right the output meter and the level.
void txTopBar(App& a, ImVec2 disp) {
    using namespace ui2;
    TxUi& s = ui();
    gBarFrame = ImGui::GetFrameCount();
    const Ctx c = frameCtx(a, s);
    const bool running = s.session.running();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kRail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * gUi, 4 * gUi));
    ImGui::BeginChild("##txtop", ImVec2(0, 50 * gUi), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    {
        const ImVec2 o = ImGui::GetCursorScreenPos();
        const float W = ImGui::GetContentRegionAvail().x;
        const bool can = canStart(a, s, c.radio, c.conflict, c.mode);
        ImGui::BeginDisabled(!can && !running && !s.pendingStart);
        if (runButton(running || s.pendingStart || s.schedArmed, 40 * gUi, running || s.pendingStart || s.schedArmed ? tr("Stop transmitting") : tr("Start transmitting")) && !(s.pttHold)) {
            if (running || s.pendingStart || s.schedArmed) stopTx(a, s, "by the user"); else s.pendingStart = true;
        }
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ImVec2(o.x + 58 * gUi, o.y));
        const float digitsEnd = freqDigitsFor(a, s.freqMhz, [&s](double v) { s.freqMhz = v; s.linkRx = false; });

        // linked or independent, and the band presets
        float x = digitsEnd + 14 * gUi;
        ImGui::SetCursorScreenPos(ImVec2(x, o.y + 2 * gUi));
        bool clicked = false;
        ImGui::BeginDisabled(running);
        ImGui::PushID("txlink");
        chip(s.linkRx ? tr("TX = RX") : tr("TX independent"), s.linkRx, clicked);
        ImGui::PopID();
        ImGui::EndDisabled();
        if (clicked) { s.linkRx = !s.linkRx; if (s.linkRx) s.freqMhz = a.freqMhz; }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (s.linkRx) ImGui::SetTooltip("%s", tr("The transmit frequency follows the receiver's. Typing a different one here unlinks them. Click to unlink."));
            else ImGui::SetTooltip(tr("The transmit frequency is its own. The receiver is on %.6f MHz. Click to make the transmitter follow it."), a.freqMhz);
        }
        x = ImGui::GetItemRectMax().x + 8 * gUi;
        if (c.mode) {
            for (const auto& m : allModes()) {
                if (txModeForList(m.family, m.subId, s.dvbVariant) != c.mode) continue;
                ImGui::SetCursorScreenPos(ImVec2(x, o.y + 2 * gUi));
                double mhz = 0;
                ImGui::BeginDisabled(running);
                if (freqdbPickMhz(m, 96 * gUi, mhz)) { s.freqMhz = mhz; s.linkRx = false; }
                ImGui::EndDisabled();
                x = ImGui::GetItemRectMax().x + 8 * gUi;
                break;
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(x, o.y + 22 * gUi));
        if (!s.linkRx) ImGui::TextDisabled(tr("RX %.6f MHz"), a.freqMhz);
        else ImGui::TextDisabled("%s", tr("follows the receiver"));

        // right: the output meter and the level
        const float lvW = 230 * gUi, mW = 200 * gUi;
        const float lvX = o.x + W - lvW, mX = lvX - mW - 18 * gUi;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(lvX - 9 * gUi, o.y + 4 * gUi), ImVec2(lvX - 9 * gUi, o.y + 38 * gUi), IM_COL32(255, 255, 255, 28));
        dl->AddLine(ImVec2(mX - 9 * gUi, o.y + 4 * gUi), ImVec2(mX - 9 * gUi, o.y + 38 * gUi), IM_COL32(255, 255, 255, 28));
        if (mX > x + 160 * gUi) {
            const float db = s.meterPeak > 1e-6f ? 20.f * std::log10(s.meterPeak) : -120.f;
            const int n = 22, lit = (int)std::lround(std::min(1.f, std::max(0.f, (db + 60.f) / 60.f)) * n);
            const float sw = (mW - (n - 1) * 2 * gUi) / n, sy = o.y + 20 * gUi, sh = 12 * gUi;
            char lv[32];
            snprintf(lv, sizeof lv, db > -100 ? "%.1f dBFS" : "-", db);
            dl->AddText(ImVec2(mX, o.y), u32(kDim), tr("Output level"));
            dl->AddText(ImVec2(mX + mW - ImGui::CalcTextSize(lv).x, o.y), u32(kText), lv);
            for (int k = 0; k < n; k++) {
                const bool on = k < lit, hot = k >= n - 2;
                const ImU32 col = on ? (hot ? u32(pal::badRed()) : k >= n - 5 ? u32(pal::warnAmber()) : u32(ImVec4(0.62f, 0.70f, 0.64f, 1))) : u32(ImVec4(0.25f, 0.26f, 0.27f, 1));
                dl->AddRectFilled(ImVec2(mX + k * (sw + 2 * gUi), sy), ImVec2(mX + k * (sw + 2 * gUi) + sw, sy + sh), col);
            }
        }
        // the level: the radio's own gain scale
        ImGui::SetCursorScreenPos(ImVec2(lvX, o.y + 8 * gUi));
        const DeviceInfo* radio = c.radio;
        const bool hasRange = radio && radio->txGainMaxDb > radio->txGainMinDb;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("Level"));
        ImGui::SameLine(0, 8 * gUi);
        ImGui::SetNextItemWidth(lvW - 52 * gUi);
        if (hasRange) {
            if (!s.gainSet || s.gainDb < radio->txGainMinDb - 1e-3 || s.gainDb > radio->txGainMaxDb + 1e-3) { s.gainDb = (float)radio->txGainMinDb; s.gainSet = true; }
            ImGui::SliderFloat("##txlvl", &s.gainDb, (float)radio->txGainMinDb, (float)radio->txGainMaxDb, "%.1f dB");
        } else ImGui::InputFloat("##txlvl", &s.gainDb, 0, 0, "%.1f dB");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The radio's output level. Start low."));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetWindowPos().x, ImGui::GetCursorScreenPos().y), ImVec2(ImGui::GetWindowPos().x + disp.x, ImGui::GetCursorScreenPos().y), u32(kEdge));
}

// The classic and touch interfaces have no top bar of their own for the TX view: one row with the frequency, the link to the receiver and the presets
void fallbackBar(App& a) {
    TxUi& s = ui();
    const Ctx c = frameCtx(a, s);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("FREQ"));
    ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::InputDouble("##txfreqc", &s.freqMhz, 0, 0, "%.6f MHz")) s.linkRx = false;
    ImGui::SameLine(0, 10 * gUi);
    bool link = s.linkRx;
    if (ImGui::Checkbox(tr("Follow the receiver"), &link)) { s.linkRx = link; if (link) s.freqMhz = a.freqMhz; }
    if (!s.linkRx) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled(tr("RX %.6f MHz"), a.freqMhz); }
    if (c.mode) for (const auto& m : allModes()) {
        if (txModeForList(m.family, m.subId, s.dvbVariant) != c.mode) continue;
        double mhz = 0;
        ImGui::SameLine(0, 10 * gUi);
        if (freqdbPickMhz(m, 96 * gUi, mhz)) { s.freqMhz = mhz; s.linkRx = false; }
        break;
    }
    ImGui::Separator();
}

// The Transmit panel in the main area; "Pop out" moves it to a floating window of its own (the main area then says so)
void txPanel(App& a) {
    TxUi& s = ui();
    if (s.popped) {
        ImGui::Dummy(ImVec2(1, 8 * gUi));
        ImGui::TextWrapped("%s", tr("The transmit panel is in its own window."));
        if (ImGui::Button(tr("Bring it back here"))) s.popped = false;
        firstWindowSize(560 * gUi, 720 * gUi);
        bool open = true;
        if (ImGui::Begin(trw("Transmit##txpop"), &open, ImGuiWindowFlags_NoSavedSettings)) {
            if (gBarFrame != ImGui::GetFrameCount()) fallbackBar(a);
            body(a, true);
        }
        ImGui::End();
        if (!open) s.popped = false;
        return;
    }
    if (gBarFrame != ImGui::GetFrameCount()) fallbackBar(a);
    const ImVec2 tp = ImGui::GetCursorScreenPos();
    body(a, false);
    // the pop-out button sits in the top right corner of the panel
    const float bw = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(tp.x + ImGui::GetWindowContentRegionMax().x - bw - 2 * gUi, tp.y + 2 * gUi));
    ImGui::PushID("##txpopbtn");
    if (ImGui::InvisibleButton("##p", ImVec2(bw, bw))) s.popped = true;
    const bool hot = ImGui::IsItemHovered();
    icons::draw(Ic::Popout, ImVec2(tp.x + ImGui::GetWindowContentRegionMax().x - bw * 0.5f - 2 * gUi, tp.y + 2 * gUi + bw * 0.5f), bw * 0.6f, ImGui::GetColorU32(hot ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    if (hot) ImGui::SetTooltip("%s", tr("Pop out into a window of its own"));
    ImGui::PopID();
}
