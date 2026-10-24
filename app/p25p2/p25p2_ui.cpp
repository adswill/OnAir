// P25 Phase 2 screens: the two time slots (unit ID, talkgroup, emergency, encryption, voice), the call log with the voice channel grants, the trunking
// system (site, adjacent sites, channel identifiers), the talkgroups, the decoded MAC messages, the "Listen to" slot choice and the descrambler seed
// (WACN / system / NAC of the site, which the P25 trunking follower fills in by itself). Receive only; encrypted calls are only reported. The burst layer
// follows the on-air layout; the MAC PDUs are the program's own reading (core/include/dect2/p25p2/p25p2_proto.h).
#include "app.h"
#include "dect2/codec/codec_registry.h"
#include "../codec/speech_codec_ui.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/p25/p25_proto.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/p25p2/p25p2_tel.h"
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>

namespace {

struct State {
    int view = 0;                        // 0 slots, 1 calls, 2 trunking, 3 talkgroups, 4 messages
    int listen = 0;                      // 0 first heard, 1, 2
    bool wasRunning = false;
    float pushedVol = -1; bool pushedMute = false; int pushedListen = -1;
    uint32_t wacn = 0, sysId = 0, nac = 0;   // the descrambler seed typed in (all 0: not descrambled)
    bool seedDirty = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 181; }   // family 180, engine standard 182, active standard 181

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}
std::string dstOf(const dect2::P25p2Call& c) { return (c.group ? "TG " : "") + dect2::p25Id(c.dst); }
std::string mhz(double hz) { if (hz <= 0) return "-"; char b[32]; snprintf(b, sizeof b, "%.5f MHz", hz / 1e6); return b; }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void tick(App& a) {
    if (live(a)) appcodec::voiceCodecTick("ambe2");   // the user's codec plug-in (docs/CODECS.md), loaded whichever tab is shown
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::P25p2Receiver& r = a.engine.p25p2();
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    if (!S.wasRunning || S.listen != S.pushedListen) { r.setListenSlot(S.listen); S.pushedListen = S.listen; }
    if (!S.wasRunning || S.seedDirty) { r.setSystem((int)S.wacn, (int)S.sysId, (int)S.nac); S.seedDirty = false; }
    S.wasRunning = true;
}

void voiceLine() {
    ImGui::AlignTextToFramePadding();
    std::string lib;
    if (!dect2::codec::pluginLoaded("ambe2")) dect2::nxdnAmbeAvailable(&lib);
    appcodec::voiceCodecLine("ambe2", "AMBE+2", lib);
    ImGui::SameLine(0, 4 * gUi);
    codecHelpButton(CodecHelp::DmrAmbe);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("OnAir only receives. It shows what P25 Phase 2 radios send in the clear: talkgroups, unit IDs, the trunking system and,\n"
                          "with your own AMBE+2 codec plug-in, clear voice. Encrypted calls are only reported as encrypted (from the encryption sync);\n"
                          "nothing is decrypted. A Phase 2 channel is scrambled with its site's WACN, system and NAC: enter them (the Phase 1 control\n"
                          "channel shows them), or let the P25 trunking follower open the channel.\n"
                          "Listening to radio services is regulated in many countries: follow the rules where you live.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Listen to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * gUi);
    const char* ls[3] = {"first heard", "slot 1", "slot 2"};
    if (ImGui::BeginCombo("##p2listen", ls[S.listen])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(ls[i], S.listen == i)) S.listen = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 14 * gUi);
    ImGui::TextUnformatted("WACN / system / NAC");
    auto hexField = [](const char* id, uint32_t* v, uint32_t max, float w) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(w * gUi);
        if (ImGui::InputScalar(id, ImGuiDataType_U32, v, nullptr, nullptr, max > 0xFFF ? "%05X" : "%03X", ImGuiInputTextFlags_CharsHexadecimal)) {
            if (*v > max) *v = max;
            S.seedDirty = true;
        }
    };
    hexField("##p2wacn", &S.wacn, 0xFFFFF, 60);
    hexField("##p2sys", &S.sysId, 0xFFF, 45);
    hexField("##p2nac", &S.nac, 0xFFF, 45);
}

