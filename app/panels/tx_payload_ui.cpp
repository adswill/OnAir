// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The signal's own options in step 2 of the transmit panel: what to say (callsign, text, IDs), where the audio / video / file comes from, and the standard's
// own choices, one block per kind of signal (TxPayloadKind). Empty fields fall back to the mode's demo signal; nothing here limits what is typed.
#include "tx_state.h"
#include "dect2/mesh/mesh_tx.h"
#include "dect2/fm/rds_live.h"
#include "dect2/tx/tx_dtmf.h"
#include "dect2/dmr/dmr_slot_sync.h"
#include "platform/platform.h"
#include "../ui/typography.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/cp16/cp16_proto.h"
#include "../ui/cjk_fonts.h"
#include "dect2/tv/videogen.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace dect2;

namespace txui {

bool isVoiceId(const std::string& id) {
    return id == "fm" || id == "airband" || id == "qo100" ||
           id == "m17" || id == "freedv" || id == "dmr" || id == "p25" || id == "nxdn" ||
           id == "dpmr" || id == "dstar" || id == "tetra" || id == "kaal";
}
bool isFtxId(const std::string& id) {
    return id == "ft8" || id == "ft4" || id == "wspr" || id == "ft2" || id == "jt65" ||
           id == "jt9" || id == "q65" || id == "fst4" || id == "msk144";
}
bool isKbdId(const std::string& id) {
    return id == "olivia" || id == "contestia" || id == "mfsk" || id == "thor" || id == "dominoex" || id == "hell";
}
bool isTvId(const std::string& id) {
    return id == "dvbt2" || id == "dvbt" || id == "atsc" || id == "atsc3" || id == "dab" ||
           id == "isdbt" || id == "dvbs" || id == "dtmb" || id == "dvbc";
}

// Mesh (LoRa) transmit: who the message comes from. Nothing here refuses a transmission: problems are listed as warnings.
void meshTxPanel(TxUi& s, bool running) {
    plat::Prefs& pf = plat::basePrefs();
    ImGui::BeginDisabled(running);
    ImGui::SeparatorText(tr("Your node"));
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::InputText(tr("Long name"), s.meshLong, sizeof s.meshLong)) saveStr("tx.meshLong", s.meshLong);
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::InputText(tr("Short name (4)"), s.meshShort, sizeof s.meshShort)) saveStr("tx.meshShort", s.meshShort);
    ImGui::SetNextItemWidth(140 * gUi);
    if (ImGui::InputText(tr("Node ID"), s.meshNode, sizeof s.meshNode)) saveStr("tx.meshNode", s.meshNode);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Like !1a2b3c4d (8 hex digits). Empty: a stable number is made from your callsign, or the long name."));
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText(tr("Callsign (ham)"), s.callsign, sizeof s.callsign)) saveStr("tx.callsign", s.callsign);
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Callsign in the message"), &s.meshCallInMsg)) pf.setB("tx.meshCallInMsg", s.meshCallInMsg);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Puts \"CALLSIGN: \" in front of the text, so every message identifies you."));
    if (ImGui::Checkbox(tr("Send my node info first"), &s.meshNodeInfo)) pf.setB("tx.meshNodeInfo", s.meshNodeInfo);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Other nodes learn your names from the node info packet, which goes out just before the text."));
    ImGui::SeparatorText(tr("Channel"));
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText(tr("Preset"), s.meshPreset, sizeof s.meshPreset)) saveStr("tx.meshPreset", s.meshPreset);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The modem preset: LongFast, LongSlow, MediumFast, ShortFast, ... (the receiver must use the same)."));
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText(tr("Channel name"), s.meshChan, sizeof s.meshChan)) saveStr("tx.meshChan", s.meshChan);
    ImGui::SetNextItemWidth(300 * gUi);
    if (ImGui::InputText(tr("PSK"), s.meshPsk, sizeof s.meshPsk)) saveStr("tx.meshPsk", s.meshPsk);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("default (the public key), none, or the key as base64 / 32 or 64 hex digits."));
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::SliderInt(tr("Hop limit"), &s.meshHop, 0, 7)) pf.setI("tx.meshHop", s.meshHop);
    ImGui::EndDisabled();
    dect2::MeshTxIdentity id;
    id.longName = s.meshLong; id.shortName = s.meshShort; id.nodeId = s.meshNode; id.channelName = s.meshChan; id.psk = s.meshPsk;
    id.preset = s.meshPreset; id.hopLimit = s.meshHop; id.callsign = s.callsign; id.callsignInMessage = s.meshCallInMsg;
    char nid[16];
    snprintf(nid, sizeof nid, "!%08x", (unsigned)dect2::meshTxNodeNum(id));
    ImGui::TextDisabled("%s %s", tr("Sending as node"), nid);
    for (const std::string& w : dect2::meshTxWarnings(id, s.text)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.f), "%s", tr(w.c_str()));
        ImGui::PopTextWrapPos();
    }
}

