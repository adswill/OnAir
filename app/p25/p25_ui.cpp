// P25 Phase 1 screens: the current call (unit ID, talkgroup, emergency, encryption, voice), the call log (calls, voice grants, data
// packets), the trunking system (site, adjacent sites, channel identifiers), the talkgroups with their activity, the decoded control
// messages, and a "?" about the mbelib library the voice needs (docs/CODECS.md). Receive only; encrypted calls are only reported.
#include "dect2/common/time_fmt.h"
#include "app.h"
#include "voice/voice_monitor_ui.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/codec/codec_registry.h"
#include "../codec/speech_codec_ui.h"
#include "dect2/p25/p25_imbe.h"
#include "dect2/p25/p25_proto.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25/trunk.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/p25p2/p25p2_tel.h"
#include "dect2/audio/audio_rec.h"
#include "dect2/common/u8file.h"
#include "platform/platform.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <map>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 now, 1 calls, 2 trunking, 3 talkgroups, 4 messages, 5 follow
    bool wasRunning = false;
    float hang = 2.0f;
    float pushedVol = -1; bool pushedMute = false;
};
State S;

// ---------------------------------------------------------------- the trunking follower (core/src/p25/trunk_*.cpp, docs/trunking.md)
struct Trunk {
    dect2::trunk::Controller ctl;
    bool on = false, loaded = false, dirty = false;
    uint64_t tsbkSeen = 0;
    int forcedSel = 0;
    uint64_t recCall = 0;
    std::string status;
    char csvPath[300] = "";
    // the call the radio left the control channel for (Retune with a call): its voice keeps it open, the control channel cannot be heard
    uint64_t hopCall = 0;
    uint64_t hopSeq = 0;
    // a followed P25 Phase 2 call: the Phase 2 receiver on its carrier, as the first mode (the radio hopped) or as the second mode (inside the capture)
    uint64_t p2Call = 0;
    int p2Slot = 0;
    int p2Seed[3] = {0, 0, 0};           // WACN, system, NAC the receiver was seeded with
    bool p2Hop = false, p2Second = false;
    uint64_t p2Seq = 0;
    std::vector<double> wideList;        // the Phase 1 channels the whole-span bank decodes for the follower (whole-span decoding off)
};
Trunk T;

double nowSec() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }
std::string tgFile() { return plat::dataDir() + "/trunk_talkgroups.csv"; }

void tgLoad(const std::string& path, bool merge) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) { T.status = "cannot open " + path; return; }
    std::stringstream ss; ss << f.rdbuf();
    std::string err; int skipped = 0;
    const int n = T.ctl.tgs.importCsv(ss.str(), &err, &skipped, merge);
    T.status = std::to_string(n) + " talkgroups read" + (skipped ? ", " + std::to_string(skipped) + " rows skipped. " + err : std::string());
}
void tgSave(const std::string& path) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (f) f << T.ctl.tgs.exportCsv();
    else T.status = "cannot write " + path;
}

bool live(const App& a) { return a.rx.standard == 51; }   // the engine reports its standard code minus one
constexpr int kP25p2Std = 182;                             // the engine standard of P25 Phase 2 (family 180)

int p25p2ModeIdx() {
    for (int i = 0; i < kNumModes; i++) if (kModes[i].family == kP25p2Std - 2 && !kModes[i].subId) return i;
    return -1;
}

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    return dect2::timefmt::clock(t);
}