void slotCard(App& a, int i) {
    const dect2::P25p2Telemetry& t = a.rx.p25p2;
    const dect2::P25p2Call& n = t.now[i];
    ImGui::PushID(i);
    ImGui::TextDisabled("Slot %d%s", i + 1, t.audioSlot == i + 1 ? "  (playing)" : "");
    if (!n.active) { ImGui::TextDisabled("idle"); ImGui::PopID(); return; }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextColored(pal::okGreen(), "ON AIR  %s  ->  %s", dect2::p25Id(n.src).c_str(), dstOf(n).c_str());
    ImGui::PopFont();
    kvRow(a, "call", "%s%s", n.group ? "group" : "unit to unit", n.lateEntry ? ", joined late" : "");
    if (n.emergency) { ImGui::TextDisabled("note"); kvColumn(130 * gUi); ImGui::TextColored(pal::warnAmber(), "EMERGENCY"); }
    if (!n.cryptoKnown) { ImGui::TextDisabled("encryption"); kvColumn(130 * gUi); ImGui::TextColored(pal::warnAmber(), "not known yet: no sound until the encryption sync says"); }
    else if (n.encrypted) { ImGui::TextDisabled("encryption"); kvColumn(130 * gUi); ImGui::TextColored(pal::warnAmber(), "encrypted (algorithm 0x%02X): no sound (not decrypted)", n.algId); }
    kvRow(a, "duration", "%.1f s, %u voice frames", n.durSec, n.voiceFrames);
    kvRow(a, "voice", "%s", n.encrypted ? "-" : n.voice ? "playing (AMBE+2)" : t.codec ? "-" : "no codec: frames counted only");
    ImGui::PopID();
}