// DTMF keypad (tx_dtmf.h) for the analog voice modes: EchoLink / IRLP node control and repeater commands. A key goes out at once while
// transmitting; the sequence box sends a whole string. The tone and gap lengths, level and twist are under Advanced and kept in the settings.
void dtmfKeypadRow(bool running) {
    auto& q = dect2::TxDtmfQueue::global();
    static bool init = false;
    static dect2::TxDtmfTiming t;
    static char seq[64] = "";
    if (!init) {
        init = true;
        t.toneMs = plat::prefs().getD("tx.dtmf.toneMs", 100);
        t.gapMs = plat::prefs().getD("tx.dtmf.gapMs", 100);
        t.levelPct = plat::prefs().getD("tx.dtmf.levelPct", 35);
        t.twistDb = plat::prefs().getD("tx.dtmf.twistDb", 0);
        q.setTiming(t);
    }
    if (!running && (q.pending() > 0 || !q.sent().empty())) q.clear();   // nothing waits for the next transmission
    if (!ImGui::CollapsingHeader(trw("DTMF keypad (EchoLink, IRLP, repeater commands)"))) return;
    if (!running) ImGui::TextDisabled("%s", tr("Press START TRANSMITTING first: the keys go out while you transmit."));
    ImGui::BeginDisabled(!running);
    static const char* rows[4] = {"123A", "456B", "789C", "*0#D"};
    const float w = 36 * gUi;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (c) ImGui::SameLine();
            char lb[16]; snprintf(lb, sizeof lb, "%c##dtmf%d%d", rows[r][c], r, c);
            if (ImGui::Button(lb, ImVec2(w, w))) q.push(std::string(1, rows[r][c]));
        }
    }
    ImGui::SetNextItemWidth(200 * gUi);
    const bool enter = ImGui::InputText(tr("Key sequence"), seq, sizeof seq, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button(tr("Send keys")) || enter) && seq[0]) q.push(seq);
    ImGui::EndDisabled();
    if (running) {
        const std::string sent = q.sent();
        ImGui::TextDisabled("%s %s   %s %d", tr("Sent:"), sent.empty() ? "-" : sent.c_str(), tr("Waiting:"), (int)q.pending());
    }
    ImGui::TextDisabled("%s", tr("EchoLink: a node number connects, # disconnects, 08 reads the status (the node's own commands apply)."));
    if (ImGui::TreeNode(trw("Advanced: DTMF timing and level"))) {
        auto sliderD = [](const char* lb, double* v, double lo, double hi, const char* f) { return ImGui::SliderScalar(lb, ImGuiDataType_Double, v, &lo, &hi, f); };
        bool ch = false;
        ImGui::SetNextItemWidth(160 * gUi);
        ch |= sliderD(tr("Tone length (ms)"), &t.toneMs, 40, 500, "%.0f");
        ImGui::SetNextItemWidth(160 * gUi);
        ch |= sliderD(tr("Gap between keys (ms)"), &t.gapMs, 40, 500, "%.0f");
        ImGui::SetNextItemWidth(160 * gUi);
        ch |= sliderD(tr("Level of each tone (%)"), &t.levelPct, 5, 50, "%.0f");
        ImGui::SetNextItemWidth(160 * gUi);
        ch |= sliderD(tr("Twist: high tone above low (dB)"), &t.twistDb, -6, 6, "%.1f");
        if (ch) {
            q.setTiming(t);
            plat::prefs().setD("tx.dtmf.toneMs", t.toneMs);
            plat::prefs().setD("tx.dtmf.gapMs", t.gapMs);
            plat::prefs().setD("tx.dtmf.levelPct", t.levelPct);
            plat::prefs().setD("tx.dtmf.twistDb", t.twistDb);
        }
        ImGui::TreePop();
    }
}