std::string dstOf(const dect2::P25Call& c) { return (c.group ? "TG " : "") + dect2::p25Id(c.dst); }
const char* kindOf(const dect2::P25Call& c) { return c.kind == 1 ? "grant" : c.kind == 2 ? "data" : "voice"; }
std::string mhz(double hz) { if (hz <= 0) return "-"; char b[32]; snprintf(b, sizeof b, "%.5f MHz", hz / 1e6); return b; }
std::string cryptoOf(const dect2::P25Call& c) {
    char b[96];
    if (c.algId != dect2::p25::kAlgClear) snprintf(b, sizeof b, "encrypted, %s (0x%02X), key %d", dect2::p25::algName(c.algId), c.algId, c.keyId);
    else snprintf(b, sizeof b, "encrypted");
    return b;
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void voiceLine() {
    ImGui::AlignTextToFramePadding();
    std::string lib;
    if (!dect2::codec::pluginLoaded("imbe")) dect2::p25ImbeAvailable(&lib);
    appcodec::voiceCodecLine("imbe", "IMBE", lib);
    ImGui::SameLine(0, 4 * gUi);
    codecHelpButton(CodecHelp::P25Imbe);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("OnAir only receives. It shows what P25 radios send in the clear: talkgroups, unit IDs, the trunking system and,\n"
                          "with your own mbelib, clear voice. Encrypted calls are only reported as encrypted; nothing is decrypted.\n"
                          "Listening to radio services is regulated in many countries: follow the rules where you live.");
}

void nowView(App& a) {
    voiceLine();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to hear P25"); return; }
    const dect2::P25Telemetry& t = a.rx.p25;
    const dect2::P25Call& n = t.now;
    if (n.wallTime == 0) {
        ImGui::TextDisabled("%s", t.control ? "a control channel: see Calls and Trunking for the voice grants" : "no call heard yet");
        return;
    }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextColored(n.active ? pal::okGreen() : pal::grey(0.6f), "%s  %s  ->  %s", n.active ? "ON AIR" : "last", dect2::p25Id(n.src).c_str(), dstOf(n).c_str());
    ImGui::PopFont();
    kvRow(a, "call", "%s, NAC %03X%s", n.group ? "group" : "unit to unit", n.nac < 0 ? 0 : n.nac, n.lateEntry ? ", joined late" : "");
    if (n.emergency) { ImGui::TextDisabled("note"); kvColumn(130 * gUi); ImGui::TextColored(pal::warnAmber(), "EMERGENCY"); }
    if (n.encrypted) {
        ImGui::TextDisabled("encryption"); kvColumn(130 * gUi);
        if (n.decrypt.empty()) ImGui::TextColored(pal::warnAmber(), "%s: no sound (not decrypted)", cryptoOf(n).c_str());
        else { ImGui::TextUnformatted(cryptoOf(n).c_str()); ImGui::SameLine(); voiceKeysStatus(a, n.decrypt); }   // the key status and the way to the Keys window
    }
    kvRow(a, "duration", "%.1f s, %u voice frames", n.durSec, n.voiceFrames);
    kvRow(a, "started", "%s%s", clock(n.wallTime).c_str(), dect2::timefmt::sfx());
    kvRow(a, "voice", "%s", n.encrypted ? (n.decrypt.compare(0, 9, "decrypted") == 0 ? "playing (decrypted with your key)" : "-") : n.voice ? "playing (IMBE through mbelib)" : t.imbe ? "-" : "no mbelib: frames counted only");
}

void callsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the calls"); return; }
    const auto& log = a.rx.p25.log;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& e : log) {
            char b[400];
            snprintf(b, sizeof b, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%.1f s\n", clock(e.wallTime).c_str(), kindOf(e), dect2::p25Id(e.src).c_str(), dstOf(e).c_str(),
                     e.kind == 1 ? (dect2::p25::channelText(e.channel) + " " + mhz(e.freqHz)).c_str() : "", e.encrypted ? cryptoOf(e).c_str() : "clear", e.text.c_str(), e.durSec);
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##p25log", 7 + (wideActive(a) ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wideActive(a)) ImGui::TableSetupColumn("Carrier"); ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Length");
    ImGui::TableHeadersRow();
    auto row = [&](const dect2::P25Call& e, bool on) {
        ImGui::TableNextRow();
        if (wideActive(a)) { ImGui::TableNextColumn(); wideCarrierCell(a, e.carrierId, e.carrierHz); }
        const ImVec4 c = on ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn(); ImGui::TextColored(c, "%s", on ? "on air" : clock(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(kindOf(e));
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", dect2::p25Id(e.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dstOf(e).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        if (e.kind == 1) ImGui::Text("%s  %s", dect2::p25::channelText(e.channel).c_str(), mhz(e.freqHz).c_str()); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (e.emergency) { ImGui::TextColored(pal::warnAmber(), "EMERGENCY"); ImGui::SameLine(); }
        if (e.encrypted) { ImGui::TextColored(pal::warnAmber(), "%s", cryptoOf(e).c_str()); ImGui::SameLine(); }
        ImGui::TextUnformatted(e.kind == 0 ? (e.voice ? "voice played" : "") : e.text.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", e.durSec);
        if (ImGui::IsItemHovered() && e.kind == 0)
            ImGui::SetTooltip("NAC %03X, %u voice frames, %s%s", e.nac < 0 ? 0 : e.nac, e.voiceFrames, e.endSeen ? "ended with a terminator" : "signal lost", e.lateEntry ? ", joined late" : "");
    };
    if (a.rx.p25.now.active) row(a.rx.p25.now, true);
    for (const auto& e : log) row(e, false);
    ImGui::EndTable();
    if (log.empty() && !a.rx.p25.now.active) ImGui::TextDisabled("nothing heard yet");
}

void trunkingView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::P25Telemetry& t = a.rx.p25;
    const dect2::P25Site& s = t.site;
    if (!s.valid) ImGui::TextDisabled("no system information (a trunked control channel sends it; conventional channels do not)");
    else {
        kvRow(a, "WACN / system", "%05X / %03X", s.wacn < 0 ? 0 : s.wacn, s.sysId < 0 ? 0 : s.sysId);
        kvRow(a, "RFSS / site", "%d / %d", s.rfss, s.site);
        kvRow(a, "NAC", "%03X", s.nac < 0 ? 0 : s.nac);
        kvRow(a, "control channel", "%s  %s", dect2::p25::channelText(s.channel).c_str(), mhz(s.freqHz).c_str());
        kvRow(a, "service class", "0x%02X", s.ssc);
        kvRow(a, "heard", "%s%s", clock(s.wallTime).c_str(), dect2::timefmt::sfx());
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Adjacent sites");
    if (t.adjacent.empty()) ImGui::TextDisabled("  none heard");
    else if (ImGui::BeginTable("##p25adj", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("System"); ImGui::TableSetupColumn("RFSS / site"); ImGui::TableSetupColumn("Control channel"); ImGui::TableSetupColumn("Heard");
        ImGui::TableHeadersRow();
        for (const auto& j : t.adjacent) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%03X", j.sysId);
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", j.rfss, j.site);
            ImGui::TableNextColumn(); ImGui::Text("%s  %s", dect2::p25::channelText(j.channel).c_str(), mhz(j.freqHz).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(j.wallTime).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Channel identifiers");
    bool any = false;
    for (const auto& r : t.idens) any = any || r.valid;
    if (!any) ImGui::TextDisabled("  none heard (identifier updates turn channel numbers into frequencies)");
    else if (ImGui::BeginTable("##p25iden", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn("Base"); ImGui::TableSetupColumn("Spacing"); ImGui::TableSetupColumn("Transmit offset"); ImGui::TableSetupColumn("Width");
        ImGui::TableHeadersRow();
        for (int i = 0; i < 16; i++) {
            const auto& r = t.idens[i];
            if (!r.valid) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d%s", i, r.vu ? " (VHF/UHF)" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(r.baseHz).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.2f kHz", r.spacingHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%+.4f MHz", r.txOffsetHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.2f kHz", r.bwHz / 1e3);
        }
        ImGui::EndTable();
    }
}

void talkgroupView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const auto& tg = a.rx.p25.talkgroups;
    if (tg.empty()) { ImGui::TextDisabled("no talkgroups heard yet"); return; }
    if (!ImGui::BeginTable("##p25tg", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Talkgroup"); ImGui::TableSetupColumn("Grants"); ImGui::TableSetupColumn("Calls"); ImGui::TableSetupColumn("Last unit");
    ImGui::TableSetupColumn("Last channel"); ImGui::TableSetupColumn("Last heard"); ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& g : tg) {
        ImGui::TableNextRow();
        const ImVec4 c = g.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%d", g.tg); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%u", g.grants);
        ImGui::TableNextColumn(); ImGui::Text("%u", g.calls);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::p25Id(g.lastSrc).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(g.lastFreqHz).c_str());
        ImGui::TableNextColumn(); ImGui::TextColored(c, "%s", g.active ? "active" : clock(g.lastWall).c_str());
        ImGui::TableNextColumn();
        if (g.emergency) { ImGui::TextColored(pal::warnAmber(), "emergency"); ImGui::SameLine(); }
        if (g.encrypted) ImGui::TextColored(pal::warnAmber(), "encrypted");
    }
    ImGui::EndTable();
}

void messageView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); return; }
    const auto& ev = a.rx.p25.events;
    ImGui::PushFont(a.mono, 0);
    for (const auto& s : ev) ImGui::TextUnformatted(s.c_str());
    ImGui::PopFont();
    if (ev.empty()) ImGui::TextDisabled("no messages yet");
}

// ---------------------------------------------------------------- hooks

void trunkTick(App& a) {
    if (!T.loaded) { T.loaded = true; if (std::filesystem::exists(std::filesystem::u8path(tgFile()))) tgLoad(tgFile(), false); }
    // Following without whole-span decoding: the whole-span bank runs for the granted Phase 1 channels only (a decoder per call, none for the rest)
    const bool wantFollow = T.on && live(a) && !a.wideDecode;
    if (wantFollow != a.wideFollow) { a.wideFollow = wantFollow; a.engine.setWideFollow(wantFollow, {}); T.wideList.clear(); }
    if (!T.on && T.p2Hop) { a.engine.setStandard(engineStd(a)); T.p2Hop = false; T.p2Call = 0; }   // following turned off on a Phase 2 channel
    if (!T.on || (!live(a) && !T.p2Hop)) return;   // on a Phase 2 voice channel the Phase 2 receiver is the first mode
    using namespace dect2::trunk;
    const double now = nowSec();
    const dect2::P25Telemetry& t = a.rx.p25;
    for (const auto& r : t.tsbks) {
        if (r.seq <= T.tsbkSeen) continue;
        T.tsbkSeen = r.seq;
        dect2::p25::Tsbk k; k.opcode = r.opcode; k.mfid = r.mfid; k.args = r.args;
        T.ctl.onTsbk(k, r.nac, now);
    }
    // the whole capture counts for Phase 2 calls (the second mode takes any channel in it); Phase 1 calls in it need whole-span decoding
    T.ctl.setCapture(a.engine.captureCentreHz(), a.engine.sampleRate(), wideActive(a));
    // the radio is on a voice channel: what it hears keeps the call open (no grant repeats reach it there)
    if (T.hopCall && !T.p2Hop && live(a) && t.seq != T.hopSeq) { T.hopSeq = t.seq; if (t.now.active) T.ctl.onVoice(T.hopCall, now); }
    if (T.p2Call) {
        const dect2::P25p2Telemetry& p = T.p2Second ? a.second.rx.p25p2 : a.rx.p25p2;
        if (p.seq != T.p2Seq && p.followTag == T.p2Call && (T.p2Second || a.rx.standard == kP25p2Std - 1)) {   // reports of this call only
            T.p2Seq = p.seq;
            // the Phase 2 screens (running as the second mode) push their own slot choice and seed: this call's win
            dect2::P25p2Receiver& r = a.engine.p25p2();
            r.setListenSlot(T.p2Slot + 1);
            if (p.wacn != T.p2Seed[0] || p.sysId != T.p2Seed[1] || p.colorCode != T.p2Seed[2]) r.setSystem(T.p2Seed[0], T.p2Seed[1], T.p2Seed[2]);
            const dect2::P25p2Call& c = p.now[T.p2Slot];
            if (c.active && c.cryptoKnown && c.encrypted) T.ctl.onEncrypted(T.p2Call, now);   // the ESS says encrypted, whatever the grant said
            else if (p.timeSec - p.voiceSec[T.p2Slot] < 0.6) T.ctl.onVoice(T.p2Call, now);
        }
    }
    T.ctl.tick(now);
    // a Phase 2 call: seed the receiver with the site's WACN / system / NAC (the descrambler) and pick the slot. a.engine.p25p2() is the receiver
    // either way: the engine runs a second mode on the idle receiver of that mode (Engine::setSecondary), and a hop makes it the first mode.
    auto p2Start = [&](const Action& ac) {
        const SiteInfo& si = T.ctl.sys.site();
        dect2::P25p2Receiver& r = a.engine.p25p2();
        T.p2Seed[0] = si.wacn < 0 ? 0 : si.wacn;
        T.p2Seed[1] = si.sysId < 0 ? 0 : si.sysId;
        T.p2Seed[2] = si.nac >= 0 ? si.nac : T.p2Seed[1];   // the NAC from the control channel's NID; XMC_SDR uses the system ID where it is not known
        r.setSystem(T.p2Seed[0], T.p2Seed[1], T.p2Seed[2]);
        r.setListenSlot(ac.slot + 1);
        r.startFollow(ac.callId);   // afresh: nothing of the previous call (its slot's encryption above all) may reach this one
        r.setCallHint(1, -1, -1, true, 0); r.setCallHint(2, -1, -1, true, 0);
        if (const CallRecord* cr = T.ctl.log.byId(ac.callId)) r.setCallHint(ac.slot + 1, cr->src, cr->tg, cr->group, cr->freqHz);   // the voice channel does not name the talker
        T.p2Call = ac.callId; T.p2Slot = ac.slot & 1; T.p2Seq = 0;
    };
    auto p2Stop = [&]() {
        if (T.p2Second) { setSecondMode(a, -1, 0); a.engine.setSecondarySound(a.second.audio); }
        T.p2Second = false; T.p2Call = 0;
    };
    for (const Action& ac : T.ctl.takeActions()) {
        switch (ac.kind) {
        case Action::Retune:
            if (T.p2Second) p2Stop();
            if (ac.tdma) {   // the one radio leaves the control channel for a Phase 2 call: its decoder takes over (no radio restart)
                p2Start(ac);
                if (!T.p2Hop) { a.engine.setStandard(kP25p2Std); T.p2Hop = true; }
            } else {
                if (T.p2Hop) { a.engine.setStandard(engineStd(a)); T.p2Hop = false; }   // back to the Phase 1 receiver
                T.p2Call = 0;
            }
            T.hopCall = ac.callId; T.hopSeq = 0;
            tuneFreq(a, ac.freqHz / 1e6);
            break;
        case Action::OpenSlice:
            if (ac.tdma) {   // inside the capture: the Phase 2 receiver runs as the second mode, the control channel stays decoded
                p2Start(ac);
                const int idx = p25p2ModeIdx();
                T.p2Second = idx >= 0 && setSecondMode(a, idx, ac.freqHz / 1e6);
                if (T.p2Second) a.engine.setSecondarySound(true);
                else {
                    T.ctl.messages.push_back("Phase 2 call on " + mhz(ac.freqHz) + " could not start: " + a.engine.secondaryNote());
                    T.ctl.onTerminator(ac.callId, now);
                    T.p2Call = 0;
                }
            }
            break;
        case Action::CloseSlice:
            if (ac.tdma && ac.callId == T.p2Call) p2Stop();
            break;
        case Action::Notice: T.ctl.messages.push_back(ac.text); break;
        case Action::RecordStart: {
            dect2::AudioRecorder& rec = dect2::AudioRecorder::instance();
            if (!dect2::p25ImbeAvailable() || rec.active()) break;   // no vocoder: there is no sound to save
            dect2::AudioRecOptions o; o.gated = true;
            char nm[96]; time_t tt = time(nullptr); struct tm g;
#ifdef _WIN32
            localtime_s(&g, &tt);
#else
            localtime_r(&tt, &g);
#endif
            char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &g);
            snprintf(nm, sizeof nm, "/recordings/trunk_%s_%s.wav", ac.text.c_str(), ts);
            std::string err;
            if (rec.start(plat::dataDir() + nm, o, err)) { T.recCall = ac.callId; if (auto* r = T.ctl.log.byId(ac.callId)) r->audioPath = plat::dataDir() + nm; }
            break;
        }
        case Action::RecordStop:
            if (T.recCall == ac.callId) { dect2::AudioRecorder::instance().stop(); T.recCall = 0; }
            break;
        default: break;
        }
    }
    if (a.wideFollow) {   // the bank decodes what is followed now
        std::vector<double> v = T.ctl.follower.sliceFreqs(false);
        if (v != T.wideList) { T.wideList = v; a.engine.setWideFollow(true, v); }
    }
    if (wideActive(a)) {   // the sound follows the call that should be heard
        const uint64_t id = T.ctl.follower.audibleCall();
        const CallRecord* r = id ? T.ctl.log.byId(id) : nullptr;
        if (r) {
            const double ctr = radioCenterMhz(a) * 1e6;
            const dect2::WideCarrier* best = nullptr;
            for (const auto& c : a.rx.wide) if (std::fabs(ctr + c.hz - r->freqHz) < 7000 && (!best || c.levelDb > best->levelDb)) best = &c;
            if (best) { a.wideSel = best->id; T.forcedSel = best->id; }
        } else if (T.forcedSel) { a.wideSel = 0; T.forcedSel = 0; }
    }
    if (T.dirty) { T.dirty = false; tgSave(tgFile()); }
}

