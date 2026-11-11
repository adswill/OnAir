// Fleet Broadcast (UHF SATCOM, 250.450 / 250.550 / 250.650 MHz) screens: the carrier, the 1200 Bd clock, the 16-channel activity view, the message log.
// Receive only. Framing and channel activity are shown; nothing is decoded and nothing is decrypted: the channels are usually random-looking
// (encrypted) data, and the public description has no plain text on them. See docs/FBR-3.9.md for what rests on which source.
#include "app.h"
#include "dect2/fbr/fbr_rx.h"
#include "dect2/fbr/fbr_tel.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 channels, 1 messages, 2 link, 3 notes
    bool loaded = false;
    bool invert = false;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.invert = plat::prefs().getI("fbrInvert", 0) != 0;
}

bool live(const App& a) { return a.rx.standard == 871; }   // the engine reports its standard code minus one

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void receiveNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Receive only. Fleet Broadcast is a 16 channel, 75 Bd multiplex sent as 1200 Bd differential BPSK from UHF military SATCOM satellites. Channel 0 is for synchronisation; the other channels are usually random-looking, encrypted data. This screen finds the carrier, the clock and the frame and shows what each channel looks like. Nothing is decoded and nothing is decrypted."));
    ImGui::PopTextWrapPos();
}

ImU32 classColour(char c) {
    switch (c) {
    case 'S': return IM_COL32(240, 200, 80, 255);
    case 'r': return IM_COL32(110, 170, 240, 255);
    case 'i': return IM_COL32(120, 130, 140, 255);
    case 'e': return IM_COL32(220, 110, 110, 255);
    default: return IM_COL32(60, 66, 74, 255);
    }
}

const char* classText(char c) {
    switch (c) {
    case 'S': return tr("sync pattern (repeats)");
    case 'r': return tr("reversals 1010...");
    case 'i': return tr("idle (constant)");
    case 'e': return tr("random-looking, probably encrypted: not decoded");
    default: return tr("not enough data");
    }
}

// the 16 channels of the last frames as a bit matrix: one row per frame, channel 0 on the left
void plotMatrix(const dect2::FbrTelemetry& t) {
    const float W = std::max(220.f, std::min(ImGui::GetContentRegionAvail().x, 520.f * gUi));
    const float cw = W / 16.f, ch = 7.f * gUi;
    const float H = ch * (float)dect2::kFbrFramesShown;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + H), IM_COL32(16, 22, 30, 255));
    const size_t rows = t.frameLog.size();
    for (size_t r = 0; r < rows; r++) {
        const unsigned w = (unsigned)std::strtoul(t.frameLog[r].c_str(), nullptr, 16);
        const float y = p0.y + H - ch * (float)(rows - r);
        for (int c = 0; c < 16; c++) {
            if (!((w >> (15 - c)) & 1)) continue;
            dl->AddRectFilled(ImVec2(p0.x + c * cw + 1, y + 1), ImVec2(p0.x + (c + 1) * cw - 1, y + ch - 1), classColour(t.ch[c].cls));
        }
    }
    for (int c = 0; c <= 16; c++) dl->AddLine(ImVec2(p0.x + c * cw, p0.y), ImVec2(p0.x + c * cw, p0.y + H), IM_COL32(50, 58, 68, 255));
    ImGui::Dummy(ImVec2(W, H));
    ImGui::TextDisabled("%s", tr("One row per frame, newest at the bottom, one column per channel (0 on the left); a filled cell is a 1 bit."));
}