// DMR slot sync (dmr_slot_sync.h, TS 102 361-1 4.4.1): listen to the repeater's output in the DMR receiver first, then the first burst waits for
// the inbound slot of the chosen timeslot. One checkbox on top; the channel timing and the radio's latency under Advanced.
void dmrSlotSyncRow(TxUi& s) {
    if (!s.dmrSyncLoaded) {
        s.dmrSyncLoaded = true;
        s.dmrSlotSync = plat::prefs().getD("tx.dmr.slotSync", 1) > 0.5;
        s.dmrAligned = plat::prefs().getD("tx.dmr.aligned", 0) > 0.5;
        s.dmrLatencyMs = plat::prefs().getD("tx.dmr.latencyMs", 0);
    }
    if (ImGui::Checkbox(tr("Sync to the repeater's time slots (listen first)"), &s.dmrSlotSync)) plat::prefs().setD("tx.dmr.slotSync", s.dmrSlotSync ? 1 : 0);
    if (s.dmrSlotSync) {
        const dect2::DmrSlotClock::Fit f = dect2::DmrSlotClock::global().fit();
        const double age = dect2::dmrSteadySec() - f.lastSec;
        const int in = dect2::DmrSlotClock::inboundSlot(s.slot == 2 ? 2 : 1, s.dmrAligned);
        if (f.ok && age < 10) {
            const int busy = f.busy[in - 1];
            ImGui::TextDisabled(tr("Repeater timing heard %.1f s ago (%d bursts, %.2f ms): transmits in inbound slot %d%s"), age, f.used, f.rmsMs, in,
                                busy == 1 ? tr(", which the repeater reports busy") : "");
        } else {
            ImGui::TextDisabled("%s", tr("Repeater not heard in the last 10 s: open the DMR receiver on the repeater's output first, or the call starts at once (wakes the repeater)."));
        }
    }
    if (ImGui::TreeNode(trw("Advanced: DMR slot timing"))) {
        if (ImGui::Checkbox(tr("Aligned channel timing (the repeater pairs inbound slot 2 with outbound slot 1)"), &s.dmrAligned)) plat::prefs().setD("tx.dmr.aligned", s.dmrAligned ? 1 : 0);
        ImGui::SetNextItemWidth(160 * gUi);
        double lo = -100, hi = 200;
        if (ImGui::SliderScalar(tr("Radio latency to the air (ms)"), ImGuiDataType_Double, &s.dmrLatencyMs, &lo, &hi, "%.2f")) plat::prefs().setD("tx.dmr.latencyMs", s.dmrLatencyMs);
        ImGui::TextDisabled("%s", tr("The receive and transmit buffering of the radio, from the moment OnAir hands over the samples to the air. Adjust until the repeater takes the call."));
        ImGui::TreePop();
    }
}

