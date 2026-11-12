// The transmit panel, in the receiver's own style: a top bar with the run button and the big frequency digits (the same widget as the receiver's), then three
// panes: the signal list, the Basic block (state, radio, level, what to send, with Advanced folded below it) and the preview. A narrow window (or the pop-out
// window) stacks them in one scrolling column. Sending is two deliberate steps: ARM, then TRANSMIT (a test or an IQ file needs no arming); a big STOP is always
// there, and Space or Esc stop (and disarm) when no text box has the keyboard. Space never starts anything. Nothing starts by itself.
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

enum class State { Idle, Armed, Preparing, Transmitting, Test, Carrier, Done };

State stateOf(const TxUi& s) {
    if (s.pendingStart || s.pendingCarrier) return State::Preparing;
    if (!s.session.running()) return s.armed ? State::Armed : s.doneShown ? State::Done : State::Idle;
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

// Space / Esc: only when no text box wants the keyboard
void shortcuts(App& a, TxUi& s, const DeviceInfo* radio, int conflict, const TxModeSpec* mode) {
    ImGuiIO& io = ImGui::GetIO();
    gSpaceClaimFrame = ImGui::GetFrameCount();   // Space is the transmitter's here, not the receiver's
    if (io.WantTextInput || ImGui::IsPopupOpen("##txnotice") || ImGui::IsPopupOpen("##freqtype")) return;
    const bool running = s.session.running();
    (void)radio; (void)conflict; (void)mode;
    const bool any = running || s.pendingStart || s.schedArmed || s.armed;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && any) { stopTx(a, s, "(Esc)"); txArm(s, false); }
    else if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && any) { stopTx(a, s, "(Space)"); txArm(s, false); }   // Space stops; it never starts
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
    if (s.wasRunning && !running && !s.pendingStart) {   // it ended by itself: one pass done, the message sent, the timeout, or the radio failed
        s.wasRunning = false;
        const TxSession::Stats st = s.session.stats();
        txSessionEnded(s, st.failed ? "(the radio failed)" : "(finished)");
        s.armed = false;
    }
    if (s.armed && !running && !s.pendingStart && ImGui::GetTime() - s.armedAt > kArmTimeoutSec) { s.armed = false; s.info = tr("Disarmed: nothing was sent within two minutes of arming."); }
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

bool txNeedsArm(const TxUi& s) { return !noRadio(s); }

void txArm(TxUi& s, bool on) {
    s.armed = on;
    if (on) { s.armedAt = ImGui::GetTime(); s.doneShown = false; s.info.clear(); }
}

void txSessionEnded(TxUi& s, const char* why) {
    if (s.current.mode.empty()) return;
    s.current.seconds = std::max(0.0, ImGui::GetTime() - s.startedAt);
    s.current.ended = why && *why == '(' ? std::string(why).substr(1, std::string(why).size() - 2) : (why ? why : "");
    s.history.insert(s.history.begin(), s.current);
    if (s.history.size() > 200) s.history.resize(200);
    s.current = TxUi::Sent();
    s.doneShown = true;
    s.doneWhy = why ? why : "";
    s.doneAt = ImGui::GetTime();
}