void channelsView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to look for a Fleet Broadcast carrier")); return; }
    const dect2::FbrTelemetry& t = a.rx.fbr;
    if (ImGui::Button(tr("Clear##fbr"))) a.engine.fbr().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Forget the log and the symbol count"));
    ImGui::SameLine(0, 8 * gUi);
    ImGui::TextDisabled("%s", dect2::fbrSummary(t).c_str());
    ImGui::Spacing();
    if (t.state < 2) { ImGui::TextDisabled("%s", tr("no bits yet: waiting for a BPSK carrier with a 1200 Bd clock")); return; }
    if (!t.framed) {
        ImGui::TextWrapped("%s", t.ambiguous ? tr("Several columns of the 16-bit frame repeat, so the frame start cannot be told. The columns are shown as they come.")
                                              : tr("Bits are flowing but no column repeats yet (the sync pattern of channel 0 is not published: a repeat of 3 to 64 frames is looked for). The columns are shown as they come."));
    }
    kvRow(a, tr("channels"), "%s", t.status.c_str());
    ImGui::Spacing();
    if (ImGui::BeginTable("##fbrch", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(t.framed ? tr("Ch") : tr("Col")); ImGui::TableSetupColumn(tr("Looks like"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Changes")); ImGui::TableSetupColumn(tr("Ones")); ImGui::TableSetupColumn(tr("Last 64 bits (hex)"));
        ImGui::TableHeadersRow();
        for (int c = 0; c < dect2::kFbrChannels; c++) {
            const dect2::FbrChannel& ch = t.ch[c];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(classColour(ch.cls)), "%d", c);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(classText(ch.cls));
            if (ch.cls == 'S') { ImGui::SameLine(); ImGui::TextDisabled("%d %s", ch.period, tr("frames")); }
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", ch.activity * 100);
            ImGui::TableNextColumn(); ImGui::Text("%.0f%%", ch.ones * 100);
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(ch.hex.c_str()); ImGui::PopFont();
        }
        ImGui::EndTable();
    }
    if (t.framed) {
        ImGui::Spacing();
        plotMatrix(t);
    }
}

void messagesView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to look for a Fleet Broadcast carrier")); return; }
    const dect2::FbrTelemetry& t = a.rx.fbr;
    ImGui::TextDisabled("%s", tr("Log"));
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0);
    if (t.log.empty()) ImGui::TextDisabled("-");
    for (const std::string& l : t.log) ImGui::TextUnformatted(l.c_str());
    ImGui::PopTextWrapPos(); ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Frames as 16 bit words (hex, channel 0 is the top bit, newest last)"));
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0);
    std::string all;
    for (const std::string& f : t.frameLog) all += f + " ";
    ImGui::TextUnformatted(all.empty() ? "-" : all.c_str());
    ImGui::PopTextWrapPos(); ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("No plain text is expected here: the public description has never seen any on Fleet Broadcast. A random-looking channel is shown as hex and left alone."));
}

void linkView(App& a) {
    receiveNote();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("start the receiver to look for a Fleet Broadcast carrier")); return; }
    const dect2::FbrTelemetry& t = a.rx.fbr;
    ImGui::TextDisabled("%s", dect2::fbrSummary(t).c_str());
    if (t.state == 0) { ImGui::TextDisabled("%s", tr("no BPSK carrier heard yet")); return; }
    kvRow(a, tr("carrier"), "%+.1f Hz from the tuned frequency%s%s", t.offsetHz, t.freqMhz > 0 ? ", " : "", t.freqMhz > 0 ? (std::to_string(t.freqMhz).substr(0, 10) + " MHz").c_str() : "");
    if (!t.carrier.empty()) kvRow(a, tr("listed carrier"), "%s", t.carrier.c_str());
    kvRow(a, tr("acquisition"), "%.0f dB over the floor (the carrier is found as a line at twice its frequency in the squared signal)", t.toneDb);
    kvRow(a, tr("input"), "%.0f ksps, %.1f dBFS", t.inputRate / 1e3, t.levelDb);
    if (t.state < 2) return;
    kvRow(a, tr("symbol clock"), "%.0f Bd (published), %+.0f ppm measured, %llu symbols", t.baud, t.clockPpm, (unsigned long long)t.symbols);
    kvRow(a, tr("decision quality"), "%.1f dB symbol to noise, quadrature share %.2f", t.snrDb, t.eye);
    kvRow(a, tr("polarity"), "%s", t.inverted ? "inverted (phase change = 0)" : "phase change = 1");
    kvRow(a, tr("frames"), "%llu since the alignment, %llu alignments lost", (unsigned long long)t.frames, (unsigned long long)t.blocksBad);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Last bits as received (the polarity is not published)"));
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(t.bits.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void notesView() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextWrapped("%s", tr("What each part rests on:"));
    ImGui::BulletText("%s", tr("Carriers 250.450, 250.550 and 250.650 MHz; 16 channel, 75 Bd multiplex; differential BPSK at 1200 Bd; channel 0 for synchronisation; the other channels usually random-looking, encrypted data, sometimes KG-84; no plain text ever seen: the description on r00t.cz (Fleet Broadcast Decoder v0.5, 2009)."));
    ImGui::BulletText("%s", tr("16 channels of 75 Bd make 1200 Bd: a frame is 16 bits, one bit per channel. A US Navy FLTSATCOM shore-terminal description also gives 75 bit/s subchannels sent time-division multiplexed at 1200 bit/s."));
    ImGui::BulletText("%s", tr("The software on that page is personal-use, binary only: none of it is used. Everything here is written from the description."));
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr("Not published, so not claimed: the sync pattern of channel 0, the pulse shape, the bit polarity, any message framing inside a channel, which satellite uses which carrier. The frame start is found as the one column of the 16-bit frame that repeats (3 to 64 frames) and taken for channel 0: that is an assumption, not a check against the real sync word. Use the polarity switch if the idle channels look swapped."));
    ImGui::Spacing();
    ImGui::TextWrapped("%s", tr("Checked only on simulated signals built from the description (the test sync pattern is our own 31-bit sequence). No real recording was available. Nothing is decrypted and nothing is transmitted. Know your local rules before receiving."));
    ImGui::PopTextWrapPos();
}