void audioSourceRow(TxUi& s, bool running) {
    const char* src[] = {tr("Microphone"), tr("WAV / MP3 file"), tr("1 kHz test tone"), tr("Silence")};
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::BeginCombo(tr("Audio input"), src[std::max(0, std::min(3, s.audioSrc))])) {
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(src[i], s.audioSrc == i)) s.audioSrc = i;
        ImGui::EndCombo();
    }
    if (s.audioSrc == 0) {
        const auto devs = listAudioInputs();
        const char* cur = s.audioDevice.empty() ? tr("Default input") : s.audioDevice.c_str();
        ImGui::SetNextItemWidth(280 * gUi);
        if (ImGui::BeginCombo(tr("Input device"), cur)) {
            for (const auto& d : devs) {
                const bool sel = s.audioDevice == d.id;
                if (ImGui::Selectable(d.name.c_str(), sel)) s.audioDevice = d.id;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Checkbox(tr("Mute"), &s.audioMute);
        ImGui::SetNextItemWidth(180 * gUi);
        ImGui::SliderFloat(tr("Mic level"), &s.micGain, 0.f, 2.f, "%.2f");
        ImGui::TextDisabled("%s", tr("The microphone starts only when you press START TRANSMITTING."));
    } else if (s.audioSrc == 1) {
        ImGui::SetNextItemWidth(320 * gUi);
        ImGui::InputText(tr("Audio file"), s.file, sizeof s.file);
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse…##audio"))) { const std::string p = openFileDialog(); if (!p.empty()) snprintf(s.file, sizeof s.file, "%s", p.c_str()); }
    }
    if (running) {
        const float pk = s.session.stats().audioPeak;
        ImGui::ProgressBar(pk, ImVec2(220 * gUi, 0), tr("Mic level"));
    }
    // the keypad where the audio goes out as analog voice (NFM / AM / SSB); broadcast FM and the vocoders do not carry it
    if (s.modeId != "fm" && (s.analogVoice || s.modeId == "airband" || s.modeId == "qo100")) dtmfKeypadRow(running);
}

// Hellschreiber: the font file for characters the built-in glyphs cannot draw (Chinese, Japanese, Korean, symbols). Under Advanced; empty = built in.
void hellFontRow() {
    static bool init = false;
    static char path[512] = "";
    static std::string msg;
    auto apply = [&](const std::string& p) {
        std::string err;
        if (dect2::kbd::hellLoadUserFont(p, err)) { msg = p.empty() ? std::string() : std::string(tr("Font loaded.")); plat::prefs().setS("hellFont", p); }
        else { msg = tr(err.c_str()); }
    };
    if (!init) { init = true; snprintf(path, sizeof path, "%s", plat::prefs().getS("hellFont", "").c_str()); if (path[0]) apply(path); }
    if (!ImGui::TreeNode(trw("Advanced: font for other scripts"))) return;
    ImGui::TextWrapped("%s", tr("Letters, digits, accented Latin, Cyrillic and Greek are built in. For Chinese, Japanese, Korean or symbols choose a .ttf or .otf font file that has them "
                                "(for example GNU Unifont or Noto Sans CJK, downloaded from their own sites; OnAir does not ship it). Characters the font lacks stay '?'."));
    ImGui::SetNextItemWidth(320 * gUi);
    ImGui::InputText(tr("Font file"), path, sizeof path);
    ImGui::SameLine();
    if (ImGui::Button(tr("Browse…##hellfont"))) { const std::string p = openFileDialog(); if (!p.empty()) snprintf(path, sizeof path, "%s", p.c_str()); }
    if (ImGui::Button(tr("Use this font"))) apply(path);
    ImGui::SameLine();
    if (ImGui::Button(tr("Built-in glyphs only"))) { path[0] = 0; apply(""); }
    if (!msg.empty()) ImGui::TextDisabled("%s", msg.c_str());
    ImGui::TreePop();
}

// CP16: the characters are drawn with the font of the mode's screen (the bundled Chinese font, or the file chosen under its Advanced)
void cp16FontRow() {
    static bool init = false;
    if (!init) {
        init = true;
        std::string err;
        dect2::cp16::setDefaultFontData(cjk::regularFontData(), "Noto Sans CJK SC", err);
        const std::string p = plat::prefs().getS("cp16Font", "");
        if (!p.empty()) dect2::cp16::loadFontFile(p, err);
    }
    ImGui::TextDisabled("%s %s", tr("Font:"), dect2::cp16::fontName().c_str());
}

void browseFile(TxUi& s, const char* id) {
    const std::string p = openFileDialog();
    if (!p.empty()) snprintf(s.file, sizeof s.file, "%s", p.c_str());
    (void)id;
}

void fileRow(TxUi& s, const char* label, const char* btnId) {
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputText(label, s.file, sizeof s.file);
    ImGui::SameLine();
    if (ImGui::Button(btnId)) browseFile(s, btnId);
}

// Tools > Digital video generator, as the video source: its running stream (Output: Transmitter) is read by the name generator:N
void generatorPick(TxUi& s) {
    ImGui::TextDisabled("%s", tr("Video generator:"));
    for (int i = 1; i <= 4; i++) {
        ImGui::SameLine();
        const std::string spec = dect2::vgen::generatorSpec(i);
        const bool on = spec == s.file;
        char lb[40]; snprintf(lb, sizeof lb, "%d%s##tvgen%d", i, dect2::vgen::generatorQueued(i) > 0 ? " *" : "", i);
        if (ImGui::SmallButton(lb)) snprintf(s.file, sizeof s.file, "%s", on ? "" : spec.c_str());
    }
    { ImGui::SameLine(); ImGui::TextDisabled("(?)"); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Uses the stream of a generator of Tools > Digital video generator whose output is \"Transmitter\" (* = it is sending now). Start it there; click again to clear.")); }
}