void followView(App& a) {
    using namespace dect2::trunk;
    ImGui::Checkbox("Follow calls", &T.on);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Follows the voice grants of a P25 control channel. Inside a wide capture every channel is decoded and the sound moves to the call you should hear;\n"
                          "outside it the one radio leaves the control channel for the call and comes back. Encrypted calls are logged as encrypted and never followed.\n"
                          "Sound needs your own mbelib; the talkgroups and the call log work without it. See docs/trunking.md.");
    if (!T.on) { ImGui::TextDisabled("%s", live(a) ? "off: the control channel is only decoded, nothing is followed" : "start the receiver first"); }
    const double now = nowSec();
    const ControllerStatus st = T.ctl.status(now);
    kvRow(a, "state", "%s%s", followStateName(st.state), st.controlLocked ? "" : " (no control channel message lately)");
    kvRow(a, "control channel", "%llu good, %llu bad (%.0f %% ok), %u calls on air, %llu not followed",
          (unsigned long long)st.tsbkGood, (unsigned long long)st.tsbkBad, st.crcOkPct, (unsigned)st.activeCalls, (unsigned long long)st.missed);
    const SiteInfo& si = T.ctl.sys.site();
    if (si.valid) kvRow(a, "site", "WACN %05X system %03X RFSS %d site %d NAC %03X", si.wacn < 0 ? 0 : si.wacn, si.sysId < 0 ? 0 : si.sysId, si.rfss, si.site, si.nac < 0 ? 0 : si.nac);
    if (T.p2Call) {
        const dect2::P25p2Telemetry& p = T.p2Second ? a.second.rx.p25p2 : a.rx.p25p2;
        kvRow(a, "Phase 2", "slot %d %s, descrambler %05X / %03X / %03X, ESS %llu ok, %llu voice frames", T.p2Slot + 1, T.p2Second ? "(second mode)" : "(radio on the channel)",
              p.wacn, p.sysId, p.colorCode, (unsigned long long)p.essOk, (unsigned long long)p.voiceFrames);
    }
    if (a.wideFollow) ImGui::TextDisabled("calls inside the capture get a decoder when they are granted (up to %d Phase 1 at once, one Phase 2 as the second mode); outside it the radio leaves the control channel for them", T.ctl.follower.config().maxSlices);
    else if (wideActive(a)) ImGui::TextDisabled("whole span: every carrier in the capture is decoded, the sound moves to the followed call (up to %d at once)", T.ctl.follower.config().maxSlices);
    else ImGui::TextDisabled("single channel: the radio leaves the control channel for each call");
    FollowConfig fc = T.ctl.follower.config();
    ImGui::SetNextItemWidth(110 * gUi); bool ch = ImGui::SliderInt("simultaneous calls", &fc.maxSlices, 1, 4);
    ImGui::SameLine(0, 10 * gUi); ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::SliderFloat("hang time s", &S.hang, 0.5f, 6.f, "%.1f");
    fc.hangSec = S.hang;
    ch |= ImGui::Checkbox("retune the radio", &fc.allowRetune);
    ImGui::SameLine(0, 10 * gUi);
    ch |= ImGui::Checkbox("Phase 2 calls", &fc.followPhase2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Follow P25 Phase 2 (TDMA) voice grants: the Phase 2 receiver decodes the granted slot, descrambled with the site's\n"
                          "WACN, system and NAC from this control channel. Inside a wide capture one Phase 2 call at a time runs as the second mode.");
    ImGui::SameLine(0, 10 * gUi);
    bool so = T.ctl.tgs.scanOnly();
    if (ImGui::Checkbox("scan list only", &so)) T.ctl.tgs.setScanOnly(so);
    if (ch) T.ctl.follower.setConfig(fc);
    ImGui::Spacing();
    ImGui::TextDisabled("Now talking");
    const auto nt = T.ctl.nowTalking();
    if (nt.empty()) ImGui::TextDisabled("  nobody");
    else if (ImGui::BeginTable("##tnow", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Talkgroup"); ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Unit"); ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const CallRecord* r : nt) {
            const TgEntry* e = T.ctl.tgs.find(r->tg);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s%d", r->group ? "" : "unit ", r->tg);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e ? e->alpha.c_str() : "");
            ImGui::TableNextColumn(); ImGui::Text("%d", r->src);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(r->freqHz).c_str());
            ImGui::TableNextColumn();
            if (r->encrypted) ImGui::TextColored(pal::warnAmber(), "encrypted: not followed");
            else if (r->followed) ImGui::TextColored(pal::okGreen(), "following");
            else ImGui::TextDisabled("%s", r->note.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Talkgroup list");
    if (ImGui::Button("Import CSV")) { const std::string p = openFileDialog(); if (!p.empty()) { tgLoad(p, true); T.dirty = true; } }
    ImGui::SameLine();
    if (ImGui::Button("Export CSV")) { const std::string p = saveFileDialog("talkgroups.csv"); if (!p.empty()) tgSave(p); }
    ImGui::SameLine();
    if (ImGui::Button("Add heard")) { for (const auto& g : a.rx.p25.talkgroups) T.ctl.tgs.upsert(g.tg); T.dirty = true; }
    if (!T.status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", T.status.c_str()); }
    if (ImGui::BeginTable("##ttg", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, 170 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Talkgroup"); ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Listen"); ImGui::TableSetupColumn("Lockout");
        ImGui::TableSetupColumn("Priority"); ImGui::TableSetupColumn("Tag");
        ImGui::TableHeadersRow();
        std::vector<TgEntry>& v = const_cast<std::vector<TgEntry>&>(T.ctl.tgs.entries());
        for (TgEntry& e : v) {
            ImGui::PushID(e.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", e.id);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.alpha.c_str());
            ImGui::TableNextColumn(); if (ImGui::Checkbox("##l", &e.listen)) T.dirty = true;
            ImGui::TableNextColumn(); if (ImGui::Checkbox("##x", &e.lockout)) T.dirty = true;
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(70 * gUi); if (ImGui::DragInt("##p", &e.priority, 0.1f, 0, 10, e.priority ? "%d" : "none")) T.dirty = true;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.tag.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Call log");
    if (ImGui::Button("Export log CSV")) { const std::string p = saveFileDialog("call_log.csv"); if (!p.empty()) { std::ofstream f(std::filesystem::u8path(p), std::ios::binary); f << T.ctl.log.exportCsv(&T.ctl.tgs); } }
    if (ImGui::BeginTable("##tlog", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(dect2::timefmt::headTime()); ImGui::TableSetupColumn("Talkgroup"); ImGui::TableSetupColumn("Unit"); ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("Channel");
        ImGui::TableSetupColumn("Encrypted"); ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        const auto& rs = T.ctl.log.records();
        for (size_t i = rs.size(); i-- > 0;) {
            const CallRecord& r = rs[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(r.wallTime).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", r.tg);
            ImGui::TableNextColumn(); ImGui::Text("%d", r.src);
            ImGui::TableNextColumn(); ImGui::Text("%.1f s", r.active ? std::max(r.durSec, r.lastSec - r.startSec) : r.durSec);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(r.freqHz).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.encrypted ? "yes" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.note.c_str());
        }
        ImGui::EndTable();
    }
}

void tick(App& a) {
    if (live(a)) appcodec::voiceCodecTick("imbe");   // the user's codec plug-in (docs/CODECS.md), loaded whichever tab is shown
    trunkTick(a);
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::P25Receiver& r = a.engine.p25();
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        a.engine.p25p2().setVolume(a.volume); a.engine.p25p2().setMuted(a.muted);   // the followed Phase 2 calls
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tab(App& a) {
    {   // the voice monitor's strip, the same in every voice mode
        const dect2::P25Telemetry& t = a.rx.p25;
        voiceui::Link l;
        l.live = live(a); l.locked = l.live && t.state >= 1; l.codecName = "IMBE"; l.codecId = "imbe";
        l.codecReady = t.imbe || dect2::codec::pluginLoaded("imbe");
        l.errorRate = l.locked ? t.berPct / 100.f : -1.f;
        voiceui::strip(a, "p25", "P25", l);
    }
    subNav("p25v", S.view, {"Now", "Calls", "Trunking", "Talkgroups", "Messages", "Follow"});
    if (S.view == 0) nowView(a); else if (S.view == 1) callsView(a); else if (S.view == 2) trunkingView(a); else if (S.view == 3) talkgroupView(a); else if (S.view == 4) messageView(a); else followView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (resultsClearButton("p25")) a.engine.p25().clearResults();
    const auto& tg = a.rx.p25.talkgroups;
    if (!ImGui::BeginTable("##p25l", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("TG"); ImGui::TableSetupColumn("Heard", ImGuiTableColumnFlags_WidthStretch);
    const auto& n = a.rx.p25.now;
    if (n.active && !n.group) {
        const ImVec4 c = n.encrypted ? pal::warnAmber() : pal::okGreen();
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", dect2::p25Id(n.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextColored(c, "on air -> %s%s", dstOf(n).c_str(), n.encrypted ? ", encrypted" : "");
    }
    // a running call is encrypted when the follower's record says so (that includes what a Phase 2 voice channel's encryption sync said: grants
    // do not always tell), else when the talkgroup was heard encrypted
    std::map<int, bool> runningEnc;
    for (const auto& r : T.ctl.log.records()) if (r.active && r.group && !r.dmr) runningEnc[r.tg] = r.encrypted;
    for (const auto& g : tg) {
        const auto it = runningEnc.find(g.tg);
        const bool enc = g.encrypted || (it != runningEnc.end() && it->second);
        const ImVec4 on = enc ? pal::warnAmber() : pal::okGreen();   // active: green in the clear, orange encrypted
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(g.active ? on : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%d", g.tg); ImGui::PopFont();
        ImGui::TableNextColumn();
        if (g.active) ImGui::TextColored(on, "active, unit %s%s", dect2::p25Id(g.lastSrc).c_str(), enc ? ", encrypted" : "");
        else ImGui::TextDisabled("%s  %u grants, %u calls", clock(g.lastWall).c_str(), g.grants, g.calls);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    wideCarriersTable(a, 0);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::P25Telemetry& t = a.rx.p25;
    kvRow(a, "state", "%s", dect2::p25Summary(t).c_str());
    kvRow(a, "signal", "C4FM, symbol SNR %.1f dB, +3 deviation %.0f Hz, offset %+.0f Hz%s", t.snrDb, t.deviationHz, t.cfoHz, t.inverted ? ", inverted" : "");
    kvRow(a, "NAC", "%03X", t.nac < 0 ? 0 : t.nac);
    kvRow(a, "bit errors", "%.2f %% (NID, before the BCH)", t.berPct);
    kvRow(a, "frames", "%llu, %llu with a damaged NID", (unsigned long long)t.frames, (unsigned long long)t.nidBad);
    kvRow(a, "data units", "HDU %llu, LDU1 %llu, LDU2 %llu, TDU %llu, TDULC %llu, PDU %llu", (unsigned long long)t.hdu, (unsigned long long)t.ldu1, (unsigned long long)t.ldu2,
          (unsigned long long)t.tdu, (unsigned long long)t.tdulc, (unsigned long long)t.pdu);
    kvRow(a, "TSBK", "%llu good, %llu failed CRC", (unsigned long long)t.tsbkOk, (unsigned long long)t.tsbkBad);
    kvRow(a, "link control", "%llu good, %llu failed", (unsigned long long)t.lcOk, (unsigned long long)t.lcBad);
    kvRow(a, "encryption sync", "%llu good, %llu failed; header %llu good, %llu failed", (unsigned long long)t.esOk, (unsigned long long)t.esBad, (unsigned long long)t.hduOk, (unsigned long long)t.hduBad);
    kvRow(a, "voice", "%llu frames, %llu decoded%s, %llu calls joined late", (unsigned long long)t.voiceFrames, (unsigned long long)t.voiceDecoded, t.imbe ? "" : " (mbelib not found: click ? for the file names and folders)",
          (unsigned long long)t.lateEntries);
    if (t.imbe) kvRow(a, "mbelib", "%s", t.imbePath.c_str());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : a.rx.p25.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Control", !on ? 0 : a.rx.p25.control ? 1 : 0); flowNext(10 * gUi);
    lamp("On air", !on ? 0 : a.rx.p25.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::p25Summary(a.rx.p25).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "P25";
    if (live(a)) l2 = dect2::p25Summary(a.rx.p25);
}

} // namespace

extern const ModeUi kP25Ui;
const ModeUi kP25Ui = {
    .sideTitle = "TALKGROUPS",
    .tabName = "P25",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