void tab(App& a) {
    subNav("fbrv", S.view, {tr("Channels"), tr("Messages"), tr("Link"), tr("Notes")});
    if (S.view == 0) channelsView(a); else if (S.view == 1) messagesView(a); else if (S.view == 2) linkView(a); else notesView();
}

void list(App& a) {
    ImGui::TextDisabled("%s", tr("Carriers (click to tune)"));
    int n = 0;
    const dect2::fbr::Carrier* cs = dect2::fbr::carriers(n);
    if (!ImGui::BeginTable("##fbrcar", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 0))) return;
    ImGui::TableSetupColumn(tr("Carrier"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("MHz");
    for (int i = 0; i < n; i++) {
        ImGui::TableNextRow();
        ImGui::PushID(i);
        ImGui::TableNextColumn();
        if (ImGui::Selectable(cs[i].name, std::fabs(a.freqMhz - cs[i].mhz) < 0.0005, ImGuiSelectableFlags_SpanAllColumns)) {
            a.freqMhz = cs[i].mhz;
            a.tune.centerHz = cs[i].mhz * 1e6;
            a.engine.retune(a.tune);
        }
        ImGui::TableNextColumn(); ImGui::TextDisabled("%.3f", cs[i].mhz);
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (live(a)) {
        ImGui::Spacing();
        const dect2::FbrTelemetry& t = a.rx.fbr;
        ImGui::TextDisabled("%s", tr("Channels"));
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(t.status.empty() ? "-" : t.status.c_str()); ImGui::PopFont();
    }
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::FbrTelemetry& t = a.rx.fbr;
    kvRow(a, tr("state"), "%s", dect2::fbrSummary(t).c_str());
    if (t.centerMhz > 0) kvRow(a, tr("tuned"), "%.4f MHz", t.centerMhz);
    kvRow(a, tr("input"), "%.0f ksps, %.1f dBFS", t.inputRate / 1e3, t.levelDb);
    kvRow(a, tr("frames"), "%llu since the alignment, %llu alignments lost", (unsigned long long)t.frames, (unsigned long long)t.blocksBad);
    kvRow(a, tr("signal time"), "%.1f s", t.timeSec);
    ImGui::Spacing();
    receiveNote();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Carrier"), !on ? 0 : a.rx.fbr.state >= 1 ? 1 : 0); flowNext(6 * gUi);
    lamp(tr("Bits"), !on ? 0 : a.rx.fbr.state >= 2 ? 1 : 0); flowNext(6 * gUi);
    lamp(tr("Frame"), !on ? 0 : a.rx.fbr.state >= 3 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::fbrSummary(a.rx.fbr).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Fleet Broadcast (UHF SATCOM)";
    if (live(a)) l2 = dect2::fbrSummary(a.rx.fbr);
}

void tick(App& a) {
    loadState();
    if (a.engine.running()) a.engine.fbr().setInvert(S.invert);
}

void tuner(App& a, bool& retune) {
    loadState();
    int n = 0;
    const dect2::fbr::Carrier* cs = dect2::fbr::carriers(n);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Carrier"));
    ImGui::SameLine(0, 5 * gUi);
    std::string cur = dect2::fbr::carrierNear(a.freqMhz, 1);
    ImGui::SetNextItemWidth(std::min(120 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##fbrcarrier", cur.empty() ? tr("other") : cur.c_str())) {
        for (int i = 0; i < n; i++) if (ImGui::Selectable(cs[i].name, cur == cs[i].name)) { tuneFreq(a, cs[i].mhz); retune = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The three Fleet Broadcast carriers listed on r00t.cz. The satellite's Doppler shift of a few kHz is searched around the tuned frequency."));
    flowNext(10 * gUi);
    if (ImGui::Checkbox(tr("Invert bits##fbr"), &S.invert)) {
        plat::prefs().setI("fbrInvert", S.invert ? 1 : 0); plat::prefs().flush(); a.engine.fbr().setInvert(S.invert);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The bit polarity is not published. Switch it when the idle channels look swapped."));
    flowEnd();
}

} // namespace

extern const ModeUi kFbrUi;
const ModeUi kFbrUi = {
    .sideTitle = "CARRIERS",
    .tabName = "Fleet Broadcast",
    .tabIcon = Ic::Signal,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