void analogNfmBox(TxUi& s, bool running) {
    if (s.modeId == "general")   // the general receiver sends the audio with the modulation its test signal is set to (#373)
        ImGui::Checkbox(tr("Send the microphone or a file with the modulation above, instead of the test signal"), &s.analogVoice);
    else ImGui::Checkbox(tr("Send analog NFM of the microphone or a file instead of the test scene"), &s.analogVoice);
    if (s.analogVoice) audioSourceRow(s, running);
}

void muxIdentity(TxUi& s) {
    typo::separatorText(tr("Multiplex"));
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputText(tr("Network name"), s.network, sizeof s.network);
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputText(tr("Provider"), s.provider, sizeof s.provider);
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::InputInt(tr("First service ID"), &s.serviceId);
    if (s.serviceId < 1) s.serviceId = 1;
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::SliderInt(tr("Services"), &s.nsvc, 1, 4)) s.nsvc = std::max(1, std::min(4, s.nsvc));
    ImGui::TextDisabled("%s", tr("Each service is its own programme (own video and audio PIDs in the PAT / PMT / SDT). The whole-mux MPEG-TS above, if set, replaces this."));
    const auto cams = listVideoInputs();
    for (int i = 0; i < s.nsvc; i++) {
        char lb[48];
        snprintf(lb, sizeof lb, "%s %d", tr("Service name"), i + 1);
        ImGui::SetNextItemWidth(220 * gUi);
        ImGui::InputText(lb, s.svc[i], sizeof s.svc[i]);
        const char* srcn[] = {tr("Test card"), tr("Video file"), tr("Camera / capture card")};
        snprintf(lb, sizeof lb, "%s %d", tr("Programme source"), i + 1);
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo(lb, srcn[std::max(0, std::min(2, s.svcSrc[i]))])) {
            for (int k = 0; k < 3; k++) if (ImGui::Selectable(srcn[k], s.svcSrc[i] == k)) s.svcSrc[i] = k;
            ImGui::EndCombo();
        }
        if (s.svcSrc[i] == 1) {
            snprintf(lb, sizeof lb, "%s %d", tr("Video file"), i + 1);
            ImGui::SetNextItemWidth(220 * gUi);
            ImGui::InputText(lb, s.svcFile[i], sizeof s.svcFile[i]);
            ImGui::SameLine();
            snprintf(lb, sizeof lb, "%s##svcfile%d", tr("Browse…"), i);
            if (ImGui::Button(lb)) { const std::string p = openFileDialog(); if (!p.empty()) snprintf(s.svcFile[i], sizeof s.svcFile[i], "%s", p.c_str()); }
        } else if (s.svcSrc[i] == 2) {
            snprintf(lb, sizeof lb, "%s %d", tr("Camera"), i + 1);
            const char* cur = tr("Default camera");
            for (const auto& d : cams) if (s.svcCam[i][0] && std::string(s.svcCam[i]) == d.id) { cur = d.name.c_str(); break; }
            ImGui::SetNextItemWidth(280 * gUi);
            if (ImGui::BeginCombo(lb, cur)) {
                if (ImGui::Selectable(tr("Default camera"), s.svcCam[i][0] == 0)) s.svcCam[i][0] = 0;
                for (const auto& d : cams) {
                    const bool sel = std::string(s.svcCam[i]) == d.id;
                    if (ImGui::Selectable(d.name.c_str(), sel)) snprintf(s.svcCam[i], sizeof s.svcCam[i], "%s", d.id.c_str());
                }
                ImGui::EndCombo();
            }
        }
    }
}