namespace {
// a big button of the flow, in its state's colour
bool flowButton(const char* lab, ImVec4 col, ImVec4 text, bool enabled, float w, float hMul = 1.7f) {
    const float h = ImGui::GetFrameHeight() * hMul;
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(1.f, col.x * 1.15f + 0.05f), std::min(1.f, col.y * 1.15f + 0.03f), std::min(1.f, col.z * 1.15f + 0.03f), 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    const bool hit = ImGui::Button(lab, ImVec2(w, h));
    ImGui::PopStyleColor(4);
    ImGui::EndDisabled();
    return hit;
}
const ImVec4 kAmber(0.95f, 0.62f, 0.10f, 1), kOnAir(0.80f, 0.10f, 0.10f, 1), kStopCol(0.70f, 0.06f, 0.06f, 1), kDone(0.25f, 0.62f, 0.35f, 1);
const ImVec4 kDark(0.05f, 0.07f, 0.09f, 1), kWhite(1, 1, 1, 1);

// how long is left: a one-pass transmission, the timeout timer, or a schedule's next start ("" when it runs until stopped)
std::string remainingText(const TxUi& s, const TxModeSpec* mode, bool running) {
    if (!running) return "";
    const double el = ImGui::GetTime() - s.startedAt;
    double left = -1;
    if (!s.loop && !(mode && txModeFinite(*mode))) left = s.onceSeconds - el;
    const auto tot = s.pv.find("tx.totSec");
    if (tot != s.pv.end() && tot->second > 0) left = left < 0 ? tot->second - el : std::min(left, tot->second - el);
    if (mode && txModeFinite(*mode) && left < 0) return tr("until the message is sent");
    if (left < 0) return tr("until stopped");
    return std::string(tr("ends in ")) + elapsedText(std::max(0.0, left));
}
} // namespace