void nowView(App& a) {
    voiceLine();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to hear P25 Phase 2"); return; }
    const dect2::P25p2Telemetry& t = a.rx.p25p2;
    if (!t.locked) ImGui::TextDisabled("looking for the Phase 2 frame sync (6000 symbols/s, two time slots)");
    slotCard(a, 0);
    ImGui::Spacing();
    slotCard(a, 1);
    ImGui::Spacing();
    ImGui::TextDisabled("Bursts: 4V %llu, 2V %llu, SACCH %llu, FACCH %llu; MAC PDUs %llu good, %llu bad (%llu hexbits corrected)",
                        (unsigned long long)t.v4, (unsigned long long)t.v2, (unsigned long long)t.sacch, (unsigned long long)t.facch,
                        (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.rsCorrected);
}

void callsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the calls"); return; }
    const auto& log = a.rx.p25p2.log;
    if (!ImGui::BeginTable("##p2log", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time UTC"); ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("Slot"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Length");
    ImGui::TableHeadersRow();
    auto row = [&](const dect2::P25p2Call& e, bool on) {
        ImGui::TableNextRow();
        const ImVec4 c = on ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn(); ImGui::TextColored(c, "%s", on ? "on air" : clock(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.kind == 1 ? "grant" : "voice");
        ImGui::TableNextColumn(); if (e.slot) ImGui::Text("%d", e.slot); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", dect2::p25Id(e.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dstOf(e).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        if (e.emergency) { ImGui::TextColored(pal::warnAmber(), "EMERGENCY"); ImGui::SameLine(); }
        if (e.encrypted) { ImGui::TextColored(pal::warnAmber(), "encrypted"); ImGui::SameLine(); }
        if (e.kind == 1) ImGui::Text("%s  %s", dect2::p25::channelText(e.channel).c_str(), mhz(e.freqHz).c_str());
        else ImGui::TextUnformatted(e.voice ? "voice played" : "");
        ImGui::TableNextColumn(); if (e.kind == 0) ImGui::Text("%.1f s", e.durSec); else ImGui::TextDisabled("-");
        if (ImGui::IsItemHovered() && e.kind == 0)
            ImGui::SetTooltip("%u voice frames, %s%s", e.voiceFrames, e.endSeen ? "ended with an END_PTT" : "signal lost", e.lateEntry ? ", joined late" : "");
    };
    for (int i = 0; i < 2; i++) if (a.rx.p25p2.now[i].active) row(a.rx.p25p2.now[i], true);
    for (const auto& e : log) row(e, false);
    ImGui::EndTable();
    if (log.empty()) ImGui::TextDisabled("nothing heard yet");
}

void trunkingView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::P25p2Telemetry& t = a.rx.p25p2;
    const dect2::P25Site& s = t.site;
    if (!s.valid) ImGui::TextDisabled("no system information yet (RFSS / network status messages)");
    else {
        kvRow(a, "WACN / system", "%05X / %03X", s.wacn < 0 ? 0 : s.wacn, s.sysId < 0 ? 0 : s.sysId);
        kvRow(a, "RFSS / site", "%d / %d", s.rfss, s.site);
        kvRow(a, "control channel", "%s  %s", dect2::p25::channelText(s.channel).c_str(), mhz(s.freqHz).c_str());
        kvRow(a, "service class", "0x%02X", s.ssc);
    }
    if (t.nac >= 0) kvRow(a, "colour code", "%03X", t.nac);
    ImGui::Spacing();
    ImGui::TextDisabled("Adjacent sites");
    if (t.adjacent.empty()) ImGui::TextDisabled("  none heard");
    else if (ImGui::BeginTable("##p2adj", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("System"); ImGui::TableSetupColumn("RFSS / site"); ImGui::TableSetupColumn("Control channel");
        ImGui::TableHeadersRow();
        for (const auto& j : t.adjacent) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%03X", j.sysId);
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", j.rfss, j.site);
            ImGui::TableNextColumn(); ImGui::Text("%s  %s", dect2::p25::channelText(j.channel).c_str(), mhz(j.freqHz).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Channel identifiers");
    bool any = false;
    for (int i = 0; i < 16; i++) {
        const dect2::P25IdenRow& r = t.idens[i];
        if (!r.valid) continue;
        any = true;
        ImGui::Text("  %2d  base %.5f MHz, spacing %.2f kHz, transmit offset %+.3f MHz", i, r.baseHz / 1e6, r.spacingHz / 1e3, r.txOffsetHz / 1e6);
    }
    if (!any) ImGui::TextDisabled("  none heard");
}

void talkgroupsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const auto& tgs = a.rx.p25p2.talkgroups;
    if (tgs.empty()) { ImGui::TextDisabled("no talkgroups heard yet"); return; }
    if (!ImGui::BeginTable("##p2tg", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Talkgroup"); ImGui::TableSetupColumn("Calls"); ImGui::TableSetupColumn("Grants"); ImGui::TableSetupColumn("Last unit"); ImGui::TableSetupColumn("Frequency"); ImGui::TableSetupColumn("Flags");
    ImGui::TableHeadersRow();
    for (const auto& g : tgs) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(g.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%d", g.tg); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%u", g.calls);
        ImGui::TableNextColumn(); ImGui::Text("%u", g.grants);
        ImGui::TableNextColumn(); ImGui::Text("%s", dect2::p25Id(g.lastSrc).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(g.lastFreqHz).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%s%s", g.encrypted ? "encrypted " : "", g.emergency ? "emergency" : "");
    }
    ImGui::EndTable();
}

void messagesView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    ImGui::PushFont(a.mono, 0);
    for (const auto& e : a.rx.p25p2.events) ImGui::TextUnformatted(e.c_str());
    ImGui::PopFont();
    if (a.rx.p25p2.events.empty()) ImGui::TextDisabled("no messages decoded yet");
}

void tab(App& a) {
    const char* names[5] = {"Slots", "Calls", "Trunking", "Talkgroups", "Messages"};
    for (int i = 0; i < 5; i++) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(names[i], S.view == i)) S.view = i;
    }
    ImGui::Separator();
    switch (S.view) {
    case 0: nowView(a); break;
    case 1: callsView(a); break;
    case 2: trunkingView(a); break;
    case 3: talkgroupsView(a); break;
    default: messagesView(a); break;
    }
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::P25p2Telemetry& t = a.rx.p25p2;
    kvRow(a, "frame sync", "%s", t.locked ? "locked" : "searching");
    kvRow(a, "syncs", "%llu (%llu unlocks)", (unsigned long long)t.syncs, (unsigned long long)t.unlocks);
    kvRow(a, "carrier offset", "%+.0f Hz", t.cfoHz);
    kvRow(a, "symbol SNR", "%.1f dB", t.snrDb);
    kvRow(a, "slots", "%llu (%llu with an unknown burst type, %llu bad ISCH)", (unsigned long long)t.slots, (unsigned long long)t.slotsBad, (unsigned long long)t.ischBad);
    kvRow(a, "MAC PDUs", "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kvRow(a, "voice frames", "%llu (%llu decoded, %llu encrypted not decoded)", (unsigned long long)t.voiceFrames, (unsigned long long)t.voiceDecoded, (unsigned long long)t.encryptedSkipped);
    if (t.codec) kvRow(a, "codec", "%s", t.codecPath.empty() ? "AMBE+2" : t.codecPath.c_str());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
}

void list(App& a) {
    if (!live(a)) return;
    talkgroupsView(a);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : a.rx.p25p2.locked ? 1 : 0); flowNext(10 * gUi);
    lamp("Slot 1", !on ? 0 : a.rx.p25p2.now[0].active ? 1 : 0); flowNext(10 * gUi);
    lamp("Slot 2", !on ? 0 : a.rx.p25p2.now[1].active ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::p25p2Summary(a.rx.p25p2).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "P25 Phase 2";
    if (live(a)) l2 = dect2::p25p2Summary(a.rx.p25p2);
}

} // namespace

extern const ModeUi kP25p2Ui;
const ModeUi kP25p2Ui = {
    .sideTitle = "TALKGROUPS",
    .tabName = "P25 Phase 2",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