// The DVB standard of the TV list entry, the digital video generator as the source, and what was done with the file last time
void tvExtras(TxUi& s, const TxModeSpec& mode) {
    const std::string id = mode.id ? mode.id : "";
    generatorPick(s);
    if (!s.fileNote.empty()) ImGui::TextWrapped("%s", s.fileNote.c_str());
    if (id == "dvbt2" || id == "dvbt") {
        const char* dvb[] = {"DVB-T2", "DVB-T"};
        const int vi = s.dvbVariant == 2 ? 1 : 0;
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::BeginCombo(tr("DVB standard"), dvb[vi])) {
            if (ImGui::Selectable("DVB-T2", vi == 0)) { s.dvbVariant = 1; s.modeId = "dvbt2"; }
            if (ImGui::Selectable("DVB-T", vi == 1)) { s.dvbVariant = 2; s.modeId = "dvbt"; }
            ImGui::EndCombo();
        }
    }
}

void atvPicture(TxUi& s) {
    const char* vsrc[] = {tr("Test card / bars / ramp"), tr("Video file"), tr("Camera / capture card")};
    ImGui::SetNextItemWidth(240 * gUi);
    if (ImGui::BeginCombo(tr("Picture source"), vsrc[std::max(0, std::min(2, s.videoSrc))])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(vsrc[i], s.videoSrc == i)) s.videoSrc = i;
        ImGui::EndCombo();
    }
    if (s.videoSrc == 1) {
        ImGui::SetNextItemWidth(220 * gUi);
        ImGui::InputText(tr("Video file"), s.file, sizeof s.file);
        ImGui::SameLine();
        if (ImGui::Button(tr("Browse…##atvvid"))) { const std::string p = openFileDialog(); if (!p.empty()) snprintf(s.file, sizeof s.file, "%s", p.c_str()); }
        generatorPick(s);
    } else if (s.videoSrc == 2) {
        const auto cams = listVideoInputs();
        const char* cur = tr("Default camera");
        for (const auto& d : cams) if (s.videoDevice[0] && std::string(s.videoDevice) == d.id) { cur = d.name.c_str(); break; }
        ImGui::SetNextItemWidth(280 * gUi);
        if (ImGui::BeginCombo(tr("Camera / capture card"), cur)) {
            if (ImGui::Selectable(tr("Default camera"), s.videoDevice[0] == 0)) s.videoDevice[0] = 0;
            for (const auto& d : cams) {
                const bool sel = std::string(s.videoDevice) == d.id;
                if (ImGui::Selectable(d.name.c_str(), sel)) snprintf(s.videoDevice, sizeof s.videoDevice, "%s", d.id.c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("%s", tr("The camera starts only when you press TRANSMIT."));
    }
}

void atvSound(TxUi& s, bool running) {
    const char* sndn[] = {"1 kHz tone (with gaps)", "melody", "sound carrier only", "no sound carrier", "microphone", "WAV / MP3 file"};
    const int snd = s.audioSrc == 0 ? 4 : s.audioSrc == 1 ? 5 : std::max(0, std::min(3, s.synth.modeOpt[3]));
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::BeginCombo(tr("Sound carrier"), sndn[snd])) {
        for (int i = 0; i < 6; i++) if (ImGui::Selectable(sndn[i], i == snd)) {
            if (i < 4) { s.audioSrc = 2; s.synth.modeOpt[3] = i; }
            else { s.audioSrc = i == 4 ? 0 : 1; s.synth.modeOpt[3] = 0; }
        }
        ImGui::EndCombo();
    }
    if (s.audioSrc == 0 || s.audioSrc == 1) audioSourceRow(s, running);
}

void liveRds(bool running) {
    auto& L = RdsLive::global();
    if (running) ImGui::TextDisabled("%s", tr("On the air: changes to the name, text and PI are sent with the next RDS group."));
    if (!ImGui::CollapsingHeader(trw("Live RDS control (programme type, flags, clock, network commands)"))) return;
    RdsState st = L.get();
    if (ImGui::SliderInt(tr("Programme type (PTY)"), &st.pty, 0, 31)) { std::string e; L.apply("PTY", std::to_string(st.pty), e); }
    if (ImGui::Checkbox(tr("Traffic programme (TP)"), &st.tp)) { std::string e; L.apply("TP", st.tp ? "1" : "0", e); }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Traffic announcement (TA)"), &st.ta)) { std::string e; L.apply("TA", st.ta ? "1" : "0", e); }
    if (ImGui::Checkbox(tr("Music (not speech)"), &st.music)) { std::string e; L.apply("MS", st.music ? "M" : "S", e); }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Send the clock time"), &st.ct)) { std::string e; L.apply("CT", st.ct ? "1" : "0", e); }
    static int port = 9101;
    static bool on = false;
    static char tok[64] = {}, wfile[512] = {};
    static std::string msg;
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::InputInt(tr("Command port (UDP and TCP)"), &port, 0, 0);
    ImGui::InputText(tr("Token (optional on this computer)"), tok, sizeof tok);
    if (ImGui::Checkbox(tr("Listen for RDS commands (PS=NAME, RT=text, PI=4A21 ...)"), &on)) {
        std::string e;
        if (!L.listen(on ? port : 0, false, tok, e)) { on = false; msg = e; } else msg.clear();
    }
    if (!msg.empty()) ImGui::TextColored(kRed, "%s", msg.c_str());
    ImGui::TextDisabled("%s", tr("This computer only. For other computers use the headless server or the remote API (it has the token)."));
    if (ImGui::InputText(tr("Radio text from a file (first line, for a now-playing script)"), wfile, sizeof wfile)) L.watchFile(wfile);
}

void dabServices(TxUi& s) {
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::SliderInt(tr("Named services"), &s.nsvc, 1, 4)) s.nsvc = std::max(1, std::min(4, s.nsvc));
    for (int i = 0; i < s.nsvc; i++) {
        char lb[40]; snprintf(lb, sizeof lb, "%s %d", tr("Service"), i + 1);
        ImGui::SetNextItemWidth(220 * gUi);
        ImGui::InputText(lb, s.svc[i], sizeof s.svc[i]);
    }
    ImGui::TextDisabled("%s", tr("The audio is the built-in tones / melody: this encoder does not take a playlist."));
}