// The state and the flow: the state word in its colour (Idle grey, Armed amber, On air red, Test blue, Done green), the elapsed time and what is left,
// then ARM and TRANSMIT as two separate buttons with a STOP that is always there. A test (no RF) or an IQ file starts with one button.
// In a voice signal "Push to talk" makes TRANSMIT a button that sends only while it is held (once armed).
void drawStatusHeader(App& a, TxUi& s, const DeviceInfo* radio, int conflict, const TxModeSpec* mode) {
    const State st = stateOf(s);
    const bool running = s.session.running();
    const float w = ImGui::GetContentRegionAvail().x;
    const char* word = st == State::Idle ? tr("Idle") : st == State::Armed ? tr("ARMED") : st == State::Preparing ? tr("Preparing")
                     : st == State::Test ? (s.sessionFile ? tr("Writing IQ file") : tr("Test (no RF)")) : st == State::Carrier ? tr("ON AIR: carrier")
                     : st == State::Done ? tr("Done") : tr("ON AIR");
    const ImVec4 col = st == State::Idle ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : st == State::Armed || st == State::Preparing ? kAmber
                     : st == State::Test ? pal::accent() : st == State::Done ? kDone : kRed;
    const bool live = st == State::Transmitting || st == State::Carrier;
    const float pulse = live ? 0.5f + 0.5f * (float)std::sin(ImGui::GetTime() * 5.0) : 1.f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float fh = ImGui::GetFrameHeight(), r = fh * 0.32f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // a band in the state's colour behind the state line, so the state reads from across the room
    if (st != State::Idle) dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + fh + 6 * gUi), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, live ? 0.18f + 0.10f * pulse : 0.12f)), 4 * gUi);
    const ImVec2 dc(p0.x + r + 6 * gUi, p0.y + fh * 0.5f + 3 * gUi);
    if (live) dl->AddCircleFilled(dc, r * (1.5f + 0.5f * pulse), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.25f * pulse)));
    dl->AddCircleFilled(dc, r, ImGui::GetColorU32(col));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 2 * r + 16 * gUi, p0.y + 3 * gUi));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col, "%s", word);
    if (running) {
        ImGui::SameLine(0, 12 * gUi); ImGui::PushFont(a.mono, 0); ImGui::Text("%s", elapsedText(ImGui::GetTime() - s.startedAt).c_str()); ImGui::PopFont();
        const std::string left = remainingText(s, mode, running);
        if (!left.empty()) { ImGui::SameLine(0, 10 * gUi); ImGui::TextDisabled("%s", left.c_str()); }
    } else if (st == State::Armed) {
        ImGui::SameLine(0, 12 * gUi);
        ImGui::TextDisabled(tr("disarms in %d s"), (int)std::max(0.0, kArmTimeoutSec - (ImGui::GetTime() - s.armedAt)));
    } else if (st == State::Done && !s.history.empty()) {
        ImGui::SameLine(0, 12 * gUi);
        ImGui::TextDisabled("%.1f s, %s", s.history.front().seconds, s.history.front().ended.c_str());
    }
    if (mode) { ImGui::SameLine(0, 12 * gUi); ImGui::TextDisabled("%s  %.6f MHz", mode->name, running ? s.freqSent / 1e6 : s.freqMhz); }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + fh + 10 * gUi));

    const bool can = canStart(a, s, radio, conflict, mode);
    const bool needArm = txNeedsArm(s);
    const bool voice = mode && (isVoiceId(mode->id ? mode->id : "") || s.analogVoice);
    const bool busy = running || s.pendingStart || s.schedArmed;
    const float stopW = std::max(90.f * gUi, w * 0.34f), gapW = 6 * gUi, mainW = w - stopW - gapW;
    // the main step: ARM, then TRANSMIT (or HOLD TO TALK); one step for a test or a file
    if (busy && voice && s.pttHold) {   // the push-to-talk button keeps its id (###ptt) while it is held
        flowButton((std::string(tr("TRANSMITTING (release to stop)")) + "###ptt").c_str(), kOnAir, kWhite, true, mainW);
        const bool down = ImGui::IsItemActive();
        if (!down && s.pttDown) stopTx(a, s, "(push to talk released)");
        s.pttDown = down;
    } else if (busy) {
        ImGui::Dummy(ImVec2(mainW, ImGui::GetFrameHeight() * 1.7f));
    } else if (needArm && !s.armed) {
        if (flowButton(tr("ARM"), kAmber, kDark, can && mode, mainW)) txArm(s, true);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", !mode ? tr("Choose a signal in the list.") : !can ? tr("Choose a radio below, or send to a dummy load (test, no RF) or an IQ file.")
                                          : tr("Step 1: arming makes TRANSMIT live. Nothing is sent yet. It disarms itself after two minutes, and on Stop, Space or Esc."));
    } else if (voice && s.pttHold) {
        flowButton((std::string(tr("HOLD TO TRANSMIT")) + "###ptt").c_str(), kOnAir, kWhite, can, mainW);
        const bool down = ImGui::IsItemActive();
        if (down && !s.pttDown && !running) s.pendingStart = true;
        s.pttDown = down;
    } else {
        const char* lab = !needArm ? (s.destKind == 1 ? tr("WRITE THE IQ FILE") : tr("RUN THE TEST (no RF)")) : scheduleConfigured(s) ? tr("TRANSMIT ON SCHEDULE") : tr("TRANSMIT");
        if (flowButton(lab, needArm ? kOnAir : pal::accent(0.85f), needArm ? kWhite : kDark, can, mainW)) s.pendingStart = true;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", needArm ? tr("Step 2: the signal goes out now (or at the scheduled time).") : tr("Makes the signal without a radio: the preview, the meters and the log show what would go out."));
    }
    ImGui::SameLine(0, gapW);
    // STOP: always there; it stops, cancels a schedule or a pending start, and disarms
    const bool stopLive = busy || s.armed;
    if (flowButton(tr("STOP"), stopLive ? kStopCol : ImVec4(0.30f, 0.12f, 0.12f, 1), kWhite, stopLive, stopW)) { stopTx(a, s, "by the user"); txArm(s, false); }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Stops the transmission, a carrier, a pending start and the schedule, and disarms. Space and Esc do the same."));
    if (voice) {
        ImGui::BeginDisabled(running);
        ImGui::Checkbox(tr("Push to talk (hold the button)"), &s.pttHold);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Once armed, the signal goes out only while you hold the button down."));
        ImGui::SameLine();
    }
    ImGui::TextDisabled("%s", tr("Space or Esc = STOP"));
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
        if (runButton(running || s.pendingStart || s.schedArmed, 40 * gUi, running || s.pendingStart || s.schedArmed ? tr("Stop transmitting") : txNeedsArm(s) && !s.armed ? tr("Arm the transmitter (press again to transmit)") : tr("Start transmitting")) && !(s.pttHold)) {
            if (running || s.pendingStart || s.schedArmed) { stopTx(a, s, "by the user"); txArm(s, false); }
            else if (txNeedsArm(s) && !s.armed) txArm(s, true);   // the same two steps as the panel: the first press arms
            else s.pendingStart = true;
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