// the rows the mode draws with its own widget (tx_params.h custom rows)
void customRow(TxUi& s, const TxModeSpec& mode, const PDef& d, bool running) {
    const std::string k = d.key, id = mode.id ? mode.id : "";
    if (k == "audio.source") audioSourceRow(s, running);
    else if (k == "analog.voice") {
        const char* lab = id == "general" ? tr("Send the microphone or a file with the generator's modulation, instead of the test signal")
                        : txPayloadKind(id) == TxPayloadKind::VoiceDigital ? tr("Send analog NFM of the audio instead of the digital test pattern")
                        : tr("Send analog NFM of the microphone or a file instead of the test scene");
        ImGui::Checkbox(lab, &s.analogVoice);
    } else if (k == "voice.note") {
        const std::string note = txVocoderNote(mode);
        if (!note.empty()) banner(Note::Info, tr(note.c_str()));
        if (id == "dmr") banner(Note::Info, tr("Your DMR ID and callsign go on the air. Voice bits are a test pattern (OnAir cannot encode AMBE+2). Not tested on air against a real repeater."));
    } else if (k == "dmr.slotSync") dmrSlotSyncRow(s);
    else if (k == "tv.mux") { tvExtras(s, mode); muxIdentity(s); }
    else if (k == "atv.picture") atvPicture(s);
    else if (k == "atv.sound") atvSound(s, running);
    else if (k == "dab.services") dabServices(s);
    else if (k == "bfm.live") liveRds(running);
    else if (k == "qo100.path") {
        const char* paths[] = {tr("SSB (microphone)"), tr("DATV (DVB-S/S2 file)")};
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo(tr("QO-100 path"), paths[s.variant == 1 ? 1 : 0])) {
            if (ImGui::Selectable(paths[0], s.variant != 1)) s.variant = 0;
            if (ImGui::Selectable(paths[1], s.variant == 1)) s.variant = 1;
            ImGui::EndCombo();
        }
    } else if (k == "mesh.node") meshTxPanel(s, running);
    else if (k == "kaal.panel") kaalPanel(s, running);
    else if (k == "kait.panel") kaitPanel(s, running);
    else if (k == "kavb.panel") kavbPanel(s, running);
    else if (k == "kapp.panel") kappPanel(s, running);
    else if (k == "kaft.panel") kaftPanel(s, running);
    else if (k == "kaws.panel") kawsPanel(s, running);
    else if (k == "winlink.session") ImGui::TextDisabled("%s", tr("The Winlink screen fills this transmission with its session's frames. From here: one test frame."));
    // scene.options is drawn by stepSettings (it needs the receiver's own options widget)
}

// What to send: the Simple rows of the mode's table, then the notes that belong to the mode (never a block)
void payloadPanel(TxUi& s, const TxModeSpec& mode, bool running) {
    loadPayloadPrefs(s);
    const std::string id = mode.id ? mode.id : "";
    typo::separatorText(tr("What to send"));
    simpleRows(s, mode, running);
    const TxPayloadKind k = txPayloadKind(id);
    if (id == "hell") hellFontRow();
    if (id == "cp16") cp16FontRow();
    if (id == "atsc3") ImGui::TextDisabled("%s", tr("ATSC 3.0 sends its own ROUTE / fMP4 test programme: there is no file or name to set."));
    if (id == "hdr" || id == "cdr") ImGui::TextDisabled("%s", tr("This generator sends its built-in demo programme: there is nothing to type."));
    if (id == "tetra") ImGui::TextDisabled("%s", tr("TETRA sends the downlink test pattern: there is nothing to type."));
    if (id == "marine" && std::string(s.subId) == "wefax") ImGui::TextDisabled("%s", tr("The weather fax sends its test chart (a custom image is not wired)."));
    if (k == TxPayloadKind::Ais || k == TxPayloadKind::AdsB) {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextWrapped("%s", k == TxPayloadKind::Ais
            ? tr("Warning: AIS simulates vessels. Send it only into a dummy load or a shielded setup, never to an antenna: false ships can confuse real receivers, which is a safety risk and illegal in most places.")
            : tr("Warning: ADS-B simulates aircraft. Send it only into a dummy load or a shielded setup, never to an antenna."));
        ImGui::PopStyleColor();
    }
    if (k == TxPayloadKind::LoraAprs) {
        const TxRequest r = makeReq(s, mode, false);   // the packet and the LoRa settings, as the transmission gets them
        const std::string tnc2 = txLoraAprsTnc2(r);
        const TxLoraAprsRadio lr = txLoraAprsRadio(r);
        if (tnc2.empty()) ImGui::TextDisabled("%s", tr("No callsign: the demo LoRa APRS stations are sent (EU settings)."));
        else ImGui::TextDisabled("%s: %s", tr("Sent"), tnc2.c_str());
        ImGui::TextDisabled("SF%d, %.1f kHz, 4/%d, %s %d, %s 0x%02X, CRC %s", lr.sf, lr.bwHz / 1e3, lr.cr, tr("preamble"), lr.preamble, tr("sync word"), lr.syncWord,
                            lr.crc ? tr("on") : tr("off"));
        if (lr.mhz > 0 && std::fabs(s.freqMhz - lr.mhz) > 1e-6) {
            std::string mhz = std::to_string(lr.mhz);   // "433.775000": four decimals, as the presets are written
            mhz.resize(std::min(mhz.size(), mhz.find('.') + 5));
            const std::string b = std::string(tr("Tune to")) + " " + mhz + " MHz";
            ImGui::BeginDisabled(running);
            if (ImGui::SmallButton(b.c_str())) { s.freqMhz = lr.mhz; s.linkRx = false; }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("The frequency of the chosen LoRa APRS preset; the transmitter is on another one."));
        }
    }
    for (const std::string& w : txParamWarnings(mode, makeReq(s, mode, false))) banner(Note::Warn, tr(w.c_str()));
}

} // namespace txui
